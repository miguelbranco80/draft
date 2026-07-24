// Direct LLVM package-unit construction from verified Draft MIR.
//
// The package-unit task owns one LLVM context, module, and builder. Its inputs
// are immutable semantic types, ABI classifications, global constants, and an
// ordered subset of artifact-live MIR selected by native reachability. O2 and
// retained-IR operations pass the complete package set; native-only O0 may pass
// a deterministic chunk so independent target machines can execute in
// parallel. The output is optional inspection text plus optional native bytes;
// no LLVM allocation or pointer survives the synchronous call.
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
#include <llvm-c/DebugInfo.h>

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

// DIBuilder owns temporary metadata nodes until Finalize resolves their graph.
// The direct package path must finalize before verification, printing, or
// machine-code emission; the destructor repeats that operation only on an
// early return so partially constructed debug state is still disposed in the
// LLVM-required order while the module remains alive.
struct DebugBuilderOwner {
  LLVMDIBuilderRef value = nullptr;
  bool finalized = false;

  void finalize() {
    if (value == nullptr || finalized)
      return;
    LLVMDIBuilderFinalize(value);
    finalized = true;
  }

  ~DebugBuilderOwner() {
    finalize();
    if (value != nullptr)
      LLVMDisposeDIBuilder(value);
  }
  DebugBuilderOwner(const DebugBuilderOwner &) = delete;
  DebugBuilderOwner &operator=(const DebugBuilderOwner &) = delete;
  DebugBuilderOwner() = default;
};

// Debug files and cross-file lexical scopes are tiny task-local lookup tables.
// Names are logical package/source names, never physical checkout paths.
// Metadata pointers live in the task's LLVM context and die with the module.
struct DirectDebugFile {
  std::string name;
  LLVMMetadataRef metadata = nullptr;
};

struct DirectDebugScope {
  LLVMMetadataRef subprogram = nullptr;
  LLVMMetadataRef file = nullptr;
  LLVMMetadataRef metadata = nullptr;
};

// One source coordinate selects the authored file/location for ordinary code
// and the surface synthesis site for generated source. The generated position
// remains available to diagnostics, while native line tables intentionally
// lead the debugger to code the programmer can edit.
struct DirectDebugCoordinate {
  std::string file_name;
  LineColumn location;
};

// One linker relocation leaf inside an otherwise byte-exact Draft aggregate.
// offset and size use the enclosing semantic layout, while value owns the LLVM
// typed constant whose address relocation must survive object emission. These
// rows are transient during construction of one initializer-specific packed
// LLVM value and never escape the package task.
struct RelocatableConstantField {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  LLVMValueRef value = nullptr;
};

// Entry-block storage used to translate logical Draft values to physical C ABI
// carriers. Rows are indexed by MIR instruction and logical argument so a call
// inside a loop reuses bounded storage rather than allocating on every trip.
struct DirectAbiScratch {
  std::vector<std::vector<LLVMValueRef>> call_arguments;
  std::vector<LLVMValueRef> call_results;
  LLVMValueRef procedure_result = nullptr;
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
        procedures_(procedures), sources_(sources), diagnostics_(diagnostics),
        llvm_types_(semantic.types.size(), nullptr),
        functions_(semantic.symbols.symbol_count(), nullptr),
        global_values_(semantic.symbols.symbol_count(), nullptr) {}

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
    if (options_.module.emit_debug_information)
      initialize_debug_metadata();

    create_nominal_type_identities();
    define_nominal_type_bodies();
    declare_procedures();
    emit_globals();
    emit_procedures();
    if (options_.module.emit_program_entry) {
      if (options_.module.validation_kind == ValidationKind::None) {
        emit_program_entry();
      } else {
        emit_validation_entry();
      }
    }
    debug_builder_.finalize();
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
      if (options_.native_options->optimization ==
          NativeOptimizationLevel::O2) {
        result.thinlto_bitcode =
            prepare_constructed_llvm_module_for_thinlto(
                target_, module_name, module_.value,
                *options_.native_options);
        if (!result.thinlto_bitcode.ok)
          return result;
      } else {
        result.native = emit_constructed_llvm_module_in_process(
            target_, module_name, module_.value, *options_.native_options);
        if (!result.native.ok)
          return result;
      }
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

  [[nodiscard]] std::string debug_directory() const {
    // Debug paths are deliberately virtual and reproducible. The package
    // identity disambiguates same-named files without leaking a checkout path
    // into native artifacts.
    return "draft/" + encoded_name(options_.module.package.root_identity) +
        "/" + encoded_name(options_.module.package.root_relative_path);
  }

  [[nodiscard]] static std::string debug_file_name(std::string path) {
    static constexpr std::string_view resolved_suffix = " [resolved]";
    if (path.size() >= resolved_suffix.size() &&
        std::string_view(path).substr(path.size() - resolved_suffix.size()) ==
            resolved_suffix) {
      path.resize(path.size() - resolved_suffix.size());
    }
    const std::size_t separator = path.find_last_of("/\\");
    if (separator != std::string::npos)
      path.erase(0, separator + 1);
    return path.empty() ? std::string("source.draft") : std::move(path);
  }

  [[nodiscard]] DirectDebugCoordinate debug_coordinate(
      SourceRange range) const {
    DirectDebugCoordinate result;
    if (!range.is_valid()) {
      result.file_name = "compiler.draft";
      return result;
    }
    result.file_name =
        debug_file_name(sources_.file(range.begin.file).display_path);
    result.location = sources_.line_column(range.begin);
    if (const SourceExpansionMap *expansion =
            sources_.expansion_map(range.begin)) {
      result.file_name = debug_file_name(expansion->surface_display_path);
      result.location = expansion->surface_begin;
    }
    return result;
  }

  [[nodiscard]] LLVMMetadataRef debug_file(std::string name) {
    for (const DirectDebugFile &file : debug_files_) {
      if (file.name == name)
        return file.metadata;
    }
    const std::string directory = debug_directory();
    LLVMMetadataRef metadata = LLVMDIBuilderCreateFile(
        debug_builder_.value, name.data(), name.size(), directory.data(),
        directory.size());
    debug_files_.push_back({std::move(name), metadata});
    return metadata;
  }

  void add_debug_module_flag(std::string_view name, std::uint32_t value) {
    LLVMValueRef integer = LLVMConstInt(
        LLVMInt32TypeInContext(context_.value), value, 0);
    LLVMAddModuleFlag(
        module_.value, LLVMModuleFlagBehaviorWarning, name.data(), name.size(),
        LLVMValueAsMetadata(integer));
  }

  // The debug graph is intentionally line-table oriented: it gives each Draft
  // procedure a source subprogram and each emitted instruction the authored
  // location, while semantic type inspection remains the compiler/tooling
  // index's job. Keeping debug metadata at this boundary also lets native-only
  // builds omit DIBuilder allocation entirely when the user did not request
  // debugger artifacts.
  void initialize_debug_metadata() {
    debug_builder_.value = LLVMCreateDIBuilder(module_.value);
    if (debug_builder_.value == nullptr) {
      error(SourceRange::invalid(), "could not allocate an LLVM DIBuilder");
      return;
    }
    LLVMMetadataRef compile_file = debug_file("package.draft");
    constexpr std::string_view producer = "Draft bootstrap compiler";
    const bool optimized =
        options_.native_options.has_value() &&
        options_.native_options->optimization == NativeOptimizationLevel::O2;
    debug_compile_unit_ = LLVMDIBuilderCreateCompileUnit(
        debug_builder_.value, LLVMDWARFSourceLanguageC11, compile_file,
        producer.data(), producer.size(), optimized, nullptr, 0, 0, nullptr, 0,
        LLVMDWARFEmissionFull, 0, 0, 0, nullptr, 0, nullptr, 0);
    LLVMMetadataRef unspecified_types[]{nullptr};
    debug_subroutine_type_ = LLVMDIBuilderCreateSubroutineType(
        debug_builder_.value, compile_file, unspecified_types, 1,
        LLVMDIFlagZero);

    if (target_.facts.object_format == "coff") {
      add_debug_module_flag("CodeView", 1);
    } else {
      add_debug_module_flag("Dwarf Version", 4);
    }
    add_debug_module_flag("Debug Info Version", LLVMDebugMetadataVersion());
  }

  [[nodiscard]] LLVMMetadataRef debug_subprogram(
      const MirProcedure &procedure,
      LLVMValueRef function) {
    if (debug_builder_.value == nullptr)
      return nullptr;
    const DirectDebugCoordinate coordinate =
        debug_coordinate(procedure.range);
    LLVMMetadataRef file = debug_file(coordinate.file_name);
    const Symbol &symbol = semantic_.symbols.symbol(procedure.symbol);
    const std::string linkage_name = symbol_name(procedure.symbol);
    LLVMMetadataRef subprogram = LLVMDIBuilderCreateFunction(
        debug_builder_.value, file, symbol.name.data(), symbol.name.size(),
        linkage_name.data(), linkage_name.size(), file, coordinate.location.line,
        debug_subroutine_type_, 0, 1, coordinate.location.line,
        LLVMDIFlagZero,
        options_.native_options.has_value() &&
            options_.native_options->optimization ==
                NativeOptimizationLevel::O2);
    LLVMSetSubprogram(function, subprogram);
    return subprogram;
  }

  [[nodiscard]] LLVMMetadataRef debug_scope(
      LLVMMetadataRef subprogram,
      LLVMMetadataRef file) {
    for (const DirectDebugScope &scope : debug_scopes_) {
      if (scope.subprogram == subprogram && scope.file == file)
        return scope.metadata;
    }
    LLVMMetadataRef metadata = LLVMDIBuilderCreateLexicalBlockFile(
        debug_builder_.value, subprogram, file, 0);
    debug_scopes_.push_back({subprogram, file, metadata});
    return metadata;
  }

  void set_debug_location(SourceRange range) {
    if (debug_builder_.value == nullptr || current_debug_subprogram_ == nullptr ||
        !range.is_valid()) {
      LLVMSetCurrentDebugLocation2(builder_.value, nullptr);
      return;
    }
    const DirectDebugCoordinate coordinate = debug_coordinate(range);
    LLVMMetadataRef file = debug_file(coordinate.file_name);
    LLVMMetadataRef scope = debug_scope(current_debug_subprogram_, file);
    LLVMMetadataRef location = LLVMDIBuilderCreateDebugLocation(
        context_.value, coordinate.location.line, coordinate.location.column,
        scope, nullptr);
    LLVMSetCurrentDebugLocation2(builder_.value, location);
  }

  void clear_debug_location() {
    LLVMSetCurrentDebugLocation2(builder_.value, nullptr);
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

  [[nodiscard]] bool endian_requires_swap(TypeId id) const {
    const Type &storage = type(runtime_scalar_id(id));
    if (storage.kind != TypeKind::EndianScalar)
      return false;
    const bool target_is_little = target_.facts.byte_order == "little";
    return (storage.scalar_byte_order == ScalarByteOrder::Little &&
            !target_is_little) ||
        (storage.scalar_byte_order == ScalarByteOrder::Big &&
         target_is_little);
  }

  // LLVM byte-swap intrinsics are overloaded by integer width. Declaring the
  // exact canonical intrinsic name lets the target optimizer fold constants
  // and select one native instruction without routing through textual IR.
  [[nodiscard]] LLVMValueRef byte_swap(
      LLVMValueRef value,
      std::uint32_t bit_width) {
    LLVMTypeRef integer =
        LLVMIntTypeInContext(context_.value, bit_width);
    const std::string name = "llvm.bswap.i" + std::to_string(bit_width);
    LLVMValueRef intrinsic = LLVMGetNamedFunction(module_.value, name.c_str());
    if (intrinsic == nullptr) {
      LLVMTypeRef parameters[]{integer};
      intrinsic = LLVMAddFunction(
          module_.value, name.c_str(),
          LLVMFunctionType(integer, parameters, 1, 0));
    }
    LLVMValueRef arguments[]{value};
    return LLVMBuildCall2(
        builder_.value, LLVMGlobalGetValueType(intrinsic), intrinsic,
        arguments, 1, "");
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
      // The command-local store retains symbolic and intermediate generic
      // aggregates which have no runtime layout and may be completely absent
      // from the artifact-live MIR projection. Do not diagnose those semantic
      // rows merely because they coexist with emitted types. If reachable MIR
      // later requests one, llvm_type reports that precise illegal runtime use.
      if (!value.layout.known)
        continue;
      if (value.kind == TypeKind::Variant || value.kind == TypeKind::Union ||
          struct_has_bit_fields(id)) {
        // LLVM's C API names identified structs but cannot name an array type.
        // These Draft forms are intentionally byte storage rather than an LLVM
        // structural aggregate, so use the exact array directly. Opaque
        // pointers make a nominal alias unnecessary even for recursive source
        // types. The byte array is therefore the direct representation of the
        // target profile's already-computed storage layout.
        llvm_types_[index] = LLVMArrayType2(
            LLVMInt8TypeInContext(context_.value), value.layout.size);
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
      // llvm_type recursively memoizes every structural and scalar type in the
      // same table as the identified struct placeholders. Defining one early
      // struct can therefore populate a later primitive/array row before this
      // loop reaches it. LLVMStructSetBody accepts only an identified struct;
      // the semantic kind, not a non-null cache entry, is the discriminator.
      if (value.kind != TypeKind::Struct)
        continue;
      LLVMTypeRef nominal = llvm_types_[index];
      if (nominal == nullptr)
        continue;
      if (struct_has_bit_fields(id)) {
        continue;
      }
      if (!value.layout.known) {
        error(value.declaration, "nominal runtime type has no complete layout");
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

  [[nodiscard]] std::string symbol_name(SymbolId symbol_id) const {
    if (const std::optional<std::string> native =
            native_symbol_name(symbol_id)) {
      return *native;
    }
    for (const ImportedSymbol &imported :
         semantic_.imported_symbols_for_read()) {
      if (imported.proxy == symbol_id) {
        return llvm_package_symbol_name(
            {imported.root_identity, imported.root_relative_path},
            imported.public_name);
      }
    }
    const Symbol &symbol = semantic_.symbols.symbol(symbol_id);
    return llvm_package_symbol_name(
        options_.module.package,
        symbol.linkage_name.empty() ? symbol.name : symbol.linkage_name);
  }

  [[nodiscard]] TypeId function_result(TypeId type_id) const {
    const Type &signature = type(type_id);
    return signature.members.empty() ? semantic_.types.builtins().void_type
                                     : signature.members.back();
  }

  [[nodiscard]] CAbiType c_abi_type(TypeId type_id) const {
    if (type_id == semantic_.runtime_context_type) {
      const Type &context = type(type_id);
      CAbiType abi;
      abi.classification = CAbiClass::Indirect;
      abi.size = context.layout.size;
      abi.alignment = context.layout.alignment;
      return abi;
    }
    const CAbiType *abi = abi_.find(type_id);
    if (abi == nullptr) {
      error(SourceRange::invalid(),
            "C ABI query names a type outside the classified semantic prefix");
      return {};
    }
    return *abi;
  }

  [[nodiscard]] CAbiFunctionPlan c_abi_function_plan(TypeId type_id) const {
    CAbiFunctionPlan result =
        plan_c_abi_function(semantic_.types, type_id, abi_, target_.facts);
    if (result.ok)
      return result;
    const Type &signature = type(type_id);
    if (signature.kind == TypeKind::Procedure &&
        signature.c_calling_convention && signature.members.size() == 1 &&
        signature.members.back() == semantic_.runtime_context_type) {
      result.ok = true;
      result.result = c_abi_type(signature.members.back());
    }
    return result;
  }

  [[nodiscard]] LLVMTypeRef homogeneous_llvm_type(const CAbiType &abi) {
    LLVMTypeRef element = nullptr;
    if (abi.homogeneous_element_bits == 16)
      element = LLVMHalfTypeInContext(context_.value);
    if (abi.homogeneous_element_bits == 32)
      element = LLVMFloatTypeInContext(context_.value);
    if (abi.homogeneous_element_bits == 64)
      element = LLVMDoubleTypeInContext(context_.value);
    if (element == nullptr) {
      error(SourceRange::invalid(), "C ABI HFA has an invalid lane width");
      element = LLVMInt8TypeInContext(context_.value);
    }
    return LLVMArrayType2(element, abi.homogeneous_element_count);
  }

  [[nodiscard]] LLVMTypeRef integer_container_type(std::uint32_t bits,
                                                   std::uint32_t count) {
    LLVMTypeRef integer = LLVMIntTypeInContext(context_.value, bits);
    return count == 1 ? integer : LLVMArrayType2(integer, count);
  }

  [[nodiscard]] LLVMTypeRef
  sysv_eightbyte_llvm_type(const CAbiEightbyte &component) {
    if (component.classification == CAbiEightbyteClass::Integer)
      return LLVMIntTypeInContext(context_.value, component.bits);
    if (component.classification == CAbiEightbyteClass::Sse) {
      if (component.bits <= 16)
        return LLVMHalfTypeInContext(context_.value);
      if (component.bits <= 32)
        return LLVMFloatTypeInContext(context_.value);
      return LLVMDoubleTypeInContext(context_.value);
    }
    error(SourceRange::invalid(), "C ABI has an invalid SysV eightbyte");
    return LLVMInt8TypeInContext(context_.value);
  }

  [[nodiscard]] LLVMTypeRef sysv_aggregate_llvm_type(const CAbiType &abi) {
    if (abi.eightbyte_count == 1)
      return sysv_eightbyte_llvm_type(abi.eightbytes[0]);
    if (abi.eightbyte_count == 2) {
      LLVMTypeRef members[]{sysv_eightbyte_llvm_type(abi.eightbytes[0]),
                            sysv_eightbyte_llvm_type(abi.eightbytes[1])};
      return LLVMStructTypeInContext(context_.value, members, 2, 0);
    }
    error(SourceRange::invalid(), "C ABI has no SysV aggregate components");
    return LLVMInt8TypeInContext(context_.value);
  }

  [[nodiscard]] LLVMTypeRef c_parameter_type(TypeId type_id) {
    const CAbiType abi = c_abi_type(type_id);
    switch (abi.classification) {
    case CAbiClass::Direct:
      return llvm_type(type_id);
    case CAbiClass::HomogeneousFloatAggregate:
      return homogeneous_llvm_type(abi);
    case CAbiClass::SmallAggregate:
      return integer_container_type(abi.argument_integer_bits,
                                    abi.argument_integer_count);
    case CAbiClass::EightbyteAggregate:
      return sysv_aggregate_llvm_type(abi);
    case CAbiClass::Indirect:
    case CAbiClass::Win64WideInteger:
      return LLVMPointerTypeInContext(context_.value, 0);
    case CAbiClass::Illegal:
      error(SourceRange::invalid(), "illegal C ABI parameter reached LLVM");
      return LLVMInt8TypeInContext(context_.value);
    }
    return LLVMInt8TypeInContext(context_.value);
  }

  [[nodiscard]] LLVMTypeRef c_result_type(TypeId type_id) {
    if (type_id == semantic_.types.builtins().void_type)
      return LLVMVoidTypeInContext(context_.value);
    const CAbiType abi = c_abi_type(type_id);
    switch (abi.classification) {
    case CAbiClass::Direct:
      return llvm_type(type_id);
    case CAbiClass::Win64WideInteger:
      return LLVMVectorType(LLVMInt64TypeInContext(context_.value), 2);
    case CAbiClass::HomogeneousFloatAggregate:
      return homogeneous_llvm_type(abi);
    case CAbiClass::SmallAggregate:
      return integer_container_type(abi.result_integer_bits,
                                    abi.result_integer_count);
    case CAbiClass::EightbyteAggregate:
      return sysv_aggregate_llvm_type(abi);
    case CAbiClass::Indirect:
      return LLVMVoidTypeInContext(context_.value);
    case CAbiClass::Illegal:
      error(SourceRange::invalid(), "illegal C ABI result reached LLVM");
      return LLVMVoidTypeInContext(context_.value);
    }
    return LLVMVoidTypeInContext(context_.value);
  }

  [[nodiscard]] std::vector<LLVMTypeRef>
  c_parameter_types(TypeId type_id, CAbiParameterMode mode) {
    const CAbiType abi = c_abi_type(type_id);
    if (mode == CAbiParameterMode::Indirect)
      return {LLVMPointerTypeInContext(context_.value, 0)};
    if (abi.classification == CAbiClass::EightbyteAggregate) {
      std::vector<LLVMTypeRef> result;
      result.reserve(abi.eightbyte_count);
      for (std::size_t index = 0; index < abi.eightbyte_count; ++index)
        result.push_back(sysv_eightbyte_llvm_type(abi.eightbytes[index]));
      return result;
    }
    return {c_parameter_type(type_id)};
  }

  [[nodiscard]] std::uint64_t
  abi_argument_storage_size(const CAbiType &abi) const {
    if (abi.classification == CAbiClass::SmallAggregate) {
      return static_cast<std::uint64_t>(abi.argument_integer_bits / 8U) *
             abi.argument_integer_count;
    }
    if (abi.classification == CAbiClass::EightbyteAggregate)
      return static_cast<std::uint64_t>(abi.eightbyte_count) * 8U;
    return abi.size;
  }

  [[nodiscard]] std::uint64_t
  abi_result_storage_size(const CAbiType &abi) const {
    if (abi.classification == CAbiClass::SmallAggregate) {
      return static_cast<std::uint64_t>(abi.result_integer_bits / 8U) *
             abi.result_integer_count;
    }
    if (abi.classification == CAbiClass::EightbyteAggregate)
      return static_cast<std::uint64_t>(abi.eightbyte_count) * 8U;
    return abi.size;
  }

  [[nodiscard]] std::uint32_t
  sysv_component_alignment(const CAbiType &abi, std::size_t component) const {
    return component == 0 ? abi.alignment
                          : std::min<std::uint32_t>(abi.alignment, 8U);
  }

  [[nodiscard]] std::optional<std::string_view>
  c_integer_extension(TypeId type_id) const {
    if (target_.facts.abi != "darwin_arm64" &&
        target_.facts.abi != "sysv_amd64") {
      return std::nullopt;
    }
    const Type &value = type(type_id);
    if (value.kind == TypeKind::Enum && value.element.is_valid())
      return c_integer_extension(value.element);
    if (value.bit_width >= 32)
      return std::nullopt;
    if (value.kind == TypeKind::SignedInteger)
      return "signext";
    if (value.kind == TypeKind::UnsignedInteger ||
        value.kind == TypeKind::BooleanStorage ||
        value.kind == TypeKind::EndianScalar) {
      return "zeroext";
    }
    return std::nullopt;
  }

  [[nodiscard]] LLVMTypeRef function_type(TypeId type_id) {
    const Type &signature = type(type_id);
    if (signature.kind != TypeKind::Procedure || signature.members.empty()) {
      error(signature.declaration, "procedure symbol has no function type");
      return LLVMFunctionType(LLVMVoidTypeInContext(context_.value), nullptr, 0,
                              0);
    }
    std::vector<LLVMTypeRef> parameters;
    const TypeId result_id = function_result(type_id);
    LLVMTypeRef result_type = llvm_type(result_id);
    if (signature.c_calling_convention) {
      const CAbiFunctionPlan plan = c_abi_function_plan(type_id);
      if (!plan.ok || plan.parameters.size() + 1 != signature.members.size()) {
        error(signature.declaration, "cannot plan C ABI function signature");
      }
      const CAbiType result_abi = plan.ok ? plan.result : c_abi_type(result_id);
      result_type = c_result_type(result_id);
      if (result_abi.classification == CAbiClass::Indirect) {
        parameters.push_back(LLVMPointerTypeInContext(context_.value, 0));
      }
      for (std::size_t index = 0; index + 1 < signature.members.size();
           ++index) {
        const CAbiParameterMode mode =
            plan.ok ? plan.parameters[index].mode : CAbiParameterMode::Expanded;
        std::vector<LLVMTypeRef> physical =
            c_parameter_types(signature.members[index], mode);
        parameters.insert(parameters.end(), physical.begin(), physical.end());
      }
    } else {
      parameters.reserve(signature.members.size());
      parameters.push_back(LLVMPointerTypeInContext(context_.value, 0));
      for (std::size_t index = 0; index + 1 < signature.members.size();
           ++index) {
        parameters.push_back(llvm_type(signature.members[index]));
      }
    }
    return LLVMFunctionType(
        result_type, parameters.empty() ? nullptr : parameters.data(),
        static_cast<unsigned>(parameters.size()),
        signature.c_calling_convention && signature.c_variadic ? 1 : 0);
  }

  [[nodiscard]] bool is_c_export(SymbolId symbol_id) const {
    for (const NativeBinding &binding : semantic_.native_bindings) {
      if (binding.kind == NativeBindingKind::CExport &&
          binding.symbol == symbol_id) {
        return true;
      }
    }
    return false;
  }

  void add_enum_attribute(LLVMValueRef function, LLVMAttributeIndex index,
                          std::string_view name, std::uint64_t value = 0) {
    const unsigned kind =
        LLVMGetEnumAttributeKindForName(name.data(), name.size());
    if (kind == 0) {
      error(SourceRange::invalid(),
            "linked LLVM has no attribute '" + std::string(name) + "'");
      return;
    }
    LLVMAddAttributeAtIndex(
        function, index, LLVMCreateEnumAttribute(context_.value, kind, value));
  }

  void add_type_attribute(LLVMValueRef function, LLVMAttributeIndex index,
                          std::string_view name, LLVMTypeRef type_value) {
    const unsigned kind =
        LLVMGetEnumAttributeKindForName(name.data(), name.size());
    if (kind == 0) {
      error(SourceRange::invalid(),
            "linked LLVM has no type attribute '" + std::string(name) + "'");
      return;
    }
    LLVMAddAttributeAtIndex(
        function, index,
        LLVMCreateTypeAttribute(context_.value, kind, type_value));
  }

  // Applies the target ABI facts which are not part of LLVM's function type:
  // scalar extension, sret/byval pointee types, and promised alignment. The
  // physical parameter cursor is one-based because LLVM index zero is return.
  void apply_c_function_attributes(LLVMValueRef function, TypeId type_id) {
    const Type &signature = type(type_id);
    if (!signature.c_calling_convention)
      return;
    const CAbiFunctionPlan plan = c_abi_function_plan(type_id);
    if (!plan.ok)
      return;

    const TypeId result_id = function_result(type_id);
    if (const std::optional<std::string_view> extension =
            c_integer_extension(result_id)) {
      add_enum_attribute(function, LLVMAttributeReturnIndex, *extension);
    }

    LLVMAttributeIndex physical = 1;
    if (plan.result.classification == CAbiClass::Indirect) {
      add_type_attribute(function, physical, "sret", llvm_type(result_id));
      add_enum_attribute(function, physical, "align", plan.result.alignment);
      LLVMSetValueName2(LLVMGetParam(function, physical - 1), "sret", 4);
      ++physical;
    }
    for (std::size_t logical = 0; logical < plan.parameters.size(); ++logical) {
      const TypeId logical_type = signature.members[logical];
      const CAbiParameterMode mode = plan.parameters[logical].mode;
      const std::vector<LLVMTypeRef> types =
          c_parameter_types(logical_type, mode);
      const CAbiType abi = c_abi_type(logical_type);
      for (std::size_t component = 0; component < types.size(); ++component) {
        if (mode == CAbiParameterMode::Indirect) {
          if (target_.facts.abi == "sysv_amd64") {
            add_type_attribute(function, physical, "byval",
                               llvm_type(logical_type));
          }
          add_enum_attribute(function, physical, "align", abi.alignment);
        } else if (types.size() == 1) {
          if (const std::optional<std::string_view> extension =
                  c_integer_extension(logical_type)) {
            add_enum_attribute(function, physical, *extension);
          }
        }
        const std::string name = types.size() == 1
                                     ? "arg" + std::to_string(logical)
                                     : "arg" + std::to_string(logical) + "." +
                                           std::to_string(component);
        LLVMSetValueName2(LLVMGetParam(function, physical - 1), name.data(),
                          name.size());
        ++physical;
      }
    }
  }

  void add_call_enum_attribute(LLVMValueRef call, LLVMAttributeIndex index,
                               std::string_view name, std::uint64_t value = 0) {
    const unsigned kind =
        LLVMGetEnumAttributeKindForName(name.data(), name.size());
    if (kind == 0) {
      error(SourceRange::invalid(),
            "linked LLVM has no call attribute '" + std::string(name) + "'");
      return;
    }
    LLVMAddCallSiteAttribute(
        call, index, LLVMCreateEnumAttribute(context_.value, kind, value));
  }

  void add_call_type_attribute(LLVMValueRef call, LLVMAttributeIndex index,
                               std::string_view name, LLVMTypeRef type_value) {
    const unsigned kind =
        LLVMGetEnumAttributeKindForName(name.data(), name.size());
    if (kind == 0) {
      error(SourceRange::invalid(), "linked LLVM has no call type attribute '" +
                                        std::string(name) + "'");
      return;
    }
    LLVMAddCallSiteAttribute(
        call, index, LLVMCreateTypeAttribute(context_.value, kind, type_value));
  }

  void apply_c_call_attributes(LLVMValueRef call, TypeId type_id) {
    const Type &signature = type(type_id);
    const CAbiFunctionPlan plan = c_abi_function_plan(type_id);
    if (!signature.c_calling_convention || !plan.ok)
      return;
    const TypeId result_id = function_result(type_id);
    if (const std::optional<std::string_view> extension =
            c_integer_extension(result_id)) {
      add_call_enum_attribute(call, LLVMAttributeReturnIndex, *extension);
    }
    LLVMAttributeIndex physical = 1;
    if (plan.result.classification == CAbiClass::Indirect) {
      add_call_type_attribute(call, physical, "sret", llvm_type(result_id));
      add_call_enum_attribute(call, physical, "align", plan.result.alignment);
      ++physical;
    }
    for (std::size_t logical = 0; logical < plan.parameters.size(); ++logical) {
      const TypeId logical_type = signature.members[logical];
      const CAbiParameterMode mode = plan.parameters[logical].mode;
      const std::vector<LLVMTypeRef> types =
          c_parameter_types(logical_type, mode);
      const CAbiType abi = c_abi_type(logical_type);
      for (std::size_t component = 0; component < types.size(); ++component) {
        if (mode == CAbiParameterMode::Indirect) {
          if (target_.facts.abi == "sysv_amd64") {
            add_call_type_attribute(call, physical, "byval",
                                    llvm_type(logical_type));
          }
          add_call_enum_attribute(call, physical, "align", abi.alignment);
        } else if (types.size() == 1) {
          if (const std::optional<std::string_view> extension =
                  c_integer_extension(logical_type)) {
            add_call_enum_attribute(call, physical, *extension);
          }
        }
        ++physical;
      }
    }
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
    if (is_c_export(symbol_id)) {
      if (target_.facts.object_format == "coff")
        LLVMSetDLLStorageClass(function, LLVMDLLExportStorageClass);
    } else if (!native_symbol_name(symbol_id).has_value()) {
      LLVMSetVisibility(function, LLVMHiddenVisibility);
    }
    apply_c_function_attributes(function, symbol.type);
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

  // Returns one compiler-runtime helper declaration with an exact fixed
  // signature. Root runtime emission later supplies definitions under these
  // same names; dependency package units retain hidden external references.
  [[nodiscard]] LLVMValueRef runtime_helper(std::string_view name,
                                            LLVMTypeRef result,
                                            std::span<LLVMTypeRef> parameters) {
    const std::string owned_name(name);
    LLVMValueRef function =
        LLVMGetNamedFunction(module_.value, owned_name.c_str());
    if (function != nullptr)
      return function;
    LLVMTypeRef function_type = LLVMFunctionType(
        result, parameters.empty() ? nullptr : parameters.data(),
        static_cast<unsigned>(parameters.size()), 0);
    function =
        LLVMAddFunction(module_.value, owned_name.c_str(), function_type);
    LLVMSetVisibility(function, LLVMHiddenVisibility);
    return function;
  }

  // Creates or returns one global declaration. Definitions are selected by the
  // artifact reachability product and initialized later by emit_globals;
  // foreign/imported references use the same SymbolId slot but remain external.
  void configure_global(LLVMValueRef global, SymbolId symbol_id,
                        bool definition) {
    const Symbol &symbol = semantic_.symbols.symbol(symbol_id);
    LLVMSetThreadLocal(global, symbol.flags.is_thread_local ? 1 : 0);
    // Exact foreign linker names belong to another provider and retain default
    // visibility. Package-qualified definitions and imported declarations are
    // hidden so only explicit C exports cross the final artifact boundary.
    if (!native_symbol_name(symbol_id).has_value()) {
      LLVMSetVisibility(global, LLVMHiddenVisibility);
    }
    if (!definition)
      LLVMSetLinkage(global, LLVMExternalLinkage);
  }

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
    configure_global(global, symbol_id, definition);
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

    const std::string name = llvm_package_symbol_name(
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

  // Bounds helpers accept a C-style path because their small runtime ABI
  // predates the richer assertion string-view record. Keep the terminator in
  // compiler-owned private storage and return only its opaque address.
  [[nodiscard]] LLVMValueRef cstring_constant(std::string_view text) {
    LLVMValueRef bytes =
        LLVMConstStringInContext2(context_.value, text.data(), text.size(), 0);
    LLVMValueRef global = LLVMAddGlobal(
        module_.value, LLVMTypeOf(bytes),
        (".draft.cstring." + std::to_string(next_cstring_constant_++)).c_str());
    LLVMSetInitializer(global, bytes);
    LLVMSetLinkage(global, LLVMPrivateLinkage);
    LLVMSetGlobalConstant(global, 1);
    LLVMSetUnnamedAddress(global, LLVMGlobalUnnamedAddr);
    LLVMSetAlignment(global, 1);
    return global;
  }

  [[nodiscard]] std::string source_file_path(SourceRange range) const {
    if (!range.is_valid())
      return {};
    return sources_.file(range.begin.file).display_path;
  }

  [[nodiscard]] LineColumn source_location(SourceRange range) const {
    return range.is_valid() ? sources_.line_column(range.begin) : LineColumn{};
  }

  [[nodiscard]] bool target_uses_little_endian() const {
    return target_.facts.byte_order == "little";
  }

  [[nodiscard]] bool scalar_uses_little_endian(TypeId type_id) const {
    const Type &storage = type(runtime_scalar_id(type_id));
    if (storage.kind == TypeKind::EndianScalar) {
      return storage.scalar_byte_order != ScalarByteOrder::Big;
    }
    return target_uses_little_endian();
  }

  [[nodiscard]] std::optional<IeeeBinaryFormat>
  ieee_format(TypeId type_id) const {
    Type value = type(runtime_scalar_id(type_id));
    if (value.kind == TypeKind::EndianScalar && value.element.is_valid()) {
      value = type(value.element);
    }
    return value.kind == TypeKind::Float
               ? ieee_format_for_width(value.bit_width)
               : std::nullopt;
  }

  [[nodiscard]] std::optional<BigInteger>
  enum_label_integer(const ConstantValue &value, TypeId type_id) const {
    if (value.kind != ConstantKind::EnumLabel)
      return std::nullopt;
    const TypeId enum_type = runtime_scalar_id(type_id);
    for (const AggregateMember &member :
         semantic_.aggregate_members_for_read()) {
      if (!member.owner.is_valid() ||
          semantic_.symbols.symbol(member.owner).type != enum_type ||
          semantic_.symbols.symbol(member.member).name != value.text) {
        continue;
      }
      for (const EnumMemberValue &entry :
           semantic_.enum_member_values_for_read()) {
        if (entry.member == member.member)
          return entry.value;
      }
    }
    return std::nullopt;
  }

  // Encodes one integer-like constant into the exact bytes owned by its Draft
  // storage type. Semantic conversion has already established the finite-width
  // value; this operation only makes the selected scalar byte order explicit.
  [[nodiscard]] bool write_integer_bytes(const BigInteger &value,
                                         TypeId type_id,
                                         std::vector<std::uint8_t> &bytes,
                                         std::uint64_t offset,
                                         SourceRange range) {
    const Type &storage = type(runtime_scalar_id(type_id));
    const std::uint32_t bits = integer_bits(type_id);
    const std::uint64_t size = storage.layout.size;
    if (bits == 0 || size == 0 || offset > bytes.size() ||
        size > bytes.size() - offset) {
      error(range, "integer constant does not fit aggregate byte storage");
      return false;
    }

    const BigInteger modulus = BigInteger::from_u64(1).shifted_left(bits);
    BigInteger encoded = value;
    if (encoded.is_negative())
      encoded = encoded.added(modulus);
    BigInteger quotient;
    BigInteger remainder;
    if (!encoded.divide(modulus, quotient, remainder)) {
      error(range, "could not encode integer constant bytes");
      return false;
    }
    encoded = std::move(remainder);

    const bool little = scalar_uses_little_endian(type_id);
    for (std::uint64_t byte_index = 0; byte_index < size; ++byte_index) {
      const std::optional<std::uint64_t> byte =
          encoded.shifted_right(static_cast<std::size_t>(byte_index * 8U))
              .bitwise_and(BigInteger::from_u64(0xffU))
              .to_u64();
      if (!byte.has_value()) {
        error(range, "could not encode integer constant byte");
        return false;
      }
      const std::uint64_t destination =
          little ? offset + byte_index : offset + size - byte_index - 1U;
      bytes[static_cast<std::size_t>(destination)] =
          static_cast<std::uint8_t>(*byte);
    }
    return true;
  }

  [[nodiscard]] std::optional<BigInteger>
  bit_field_constant_integer(const ConstantValue &value, TypeId type_id) const {
    if (value.kind == ConstantKind::Bool) {
      return BigInteger::from_u64(value.boolean ? 1U : 0U);
    }
    if (value.kind == ConstantKind::Integer)
      return value.integer;
    if (value.kind == ConstantKind::Nil ||
        value.kind == ConstantKind::Unavailable) {
      return BigInteger::from_u64(0);
    }
    return enum_label_integer(value, type_id);
  }

  // Draft bit zero is the low bit of the first storage byte on every target.
  // Write one bit at a time so adjacent fields and cross-byte fields share the
  // same checked layout rule as runtime read-modify-write lowering.
  [[nodiscard]] bool write_bit_field_constant(
      const ConstantValue &value, TypeId type_id,
      std::vector<std::uint8_t> &bytes, std::uint64_t storage_offset,
      std::uint64_t bit_offset, std::uint32_t bit_width, SourceRange range) {
    const std::optional<BigInteger> source =
        bit_field_constant_integer(value, type_id);
    if (!source.has_value() || bit_width == 0) {
      error(range, "bit-field constant is not an integer-like value");
      return false;
    }
    const BigInteger modulus = BigInteger::from_u64(1).shifted_left(bit_width);
    BigInteger quotient;
    BigInteger remainder;
    if (!source->divide(modulus, quotient, remainder)) {
      error(range, "could not encode bit-field constant");
      return false;
    }
    if (remainder.is_negative())
      remainder = remainder.added(modulus);
    const BigInteger encoded = std::move(remainder);

    for (std::uint32_t index = 0; index < bit_width; ++index) {
      const std::uint64_t absolute = bit_offset + index;
      const std::uint64_t byte_offset = storage_offset + absolute / 8U;
      if (byte_offset >= bytes.size()) {
        error(range, "bit-field constant exceeds aggregate byte storage");
        return false;
      }
      const std::optional<std::uint64_t> bit =
          encoded.shifted_right(index)
              .bitwise_and(BigInteger::from_u64(1))
              .to_u64();
      if (!bit.has_value()) {
        error(range, "could not encode bit-field constant bit");
        return false;
      }
      const std::uint8_t mask =
          static_cast<std::uint8_t>(1U << static_cast<unsigned>(absolute % 8U));
      std::uint8_t &destination = bytes[static_cast<std::size_t>(byte_offset)];
      destination = *bit == 0 ? static_cast<std::uint8_t>(destination & ~mask)
                              : static_cast<std::uint8_t>(destination | mask);
    }
    return true;
  }

  [[nodiscard]] bool write_float_bytes(const ConstantValue &value,
                                       TypeId type_id,
                                       std::vector<std::uint8_t> &bytes,
                                       std::uint64_t offset,
                                       SourceRange range) {
    const Type &storage = type(runtime_scalar_id(type_id));
    const std::optional<IeeeBinaryFormat> format = ieee_format(type_id);
    const std::uint32_t bit_width =
        format.has_value() ? 1U + format->exponent_bits + format->fraction_bits
                           : 0;
    const std::optional<std::uint64_t> bits =
        value.float_bit_width != 0 && value.float_bit_width == bit_width
            ? std::optional<std::uint64_t>(value.float_bits)
            : (format.has_value() ? round_ieee_bits(value.floating, *format)
                                  : std::nullopt);
    if (!bits.has_value()) {
      error(range, "floating constant has no aggregate byte encoding");
      return false;
    }
    const std::uint64_t size = storage.layout.size;
    if (offset > bytes.size() || size > bytes.size() - offset) {
      error(range, "floating constant does not fit aggregate byte storage");
      return false;
    }
    const bool little = scalar_uses_little_endian(type_id);
    for (std::uint64_t byte_index = 0; byte_index < size; ++byte_index) {
      const std::uint64_t destination =
          little ? offset + byte_index : offset + size - byte_index - 1U;
      bytes[static_cast<std::size_t>(destination)] =
          static_cast<std::uint8_t>((*bits >> (byte_index * 8U)) & 0xffU);
    }
    return true;
  }

  // Flattens a relocation-free constant according to semantic byte offsets.
  // This is used only for language forms whose canonical LLVM value is opaque
  // bytes. Strings and procedure values deliberately fail here; their linker
  // relocations require typed initializer fields and are handled separately.
  [[nodiscard]] bool write_constant_bytes(const ConstantValue &value,
                                          TypeId type_id,
                                          std::vector<std::uint8_t> &bytes,
                                          std::uint64_t offset,
                                          SourceRange range) {
    while (type(type_id).kind == TypeKind::Distinct)
      type_id = type(type_id).element;
    const Type &storage = type(type_id);
    if (!storage.layout.known || offset > bytes.size() ||
        storage.layout.size > bytes.size() - offset) {
      error(range, "constant does not fit aggregate byte storage");
      return false;
    }

    if (value.kind == ConstantKind::Nil ||
        value.kind == ConstantKind::Unavailable) {
      return true;
    }
    if (value.kind == ConstantKind::Bool && storage.kind == TypeKind::Bool) {
      bytes[static_cast<std::size_t>(offset)] = value.boolean ? 1U : 0U;
      return true;
    }
    if (value.kind == ConstantKind::Integer && integer_kind(storage.kind)) {
      return write_integer_bytes(value.integer, type_id, bytes, offset, range);
    }
    if (value.kind == ConstantKind::EnumLabel &&
        storage.kind == TypeKind::Enum) {
      const std::optional<BigInteger> integer =
          enum_label_integer(value, type_id);
      if (!integer.has_value()) {
        error(range, "enum constant has no selected member value");
        return false;
      }
      return write_integer_bytes(*integer, type_id, bytes, offset, range);
    }
    if (value.kind == ConstantKind::Float &&
        (storage.kind == TypeKind::Float ||
         storage.kind == TypeKind::EndianScalar)) {
      return write_float_bytes(value, type_id, bytes, offset, range);
    }
    if (value.kind == ConstantKind::String ||
        value.kind == ConstantKind::Procedure) {
      error(range, "relocation reached byte-only constant encoding");
      return false;
    }
    if (value.kind != ConstantKind::Aggregate) {
      error(range, "constant kind has no aggregate byte encoding");
      return false;
    }

    if (storage.kind == TypeKind::Array || storage.kind == TypeKind::Simd) {
      if (value.elements.size() != storage.element_count) {
        error(range, "constant array has the wrong element count");
        return false;
      }
      const std::uint64_t stride = type(storage.element).layout.size;
      for (std::size_t index = 0; index < value.elements.size(); ++index) {
        if (!write_constant_bytes(
                value.elements[index], storage.element, bytes,
                offset + static_cast<std::uint64_t>(index) * stride, range)) {
          return false;
        }
      }
      return true;
    }
    if (storage.kind == TypeKind::Tuple || storage.kind == TypeKind::Struct) {
      if (value.elements.size() != storage.members.size()) {
        error(range, "constant aggregate has the wrong member count");
        return false;
      }
      for (std::size_t index = 0; index < value.elements.size(); ++index) {
        const std::uint64_t member_offset =
            index < storage.member_offsets.size()
                ? storage.member_offsets[index]
                : 0;
        if (storage.kind == TypeKind::Struct &&
            index < storage.member_layouts.size() &&
            storage.member_layouts[index].kind == FieldLayoutKind::BitField) {
          const std::uint64_t member_bit_offset =
              index < storage.member_bit_offsets.size()
                  ? storage.member_bit_offsets[index]
                  : member_offset * 8U;
          if (!write_bit_field_constant(
                  value.elements[index], storage.members[index], bytes, offset,
                  member_bit_offset, storage.member_layouts[index].bit_width,
                  range)) {
            return false;
          }
          continue;
        }
        if (!write_constant_bytes(value.elements[index], storage.members[index],
                                  bytes, offset + member_offset, range)) {
          return false;
        }
      }
      return true;
    }
    if (storage.kind == TypeKind::Union || storage.kind == TypeKind::Variant) {
      if (value.member_index >= storage.members.size()) {
        error(range,
              "constant variant or union has an invalid selected member");
        return false;
      }
      if (storage.kind == TypeKind::Variant &&
          !write_integer_bytes(BigInteger::from_u64(value.member_index),
                               storage.element, bytes, offset, range)) {
        return false;
      }
      if (value.elements.empty())
        return true;
      if (value.elements.size() != 1) {
        error(range,
              "constant variant or union has more than one selected value");
        return false;
      }
      const std::uint64_t payload_offset =
          value.member_index < storage.member_offsets.size()
              ? storage.member_offsets[value.member_index]
              : 0;
      return write_constant_bytes(value.elements.front(),
                                  storage.members[value.member_index], bytes,
                                  offset + payload_offset, range);
    }

    error(range, "constant type has no aggregate byte encoding");
    return false;
  }

  [[nodiscard]] LLVMValueRef
  byte_array_constant(const std::vector<std::uint8_t> &bytes) {
    LLVMTypeRef byte_type = LLVMInt8TypeInContext(context_.value);
    std::vector<LLVMValueRef> elements;
    elements.reserve(bytes.size());
    for (std::uint8_t byte : bytes) {
      elements.push_back(LLVMConstInt(byte_type, byte, 0));
    }
    return LLVMConstArray2(byte_type, elements.data(), elements.size());
  }

  [[nodiscard]] bool contains_relocation(const ConstantValue &value) const {
    if (value.kind == ConstantKind::String ||
        value.kind == ConstantKind::Procedure) {
      return true;
    }
    for (const ConstantValue &element : value.elements) {
      if (contains_relocation(element))
        return true;
    }
    return false;
  }

  [[nodiscard]] bool
  requires_relocatable_aggregate_storage(const ConstantValue &value,
                                         TypeId type_id) const {
    while (type(type_id).kind == TypeKind::Distinct)
      type_id = type(type_id).element;
    const TypeKind kind = type(type_id).kind;
    const bool aggregate = kind == TypeKind::Array || kind == TypeKind::Tuple ||
                           kind == TypeKind::Struct ||
                           kind == TypeKind::Variant || kind == TypeKind::Union;
    return aggregate && contains_relocation(value);
  }

  // Separates an aggregate into ordinary byte segments and typed relocation
  // leaves. The semantic layout remains authoritative: LLVM receives no
  // opportunity to add padding around a string or procedure pointer field.
  [[nodiscard]] bool collect_relocatable_constant_fields(
      const ConstantValue &value, TypeId type_id,
      std::vector<std::uint8_t> &bytes, std::uint64_t offset, SourceRange range,
      std::vector<RelocatableConstantField> &fields) {
    while (type(type_id).kind == TypeKind::Distinct)
      type_id = type(type_id).element;
    const Type &storage = type(type_id);
    if (!storage.layout.known || offset > bytes.size() ||
        storage.layout.size > bytes.size() - offset) {
      error(range, "relocatable constant does not fit aggregate storage");
      return false;
    }

    if (!contains_relocation(value)) {
      return write_constant_bytes(value, type_id, bytes, offset, range);
    }
    if (value.kind == ConstantKind::String ||
        value.kind == ConstantKind::Procedure) {
      LLVMValueRef operand = constant_operand(value, type_id, range);
      fields.push_back(
          RelocatableConstantField{offset, storage.layout.size, operand});
      return true;
    }
    if (value.kind != ConstantKind::Aggregate) {
      error(range, "relocatable constant has no aggregate representation");
      return false;
    }

    if (storage.kind == TypeKind::Array || storage.kind == TypeKind::Simd) {
      if (value.elements.size() != storage.element_count ||
          !type(storage.element).layout.known) {
        error(range, "relocatable array constant has the wrong shape");
        return false;
      }
      const std::uint64_t stride = type(storage.element).layout.size;
      for (std::size_t index = 0; index < value.elements.size(); ++index) {
        if (!collect_relocatable_constant_fields(
                value.elements[index], storage.element, bytes,
                offset + static_cast<std::uint64_t>(index) * stride, range,
                fields)) {
          return false;
        }
      }
      return true;
    }
    if (storage.kind == TypeKind::Tuple || storage.kind == TypeKind::Struct) {
      if (value.elements.size() != storage.members.size()) {
        error(range, "relocatable product constant has the wrong shape");
        return false;
      }
      for (std::size_t index = 0; index < value.elements.size(); ++index) {
        if (!type(storage.members[index]).layout.known) {
          error(range, "relocatable product member has no physical layout");
          return false;
        }
        const std::uint64_t member_offset =
            index < storage.member_offsets.size()
                ? storage.member_offsets[index]
                : 0;
        if (storage.kind == TypeKind::Struct &&
            index < storage.member_layouts.size() &&
            storage.member_layouts[index].kind == FieldLayoutKind::BitField) {
          if (contains_relocation(value.elements[index])) {
            error(range, "bit-field constant cannot contain a relocation");
            return false;
          }
          const std::uint64_t member_bit_offset =
              index < storage.member_bit_offsets.size()
                  ? storage.member_bit_offsets[index]
                  : member_offset * 8U;
          if (!write_bit_field_constant(
                  value.elements[index], storage.members[index], bytes, offset,
                  member_bit_offset, storage.member_layouts[index].bit_width,
                  range)) {
            return false;
          }
          continue;
        }
        if (!collect_relocatable_constant_fields(
                value.elements[index], storage.members[index], bytes,
                offset + member_offset, range, fields)) {
          return false;
        }
      }
      return true;
    }
    if (storage.kind == TypeKind::Variant || storage.kind == TypeKind::Union) {
      if (value.member_index >= storage.members.size() ||
          value.elements.size() != 1) {
        error(range,
              "relocatable variant or union constant has an invalid selected "
              "value");
        return false;
      }
      if (storage.kind == TypeKind::Variant &&
          !write_integer_bytes(BigInteger::from_u64(value.member_index),
                               storage.element, bytes, offset, range)) {
        return false;
      }
      const std::uint64_t payload_offset =
          value.member_index < storage.member_offsets.size()
              ? storage.member_offsets[value.member_index]
              : 0;
      return collect_relocatable_constant_fields(
          value.elements.front(), storage.members[value.member_index], bytes,
          offset + payload_offset, range, fields);
    }

    error(range, "relocatable constant has an unsupported aggregate type");
    return false;
  }

  [[nodiscard]] LLVMValueRef
  relocatable_aggregate_constant(const ConstantValue &value, TypeId type_id,
                                 SourceRange range) {
    while (type(type_id).kind == TypeKind::Distinct)
      type_id = type(type_id).element;
    const Type &storage = type(type_id);
    if (!storage.layout.known) {
      error(range, "relocatable aggregate constant has no physical layout");
      return nullptr;
    }

    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(storage.layout.size), 0);
    std::vector<RelocatableConstantField> fields;
    if (!collect_relocatable_constant_fields(value, type_id, bytes, 0, range,
                                             fields)) {
      return nullptr;
    }
    std::sort(fields.begin(), fields.end(),
              [](const RelocatableConstantField &left,
                 const RelocatableConstantField &right) {
                return left.offset < right.offset;
              });

    std::vector<LLVMValueRef> components;
    std::uint64_t cursor = 0;
    auto append_bytes = [&](std::uint64_t begin, std::uint64_t end) {
      if (begin == end)
        return;
      std::vector<std::uint8_t> segment(
          bytes.begin() + static_cast<std::ptrdiff_t>(begin),
          bytes.begin() + static_cast<std::ptrdiff_t>(end));
      components.push_back(byte_array_constant(segment));
    };
    for (const RelocatableConstantField &field : fields) {
      if (field.value == nullptr || field.offset < cursor ||
          field.offset > storage.layout.size ||
          field.size > storage.layout.size - field.offset) {
        error(range, "relocatable aggregate fields overlap or exceed storage");
        return nullptr;
      }
      append_bytes(cursor, field.offset);
      components.push_back(field.value);
      cursor = field.offset + field.size;
    }
    append_bytes(cursor, storage.layout.size);
    if (fields.empty()) {
      error(range, "relocatable aggregate contains no relocation fields");
      return nullptr;
    }
    return LLVMConstStructInContext(context_.value, components.data(),
                                    static_cast<unsigned>(components.size()),
                                    1);
  }

  [[nodiscard]] LLVMValueRef aggregate_constant(const ConstantValue &value,
                                                TypeId type_id,
                                                SourceRange range) {
    const TypeId storage_id = runtime_scalar_id(type_id);
    const Type &aggregate = type(storage_id);
    if (aggregate.kind == TypeKind::Variant ||
        aggregate.kind == TypeKind::Union ||
        struct_has_bit_fields(storage_id)) {
      std::vector<std::uint8_t> bytes(
          static_cast<std::size_t>(aggregate.layout.size), 0);
      if (!write_constant_bytes(value, storage_id, bytes, 0, range)) {
        return LLVMConstNull(llvm_type(type_id));
      }
      return byte_array_constant(bytes);
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
      const ConstantValue *initializer = global_initializers_.find(symbol_id);
      const bool relocatable =
          initializer != nullptr &&
          requires_relocatable_aggregate_storage(*initializer, symbol.type);
      LLVMValueRef initializer_value =
          relocatable ? relocatable_aggregate_constant(
                            *initializer, symbol.type, symbol.name_range)
                      : (initializer == nullptr
                             ? LLVMConstNull(llvm_type(symbol.type))
                             : constant_operand(*initializer, symbol.type,
                                                symbol.name_range));
      if (initializer_value == nullptr)
        continue;

      // A relocation-bearing aggregate needs an initializer-specific packed
      // storage type. LLVM opaque pointers let every later Draft load continue
      // naming the canonical logical type despite this global allocation type.
      LLVMValueRef global = nullptr;
      if (relocatable) {
        const std::string name = symbol_name(symbol_id);
        global = LLVMAddGlobal(module_.value, LLVMTypeOf(initializer_value),
                               name.c_str());
        configure_global(global, symbol_id, true);
        global_values_[symbol_id.value] = global;
      } else {
        global = global_for_symbol(symbol_id, true);
      }
      if (global == nullptr)
        continue;
      LLVMSetInitializer(global, initializer_value);
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
    if (requires_relocatable_aggregate_storage(instruction.constant,
                                               instruction.type)) {
      LLVMValueRef initializer = relocatable_aggregate_constant(
          instruction.constant, instruction.type, instruction.range);
      if (initializer == nullptr)
        return LLVMConstNull(llvm_type(instruction.type));
      const std::string name =
          ".draft.constant." + std::to_string(next_relocatable_constant_++);
      LLVMValueRef global =
          LLVMAddGlobal(module_.value, LLVMTypeOf(initializer), name.c_str());
      LLVMSetInitializer(global, initializer);
      LLVMSetLinkage(global, LLVMPrivateLinkage);
      LLVMSetGlobalConstant(global, 1);
      LLVMSetUnnamedAddress(global, LLVMGlobalUnnamedAddr);
      LLVMSetAlignment(global, type(instruction.type).layout.alignment);
      LLVMValueRef loaded = LLVMBuildLoad2(
          builder_.value, llvm_type(instruction.type), global, "");
      LLVMSetAlignment(loaded, type(instruction.type).layout.alignment);
      return loaded;
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

    // Endian scalar values are integer-shaped storage even when their native
    // counterpart is floating point. Conversions preserve the scalar's exact
    // bit pattern and swap bytes only when the declared storage order differs
    // from the selected target. Numeric FP/integer conversions would corrupt
    // non-integral values such as 1.5 and are therefore never used here.
    const TypeId source_runtime = runtime_scalar_id(source_type);
    const TypeId target_runtime = runtime_scalar_id(instruction.type);
    if (target_kind == TypeKind::EndianScalar &&
        type(target_runtime).element == source_runtime) {
      const std::uint32_t bits = type(target_runtime).bit_width;
      LLVMValueRef stored_bits = source_kind == TypeKind::Float
          ? LLVMBuildBitCast(builder_.value, source, target_llvm, "")
          : source;
      if (endian_requires_swap(target_runtime))
        stored_bits = byte_swap(stored_bits, bits);
      return stored_bits;
    }
    if (source_kind == TypeKind::EndianScalar &&
        type(source_runtime).element == target_runtime) {
      const std::uint32_t bits = type(source_runtime).bit_width;
      LLVMValueRef native_bits = source;
      if (endian_requires_swap(source_runtime))
        native_bits = byte_swap(native_bits, bits);
      return target_kind == TypeKind::Float
          ? LLVMBuildBitCast(builder_.value, native_bits, target_llvm, "")
          : native_bits;
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

  void emit_assertion(const MirProcedure &procedure,
                      const MirInstruction &instruction,
                      const std::vector<LLVMValueRef> &values,
                      LLVMValueRef context_parameter) {
    SourceRange condition_range = instruction.range;
    if (!instruction.operands.empty()) {
      const MirValueId condition = instruction.operands.front();
      if (condition.is_valid() && condition.value < procedure.values.size()) {
        const MirInstructionId definition =
            procedure.value(condition).definition;
        if (definition.is_valid() &&
            definition.value < procedure.instructions.size()) {
          condition_range = procedure.instruction(definition).range;
        }
      }
    }
    const std::string condition_text =
        condition_range.is_valid() ? std::string(sources_.text(condition_range))
                                   : std::string();
    const std::string file_path = source_file_path(instruction.range);
    const LineColumn location = source_location(instruction.range);
    LLVMTypeRef string_type = llvm_type(semantic_.types.builtins().string_type);
    LLVMTypeRef pointer_type = LLVMPointerTypeInContext(context_.value, 0);
    LLVMTypeRef integer_type = LLVMInt64TypeInContext(context_.value);
    LLVMTypeRef parameter_types[]{
        pointer_type, LLVMInt1TypeInContext(context_.value),
        string_type,  string_type,
        string_type,  integer_type,
        integer_type};
    LLVMValueRef function =
        runtime_helper("__draft.assert", LLVMVoidTypeInContext(context_.value),
                       parameter_types);
    LLVMValueRef arguments[]{
        context_parameter,
        value_operand(values, instruction.operands[0], instruction.range),
        string_constant(condition_text),
        instruction.operands.size() == 2
            ? value_operand(values, instruction.operands[1], instruction.range)
            : LLVMConstNull(string_type),
        string_constant(file_path),
        LLVMConstInt(integer_type, location.line, 0),
        LLVMConstInt(integer_type, location.column, 0)};
    (void)LLVMBuildCall2(builder_.value, LLVMGlobalGetValueType(function),
                         function, arguments, 7, "");
  }

  void emit_bounds_check(const MirInstruction &instruction,
                         const std::vector<LLVMValueRef> &values, bool slice) {
    const std::string file_path = source_file_path(instruction.range);
    const LineColumn location = source_location(instruction.range);
    LLVMTypeRef integer_type = LLVMInt64TypeInContext(context_.value);
    LLVMTypeRef pointer_type = LLVMPointerTypeInContext(context_.value, 0);
    const std::size_t value_count = slice ? 3U : 2U;
    std::vector<LLVMTypeRef> parameter_types(value_count, integer_type);
    parameter_types.push_back(pointer_type);
    parameter_types.push_back(integer_type);
    parameter_types.push_back(integer_type);
    LLVMValueRef function =
        runtime_helper(slice ? "__draft.slice_bounds" : "__draft.bounds",
                       LLVMVoidTypeInContext(context_.value), parameter_types);
    LLVMValueRef arguments[6]{};
    for (std::size_t index = 0; index < value_count; ++index) {
      arguments[index] =
          value_operand(values, instruction.operands[index], instruction.range);
    }
    arguments[value_count] = cstring_constant(file_path);
    arguments[value_count + 1] = LLVMConstInt(integer_type, location.line, 0);
    arguments[value_count + 2] = LLVMConstInt(integer_type, location.column, 0);
    (void)LLVMBuildCall2(builder_.value, LLVMGlobalGetValueType(function),
                         function, arguments,
                         static_cast<unsigned>(value_count + 3U), "");
  }

  [[nodiscard]] LLVMValueRef abi_scratch(std::uint64_t size,
                                         std::uint32_t alignment,
                                         std::string_view name) {
    LLVMTypeRef storage =
        LLVMArrayType2(LLVMInt8TypeInContext(context_.value), size);
    const std::string owned_name(name);
    LLVMValueRef allocation =
        LLVMBuildAlloca(builder_.value, storage, owned_name.c_str());
    LLVMSetAlignment(allocation, alignment);
    return allocation;
  }

  [[nodiscard]] DirectAbiScratch
  allocate_c_abi_scratch(const MirProcedure &procedure) {
    DirectAbiScratch result;
    result.call_arguments.resize(procedure.instructions.size());
    result.call_results.resize(procedure.instructions.size(), nullptr);

    const Type &own_signature = type(procedure.type);
    if (own_signature.c_calling_convention) {
      const CAbiFunctionPlan plan = c_abi_function_plan(procedure.type);
      if (plan.ok &&
          (plan.result.classification == CAbiClass::Win64WideInteger ||
           plan.result.classification == CAbiClass::SmallAggregate ||
           plan.result.classification == CAbiClass::HomogeneousFloatAggregate ||
           plan.result.classification == CAbiClass::EightbyteAggregate)) {
        result.procedure_result =
            abi_scratch(abi_result_storage_size(plan.result),
                        plan.result.alignment, "abi.return");
      }
    }

    for (std::size_t instruction_index = 0;
         instruction_index < procedure.instructions.size();
         ++instruction_index) {
      const MirInstruction &instruction =
          procedure.instructions[instruction_index];
      if (instruction.kind != MirInstructionKind::Call ||
          instruction.operands.empty()) {
        continue;
      }
      const MirValueId callee = instruction.operands.front();
      if (!callee.is_valid() || callee.value >= procedure.values.size())
        continue;
      const TypeId signature_id = procedure.value(callee).type;
      const Type &signature = type(signature_id);
      if (signature.kind != TypeKind::Procedure ||
          !signature.c_calling_convention || signature.members.empty()) {
        continue;
      }
      const std::size_t parameter_count = signature.members.size() - 1;
      const CAbiFunctionPlan plan = c_abi_function_plan(signature_id);
      std::vector<LLVMValueRef> &arguments =
          result.call_arguments[instruction_index];
      arguments.resize(instruction.operands.size() - 1, nullptr);
      for (std::size_t argument = 0;
           argument < parameter_count && argument < arguments.size();
           ++argument) {
        const CAbiType abi = c_abi_type(signature.members[argument]);
        const CAbiParameterMode mode =
            plan.ok && argument < plan.parameters.size()
                ? plan.parameters[argument].mode
                : CAbiParameterMode::Expanded;
        if (abi.classification == CAbiClass::Direct &&
            mode == CAbiParameterMode::Expanded) {
          continue;
        }
        arguments[argument] =
            abi_scratch(abi_argument_storage_size(abi), abi.alignment,
                        "abi.call." + std::to_string(instruction_index) +
                            ".arg." + std::to_string(argument));
      }
      for (std::size_t operand = parameter_count + 1;
           operand < instruction.operands.size(); ++operand) {
        const TypeId logical_type =
            procedure.value(instruction.operands[operand]).type;
        const CAbiType abi = c_abi_type(logical_type);
        if (abi.classification != CAbiClass::Win64WideInteger)
          continue;
        const std::size_t argument = operand - 1;
        arguments[argument] =
            abi_scratch(abi_argument_storage_size(abi), abi.alignment,
                        "abi.call." + std::to_string(instruction_index) +
                            ".arg." + std::to_string(argument));
      }
      const TypeId logical_result = function_result(signature_id);
      if (logical_result == semantic_.types.builtins().void_type)
        continue;
      const CAbiType result_abi =
          plan.ok ? plan.result : c_abi_type(logical_result);
      if (result_abi.classification == CAbiClass::Win64WideInteger ||
          result_abi.classification == CAbiClass::SmallAggregate ||
          result_abi.classification == CAbiClass::HomogeneousFloatAggregate ||
          result_abi.classification == CAbiClass::EightbyteAggregate ||
          result_abi.classification == CAbiClass::Indirect) {
        result.call_results[instruction_index] = abi_scratch(
            abi_result_storage_size(result_abi), result_abi.alignment,
            "abi.call." + std::to_string(instruction_index) + ".result");
      }
    }
    return result;
  }

  [[nodiscard]] LLVMValueRef
  emit_c_call(std::size_t instruction_index, const MirProcedure &procedure,
              const MirInstruction &instruction,
              const std::vector<LLVMValueRef> &values,
              const DirectAbiScratch &scratch) {
    const MirValueId callee_id = instruction.operands.front();
    const TypeId signature_id = procedure.value(callee_id).type;
    const Type &signature = type(signature_id);
    const std::size_t parameter_count = signature.members.size() - 1;
    const CAbiFunctionPlan plan = c_abi_function_plan(signature_id);
    if (!plan.ok || plan.parameters.size() != parameter_count) {
      error(instruction.range, "cannot plan C ABI call signature");
      return LLVMGetPoison(llvm_type(instruction.type));
    }

    const TypeId logical_result = function_result(signature_id);
    const bool returns_void =
        logical_result == semantic_.types.builtins().void_type;
    const CAbiType result_abi = returns_void ? CAbiType{} : plan.result;
    std::vector<LLVMValueRef> arguments;
    arguments.reserve(instruction.operands.size() + 2);
    if (result_abi.classification == CAbiClass::Indirect) {
      arguments.push_back(scratch.call_results[instruction_index]);
    }

    const std::vector<LLVMValueRef> &argument_scratch =
        scratch.call_arguments[instruction_index];
    for (std::size_t index = 0; index < parameter_count; ++index) {
      const TypeId logical_type = signature.members[index];
      const MirValueId value_id = instruction.operands[index + 1];
      const CAbiType abi = c_abi_type(logical_type);
      const CAbiParameterMode mode = plan.parameters[index].mode;
      if (abi.classification == CAbiClass::Direct &&
          mode == CAbiParameterMode::Expanded) {
        arguments.push_back(value_operand(values, value_id, instruction.range));
        continue;
      }
      if (abi.classification == CAbiClass::Illegal ||
          index >= argument_scratch.size() ||
          argument_scratch[index] == nullptr) {
        error(instruction.range,
              "illegal or unallocated C ABI call argument reached LLVM");
        arguments.push_back(LLVMGetPoison(c_parameter_type(logical_type)));
        continue;
      }

      LLVMValueRef storage = argument_scratch[index];
      const std::uint64_t storage_size = abi_argument_storage_size(abi);
      if (abi.classification == CAbiClass::SmallAggregate ||
          abi.classification == CAbiClass::EightbyteAggregate) {
        LLVMValueRef zero = LLVMBuildStore(
            builder_.value,
            LLVMConstNull(LLVMArrayType2(LLVMInt8TypeInContext(context_.value),
                                         storage_size)),
            storage);
        LLVMSetAlignment(zero, abi.alignment);
      }
      LLVMValueRef logical_store = LLVMBuildStore(
          builder_.value, value_operand(values, value_id, instruction.range),
          storage);
      LLVMSetAlignment(logical_store, abi.alignment);
      if (mode == CAbiParameterMode::Indirect) {
        arguments.push_back(storage);
        continue;
      }
      if (abi.classification == CAbiClass::EightbyteAggregate) {
        for (std::size_t component = 0; component < abi.eightbyte_count;
             ++component) {
          LLVMValueRef address = storage;
          if (component != 0) {
            LLVMValueRef offset = LLVMConstInt(
                LLVMInt64TypeInContext(context_.value), component * 8U, 0);
            address = LLVMBuildGEP2(builder_.value,
                                    LLVMInt8TypeInContext(context_.value),
                                    storage, &offset, 1, "");
          }
          LLVMTypeRef physical_type =
              sysv_eightbyte_llvm_type(abi.eightbytes[component]);
          LLVMValueRef physical =
              LLVMBuildLoad2(builder_.value, physical_type, address, "");
          LLVMSetAlignment(physical, sysv_component_alignment(abi, component));
          arguments.push_back(physical);
        }
        continue;
      }
      LLVMTypeRef physical_type = c_parameter_type(logical_type);
      LLVMValueRef physical =
          LLVMBuildLoad2(builder_.value, physical_type, storage, "");
      LLVMSetAlignment(physical, abi.alignment);
      arguments.push_back(physical);
    }

    for (std::size_t operand = parameter_count + 1;
         operand < instruction.operands.size(); ++operand) {
      const MirValueId value_id = instruction.operands[operand];
      const TypeId logical_type = procedure.value(value_id).type;
      const CAbiType abi = c_abi_type(logical_type);
      if (abi.classification == CAbiClass::Win64WideInteger) {
        const std::size_t argument = operand - 1;
        LLVMValueRef storage = argument < argument_scratch.size()
                                   ? argument_scratch[argument]
                                   : nullptr;
        if (storage == nullptr) {
          error(instruction.range,
                "C variadic wide integer has no caller storage");
          arguments.push_back(
              LLVMGetPoison(LLVMPointerTypeInContext(context_.value, 0)));
          continue;
        }
        LLVMValueRef store = LLVMBuildStore(
            builder_.value, value_operand(values, value_id, instruction.range),
            storage);
        LLVMSetAlignment(store, abi.alignment);
        arguments.push_back(storage);
        continue;
      }
      if (abi.classification != CAbiClass::Direct) {
        error(instruction.range,
              "non-scalar C variadic argument reached LLVM emission");
        arguments.push_back(LLVMGetPoison(llvm_type(logical_type)));
        continue;
      }
      arguments.push_back(value_operand(values, value_id, instruction.range));
    }

    LLVMValueRef call =
        LLVMBuildCall2(builder_.value, function_type(signature_id),
                       value_operand(values, callee_id, instruction.range),
                       arguments.empty() ? nullptr : arguments.data(),
                       static_cast<unsigned>(arguments.size()), "");
    apply_c_call_attributes(call, signature_id);
    if (returns_void)
      return nullptr;

    if (result_abi.classification == CAbiClass::Win64WideInteger ||
        result_abi.classification == CAbiClass::SmallAggregate ||
        result_abi.classification == CAbiClass::HomogeneousFloatAggregate ||
        result_abi.classification == CAbiClass::EightbyteAggregate) {
      LLVMValueRef storage = scratch.call_results[instruction_index];
      LLVMValueRef physical_store =
          LLVMBuildStore(builder_.value, call, storage);
      LLVMSetAlignment(physical_store, result_abi.alignment);
      LLVMValueRef logical = LLVMBuildLoad2(
          builder_.value, llvm_type(logical_result), storage, "");
      LLVMSetAlignment(logical, result_abi.alignment);
      return logical;
    }
    if (result_abi.classification == CAbiClass::Indirect) {
      LLVMValueRef logical =
          LLVMBuildLoad2(builder_.value, llvm_type(logical_result),
                         scratch.call_results[instruction_index], "");
      LLVMSetAlignment(logical, result_abi.alignment);
      return logical;
    }
    if (result_abi.classification == CAbiClass::Illegal) {
      error(instruction.range, "illegal C ABI call result reached LLVM");
      return LLVMGetPoison(llvm_type(logical_result));
    }
    return call;
  }

  [[nodiscard]] std::size_t c_parameter_start(TypeId signature_id,
                                              const CAbiFunctionPlan &plan,
                                              std::size_t logical_parameter) {
    std::size_t physical =
        plan.result.classification == CAbiClass::Indirect ? 1U : 0U;
    const Type &signature = type(signature_id);
    for (std::size_t index = 0; index < logical_parameter; ++index) {
      physical += c_parameter_types(signature.members[index],
                                    plan.parameters[index].mode)
                      .size();
    }
    return physical;
  }

  void initialize_parameter_local(LLVMValueRef function,
                                  const MirProcedure &procedure,
                                  const MirLocal &local,
                                  LLVMValueRef allocation,
                                  const CAbiFunctionPlan &plan) {
    const Type &signature = type(procedure.type);
    if (!signature.c_calling_convention) {
      LLVMValueRef parameter =
          LLVMGetParam(function, local.parameter_index + 1);
      LLVMValueRef store =
          LLVMBuildStore(builder_.value, parameter, allocation);
      LLVMSetAlignment(store, type(local.type).layout.alignment);
      return;
    }
    if (!plan.ok || local.parameter_index >= plan.parameters.size()) {
      error(local.range, "C ABI procedure parameter has no physical plan");
      return;
    }

    const CAbiType abi = c_abi_type(local.type);
    const CAbiParameterMode mode = plan.parameters[local.parameter_index].mode;
    const std::size_t physical_start =
        c_parameter_start(procedure.type, plan, local.parameter_index);
    const std::vector<LLVMTypeRef> physical_types =
        c_parameter_types(local.type, mode);
    if (mode == CAbiParameterMode::Indirect) {
      LLVMValueRef logical = LLVMBuildLoad2(
          builder_.value, llvm_type(local.type),
          LLVMGetParam(function, static_cast<unsigned>(physical_start)), "");
      LLVMSetAlignment(logical, abi.alignment);
      LLVMValueRef store = LLVMBuildStore(builder_.value, logical, allocation);
      LLVMSetAlignment(store, type(local.type).layout.alignment);
      return;
    }
    if (abi.classification == CAbiClass::Direct) {
      LLVMValueRef store = LLVMBuildStore(
          builder_.value,
          LLVMGetParam(function, static_cast<unsigned>(physical_start)),
          allocation);
      LLVMSetAlignment(store, type(local.type).layout.alignment);
      return;
    }
    if (abi.classification == CAbiClass::HomogeneousFloatAggregate) {
      LLVMValueRef store = LLVMBuildStore(
          builder_.value,
          LLVMGetParam(function, static_cast<unsigned>(physical_start)),
          allocation);
      LLVMSetAlignment(store, abi.alignment);
      return;
    }
    if (abi.classification == CAbiClass::SmallAggregate ||
        abi.classification == CAbiClass::EightbyteAggregate) {
      const std::uint64_t storage_size = abi_argument_storage_size(abi);
      LLVMValueRef storage =
          abi_scratch(storage_size, abi.alignment,
                      "abi.param." + std::to_string(local.parameter_index));
      LLVMValueRef zero = LLVMBuildStore(
          builder_.value,
          LLVMConstNull(LLVMArrayType2(LLVMInt8TypeInContext(context_.value),
                                       storage_size)),
          storage);
      LLVMSetAlignment(zero, abi.alignment);
      if (abi.classification == CAbiClass::SmallAggregate) {
        LLVMValueRef store = LLVMBuildStore(
            builder_.value,
            LLVMGetParam(function, static_cast<unsigned>(physical_start)),
            storage);
        LLVMSetAlignment(store, abi.alignment);
      } else {
        for (std::size_t component = 0; component < physical_types.size();
             ++component) {
          LLVMValueRef address = storage;
          if (component != 0) {
            LLVMValueRef offset = LLVMConstInt(
                LLVMInt64TypeInContext(context_.value), component * 8U, 0);
            address = LLVMBuildGEP2(builder_.value,
                                    LLVMInt8TypeInContext(context_.value),
                                    storage, &offset, 1, "");
          }
          LLVMValueRef store = LLVMBuildStore(
              builder_.value,
              LLVMGetParam(function,
                           static_cast<unsigned>(physical_start + component)),
              address);
          LLVMSetAlignment(store, sysv_component_alignment(abi, component));
        }
      }
      LLVMValueRef logical =
          LLVMBuildLoad2(builder_.value, llvm_type(local.type), storage, "");
      LLVMSetAlignment(logical, abi.alignment);
      LLVMValueRef store = LLVMBuildStore(builder_.value, logical, allocation);
      LLVMSetAlignment(store, type(local.type).layout.alignment);
      return;
    }
    error(local.range, "illegal C ABI procedure parameter reached LLVM");
  }

  void emit_instruction(const MirProcedure &procedure,
                        std::size_t instruction_index,
                        const MirInstruction &instruction,
                        std::vector<LLVMValueRef> &values,
                        LLVMValueRef context_parameter,
                        const std::vector<LLVMValueRef> &locals,
                        const std::vector<LLVMValueRef> &aggregate_scratch,
                        const DirectAbiScratch &abi_scratch) {
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
        result = emit_c_call(instruction_index, procedure, instruction, values,
                             abi_scratch);
        break;
      }
      if (instruction.establishes_thread_context) {
        LLVMValueRef attach =
            runtime_helper("__draft.runtime.attach_thread",
                           LLVMVoidTypeInContext(context_.value), {});
        (void)LLVMBuildCall2(builder_.value, LLVMGlobalGetValueType(attach),
                             attach, nullptr, 0, "");
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
    case MirInstructionKind::Assert:
      emit_assertion(procedure, instruction, values, context_parameter);
      break;
    case MirInstructionKind::BoundsCheck:
      emit_bounds_check(instruction, values, false);
      break;
    case MirInstructionKind::SliceBoundsCheck:
      emit_bounds_check(instruction, values, true);
      break;
    case MirInstructionKind::Invalid:
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
                       const std::vector<LLVMBasicBlockRef> &blocks,
                       LLVMValueRef function,
                       const DirectAbiScratch &abi_scratch) {
    switch (terminator.kind) {
    case MirTerminatorKind::Return:
      if (terminator.value.is_valid()) {
        const Type &signature = type(procedure.type);
        const TypeId logical_result = procedure.value(terminator.value).type;
        if (!signature.c_calling_convention) {
          LLVMBuildRet(builder_.value, value_operand(values, terminator.value,
                                                     terminator.range));
          break;
        }
        const CAbiType abi = c_abi_function_plan(procedure.type).result;
        if (abi.classification == CAbiClass::Indirect) {
          LLVMValueRef store = LLVMBuildStore(
              builder_.value,
              value_operand(values, terminator.value, terminator.range),
              LLVMGetParam(function, 0));
          LLVMSetAlignment(store, abi.alignment);
          LLVMBuildRetVoid(builder_.value);
        } else if (abi.classification == CAbiClass::Win64WideInteger ||
                   abi.classification == CAbiClass::SmallAggregate ||
                   abi.classification == CAbiClass::HomogeneousFloatAggregate ||
                   abi.classification == CAbiClass::EightbyteAggregate) {
          LLVMValueRef store = LLVMBuildStore(
              builder_.value,
              value_operand(values, terminator.value, terminator.range),
              abi_scratch.procedure_result);
          LLVMSetAlignment(store, abi.alignment);
          LLVMValueRef physical =
              LLVMBuildLoad2(builder_.value, c_result_type(logical_result),
                             abi_scratch.procedure_result, "");
          LLVMSetAlignment(physical, abi.alignment);
          LLVMBuildRet(builder_.value, physical);
        } else {
          LLVMBuildRet(builder_.value, value_operand(values, terminator.value,
                                                     terminator.range));
        }
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
    const CAbiFunctionPlan procedure_abi =
        signature.c_calling_convention ? c_abi_function_plan(procedure.type)
                                       : CAbiFunctionPlan{};
    if (signature.c_calling_convention && !procedure_abi.ok) {
      error(procedure.range, "cannot plan C ABI procedure definition");
      return;
    }
    LLVMValueRef function = function_for_symbol(procedure.symbol);
    if (function == nullptr)
      return;
    current_debug_subprogram_ = debug_subprogram(procedure, function);
    set_debug_location(procedure.range);
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
      initialize_parameter_local(function, procedure, local, allocation,
                                 procedure_abi);
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
    DirectAbiScratch abi_scratch = allocate_c_abi_scratch(procedure);

    LLVMValueRef context_parameter =
        signature.c_calling_convention ? nullptr : LLVMGetParam(function, 0);
    std::vector<LLVMValueRef> values(procedure.values.size(), nullptr);
    for (std::size_t block_index = 0; block_index < procedure.blocks.size();
         ++block_index) {
      LLVMPositionBuilderAtEnd(builder_.value, blocks[block_index]);
      const MirBlock &block = procedure.blocks[block_index];
      for (MirInstructionId instruction_id : block.instructions) {
        const MirInstruction &instruction =
            procedure.instruction(instruction_id);
        set_debug_location(instruction.range);
        emit_instruction(procedure, instruction_id.value, instruction, values,
                         context_parameter, locals, aggregate_scratch,
                         abi_scratch);
      }
      set_debug_location(block.terminator.range);
      emit_terminator(procedure, block.terminator, values, blocks, function,
                      abi_scratch);
    }
    clear_debug_location();
    current_debug_subprogram_ = nullptr;
  }

  void emit_procedures() {
    for (const MirProcedure *procedure : procedures_) {
      if (procedure != nullptr)
        emit_procedure(*procedure);
    }
  }

  // Program entry may live in a different deterministic O0 unit from main's
  // definition. Semantic reachability has already proved that a concrete body
  // exists somewhere in the package; this unit needs only the checked symbol
  // and signature so function_for_symbol can create an external declaration.
  [[nodiscard]] std::optional<SymbolId> main_symbol() const {
    const std::optional<SymbolId> found =
        semantic_.symbols.lookup_direct(semantic_.package_scope, "main");
    if (!found.has_value())
      return std::nullopt;
    const Symbol &symbol = semantic_.symbols.symbol(*found);
    if (symbol.kind != SymbolKind::Procedure)
      return std::nullopt;
    return found;
  }

  // The program-specific entry is intentionally tiny. The bundled target
  // runtime owns process views, root Context state, and shutdown; this module
  // contributes only the checked call to the selected Draft main and the
  // language-defined exit-value conversion.
  void emit_program_entry() {
    const std::optional<SymbolId> entry = main_symbol();
    if (!entry.has_value()) {
      error(SourceRange::invalid(),
            "executable root package has no defined main procedure");
      return;
    }
    const Symbol &symbol = semantic_.symbols.symbol(*entry);
    const Type &signature = type(symbol.type);
    const std::size_t parameter_count =
        signature.members.empty() ? 0 : signature.members.size() - 1;
    if (parameter_count != 0 || signature.c_calling_convention ||
        symbol.flags.parametric) {
      error(symbol.name_range,
            "Draft main must be a non-parametric ordinary zero-parameter "
            "procedure");
      return;
    }
    const TypeId result_type = function_result(symbol.type);
    if (result_type != semantic_.types.builtins().void_type &&
        result_type != semantic_.types.builtins().int_type) {
      error(symbol.name_range, "Draft main result must be void or int");
      return;
    }

    LLVMTypeRef i32 = LLVMInt32TypeInContext(context_.value);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context_.value, 0);
    LLVMTypeRef entry_parameters[]{i32, pointer, pointer};
    LLVMTypeRef entry_type = LLVMFunctionType(i32, entry_parameters, 3, 0);
    const char *entry_name =
        target_.facts.os == "windows" ? "wmain" : "main";
    LLVMValueRef hosted_entry =
        LLVMAddFunction(module_.value, entry_name, entry_type);
    LLVMBasicBlockRef block = LLVMAppendBasicBlockInContext(
        context_.value, hosted_entry, "entry");
    LLVMPositionBuilderAtEnd(builder_.value, block);

    LLVMTypeRef initialize_parameters[]{i32, pointer, pointer};
    LLVMValueRef initialize = runtime_helper(
        "__draft.runtime.initialize_process", pointer,
        initialize_parameters);
    LLVMValueRef initialize_arguments[]{LLVMGetParam(hosted_entry, 0),
                                        LLVMGetParam(hosted_entry, 1),
                                        LLVMGetParam(hosted_entry, 2)};
    LLVMValueRef runtime_context = LLVMBuildCall2(
        builder_.value, LLVMGlobalGetValueType(initialize), initialize,
        initialize_arguments, 3, "draft.context");

    LLVMValueRef main = function_for_symbol(*entry);
    LLVMValueRef main_arguments[]{runtime_context};
    LLVMValueRef draft_result = LLVMBuildCall2(
        builder_.value, LLVMGlobalGetValueType(main), main, main_arguments, 1,
        result_type == semantic_.types.builtins().void_type ? ""
                                                            : "draft.result");
    LLVMValueRef shutdown = runtime_helper(
        "__draft.runtime.shutdown_process",
        LLVMVoidTypeInContext(context_.value), {});
    (void)LLVMBuildCall2(
        builder_.value, LLVMGlobalGetValueType(shutdown), shutdown, nullptr, 0,
        "");
    if (result_type == semantic_.types.builtins().void_type) {
      LLVMBuildRet(builder_.value, LLVMConstInt(i32, 0, 0));
    } else {
      LLVMBuildRet(
          builder_.value,
          LLVMBuildTrunc(builder_.value, draft_result, i32, "exit.result"));
    }
  }

  [[nodiscard]] LLVMValueRef validation_function(
      const ValidationEntry &entry) {
    const std::string name =
        llvm_package_symbol_name(entry.package, entry.procedure);
    LLVMValueRef function = LLVMGetNamedFunction(module_.value, name.c_str());
    if (function != nullptr)
      return function;
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context_.value, 0);
    LLVMTypeRef parameters[]{pointer, pointer};
    function = LLVMAddFunction(
        module_.value, name.c_str(),
        LLVMFunctionType(LLVMVoidTypeInContext(context_.value), parameters, 2,
                         0));
    LLVMSetVisibility(function, LLVMHiddenVisibility);
    return function;
  }

  // Validation retains one straight-line wrapper in the root package because
  // the selected procedure list and state layouts are program products. The
  // invariant process setup/reporting/reset services remain in the bundled
  // runtime object.
  void emit_validation_entry() {
    LLVMTypeRef i1 = LLVMInt1TypeInContext(context_.value);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(context_.value);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context_.value);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context_.value);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context_.value, 0);
    LLVMTypeRef entry_parameters[]{i32, pointer, pointer};
    LLVMTypeRef entry_type = LLVMFunctionType(i32, entry_parameters, 3, 0);
    const char *entry_name =
        target_.facts.os == "windows" ? "wmain" : "main";
    LLVMValueRef hosted_entry =
        LLVMAddFunction(module_.value, entry_name, entry_type);
    LLVMBasicBlockRef block = LLVMAppendBasicBlockInContext(
        context_.value, hosted_entry, "entry");
    LLVMPositionBuilderAtEnd(builder_.value, block);

    LLVMTypeRef initialize_parameters[]{i32, pointer, pointer};
    LLVMValueRef initialize = runtime_helper(
        "__draft.runtime.initialize_process", pointer,
        initialize_parameters);
    LLVMValueRef initialize_arguments[]{LLVMGetParam(hosted_entry, 0),
                                        LLVMGetParam(hosted_entry, 1),
                                        LLVMGetParam(hosted_entry, 2)};
    LLVMValueRef runtime_context = LLVMBuildCall2(
        builder_.value, LLVMGlobalGetValueType(initialize), initialize,
        initialize_arguments, 3, "draft.context");
    LLVMValueRef any_failed = LLVMConstInt(i1, 0, 0);
    LLVMTypeRef report_parameters[]{pointer, i64};
    LLVMValueRef report = runtime_helper(
        "__draft.runtime.validation_report",
        LLVMVoidTypeInContext(context_.value), report_parameters);
    LLVMValueRef reset = runtime_helper(
        "__draft.runtime.reset_temporary_allocator",
        LLVMVoidTypeInContext(context_.value), {});

    for (std::size_t index = 0;
         index < options_.module.validation_entries.size(); ++index) {
      const ValidationEntry &entry =
          options_.module.validation_entries[index];
      if (entry.state_alignment >
          std::numeric_limits<unsigned>::max()) {
        error(SourceRange::invalid(),
              "validation state alignment exceeds LLVM's alignment domain");
        return;
      }
      const unsigned state_alignment =
          static_cast<unsigned>(entry.state_alignment);
      LLVMTypeRef state_type = LLVMArrayType2(i8, entry.state_size);
      LLVMValueRef state = LLVMBuildAlloca(
          builder_.value, state_type,
          ("validation.state." + std::to_string(index)).c_str());
      LLVMSetAlignment(state, state_alignment);
      (void)LLVMBuildMemSet(
          builder_.value, state, LLVMConstInt(i8, 0, 0),
          LLVMConstInt(i64, entry.state_size, 0), state_alignment);
      LLVMValueRef validation = validation_function(entry);
      LLVMValueRef validation_arguments[]{runtime_context, state};
      (void)LLVMBuildCall2(
          builder_.value, LLVMGlobalGetValueType(validation), validation,
          validation_arguments, 2, "");

      LLVMValueRef failure_offset =
          LLVMConstInt(i64, entry.failure_offset, 0);
      LLVMValueRef failure_address = LLVMBuildGEP2(
          builder_.value, i8, state, &failure_offset, 1,
          ("validation.failures.address." + std::to_string(index)).c_str());
      LLVMValueRef failures = LLVMBuildLoad2(
          builder_.value, i64, failure_address,
          ("validation.failures." + std::to_string(index)).c_str());
      LLVMSetAlignment(failures, 8);
      LLVMValueRef failed = LLVMBuildICmp(
          builder_.value, LLVMIntNE, failures, LLVMConstInt(i64, 0, 0),
          ("validation.failed." + std::to_string(index)).c_str());
      any_failed = LLVMBuildOr(
          builder_.value, any_failed, failed,
          ("validation.any_failed." + std::to_string(index)).c_str());
      LLVMValueRef report_arguments[]{
          state, LLVMConstInt(i64, entry.report_size, 0)};
      (void)LLVMBuildCall2(
          builder_.value, LLVMGlobalGetValueType(report), report,
          report_arguments, 2, "");
      (void)LLVMBuildCall2(
          builder_.value, LLVMGlobalGetValueType(reset), reset, nullptr, 0,
          "");
    }

    LLVMValueRef shutdown = runtime_helper(
        "__draft.runtime.shutdown_process",
        LLVMVoidTypeInContext(context_.value), {});
    (void)LLVMBuildCall2(
        builder_.value, LLVMGlobalGetValueType(shutdown), shutdown, nullptr, 0,
        "");
    LLVMBuildRet(
        builder_.value,
        LLVMBuildZExt(builder_.value, any_failed, i32, "validation.exit"));
  }

  const TargetProfile &target_;
  const LlvmPackageEmissionOptions &options_;
  const SemanticPackage &semantic_;
  const CAbiTable &abi_;
  const ConstantTable &global_initializers_;
  std::span<const SymbolId> globals_;
  std::span<const MirProcedure *const> procedures_;
  const SourceManager &sources_;
  DiagnosticSink &diagnostics_;
  std::size_t initial_errors_ = 0;

  ContextOwner context_;
  ModuleOwner module_;
  BuilderOwner builder_;
  DebugBuilderOwner debug_builder_;
  LLVMMetadataRef debug_compile_unit_ = nullptr;
  LLVMMetadataRef debug_subroutine_type_ = nullptr;
  LLVMMetadataRef current_debug_subprogram_ = nullptr;
  std::vector<DirectDebugFile> debug_files_;
  std::vector<DirectDebugScope> debug_scopes_;
  std::vector<LLVMTypeRef> llvm_types_;
  std::vector<LLVMValueRef> functions_;
  std::vector<LLVMValueRef> global_values_;
  std::size_t next_string_constant_ = 0;
  std::size_t next_cstring_constant_ = 0;
  std::size_t next_relocatable_constant_ = 0;
  std::size_t next_auxiliary_block_ = 0;
};

} // namespace

std::string llvm_package_symbol_name(
    const PackageIdentity &package, std::string_view declaration_name) {
  return "draft." + encoded_name(package.root_identity) + "." +
      encoded_name(package.root_relative_path) + "." +
      encoded_name(declaration_name);
}

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
