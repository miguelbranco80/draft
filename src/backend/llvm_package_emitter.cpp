// Direct LLVM package-module construction from verified Draft MIR.
//
// The package task owns one LLVM context, module, and builder. Its inputs are
// immutable semantic types, ABI classifications, global constants, and the
// artifact-live MIR procedure set selected by native reachability. The output
// is optional inspection text plus optional native bytes; no LLVM allocation or
// pointer survives the synchronous call.
//
// This file is intentionally inside the LLVM object-library boundary. It may
// depend on semantic/MIR read interfaces and LLVM's C API, but semantic and MIR
// modules never include it or call back into it. The implementation grows by
// translating one verified MIR operation directly to LLVM values; it never
// creates a textual instruction and reparses it. Relevant specification:
// docs/specification/06-compiler.md "Native lowering and summaries".

#include "backend/llvm_package_emitter.h"

#include "backend/llvm_module_emission.h"
#include "sema/ieee_float.h"
#include "syntax/literal.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

struct ContextOwner {
  LLVMContextRef value = nullptr;
  ~ContextOwner() {
    if (value != nullptr)
      LLVMContextDispose(value);
  }
  ContextOwner(const ContextOwner &) = delete;
  ContextOwner &operator=(const ContextOwner &) = delete;
  ContextOwner() = default;
};

struct ModuleOwner {
  LLVMModuleRef value = nullptr;
  ~ModuleOwner() {
    if (value != nullptr)
      LLVMDisposeModule(value);
  }
  ModuleOwner(const ModuleOwner &) = delete;
  ModuleOwner &operator=(const ModuleOwner &) = delete;
  ModuleOwner() = default;
};

struct BuilderOwner {
  LLVMBuilderRef value = nullptr;
  ~BuilderOwner() {
    if (value != nullptr)
      LLVMDisposeBuilder(value);
  }
  BuilderOwner(const BuilderOwner &) = delete;
  BuilderOwner &operator=(const BuilderOwner &) = delete;
  BuilderOwner() = default;
};

// DirectPhaseTimer avoids steady-clock reads unless --timings=all requested the
// package task's nested backend rows. finish is idempotent so failure paths can
// publish the exact completed prefix without introducing a scope framework.
class DirectPhaseTimer {
public:
  DirectPhaseTimer(bool enabled, std::uint64_t &destination)
      : destination_(enabled ? &destination : nullptr) {
    if (destination_ != nullptr)
      started_ = Clock::now();
  }

  DirectPhaseTimer(const DirectPhaseTimer &) = delete;
  DirectPhaseTimer &operator=(const DirectPhaseTimer &) = delete;
  ~DirectPhaseTimer() { finish(); }

  void finish() {
    if (destination_ == nullptr)
      return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             Clock::now() - started_)
                             .count();
    assert(elapsed >= 0 && "direct LLVM phase duration must be nonnegative");
    *destination_ = static_cast<std::uint64_t>(elapsed);
    destination_ = nullptr;
  }

private:
  using Clock = std::chrono::steady_clock;
  std::uint64_t *destination_ = nullptr;
  Clock::time_point started_;
};

[[nodiscard]] std::string encoded_name(std::string_view text) {
  static constexpr char digits[] = "0123456789ABCDEF";
  std::string result;
  for (const char character : text) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (std::isalnum(byte) != 0) {
      result.push_back(static_cast<char>(byte));
    } else {
      result.push_back('_');
      result.push_back(digits[(byte >> 4U) & 0x0fU]);
      result.push_back(digits[byte & 0x0fU]);
    }
  }
  return result;
}

// DirectPackageBuilder owns all LLVM identities for one package. Vectors use
// semantic TypeId/SymbolId/MirValueId indices directly, preserving the compact
// domains already verified by prior phases and avoiding string-keyed maps in
// the common path.
class DirectPackageBuilder {
public:
  DirectPackageBuilder(const TargetProfile &target,
                       const SourceManager &sources,
                       const LlvmPackageEmissionOptions &options,
                       const SemanticPackage &semantic, const CAbiTable &abi,
                       const ConstantTable &global_initializers,
                       std::span<const SymbolId> globals,
                       std::span<const MirProcedure *const> procedures,
                       DiagnosticSink &diagnostics)
      : target_(target), options_(options), semantic_(semantic), abi_(abi),
        globals_(globals), procedures_(procedures), diagnostics_(diagnostics),
        llvm_types_(semantic.types.size(), nullptr),
        functions_(semantic.symbols.symbol_count(), nullptr),
        global_values_(semantic.symbols.symbol_count(), nullptr) {
    // Debug locations and global initializers join this same builder as their
    // direct lowering lands. Keep them in the public package boundary now, but
    // do not retain inert references in the scalar-procedure foundation.
    (void)sources;
    (void)global_initializers;
  }

  [[nodiscard]] LlvmPackageEmissionResult run() {
    LlvmPackageEmissionResult result;
    initial_errors_ = diagnostics_.error_count();
    DirectPhaseTimer construction_timing(
        options_.collect_phase_timings,
        result.phase_timings.module_construction_nanoseconds);

    if (!abi_.valid_prefix_for(semantic_.types, target_.facts)) {
      error(SourceRange::invalid(),
            "ABI classification table does not match package and target");
      return result;
    }
    if (options_.module.emit_debug_information ||
        options_.module.emit_runtime_support ||
        options_.module.emit_program_entry ||
        options_.module.validation_kind != ValidationKind::None) {
      error(SourceRange::invalid(),
            "direct package builder has not yet implemented runtime, entry, "
            "validation, or debug module products");
      return result;
    }

    context_.value = LLVMContextCreate();
    if (context_.value == nullptr) {
      error(SourceRange::invalid(), "could not allocate an LLVM context");
      return result;
    }
    const std::string module_name =
        display_package_identity(options_.module.package);
    module_.value =
        LLVMModuleCreateWithNameInContext(module_name.c_str(), context_.value);
    builder_.value = LLVMCreateBuilderInContext(context_.value);
    if (module_.value == nullptr || builder_.value == nullptr) {
      error(SourceRange::invalid(),
            "could not allocate an LLVM module builder");
      return result;
    }
    const std::string source_name =
        "draft:" + options_.module.package.root_identity + "/" +
        options_.module.package.root_relative_path;
    LLVMSetSourceFileName(module_.value, source_name.data(),
                          source_name.size());
    LLVMSetTarget(module_.value, target_.llvm_triple.c_str());
    LLVMSetDataLayout(module_.value, target_.llvm_data_layout.c_str());

    create_nominal_type_identities();
    define_nominal_type_bodies();
    declare_procedures();
    emit_procedures();
    construction_timing.finish();
    if (diagnostics_.error_count() != initial_errors_)
      return result;

    if (options_.retain_llvm_text) {
      DirectPhaseTimer printing_timing(
          options_.collect_phase_timings,
          result.phase_timings.llvm_text_printing_nanoseconds);
      char *printed = LLVMPrintModuleToString(module_.value);
      if (printed == nullptr) {
        error(SourceRange::invalid(), "LLVM could not print the direct module");
        return result;
      }
      result.llvm_text = printed;
      LLVMDisposeMessage(printed);
      printing_timing.finish();
    }

    if (options_.native_options.has_value()) {
      result.native = emit_constructed_llvm_module_in_process(
          target_, module_name, module_.value, *options_.native_options);
      if (!result.native.ok)
        return result;
    } else {
      char *message = nullptr;
      const int status =
          LLVMVerifyModule(module_.value, LLVMReturnStatusAction, &message);
      if (status != 0) {
        std::string reason = message == nullptr
                                 ? std::string("unknown LLVM verifier failure")
                                 : std::string(message);
        if (message != nullptr)
          LLVMDisposeMessage(message);
        error(SourceRange::invalid(), "LLVM verification failed: " + reason);
        return result;
      }
      if (message != nullptr)
        LLVMDisposeMessage(message);
    }
    result.ok = diagnostics_.error_count() == initial_errors_;
    return result;
  }

private:
  void error(SourceRange range, const std::string &message) const {
    diagnostics_.error(range, "direct LLVM emission: " + message);
  }

  [[nodiscard]] const Type &type(TypeId id) const {
    return semantic_.types.type(id);
  }

  [[nodiscard]] TypeId runtime_scalar_id(TypeId id) const {
    while (type(id).kind == TypeKind::Distinct)
      id = type(id).element;
    return id;
  }

  [[nodiscard]] TypeKind runtime_scalar_kind(TypeId id) const {
    return type(runtime_scalar_id(id)).kind;
  }

  [[nodiscard]] bool signed_integer(TypeId id) const {
    const Type &value = type(runtime_scalar_id(id));
    if (value.kind == TypeKind::Enum && value.element.is_valid()) {
      return signed_integer(value.element);
    }
    return value.kind == TypeKind::SignedInteger ||
           value.kind == TypeKind::Rune;
  }

  [[nodiscard]] bool struct_has_bit_fields(TypeId id) const {
    const Type &value = type(runtime_scalar_id(id));
    if (value.kind != TypeKind::Struct)
      return false;
    return std::any_of(value.member_layouts.begin(), value.member_layouts.end(),
                       [](const FieldLayout &layout) {
                         return layout.kind == FieldLayoutKind::BitField;
                       });
  }

  [[nodiscard]] bool
  contains_type_parameter(TypeId id, std::vector<TypeId> &active) const {
    if (!id.is_valid())
      return false;
    if (std::find(active.begin(), active.end(), id) != active.end())
      return false;
    const Type &value = type(id);
    if (value.kind == TypeKind::TypeParameter)
      return true;
    active.push_back(id);
    bool result = value.element.is_valid() &&
                  contains_type_parameter(value.element, active);
    for (TypeId member : value.members) {
      result = result || contains_type_parameter(member, active);
    }
    active.pop_back();
    return result;
  }

  [[nodiscard]] bool contains_type_parameter(TypeId id) const {
    std::vector<TypeId> active;
    return contains_type_parameter(id, active);
  }

  [[nodiscard]] bool is_parametric_template_type(TypeId id) const {
    for (std::size_t index = 0; index < semantic_.symbols.symbol_count();
         ++index) {
      const Symbol &symbol =
          semantic_.symbols.symbol(SymbolId{static_cast<std::uint32_t>(index)});
      if (symbol.kind == SymbolKind::Type && symbol.flags.parametric &&
          symbol.type == id) {
        return true;
      }
    }
    return false;
  }

  void create_nominal_type_identities() {
    for (std::size_t index = 0; index < semantic_.types.size(); ++index) {
      const TypeId id{static_cast<std::uint32_t>(index)};
      const Type &value = type(id);
      if (is_parametric_template_type(id) || contains_type_parameter(id))
        continue;
      if (value.kind != TypeKind::Struct && value.kind != TypeKind::Variant &&
          value.kind != TypeKind::Union) {
        continue;
      }
      const std::string name = "draft.type." + std::to_string(index);
      llvm_types_[index] = LLVMStructCreateNamed(context_.value, name.c_str());
    }
  }

  void define_nominal_type_bodies() {
    for (std::size_t index = 0; index < semantic_.types.size(); ++index) {
      const TypeId id{static_cast<std::uint32_t>(index)};
      const Type &value = type(id);
      LLVMTypeRef nominal = llvm_types_[index];
      if (nominal == nullptr)
        continue;
      if (!value.layout.known) {
        error(value.declaration, "nominal runtime type has no complete layout");
        continue;
      }
      if (value.kind == TypeKind::Variant || value.kind == TypeKind::Union ||
          struct_has_bit_fields(id)) {
        LLVMTypeRef storage = LLVMArrayType2(
            LLVMInt8TypeInContext(context_.value), value.layout.size);
        LLVMStructSetBody(nominal, &storage, 1, 1);
        continue;
      }

      std::vector<LLVMTypeRef> fields;
      std::uint64_t cursor = 0;
      for (std::size_t member = 0; member < value.members.size(); ++member) {
        const std::uint64_t offset = member < value.member_offsets.size()
                                         ? value.member_offsets[member]
                                         : cursor;
        if (offset > cursor) {
          fields.push_back(LLVMArrayType2(LLVMInt8TypeInContext(context_.value),
                                          offset - cursor));
        }
        fields.push_back(llvm_type(value.members[member]));
        cursor = offset + type(value.members[member]).layout.size;
      }
      if (value.layout.size > cursor) {
        fields.push_back(LLVMArrayType2(LLVMInt8TypeInContext(context_.value),
                                        value.layout.size - cursor));
      }
      LLVMStructSetBody(nominal, fields.empty() ? nullptr : fields.data(),
                        static_cast<unsigned>(fields.size()), 1);
    }
  }

  [[nodiscard]] LLVMTypeRef llvm_type(TypeId id) {
    if (!id.is_valid() || id.value >= llvm_types_.size()) {
      error(SourceRange::invalid(), "invalid semantic type reached LLVM");
      return LLVMInt8TypeInContext(context_.value);
    }
    if (llvm_types_[id.value] != nullptr)
      return llvm_types_[id.value];
    const Type &value = type(id);
    LLVMTypeRef result = nullptr;
    switch (value.kind) {
    case TypeKind::Void:
      result = LLVMVoidTypeInContext(context_.value);
      break;
    case TypeKind::Bool:
      result = LLVMInt1TypeInContext(context_.value);
      break;
    case TypeKind::BooleanStorage:
    case TypeKind::SignedInteger:
    case TypeKind::UnsignedInteger:
    case TypeKind::Rune:
    case TypeKind::EndianScalar:
      result = LLVMIntTypeInContext(context_.value, value.bit_width);
      break;
    case TypeKind::Float:
      if (value.bit_width == 16)
        result = LLVMHalfTypeInContext(context_.value);
      if (value.bit_width == 32)
        result = LLVMFloatTypeInContext(context_.value);
      if (value.bit_width == 64)
        result = LLVMDoubleTypeInContext(context_.value);
      if (value.bit_width == 128)
        result = LLVMFP128TypeInContext(context_.value);
      break;
    case TypeKind::RawPointer:
    case TypeKind::CString:
    case TypeKind::Pointer:
    case TypeKind::MultiPointer:
    case TypeKind::Procedure:
      result = LLVMPointerTypeInContext(context_.value, 0);
      break;
    case TypeKind::String:
    case TypeKind::Slice: {
      LLVMTypeRef members[]{LLVMPointerTypeInContext(context_.value, 0),
                            LLVMInt64TypeInContext(context_.value)};
      result = LLVMStructTypeInContext(context_.value, members, 2, 0);
      break;
    }
    case TypeKind::Array:
      result = LLVMArrayType2(llvm_type(value.element), value.element_count);
      break;
    case TypeKind::Tuple: {
      std::vector<LLVMTypeRef> members;
      members.reserve(value.members.size());
      for (TypeId member : value.members)
        members.push_back(llvm_type(member));
      result = LLVMStructTypeInContext(
          context_.value, members.empty() ? nullptr : members.data(),
          static_cast<unsigned>(members.size()), 0);
      break;
    }
    case TypeKind::Simd:
      result = LLVMVectorType(llvm_type(value.element),
                              static_cast<unsigned>(value.element_count));
      break;
    case TypeKind::Struct:
    case TypeKind::Variant:
    case TypeKind::Union:
      result = llvm_types_[id.value];
      break;
    case TypeKind::Enum:
      result = LLVMIntTypeInContext(
          context_.value, static_cast<unsigned>(value.layout.size * 8U));
      break;
    case TypeKind::Distinct:
      result = llvm_type(value.element);
      break;
    case TypeKind::Invalid:
    case TypeKind::UntypedInteger:
    case TypeKind::UntypedFloat:
    case TypeKind::TypeParameter:
    case TypeKind::MetaType:
      break;
    }
    if (result == nullptr) {
      error(value.declaration, "non-runtime semantic type reached LLVM");
      result = LLVMInt8TypeInContext(context_.value);
    }
    llvm_types_[id.value] = result;
    return result;
  }

  [[nodiscard]] std::string
  decoded_linker_name(std::string_view spelling) const {
    if (spelling.size() >= 2 && spelling.front() == '"' &&
        spelling.back() == '"') {
      return decode_string_literal(spelling, TokenKind::StringLiteral)
          .value_or(std::string());
    }
    return std::string(spelling);
  }

  [[nodiscard]] std::optional<std::string>
  native_symbol_name(SymbolId symbol_id) const {
    for (const NativeBinding &binding : semantic_.native_bindings) {
      if (binding.symbol != symbol_id)
        continue;
      const std::string decoded =
          decoded_linker_name(binding.linker_name_spelling);
      if (!decoded.empty())
        return decoded;
    }
    for (const ImportedSymbol &imported :
         semantic_.imported_symbols_for_read()) {
      if (imported.proxy != symbol_id ||
          imported.native_linker_name_spelling.empty()) {
        continue;
      }
      const std::string decoded =
          decoded_linker_name(imported.native_linker_name_spelling);
      if (!decoded.empty())
        return decoded;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::string package_symbol_name(const PackageIdentity &identity,
                                                std::string_view name) const {
    return "draft." + encoded_name(identity.root_identity) + "." +
           encoded_name(identity.root_relative_path) + "." + encoded_name(name);
  }

  [[nodiscard]] std::string symbol_name(SymbolId symbol_id) const {
    if (const std::optional<std::string> native =
            native_symbol_name(symbol_id)) {
      return *native;
    }
    for (const ImportedSymbol &imported :
         semantic_.imported_symbols_for_read()) {
      if (imported.proxy == symbol_id) {
        return package_symbol_name(
            {imported.root_identity, imported.root_relative_path},
            imported.public_name);
      }
    }
    const Symbol &symbol = semantic_.symbols.symbol(symbol_id);
    return package_symbol_name(
        options_.module.package,
        symbol.linkage_name.empty() ? symbol.name : symbol.linkage_name);
  }

  [[nodiscard]] LLVMTypeRef function_type(TypeId type_id) {
    const Type &signature = type(type_id);
    if (signature.kind != TypeKind::Procedure || signature.members.empty()) {
      error(signature.declaration, "procedure symbol has no function type");
      return LLVMFunctionType(LLVMVoidTypeInContext(context_.value), nullptr, 0,
                              0);
    }
    if (signature.c_calling_convention) {
      error(signature.declaration,
            "direct package builder has not yet implemented C ABI signatures");
    }
    std::vector<LLVMTypeRef> parameters;
    parameters.reserve(signature.members.size());
    parameters.push_back(LLVMPointerTypeInContext(context_.value, 0));
    for (std::size_t index = 0; index + 1 < signature.members.size(); ++index) {
      parameters.push_back(llvm_type(signature.members[index]));
    }
    return LLVMFunctionType(llvm_type(signature.members.back()),
                            parameters.data(),
                            static_cast<unsigned>(parameters.size()), 0);
  }

  [[nodiscard]] LLVMValueRef function_for_symbol(SymbolId symbol_id) {
    if (!symbol_id.is_valid() || symbol_id.value >= functions_.size()) {
      error(SourceRange::invalid(), "invalid procedure symbol reached LLVM");
      return nullptr;
    }
    if (functions_[symbol_id.value] != nullptr)
      return functions_[symbol_id.value];
    const Symbol &symbol = semantic_.symbols.symbol(symbol_id);
    if (symbol.kind != SymbolKind::Procedure || !symbol.type.is_valid()) {
      error(symbol.name_range, "non-procedure symbol used as procedure");
      return nullptr;
    }
    const std::string name = symbol_name(symbol_id);
    LLVMValueRef existing = LLVMGetNamedFunction(module_.value, name.c_str());
    if (existing != nullptr) {
      functions_[symbol_id.value] = existing;
      return existing;
    }
    LLVMValueRef function = LLVMAddFunction(module_.value, name.c_str(),
                                            function_type(symbol.type));
    LLVMSetVisibility(function, LLVMHiddenVisibility);
    functions_[symbol_id.value] = function;
    return function;
  }

  void declare_procedures() {
    for (const MirProcedure *procedure : procedures_) {
      if (procedure != nullptr && procedure->valid) {
        (void)function_for_symbol(procedure->symbol);
      }
    }
  }

  [[nodiscard]] LLVMValueRef
  value_operand(const std::vector<LLVMValueRef> &values, MirValueId id,
                SourceRange range) const {
    if (!id.is_valid() || id.value >= values.size() ||
        values[id.value] == nullptr) {
      error(range, "MIR value has no constructed LLVM value");
      return LLVMGetUndef(LLVMInt8TypeInContext(context_.value));
    }
    return values[id.value];
  }

  [[nodiscard]] LLVMValueRef integer_constant(LLVMTypeRef value_type,
                                              const BigInteger &value) const {
    const std::string decimal = value.to_decimal();
    return LLVMConstIntOfStringAndSize(
        value_type, decimal.data(), static_cast<unsigned>(decimal.size()), 10);
  }

  [[nodiscard]] LLVMValueRef enum_constant(SymbolId member,
                                           LLVMTypeRef value_type) {
    for (const EnumMemberValue &entry :
         semantic_.enum_member_values_for_read()) {
      if (entry.member == member)
        return integer_constant(value_type, entry.value);
    }
    return LLVMConstNull(value_type);
  }

  [[nodiscard]] LLVMValueRef constant_value(const MirInstruction &instruction) {
    LLVMTypeRef value_type = llvm_type(instruction.type);
    const ConstantValue &value = instruction.constant;
    switch (value.kind) {
    case ConstantKind::Nil:
      return LLVMConstNull(value_type);
    case ConstantKind::Bool:
      return LLVMConstInt(value_type, value.boolean ? 1 : 0, 0);
    case ConstantKind::Integer:
      return integer_constant(value_type, value.integer);
    case ConstantKind::EnumLabel:
      return enum_constant(instruction.symbol, value_type);
    case ConstantKind::Float: {
      const Type &storage = type(runtime_scalar_id(instruction.type));
      const std::optional<IeeeBinaryFormat> format =
          storage.kind == TypeKind::Float
              ? ieee_format_for_width(storage.bit_width)
              : std::nullopt;
      const std::optional<std::uint64_t> bits =
          format.has_value()
              ? (value.float_bit_width == storage.bit_width
                     ? std::optional<std::uint64_t>(value.float_bits)
                     : round_ieee_bits(value.floating, *format))
              : std::nullopt;
      if (!bits.has_value()) {
        error(instruction.range,
              "floating constant has no supported IEEE format");
        return LLVMConstNull(value_type);
      }
      LLVMValueRef integer = LLVMConstInt(
          LLVMIntTypeInContext(context_.value, storage.bit_width), *bits, 0);
      return LLVMConstBitCast(integer, value_type);
    }
    case ConstantKind::Unavailable:
    case ConstantKind::String:
    case ConstantKind::Aggregate:
    case ConstantKind::Procedure:
    case ConstantKind::Type:
    case ConstantKind::Target:
      error(instruction.range, "constant kind is not yet implemented directly");
      return LLVMConstNull(value_type);
    }
    return LLVMConstNull(value_type);
  }

  [[nodiscard]] LLVMValueRef
  emit_unary(const MirProcedure &procedure, const MirInstruction &instruction,
             const std::vector<LLVMValueRef> &values) {
    (void)procedure;
    LLVMValueRef source =
        value_operand(values, instruction.operands.front(), instruction.range);
    switch (instruction.operation) {
    case HirOperation::Positive:
      return source;
    case HirOperation::Negate:
      return runtime_scalar_kind(instruction.type) == TypeKind::Float
                 ? LLVMBuildFNeg(builder_.value, source, "")
                 : LLVMBuildNeg(builder_.value, source, "");
    case HirOperation::LogicalNot:
    case HirOperation::BitwiseNot:
      return LLVMBuildNot(builder_.value, source, "");
    default:
      error(instruction.range, "unsupported direct unary operation");
      return LLVMGetPoison(llvm_type(instruction.type));
    }
  }

  [[nodiscard]] LLVMIntPredicate integer_predicate(HirOperation operation,
                                                   bool is_signed) const {
    switch (operation) {
    case HirOperation::Equal:
      return LLVMIntEQ;
    case HirOperation::NotEqual:
      return LLVMIntNE;
    case HirOperation::Less:
      return is_signed ? LLVMIntSLT : LLVMIntULT;
    case HirOperation::LessEqual:
      return is_signed ? LLVMIntSLE : LLVMIntULE;
    case HirOperation::Greater:
      return is_signed ? LLVMIntSGT : LLVMIntUGT;
    case HirOperation::GreaterEqual:
      return is_signed ? LLVMIntSGE : LLVMIntUGE;
    default:
      return LLVMIntEQ;
    }
  }

  [[nodiscard]] LLVMRealPredicate
  float_predicate(HirOperation operation) const {
    switch (operation) {
    case HirOperation::Equal:
      return LLVMRealOEQ;
    case HirOperation::NotEqual:
      return LLVMRealUNE;
    case HirOperation::Less:
      return LLVMRealOLT;
    case HirOperation::LessEqual:
      return LLVMRealOLE;
    case HirOperation::Greater:
      return LLVMRealOGT;
    case HirOperation::GreaterEqual:
      return LLVMRealOGE;
    default:
      return LLVMRealOEQ;
    }
  }

  [[nodiscard]] bool comparison_operation(HirOperation operation) const {
    return operation == HirOperation::Equal ||
           operation == HirOperation::NotEqual ||
           operation == HirOperation::Less ||
           operation == HirOperation::LessEqual ||
           operation == HirOperation::Greater ||
           operation == HirOperation::GreaterEqual;
  }

  [[nodiscard]] LLVMValueRef
  emit_binary(const MirProcedure &procedure, const MirInstruction &instruction,
              const std::vector<LLVMValueRef> &values) {
    const MirValueId left_id = instruction.operands[0];
    LLVMValueRef left = value_operand(values, left_id, instruction.range);
    LLVMValueRef right =
        value_operand(values, instruction.operands[1], instruction.range);
    const TypeId operand_type = procedure.value(left_id).type;
    const bool floating = runtime_scalar_kind(operand_type) == TypeKind::Float;
    if (comparison_operation(instruction.operation)) {
      return floating ? LLVMBuildFCmp(builder_.value,
                                      float_predicate(instruction.operation),
                                      left, right, "")
                      : LLVMBuildICmp(
                            builder_.value,
                            integer_predicate(instruction.operation,
                                              signed_integer(operand_type)),
                            left, right, "");
    }
    if (floating) {
      switch (instruction.operation) {
      case HirOperation::Add:
        return LLVMBuildFAdd(builder_.value, left, right, "");
      case HirOperation::Subtract:
        return LLVMBuildFSub(builder_.value, left, right, "");
      case HirOperation::Multiply:
        return LLVMBuildFMul(builder_.value, left, right, "");
      case HirOperation::Divide:
        return LLVMBuildFDiv(builder_.value, left, right, "");
      case HirOperation::Remainder:
        return LLVMBuildFRem(builder_.value, left, right, "");
      default:
        break;
      }
    } else {
      switch (instruction.operation) {
      case HirOperation::Add:
        return LLVMBuildAdd(builder_.value, left, right, "");
      case HirOperation::Subtract:
        return LLVMBuildSub(builder_.value, left, right, "");
      case HirOperation::Multiply:
        return LLVMBuildMul(builder_.value, left, right, "");
      case HirOperation::Divide:
        return signed_integer(operand_type)
                   ? LLVMBuildSDiv(builder_.value, left, right, "")
                   : LLVMBuildUDiv(builder_.value, left, right, "");
      case HirOperation::Remainder:
        return signed_integer(operand_type)
                   ? LLVMBuildSRem(builder_.value, left, right, "")
                   : LLVMBuildURem(builder_.value, left, right, "");
      case HirOperation::BitwiseAnd:
        return LLVMBuildAnd(builder_.value, left, right, "");
      case HirOperation::BitwiseOr:
        return LLVMBuildOr(builder_.value, left, right, "");
      case HirOperation::BitwiseXor:
        return LLVMBuildXor(builder_.value, left, right, "");
      case HirOperation::ShiftLeft:
        return LLVMBuildShl(builder_.value, left, right, "");
      case HirOperation::ShiftRight:
        return signed_integer(operand_type)
                   ? LLVMBuildAShr(builder_.value, left, right, "")
                   : LLVMBuildLShr(builder_.value, left, right, "");
      default:
        break;
      }
    }
    error(instruction.range, "unsupported direct binary operation");
    return LLVMGetPoison(llvm_type(instruction.type));
  }

  void emit_instruction(const MirProcedure &procedure,
                        const MirInstruction &instruction,
                        std::vector<LLVMValueRef> &values,
                        LLVMValueRef context_parameter,
                        const std::vector<LLVMValueRef> &locals) {
    LLVMValueRef result = nullptr;
    switch (instruction.kind) {
    case MirInstructionKind::Constant:
      result = constant_value(instruction);
      break;
    case MirInstructionKind::Zero:
      result = LLVMConstNull(llvm_type(instruction.type));
      break;
    case MirInstructionKind::Context:
      result = context_parameter;
      break;
    case MirInstructionKind::LocalAddress:
      if (instruction.local.is_valid() &&
          instruction.local.value < locals.size()) {
        result = locals[instruction.local.value];
      } else {
        error(instruction.range, "local address names an invalid local");
      }
      break;
    case MirInstructionKind::GlobalAddress:
      if (instruction.symbol.is_valid() &&
          instruction.symbol.value < global_values_.size()) {
        result = global_values_[instruction.symbol.value];
      }
      if (result == nullptr) {
        error(instruction.range, "global address has no direct LLVM global");
      }
      break;
    case MirInstructionKind::ProcedureReference:
      result = function_for_symbol(instruction.symbol);
      break;
    case MirInstructionKind::Load:
      result = LLVMBuildLoad2(
          builder_.value, llvm_type(instruction.type),
          value_operand(values, instruction.operands[0], instruction.range),
          "");
      LLVMSetAlignment(result, instruction.alignment);
      break;
    case MirInstructionKind::Store: {
      LLVMValueRef store = LLVMBuildStore(
          builder_.value,
          value_operand(values, instruction.operands[1], instruction.range),
          value_operand(values, instruction.operands[0], instruction.range));
      LLVMSetAlignment(store, instruction.alignment);
      break;
    }
    case MirInstructionKind::Unary:
      result = emit_unary(procedure, instruction, values);
      break;
    case MirInstructionKind::Binary:
      result = emit_binary(procedure, instruction, values);
      break;
    case MirInstructionKind::Call: {
      const MirValueId callee_id = instruction.operands.front();
      const TypeId signature_id =
          runtime_scalar_id(procedure.value(callee_id).type);
      const Type &signature = type(signature_id);
      if (signature.c_calling_convention) {
        error(instruction.range, "direct C ABI calls are not yet implemented");
        break;
      }
      std::vector<LLVMValueRef> arguments;
      arguments.reserve(instruction.operands.size() - 1);
      for (std::size_t index = 1; index < instruction.operands.size();
           ++index) {
        arguments.push_back(value_operand(values, instruction.operands[index],
                                          instruction.range));
      }
      result =
          LLVMBuildCall2(builder_.value, function_type(signature_id),
                         value_operand(values, callee_id, instruction.range),
                         arguments.empty() ? nullptr : arguments.data(),
                         static_cast<unsigned>(arguments.size()), "");
      if (instruction.type == semantic_.types.builtins().void_type)
        result = nullptr;
      break;
    }
    case MirInstructionKind::Length:
      result = LLVMBuildExtractValue(builder_.value,
                                     value_operand(values,
                                                   instruction.operands.front(),
                                                   instruction.range),
                                     1, "");
      break;
    case MirInstructionKind::RawData:
      result = LLVMBuildExtractValue(builder_.value,
                                     value_operand(values,
                                                   instruction.operands.front(),
                                                   instruction.range),
                                     0, "");
      break;
    case MirInstructionKind::Invalid:
    case MirInstructionKind::AtomicLoad:
    case MirInstructionKind::AtomicStore:
    case MirInstructionKind::AtomicExchange:
    case MirInstructionKind::AtomicReadModifyWrite:
    case MirInstructionKind::AtomicCompareExchange:
    case MirInstructionKind::AtomicFence:
    case MirInstructionKind::Convert:
    case MirInstructionKind::PointerOffset:
    case MirInstructionKind::PointerSubtract:
    case MirInstructionKind::Assert:
    case MirInstructionKind::MemberAddress:
    case MirInstructionKind::LoadBitField:
    case MirInstructionKind::StoreBitField:
    case MirInstructionKind::ExtractMember:
    case MirInstructionKind::IndexAddress:
    case MirInstructionKind::BoundsCheck:
    case MirInstructionKind::SliceBoundsCheck:
    case MirInstructionKind::Slice:
    case MirInstructionKind::Aggregate:
    case MirInstructionKind::Assembly:
    case MirInstructionKind::Trap:
      error(instruction.range,
            std::string("MIR operation is not yet implemented directly: ") +
                mir_instruction_kind_name(instruction.kind));
      break;
    }
    if (instruction.result.is_valid()) {
      if (result == nullptr) {
        error(instruction.range,
              "result-producing MIR operation built no value");
      } else {
        values[instruction.result.value] = result;
        // Alias-like MIR rows may reuse a constant, argument, global, local
        // allocation, or function. Renaming that shared LLVM value here would
        // mutate its declaration identity merely because MIR assigned another
        // local value ID. Only a newly created instruction owns this result
        // name; other values retain the name established at their definition.
        if (LLVMIsAInstruction(result) != nullptr) {
          const std::string name =
              "v" + std::to_string(instruction.result.value);
          LLVMSetValueName2(result, name.data(), name.size());
        }
      }
    }
  }

  void emit_terminator(const MirProcedure &procedure,
                       const MirTerminator &terminator,
                       const std::vector<LLVMValueRef> &values,
                       const std::vector<LLVMBasicBlockRef> &blocks) {
    switch (terminator.kind) {
    case MirTerminatorKind::Return:
      if (terminator.value.is_valid()) {
        LLVMBuildRet(builder_.value,
                     value_operand(values, terminator.value, terminator.range));
      } else {
        LLVMBuildRetVoid(builder_.value);
      }
      break;
    case MirTerminatorKind::Branch:
      LLVMBuildBr(builder_.value, blocks[terminator.targets[0].value]);
      break;
    case MirTerminatorKind::ConditionalBranch:
      LLVMBuildCondBr(builder_.value,
                      value_operand(values, terminator.value, terminator.range),
                      blocks[terminator.targets[0].value],
                      blocks[terminator.targets[1].value]);
      break;
    case MirTerminatorKind::Switch: {
      LLVMValueRef switch_value =
          value_operand(values, terminator.value, terminator.range);
      LLVMValueRef switch_instruction = LLVMBuildSwitch(
          builder_.value, switch_value, blocks[terminator.targets[0].value],
          static_cast<unsigned>(terminator.switch_arms.size()));
      for (const MirSwitchArm &arm : terminator.switch_arms) {
        LLVMAddCase(switch_instruction,
                    value_operand(values, arm.label, terminator.range),
                    blocks[arm.target.value]);
      }
      break;
    }
    case MirTerminatorKind::Unreachable:
    case MirTerminatorKind::Invalid:
      LLVMBuildUnreachable(builder_.value);
      if (terminator.kind == MirTerminatorKind::Invalid) {
        error(terminator.range, "unterminated MIR block reached LLVM");
      }
      break;
    }
    (void)procedure;
  }

  void emit_procedure(const MirProcedure &procedure) {
    if (!procedure.valid)
      return;
    const Type &signature = type(procedure.type);
    if (signature.c_calling_convention) {
      error(procedure.range,
            "direct C ABI procedure definitions are not implemented");
      return;
    }
    LLVMValueRef function = function_for_symbol(procedure.symbol);
    if (function == nullptr)
      return;
    std::vector<LLVMBasicBlockRef> blocks;
    blocks.reserve(procedure.blocks.size());
    for (std::size_t index = 0; index < procedure.blocks.size(); ++index) {
      const std::string name = "b" + std::to_string(index);
      blocks.push_back(LLVMAppendBasicBlockInContext(context_.value, function,
                                                     name.c_str()));
    }
    if (!procedure.entry.is_valid() || procedure.entry.value >= blocks.size()) {
      error(procedure.range, "MIR procedure has no valid entry block");
      return;
    }

    LLVMPositionBuilderAtEnd(builder_.value, blocks[procedure.entry.value]);
    std::vector<LLVMValueRef> locals(procedure.locals.size(), nullptr);
    for (std::size_t index = 0; index < procedure.locals.size(); ++index) {
      const MirLocal &local = procedure.locals[index];
      const std::string name = "l" + std::to_string(index);
      LLVMValueRef allocation =
          LLVMBuildAlloca(builder_.value, llvm_type(local.type), name.c_str());
      LLVMSetAlignment(allocation, type(local.type).layout.alignment);
      locals[index] = allocation;
      if (local.kind != MirLocalKind::Parameter)
        continue;
      LLVMValueRef parameter =
          LLVMGetParam(function, local.parameter_index + 1);
      LLVMValueRef store =
          LLVMBuildStore(builder_.value, parameter, allocation);
      LLVMSetAlignment(store, type(local.type).layout.alignment);
    }

    LLVMValueRef context_parameter = LLVMGetParam(function, 0);
    std::vector<LLVMValueRef> values(procedure.values.size(), nullptr);
    for (std::size_t block_index = 0; block_index < procedure.blocks.size();
         ++block_index) {
      LLVMPositionBuilderAtEnd(builder_.value, blocks[block_index]);
      const MirBlock &block = procedure.blocks[block_index];
      for (MirInstructionId instruction_id : block.instructions) {
        emit_instruction(procedure, procedure.instruction(instruction_id),
                         values, context_parameter, locals);
      }
      emit_terminator(procedure, block.terminator, values, blocks);
    }
  }

  void emit_procedures() {
    for (const MirProcedure *procedure : procedures_) {
      if (procedure != nullptr)
        emit_procedure(*procedure);
    }
  }

  const TargetProfile &target_;
  const LlvmPackageEmissionOptions &options_;
  const SemanticPackage &semantic_;
  const CAbiTable &abi_;
  std::span<const SymbolId> globals_;
  std::span<const MirProcedure *const> procedures_;
  DiagnosticSink &diagnostics_;
  std::size_t initial_errors_ = 0;

  ContextOwner context_;
  ModuleOwner module_;
  BuilderOwner builder_;
  std::vector<LLVMTypeRef> llvm_types_;
  std::vector<LLVMValueRef> functions_;
  std::vector<LLVMValueRef> global_values_;
};

} // namespace

LlvmPackageEmissionResult emit_llvm_package_direct(
    const TargetProfile &target, const SourceManager &sources,
    const LlvmPackageEmissionOptions &options, const SemanticPackage &semantic,
    const CAbiTable &abi, const ConstantTable &global_initializers,
    std::span<const SymbolId> globals,
    std::span<const MirProcedure *const> procedures,
    DiagnosticSink &diagnostics) {
  DirectPackageBuilder builder(target, sources, options, semantic, abi,
                               global_initializers, globals, procedures,
                               diagnostics);
  return builder.run();
}

} // namespace draft
