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
        global_initializers_(global_initializers), globals_(globals),
        procedures_(procedures), diagnostics_(diagnostics),
        llvm_types_(semantic.types.size(), nullptr),
        functions_(semantic.symbols.symbol_count(), nullptr),
        global_values_(semantic.symbols.symbol_count(), nullptr) {
    // Debug locations join this same builder as their direct lowering lands.
    // Keep SourceManager in the public package boundary now, but do not retain
    // an inert reference before metadata construction uses it.
    (void)sources;
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
    emit_globals();
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

  [[nodiscard]] bool integer_kind(TypeKind kind) const {
    return kind == TypeKind::Bool || kind == TypeKind::BooleanStorage ||
           kind == TypeKind::SignedInteger ||
           kind == TypeKind::UnsignedInteger || kind == TypeKind::Rune ||
           kind == TypeKind::EndianScalar || kind == TypeKind::Enum;
  }

  [[nodiscard]] std::uint32_t integer_bits(TypeId id) const {
    const Type &value = type(id);
    if (value.kind == TypeKind::Bool)
      return 1;
    if (value.kind == TypeKind::Enum) {
      return static_cast<std::uint32_t>(value.layout.size * 8U);
    }
    if (value.kind == TypeKind::Distinct)
      return integer_bits(value.element);
    return value.bit_width;
  }

  [[nodiscard]] bool signed_integer(TypeId id) const {
    const Type &value = type(runtime_scalar_id(id));
    if (value.kind == TypeKind::Enum && value.element.is_valid()) {
      return signed_integer(value.element);
    }
    return value.kind == TypeKind::SignedInteger ||
           value.kind == TypeKind::Rune;
  }

  [[nodiscard]] LLVMAtomicOrdering atomic_order(AtomicMemoryOrder order) const {
    switch (order) {
    case AtomicMemoryOrder::Relaxed:
      return LLVMAtomicOrderingMonotonic;
    case AtomicMemoryOrder::Acquire:
      return LLVMAtomicOrderingAcquire;
    case AtomicMemoryOrder::Release:
      return LLVMAtomicOrderingRelease;
    case AtomicMemoryOrder::AcquireRelease:
      return LLVMAtomicOrderingAcquireRelease;
    case AtomicMemoryOrder::SequentiallyConsistent:
      return LLVMAtomicOrderingSequentiallyConsistent;
    }
    return LLVMAtomicOrderingSequentiallyConsistent;
  }

  [[nodiscard]] LLVMAtomicRMWBinOp
  atomic_rmw_operation(HirOperation operation) const {
    switch (operation) {
    case HirOperation::Add:
      return LLVMAtomicRMWBinOpAdd;
    case HirOperation::Subtract:
      return LLVMAtomicRMWBinOpSub;
    case HirOperation::BitwiseAnd:
      return LLVMAtomicRMWBinOpAnd;
    case HirOperation::BitwiseOr:
      return LLVMAtomicRMWBinOpOr;
    case HirOperation::BitwiseXor:
      return LLVMAtomicRMWBinOpXor;
    default:
      return LLVMAtomicRMWBinOpXchg;
    }
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

  [[nodiscard]] bool memory_aggregate_kind(TypeKind kind) const {
    return kind == TypeKind::String || kind == TypeKind::Slice ||
           kind == TypeKind::Array || kind == TypeKind::Tuple ||
           kind == TypeKind::Struct || kind == TypeKind::Variant ||
           kind == TypeKind::Union;
  }

  [[nodiscard]] std::size_t aggregate_index(TypeId aggregate_type,
                                            std::uint64_t offset) const {
    const Type &aggregate = type(runtime_scalar_id(aggregate_type));
    if (aggregate.kind == TypeKind::Array) {
      const std::uint64_t stride = type(aggregate.element).layout.size;
      return stride == 0 ? 0 : static_cast<std::size_t>(offset / stride);
    }
    if (aggregate.kind == TypeKind::Tuple) {
      for (std::size_t index = 0; index < aggregate.member_offsets.size();
           ++index) {
        if (aggregate.member_offsets[index] == offset)
          return index;
      }
      return 0;
    }

    // Named structs use explicit padding fields inside a packed LLVM body.
    // Count those physical fields while walking the already-computed Draft
    // offsets; source member indices alone would select the wrong later field.
    std::size_t physical_index = 0;
    std::uint64_t cursor = 0;
    for (std::size_t index = 0; index < aggregate.member_offsets.size();
         ++index) {
      const std::uint64_t member_offset = aggregate.member_offsets[index];
      if (member_offset > cursor)
        ++physical_index;
      if (member_offset == offset)
        return physical_index;
      ++physical_index;
      if (index < aggregate.members.size()) {
        cursor = member_offset + type(aggregate.members[index]).layout.size;
      }
    }
    return 0;
  }

  [[nodiscard]] std::optional<TypeId>
  aggregate_scratch_type(const MirProcedure &procedure,
                         const MirInstruction &instruction) const {
    TypeId candidate;
    if (instruction.kind == MirInstructionKind::Aggregate) {
      candidate = instruction.type;
    } else if (instruction.kind == MirInstructionKind::ExtractMember &&
               !instruction.operands.empty()) {
      candidate = procedure.value(instruction.operands.front()).type;
    }
    if (!candidate.is_valid())
      return std::nullopt;
    const TypeKind kind = runtime_scalar_kind(candidate);
    if (kind != TypeKind::Variant && kind != TypeKind::Union &&
        !struct_has_bit_fields(candidate)) {
      return std::nullopt;
    }
    return candidate;
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

  [[nodiscard]] LLVMValueRef
  intrinsic_declaration(std::string_view name,
                        std::span<LLVMTypeRef> overloaded_types = {}) {
    const unsigned id = LLVMLookupIntrinsicID(name.data(), name.size());
    if (id == 0) {
      error(SourceRange::invalid(),
            "linked LLVM has no intrinsic '" + std::string(name) + "'");
      return nullptr;
    }
    return LLVMGetIntrinsicDeclaration(
        module_.value, id,
        overloaded_types.empty() ? nullptr : overloaded_types.data(),
        overloaded_types.size());
  }

  // Creates or returns one global declaration. Definitions are selected by the
  // artifact reachability product and initialized later by emit_globals;
  // foreign/imported references use the same SymbolId slot but remain external.
  [[nodiscard]] LLVMValueRef global_for_symbol(SymbolId symbol_id,
                                               bool definition) {
    if (!symbol_id.is_valid() || symbol_id.value >= global_values_.size()) {
      error(SourceRange::invalid(), "invalid global symbol reached LLVM");
      return nullptr;
    }
    if (global_values_[symbol_id.value] != nullptr) {
      return global_values_[symbol_id.value];
    }
    const Symbol &symbol = semantic_.symbols.symbol(symbol_id);
    if (symbol.kind != SymbolKind::Variable || !symbol.type.is_valid()) {
      error(symbol.name_range, "non-variable symbol used as a global");
      return nullptr;
    }
    const std::string name = symbol_name(symbol_id);
    LLVMValueRef global = LLVMGetNamedGlobal(module_.value, name.c_str());
    if (global == nullptr) {
      global =
          LLVMAddGlobal(module_.value, llvm_type(symbol.type), name.c_str());
    }
    LLVMSetThreadLocal(global, symbol.flags.is_thread_local ? 1 : 0);
    // Exact foreign linker names belong to another provider and retain default
    // visibility. Package-qualified definitions and imported declarations are
    // hidden so only explicit C exports cross the final artifact boundary.
    if (!native_symbol_name(symbol_id).has_value()) {
      LLVMSetVisibility(global, LLVMHiddenVisibility);
    }
    if (!definition)
      LLVMSetLinkage(global, LLVMExternalLinkage);
    global_values_[symbol_id.value] = global;
    return global;
  }

  [[nodiscard]] LLVMValueRef procedure_constant(const ConstantValue &value,
                                                TypeId type_id,
                                                SourceRange range) {
    if (value.symbol_index != std::numeric_limits<std::uint32_t>::max() &&
        value.symbol_index < semantic_.symbols.symbol_count()) {
      const SymbolId symbol{value.symbol_index};
      if (semantic_.symbols.symbol(symbol).kind == SymbolKind::Procedure) {
        return function_for_symbol(symbol);
      }
    }
    for (const ImportedSymbol &imported :
         semantic_.imported_symbols_for_read()) {
      if (imported.root_identity == value.root_identity &&
          imported.root_relative_path == value.root_relative_path &&
          imported.public_name == value.text &&
          semantic_.symbols.symbol(imported.proxy).kind ==
              SymbolKind::Procedure) {
        return function_for_symbol(imported.proxy);
      }
    }
    if (value.root_identity == options_.module.package.root_identity &&
        value.root_relative_path ==
            options_.module.package.root_relative_path) {
      const std::optional<SymbolId> local =
          semantic_.symbols.lookup_direct(semantic_.package_scope, value.text);
      if (local.has_value() &&
          semantic_.symbols.symbol(*local).kind == SymbolKind::Procedure) {
        return function_for_symbol(*local);
      }
    }
    if (value.root_identity.empty() || value.text.empty()) {
      error(range, "procedure constant has no resolvable identity");
      return LLVMConstNull(llvm_type(type_id));
    }

    const std::string name = package_symbol_name(
        {value.root_identity, value.root_relative_path}, value.text);
    LLVMValueRef function = LLVMGetNamedFunction(module_.value, name.c_str());
    if (function == nullptr) {
      function =
          LLVMAddFunction(module_.value, name.c_str(), function_type(type_id));
      LLVMSetVisibility(function, LLVMHiddenVisibility);
    }
    return function;
  }

  // Materializes one immutable byte sequence. A fresh row per source constant
  // preserves deterministic occurrence identity and avoids a string hash table;
  // LLVM may merge equal private constants later when optimization permits it.
  [[nodiscard]] LLVMValueRef string_constant(std::string_view text) {
    LLVMValueRef bytes =
        LLVMConstStringInContext2(context_.value, text.data(), text.size(), 1);
    LLVMValueRef global = LLVMAddGlobal(
        module_.value, LLVMTypeOf(bytes),
        (".draft.string." + std::to_string(next_string_constant_++)).c_str());
    LLVMSetInitializer(global, bytes);
    LLVMSetLinkage(global, LLVMPrivateLinkage);
    LLVMSetGlobalConstant(global, 1);
    LLVMSetUnnamedAddress(global, LLVMGlobalUnnamedAddr);
    LLVMSetAlignment(global, 1);

    LLVMValueRef members[]{
        global,
        LLVMConstInt(LLVMInt64TypeInContext(context_.value), text.size(), 0)};
    return LLVMConstStructInContext(context_.value, members, 2, 0);
  }

  [[nodiscard]] LLVMValueRef aggregate_constant(const ConstantValue &value,
                                                TypeId type_id,
                                                SourceRange range) {
    const TypeId storage_id = runtime_scalar_id(type_id);
    const Type &aggregate = type(storage_id);
    if (aggregate.kind == TypeKind::Variant ||
        aggregate.kind == TypeKind::Union ||
        struct_has_bit_fields(storage_id)) {
      error(range,
            "selected-member and bit-field aggregate constants are not yet "
            "implemented directly");
      return LLVMConstNull(llvm_type(type_id));
    }

    const bool homogeneous =
        aggregate.kind == TypeKind::Array || aggregate.kind == TypeKind::Simd;
    const bool product =
        aggregate.kind == TypeKind::Tuple || aggregate.kind == TypeKind::Struct;
    const std::size_t expected =
        homogeneous ? static_cast<std::size_t>(aggregate.element_count)
                    : aggregate.members.size();
    if ((!homogeneous && !product) || value.elements.size() != expected) {
      error(range, "aggregate constant does not match its runtime type");
      return LLVMConstNull(llvm_type(type_id));
    }

    std::vector<LLVMValueRef> elements;
    elements.reserve(value.elements.size() + aggregate.members.size());
    std::uint64_t cursor = 0;
    for (std::size_t index = 0; index < value.elements.size(); ++index) {
      const TypeId element_type =
          homogeneous ? aggregate.element : aggregate.members[index];
      if (aggregate.kind == TypeKind::Struct) {
        const std::uint64_t offset = index < aggregate.member_offsets.size()
                                         ? aggregate.member_offsets[index]
                                         : cursor;
        if (offset > cursor) {
          elements.push_back(LLVMConstNull(LLVMArrayType2(
              LLVMInt8TypeInContext(context_.value), offset - cursor)));
        }
        cursor = offset + type(element_type).layout.size;
      }
      elements.push_back(
          constant_operand(value.elements[index], element_type, range));
    }
    if (aggregate.kind == TypeKind::Struct && aggregate.layout.size > cursor) {
      elements.push_back(
          LLVMConstNull(LLVMArrayType2(LLVMInt8TypeInContext(context_.value),
                                       aggregate.layout.size - cursor)));
    }

    if (aggregate.kind == TypeKind::Array) {
      return LLVMConstArray2(llvm_type(aggregate.element), elements.data(),
                             elements.size());
    }
    if (aggregate.kind == TypeKind::Simd) {
      return LLVMConstVector(elements.data(),
                             static_cast<unsigned>(elements.size()));
    }
    if (aggregate.kind == TypeKind::Tuple) {
      return LLVMConstStructInContext(
          context_.value, elements.empty() ? nullptr : elements.data(),
          static_cast<unsigned>(elements.size()), 0);
    }
    return LLVMConstNamedStruct(llvm_type(storage_id),
                                elements.empty() ? nullptr : elements.data(),
                                static_cast<unsigned>(elements.size()));
  }

  [[nodiscard]] LLVMValueRef constant_operand(const ConstantValue &value,
                                              TypeId type_id,
                                              SourceRange range) {
    LLVMTypeRef value_type = llvm_type(type_id);
    switch (value.kind) {
    case ConstantKind::Unavailable:
    case ConstantKind::Nil:
      return LLVMConstNull(value_type);
    case ConstantKind::Bool:
      return LLVMConstInt(value_type, value.boolean ? 1 : 0, 0);
    case ConstantKind::Integer:
      return integer_constant(value_type, value.integer);
    case ConstantKind::Float: {
      Type storage = type(runtime_scalar_id(type_id));
      Type float_type = storage;
      if (storage.kind == TypeKind::EndianScalar &&
          storage.element.is_valid()) {
        float_type = type(storage.element);
      }
      const std::optional<IeeeBinaryFormat> format =
          float_type.kind == TypeKind::Float
              ? ieee_format_for_width(float_type.bit_width)
              : std::nullopt;
      const std::optional<std::uint64_t> bits =
          format.has_value()
              ? (value.float_bit_width == float_type.bit_width
                     ? std::optional<std::uint64_t>(value.float_bits)
                     : round_ieee_bits(value.floating, *format))
              : std::nullopt;
      if (!bits.has_value()) {
        error(range, "floating constant has no supported IEEE format");
        return LLVMConstNull(value_type);
      }
      LLVMValueRef integer = LLVMConstInt(
          LLVMIntTypeInContext(context_.value, float_type.bit_width), *bits, 0);
      return storage.kind == TypeKind::EndianScalar
                 ? integer
                 : LLVMConstBitCast(integer, value_type);
    }
    case ConstantKind::String:
      return string_constant(value.text);
    case ConstantKind::Aggregate:
      return aggregate_constant(value, type_id, range);
    case ConstantKind::Procedure:
      return procedure_constant(value, type_id, range);
    case ConstantKind::EnumLabel:
      return LLVMConstNull(value_type);
    case ConstantKind::Target:
      error(range, "target pseudo-value reached runtime emission");
      return LLVMConstNull(value_type);
    case ConstantKind::Type:
      error(range, "compile-time type value reached runtime emission");
      return LLVMConstNull(value_type);
    }
    return LLVMConstNull(value_type);
  }

  void emit_globals() {
    for (SymbolId symbol_id : globals_) {
      const Symbol &symbol = semantic_.symbols.symbol(symbol_id);
      if (symbol.kind != SymbolKind::Variable || symbol.flags.foreign ||
          !symbol.type.is_valid()) {
        continue;
      }
      LLVMValueRef global = global_for_symbol(symbol_id, true);
      if (global == nullptr)
        continue;
      const ConstantValue *initializer = global_initializers_.find(symbol_id);
      LLVMSetInitializer(
          global,
          initializer == nullptr
              ? LLVMConstNull(llvm_type(symbol.type))
              : constant_operand(*initializer, symbol.type, symbol.name_range));
      LLVMSetAlignment(global, type(symbol.type).layout.alignment);
    }
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
    if (instruction.constant.kind == ConstantKind::EnumLabel) {
      return enum_constant(instruction.symbol, llvm_type(instruction.type));
    }
    return constant_operand(instruction.constant, instruction.type,
                            instruction.range);
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

  [[nodiscard]] LLVMValueRef
  emit_convert(const MirProcedure &procedure, const MirInstruction &instruction,
               const std::vector<LLVMValueRef> &values) {
    const MirValueId source_id = instruction.operands.front();
    const TypeId source_type = procedure.value(source_id).type;
    const TypeKind source_kind = runtime_scalar_kind(source_type);
    const TypeKind target_kind = runtime_scalar_kind(instruction.type);
    LLVMValueRef source = value_operand(values, source_id, instruction.range);
    LLVMTypeRef source_llvm = llvm_type(source_type);
    LLVMTypeRef target_llvm = llvm_type(instruction.type);

    // bool is an i1 computation value while b8/b16/b32/b64 retain every stored
    // bit. Converting storage to bool must test for zero rather than truncate.
    if (source_kind == TypeKind::Bool &&
        target_kind == TypeKind::BooleanStorage) {
      return LLVMBuildZExt(builder_.value, source, target_llvm, "");
    }
    if (source_kind == TypeKind::BooleanStorage &&
        target_kind == TypeKind::Bool) {
      return LLVMBuildICmp(builder_.value, LLVMIntNE, source,
                           LLVMConstNull(source_llvm), "");
    }
    if (source_llvm == target_llvm)
      return source;

    if (integer_kind(source_kind) && integer_kind(target_kind)) {
      const std::uint32_t source_width = integer_bits(source_type);
      const std::uint32_t target_width = integer_bits(instruction.type);
      if (source_width > target_width) {
        return LLVMBuildTrunc(builder_.value, source, target_llvm, "");
      }
      return signed_integer(source_type)
                 ? LLVMBuildSExt(builder_.value, source, target_llvm, "")
                 : LLVMBuildZExt(builder_.value, source, target_llvm, "");
    }
    if (integer_kind(source_kind) && target_kind == TypeKind::Float) {
      return signed_integer(source_type)
                 ? LLVMBuildSIToFP(builder_.value, source, target_llvm, "")
                 : LLVMBuildUIToFP(builder_.value, source, target_llvm, "");
    }
    if (source_kind == TypeKind::Float && integer_kind(target_kind)) {
      return signed_integer(instruction.type)
                 ? LLVMBuildFPToSI(builder_.value, source, target_llvm, "")
                 : LLVMBuildFPToUI(builder_.value, source, target_llvm, "");
    }
    if (source_kind == TypeKind::Float && target_kind == TypeKind::Float) {
      return type(source_type).bit_width > type(instruction.type).bit_width
                 ? LLVMBuildFPTrunc(builder_.value, source, target_llvm, "")
                 : LLVMBuildFPExt(builder_.value, source, target_llvm, "");
    }
    if (LLVMGetTypeKind(source_llvm) == LLVMPointerTypeKind &&
        integer_kind(target_kind)) {
      return LLVMBuildPtrToInt(builder_.value, source, target_llvm, "");
    }
    if (integer_kind(source_kind) &&
        LLVMGetTypeKind(target_llvm) == LLVMPointerTypeKind) {
      return LLVMBuildIntToPtr(builder_.value, source, target_llvm, "");
    }

    error(instruction.range, "unsupported cast in direct LLVM emission");
    return source;
  }

  [[nodiscard]] LLVMValueRef
  emit_bit_field_load(const MirInstruction &instruction,
                      const std::vector<LLVMValueRef> &values) {
    const std::uint32_t covered_bits =
        ((instruction.bit_offset + instruction.bit_width + 7U) / 8U) * 8U;
    const std::uint32_t byte_count = covered_bits / 8U;
    LLVMTypeRef covered_type =
        LLVMIntTypeInContext(context_.value, covered_bits);
    LLVMValueRef base =
        value_operand(values, instruction.operands.front(), instruction.range);
    LLVMValueRef assembled = nullptr;
    for (std::uint32_t byte = 0; byte < byte_count; ++byte) {
      LLVMValueRef address = base;
      if (byte != 0) {
        LLVMValueRef offset =
            LLVMConstInt(LLVMInt64TypeInContext(context_.value), byte, 0);
        address =
            LLVMBuildGEP2(builder_.value, LLVMInt8TypeInContext(context_.value),
                          base, &offset, 1, "");
      }
      LLVMValueRef loaded = LLVMBuildLoad2(
          builder_.value, LLVMInt8TypeInContext(context_.value), address, "");
      LLVMSetAlignment(loaded, 1);
      LLVMValueRef part =
          covered_bits == 8
              ? loaded
              : LLVMBuildZExt(builder_.value, loaded, covered_type, "");
      if (byte != 0) {
        part = LLVMBuildShl(builder_.value, part,
                            LLVMConstInt(covered_type, byte * 8U, 0), "");
      }
      assembled = assembled == nullptr
                      ? part
                      : LLVMBuildOr(builder_.value, assembled, part, "");
    }
    if (instruction.bit_offset != 0) {
      assembled = LLVMBuildLShr(
          builder_.value, assembled,
          LLVMConstInt(covered_type, instruction.bit_offset, 0), "");
    }
    LLVMTypeRef field_type =
        LLVMIntTypeInContext(context_.value, instruction.bit_width);
    if (covered_bits != instruction.bit_width) {
      assembled = LLVMBuildTrunc(builder_.value, assembled, field_type, "");
    }
    const std::uint32_t logical_bits = integer_bits(instruction.type);
    if (instruction.bit_width < logical_bits) {
      LLVMTypeRef logical_type = llvm_type(instruction.type);
      assembled =
          signed_integer(instruction.type)
              ? LLVMBuildSExt(builder_.value, assembled, logical_type, "")
              : LLVMBuildZExt(builder_.value, assembled, logical_type, "");
    }
    return assembled;
  }

  void emit_bit_field_store_at(LLVMValueRef base, MirValueId value_id,
                               std::uint32_t bit_offset,
                               std::uint32_t bit_width, SourceRange range,
                               const MirProcedure &procedure,
                               const std::vector<LLVMValueRef> &values) {
    const TypeId value_type = procedure.value(value_id).type;
    const std::uint32_t logical_bits = integer_bits(value_type);
    LLVMValueRef field = value_operand(values, value_id, range);
    LLVMTypeRef field_type = LLVMIntTypeInContext(context_.value, bit_width);
    if (logical_bits != bit_width) {
      field = LLVMBuildTrunc(builder_.value, field, field_type, "");
    }

    const std::uint32_t covered_bits =
        ((bit_offset + bit_width + 7U) / 8U) * 8U;
    LLVMTypeRef covered_type =
        LLVMIntTypeInContext(context_.value, covered_bits);
    LLVMValueRef packed =
        bit_width == covered_bits
            ? field
            : LLVMBuildZExt(builder_.value, field, covered_type, "");
    if (bit_offset != 0) {
      packed = LLVMBuildShl(builder_.value, packed,
                            LLVMConstInt(covered_type, bit_offset, 0), "");
    }

    const std::uint32_t byte_count = covered_bits / 8U;
    const std::uint32_t first_bit = bit_offset;
    const std::uint32_t final_bit = first_bit + bit_width;
    LLVMTypeRef byte_type = LLVMInt8TypeInContext(context_.value);
    for (std::uint32_t byte = 0; byte < byte_count; ++byte) {
      LLVMValueRef address = base;
      if (byte != 0) {
        LLVMValueRef offset =
            LLVMConstInt(LLVMInt64TypeInContext(context_.value), byte, 0);
        address =
            LLVMBuildGEP2(builder_.value, byte_type, base, &offset, 1, "");
      }
      LLVMValueRef replacement = packed;
      if (byte != 0) {
        replacement =
            LLVMBuildLShr(builder_.value, packed,
                          LLVMConstInt(covered_type, byte * 8U, 0), "");
      }
      if (covered_bits != 8) {
        replacement =
            LLVMBuildTrunc(builder_.value, replacement, byte_type, "");
      }

      const std::uint32_t byte_begin = byte * 8U;
      const std::uint32_t owned_begin = std::max(first_bit, byte_begin);
      const std::uint32_t owned_end = std::min(final_bit, byte_begin + 8U);
      std::uint32_t mask = 0;
      for (std::uint32_t bit = owned_begin; bit < owned_end; ++bit) {
        mask |= 1U << (bit - byte_begin);
      }
      if (mask != 0xffU) {
        LLVMValueRef old =
            LLVMBuildLoad2(builder_.value, byte_type, address, "");
        LLVMSetAlignment(old, 1);
        LLVMValueRef stable = LLVMBuildFreeze(builder_.value, old, "");
        LLVMValueRef retained =
            LLVMBuildAnd(builder_.value, stable,
                         LLVMConstInt(byte_type, 0xffU & ~mask, 0), "");
        LLVMValueRef selected = LLVMBuildAnd(
            builder_.value, replacement, LLVMConstInt(byte_type, mask, 0), "");
        replacement = LLVMBuildOr(builder_.value, retained, selected, "");
      }
      LLVMValueRef store = LLVMBuildStore(builder_.value, replacement, address);
      LLVMSetAlignment(store, 1);
    }
  }

  void emit_bit_field_store(const MirProcedure &procedure,
                            const MirInstruction &instruction,
                            const std::vector<LLVMValueRef> &values) {
    emit_bit_field_store_at(
        value_operand(values, instruction.operands.front(), instruction.range),
        instruction.operands[1], instruction.bit_offset, instruction.bit_width,
        instruction.range, procedure, values);
  }

  void emit_instruction(const MirProcedure &procedure,
                        std::size_t instruction_index,
                        const MirInstruction &instruction,
                        std::vector<LLVMValueRef> &values,
                        LLVMValueRef context_parameter,
                        const std::vector<LLVMValueRef> &locals,
                        const std::vector<LLVMValueRef> &aggregate_scratch) {
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
      result = global_for_symbol(instruction.symbol, false);
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
    case MirInstructionKind::LoadBitField:
      result = emit_bit_field_load(instruction, values);
      break;
    case MirInstructionKind::Store: {
      const MirValueId value_id = instruction.operands[1];
      const MirValue &value = procedure.value(value_id);
      const MirInstruction &definition =
          procedure.instruction(value.definition);
      const Type &storage = type(runtime_scalar_id(value.type));
      if (definition.kind == MirInstructionKind::Zero &&
          memory_aggregate_kind(storage.kind)) {
        (void)LLVMBuildMemSet(
            builder_.value,
            value_operand(values, instruction.operands[0], instruction.range),
            LLVMConstInt(LLVMInt8TypeInContext(context_.value), 0, 0),
            LLVMConstInt(LLVMInt64TypeInContext(context_.value),
                         storage.layout.size, 0),
            instruction.alignment);
        break;
      }
      LLVMValueRef store = LLVMBuildStore(
          builder_.value, value_operand(values, value_id, instruction.range),
          value_operand(values, instruction.operands[0], instruction.range));
      LLVMSetAlignment(store, instruction.alignment);
      break;
    }
    case MirInstructionKind::StoreBitField:
      emit_bit_field_store(procedure, instruction, values);
      break;
    case MirInstructionKind::AtomicLoad:
      result = LLVMBuildLoad2(
          builder_.value, llvm_type(instruction.type),
          value_operand(values, instruction.operands[0], instruction.range),
          "");
      LLVMSetOrdering(result, atomic_order(instruction.atomic_order));
      LLVMSetAlignment(result, type(instruction.type).layout.alignment);
      break;
    case MirInstructionKind::AtomicStore: {
      const MirValueId value_id = instruction.operands[1];
      LLVMValueRef store = LLVMBuildStore(
          builder_.value, value_operand(values, value_id, instruction.range),
          value_operand(values, instruction.operands[0], instruction.range));
      LLVMSetOrdering(store, atomic_order(instruction.atomic_order));
      LLVMSetAlignment(store,
                       type(procedure.value(value_id).type).layout.alignment);
      break;
    }
    case MirInstructionKind::AtomicExchange:
      result = LLVMBuildAtomicRMW(
          builder_.value, LLVMAtomicRMWBinOpXchg,
          value_operand(values, instruction.operands[0], instruction.range),
          value_operand(values, instruction.operands[1], instruction.range),
          atomic_order(instruction.atomic_order), 0);
      break;
    case MirInstructionKind::AtomicReadModifyWrite:
      result = LLVMBuildAtomicRMW(
          builder_.value, atomic_rmw_operation(instruction.operation),
          value_operand(values, instruction.operands[0], instruction.range),
          value_operand(values, instruction.operands[1], instruction.range),
          atomic_order(instruction.atomic_order), 0);
      break;
    case MirInstructionKind::AtomicCompareExchange: {
      const MirValueId expected_pointer_id = instruction.operands[1];
      const MirValueId desired_id = instruction.operands[2];
      const TypeId value_type = procedure.value(desired_id).type;
      LLVMValueRef expected = LLVMBuildLoad2(
          builder_.value, llvm_type(value_type),
          value_operand(values, expected_pointer_id, instruction.range), "");
      LLVMSetAlignment(expected, type(value_type).layout.alignment);
      LLVMValueRef pair = LLVMBuildAtomicCmpXchg(
          builder_.value,
          value_operand(values, instruction.operands[0], instruction.range),
          expected, value_operand(values, desired_id, instruction.range),
          atomic_order(instruction.atomic_order),
          atomic_order(instruction.atomic_failure_order), 0);
      LLVMValueRef observed =
          LLVMBuildExtractValue(builder_.value, pair, 0, "");
      result = LLVMBuildExtractValue(builder_.value, pair, 1, "");

      LLVMValueRef function =
          LLVMGetBasicBlockParent(LLVMGetInsertBlock(builder_.value));
      const std::size_t branch = next_auxiliary_block_++;
      const std::string failure_name =
          "atomic.compare.failure." + std::to_string(branch);
      const std::string continuation_name =
          "atomic.compare.continue." + std::to_string(branch);
      LLVMBasicBlockRef failure_block = LLVMAppendBasicBlockInContext(
          context_.value, function, failure_name.c_str());
      LLVMBasicBlockRef continuation_block = LLVMAppendBasicBlockInContext(
          context_.value, function, continuation_name.c_str());
      LLVMBuildCondBr(builder_.value, result, continuation_block,
                      failure_block);
      LLVMPositionBuilderAtEnd(builder_.value, failure_block);
      LLVMValueRef store = LLVMBuildStore(
          builder_.value, observed,
          value_operand(values, expected_pointer_id, instruction.range));
      LLVMSetAlignment(store, type(value_type).layout.alignment);
      LLVMBuildBr(builder_.value, continuation_block);
      LLVMPositionBuilderAtEnd(builder_.value, continuation_block);
      break;
    }
    case MirInstructionKind::AtomicFence:
      if (instruction.atomic_order != AtomicMemoryOrder::Relaxed) {
        (void)LLVMBuildFence(builder_.value,
                             atomic_order(instruction.atomic_order), 0, "");
      }
      break;
    case MirInstructionKind::Unary:
      result = emit_unary(procedure, instruction, values);
      break;
    case MirInstructionKind::Binary:
      result = emit_binary(procedure, instruction, values);
      break;
    case MirInstructionKind::Convert:
      result = emit_convert(procedure, instruction, values);
      break;
    case MirInstructionKind::PointerOffset: {
      LLVMValueRef pointer =
          value_operand(values, instruction.operands[0], instruction.range);
      LLVMValueRef count =
          value_operand(values, instruction.operands[1], instruction.range);
      if (instruction.offset != 1) {
        count = LLVMBuildMul(
            builder_.value, count,
            LLVMConstInt(LLVMTypeOf(count), instruction.offset, 0), "");
      }
      result =
          LLVMBuildGEP2(builder_.value, LLVMInt8TypeInContext(context_.value),
                        pointer, &count, 1, "");
      break;
    }
    case MirInstructionKind::PointerSubtract: {
      LLVMTypeRef result_type = llvm_type(instruction.type);
      LLVMValueRef left = LLVMBuildPtrToInt(
          builder_.value,
          value_operand(values, instruction.operands[0], instruction.range),
          result_type, "");
      LLVMValueRef right = LLVMBuildPtrToInt(
          builder_.value,
          value_operand(values, instruction.operands[1], instruction.range),
          result_type, "");
      result = LLVMBuildNSWSub(builder_.value, left, right, "");
      if (instruction.offset != 1) {
        result = LLVMBuildExactSDiv(
            builder_.value, result,
            LLVMConstInt(result_type, instruction.offset, 0), "");
      }
      break;
    }
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
    case MirInstructionKind::MemberAddress: {
      LLVMValueRef offset = LLVMConstInt(LLVMInt64TypeInContext(context_.value),
                                         instruction.offset, 0);
      LLVMValueRef base =
          value_operand(values, instruction.operands[0], instruction.range);
      result =
          LLVMBuildGEP2(builder_.value, LLVMInt8TypeInContext(context_.value),
                        base, &offset, 1, "");
      break;
    }
    case MirInstructionKind::ExtractMember: {
      const MirValueId aggregate_id = instruction.operands[0];
      const TypeId aggregate_type = procedure.value(aggregate_id).type;
      const TypeKind aggregate_kind = runtime_scalar_kind(aggregate_type);
      if (aggregate_kind == TypeKind::Variant ||
          aggregate_kind == TypeKind::Union ||
          struct_has_bit_fields(aggregate_type)) {
        // LLVM represents these values as exact opaque bytes because a union,
        // tagged payload, or sub-byte field has no structural SSA equivalent.
        // Materialize the value in the instruction's entry-block scratch slot,
        // then perform the semantically checked typed load at its byte offset.
        LLVMValueRef storage = aggregate_scratch[instruction_index];
        if (storage == nullptr) {
          error(instruction.range,
                "opaque aggregate extraction has no scratch storage");
          break;
        }
        LLVMValueRef store = LLVMBuildStore(
            builder_.value,
            value_operand(values, aggregate_id, instruction.range), storage);
        LLVMSetAlignment(store, type(aggregate_type).layout.alignment);
        LLVMValueRef address = storage;
        if (instruction.offset != 0) {
          LLVMValueRef offset = LLVMConstInt(
              LLVMInt64TypeInContext(context_.value), instruction.offset, 0);
          address = LLVMBuildGEP2(builder_.value,
                                  LLVMInt8TypeInContext(context_.value),
                                  storage, &offset, 1, "");
        }
        result = LLVMBuildLoad2(builder_.value, llvm_type(instruction.type),
                                address, "");
        LLVMSetAlignment(result, struct_has_bit_fields(aggregate_type)
                                     ? 1U
                                     : type(instruction.type).layout.alignment);
        break;
      }
      result = LLVMBuildExtractValue(
          builder_.value,
          value_operand(values, aggregate_id, instruction.range),
          static_cast<unsigned>(
              aggregate_index(aggregate_type, instruction.offset)),
          "");
      break;
    }
    case MirInstructionKind::IndexAddress: {
      const MirValueId base_id = instruction.operands[0];
      LLVMValueRef base = value_operand(values, base_id, instruction.range);
      const TypeKind base_kind =
          runtime_scalar_kind(procedure.value(base_id).type);
      if (base_kind == TypeKind::Slice || base_kind == TypeKind::String) {
        base = LLVMBuildExtractValue(builder_.value, base, 0, "");
      }
      LLVMValueRef index =
          value_operand(values, instruction.operands[1], instruction.range);
      if (instruction.offset != 1) {
        index = LLVMBuildMul(
            builder_.value, index,
            LLVMConstInt(LLVMTypeOf(index), instruction.offset, 0), "");
      }
      result =
          LLVMBuildGEP2(builder_.value, LLVMInt8TypeInContext(context_.value),
                        base, &index, 1, "");
      break;
    }
    case MirInstructionKind::Slice: {
      const MirValueId base_id = instruction.operands[0];
      LLVMValueRef data = value_operand(values, base_id, instruction.range);
      const TypeKind base_kind =
          runtime_scalar_kind(procedure.value(base_id).type);
      if (base_kind == TypeKind::Slice || base_kind == TypeKind::String) {
        data = LLVMBuildExtractValue(builder_.value, data, 0, "");
      }
      const MirValueId low_id = instruction.operands[1];
      const MirValueId high_id = instruction.operands[2];
      LLVMValueRef low = value_operand(values, low_id, instruction.range);
      LLVMValueRef high = value_operand(values, high_id, instruction.range);
      const Type &slice_type = type(runtime_scalar_id(instruction.type));
      const std::uint64_t stride = slice_type.kind == TypeKind::String
                                       ? 1
                                       : type(slice_type.element).layout.size;
      if (stride != 0) {
        LLVMValueRef byte_offset = LLVMBuildMul(
            builder_.value, low, LLVMConstInt(LLVMTypeOf(low), stride, 0), "");
        data =
            LLVMBuildGEP2(builder_.value, LLVMInt8TypeInContext(context_.value),
                          data, &byte_offset, 1, "");
      }
      LLVMValueRef count = LLVMBuildSub(builder_.value, high, low, "");
      LLVMValueRef aggregate = LLVMGetUndef(llvm_type(instruction.type));
      aggregate = LLVMBuildInsertValue(builder_.value, aggregate, data, 0, "");
      result = LLVMBuildInsertValue(builder_.value, aggregate, count, 1, "");
      break;
    }
    case MirInstructionKind::Aggregate: {
      const TypeKind aggregate_kind = runtime_scalar_kind(instruction.type);
      if (aggregate_kind == TypeKind::Variant ||
          aggregate_kind == TypeKind::Union ||
          struct_has_bit_fields(instruction.type)) {
        // Build opaque aggregates in bounded entry-block storage. The initial
        // zero establishes deterministic padding, the variant zero value, and
        // neighboring bits before selected typed or bit-field writes occur.
        LLVMValueRef storage = aggregate_scratch[instruction_index];
        if (storage == nullptr) {
          error(instruction.range,
                "opaque aggregate construction has no scratch storage");
          break;
        }
        const TypeId storage_type = runtime_scalar_id(instruction.type);
        const Type &aggregate_type = type(storage_type);
        LLVMValueRef zero_store =
            LLVMBuildStore(builder_.value,
                           LLVMConstNull(llvm_type(instruction.type)), storage);
        LLVMSetAlignment(zero_store, aggregate_type.layout.alignment);

        for (std::size_t index = 0; index < instruction.operands.size();
             ++index) {
          const MirValueId operand = instruction.operands[index];
          const TypeId operand_type = procedure.value(operand).type;
          const std::uint64_t offset = index < instruction.offsets.size()
                                           ? instruction.offsets[index]
                                           : 0;
          LLVMValueRef address = storage;
          if (offset != 0) {
            LLVMValueRef byte_offset =
                LLVMConstInt(LLVMInt64TypeInContext(context_.value), offset, 0);
            address = LLVMBuildGEP2(builder_.value,
                                    LLVMInt8TypeInContext(context_.value),
                                    storage, &byte_offset, 1, "");
          }
          const std::uint32_t bit_width =
              index < instruction.aggregate_bit_widths.size()
                  ? instruction.aggregate_bit_widths[index]
                  : 0;
          if (bit_width != 0) {
            const std::uint64_t absolute_bit =
                index < instruction.aggregate_bit_offsets.size()
                    ? instruction.aggregate_bit_offsets[index]
                    : offset * 8U;
            emit_bit_field_store_at(
                address, operand, static_cast<std::uint32_t>(absolute_bit % 8U),
                bit_width, instruction.range, procedure, values);
            continue;
          }
          LLVMValueRef store = LLVMBuildStore(
              builder_.value, value_operand(values, operand, instruction.range),
              address);
          LLVMSetAlignment(store, struct_has_bit_fields(storage_type)
                                      ? 1U
                                      : type(operand_type).layout.alignment);
        }
        result = LLVMBuildLoad2(builder_.value, llvm_type(instruction.type),
                                storage, "");
        LLVMSetAlignment(result, aggregate_type.layout.alignment);
        break;
      }
      LLVMValueRef aggregate = LLVMConstNull(llvm_type(instruction.type));
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        const std::size_t member =
            aggregate_index(instruction.type, index < instruction.offsets.size()
                                                  ? instruction.offsets[index]
                                                  : 0);
        aggregate = LLVMBuildInsertValue(
            builder_.value, aggregate,
            value_operand(values, instruction.operands[index],
                          instruction.range),
            static_cast<unsigned>(member), "");
      }
      result = aggregate;
      break;
    }
    case MirInstructionKind::Assembly: {
      std::vector<LLVMTypeRef> parameter_types;
      std::vector<LLVMValueRef> arguments;
      parameter_types.reserve(instruction.operands.size());
      arguments.reserve(instruction.operands.size());
      for (MirValueId operand : instruction.operands) {
        parameter_types.push_back(llvm_type(procedure.value(operand).type));
        arguments.push_back(value_operand(values, operand, instruction.range));
      }
      LLVMTypeRef assembly_type = LLVMFunctionType(
          llvm_type(instruction.type),
          parameter_types.empty() ? nullptr : parameter_types.data(),
          static_cast<unsigned>(parameter_types.size()), 0);
      LLVMValueRef assembly =
          LLVMGetInlineAsm(assembly_type, instruction.assembly_text.data(),
                           instruction.assembly_text.size(),
                           instruction.assembly_constraints.data(),
                           instruction.assembly_constraints.size(), 1, 0,
                           LLVMInlineAsmDialectATT, 0);
      result = LLVMBuildCall2(builder_.value, assembly_type, assembly,
                              arguments.empty() ? nullptr : arguments.data(),
                              static_cast<unsigned>(arguments.size()), "");
      if (instruction.type == semantic_.types.builtins().void_type) {
        result = nullptr;
      }
      break;
    }
    case MirInstructionKind::Trap: {
      LLVMValueRef trap = intrinsic_declaration("llvm.trap");
      if (trap != nullptr) {
        (void)LLVMBuildCall2(builder_.value, LLVMGlobalGetValueType(trap), trap,
                             nullptr, 0, "");
      }
      break;
    }
    case MirInstructionKind::Invalid:
    case MirInstructionKind::Assert:
    case MirInstructionKind::BoundsCheck:
    case MirInstructionKind::SliceBoundsCheck:
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

    // Variant, union, and bit-field aggregate operations use one reusable
    // stack slot per static MIR instruction. Reserving every slot in the entry
    // block prevents a source operation inside a loop from growing the stack
    // on each iteration while keeping each operation's storage independent.
    std::vector<LLVMValueRef> aggregate_scratch(procedure.instructions.size(),
                                                nullptr);
    for (std::size_t instruction_index = 0;
         instruction_index < procedure.instructions.size();
         ++instruction_index) {
      const std::optional<TypeId> scratch_type = aggregate_scratch_type(
          procedure, procedure.instructions[instruction_index]);
      if (!scratch_type.has_value())
        continue;
      const std::string name =
          "aggregate.scratch." + std::to_string(instruction_index);
      LLVMValueRef allocation = LLVMBuildAlloca(
          builder_.value, llvm_type(*scratch_type), name.c_str());
      LLVMSetAlignment(allocation, type(*scratch_type).layout.alignment);
      aggregate_scratch[instruction_index] = allocation;
    }

    LLVMValueRef context_parameter = LLVMGetParam(function, 0);
    std::vector<LLVMValueRef> values(procedure.values.size(), nullptr);
    for (std::size_t block_index = 0; block_index < procedure.blocks.size();
         ++block_index) {
      LLVMPositionBuilderAtEnd(builder_.value, blocks[block_index]);
      const MirBlock &block = procedure.blocks[block_index];
      for (MirInstructionId instruction_id : block.instructions) {
        emit_instruction(procedure, instruction_id.value,
                         procedure.instruction(instruction_id), values,
                         context_parameter, locals, aggregate_scratch);
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
  const ConstantTable &global_initializers_;
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
  std::size_t next_string_constant_ = 0;
  std::size_t next_auxiliary_block_ = 0;
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
