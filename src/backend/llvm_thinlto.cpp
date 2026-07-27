// In-process preparation and execution of LLVM ThinLTO.
//
// See llvm_thinlto.h for the phase and ownership contract. This file alone
// uses LLVM's C++ LTO and bitcode-summary APIs because LLVM 22 does not expose
// summary construction or the resolution-aware LTO driver through its C API.
// The rest of Draft still sees value-only buffers. The constructed-module seam
// unwraps one task-owned LLVM C handle only for the duration of serialization;
// no C++ LLVM type crosses a repository header.

#include "backend/llvm_thinlto.h"

#include "backend/llvm_initialization.h"
#include "backend/llvm_module_emission.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#if defined(_MSC_VER)
// LLVM 22.1.6's public C++ headers contain four inline conversions from their
// 64-bit intermediate IDs to explicitly narrower stored IDs. MSVC attributes
// those conversions to std::pair while parsing ModuleSummaryIndex,
// ScaledNumber, and FunctionImport, even when both LLVM and the standard
// library are external headers. The LLVM types themselves establish the range
// invariant; Draft neither supplies nor converts these values. Suppress only
// that dependency-owned diagnostic while parsing the C++ API, then restore the
// repository's /W4 /WX policy before this adapter's implementation begins.
#pragma warning(push)
#pragma warning(disable : 4244)
#endif

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/ModuleSummaryAnalysis.h>
#include <llvm/Analysis/ProfileSummaryInfo.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Module.h>
#include <llvm/LTO/LTO.h>
#include <llvm/Support/CBindingWrapping.h>
#include <llvm/Support/Caching.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <llvm/Support/Threading.h>
#include <llvm/Support/raw_ostream.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cassert>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace draft {
namespace {

struct TargetMachineOwner {
  LLVMTargetMachineRef value = nullptr;
  TargetMachineOwner() = default;
  ~TargetMachineOwner() {
    if (value != nullptr)
      LLVMDisposeTargetMachine(value);
  }
  TargetMachineOwner(const TargetMachineOwner &) = delete;
  TargetMachineOwner &operator=(const TargetMachineOwner &) = delete;
};

struct TargetDataOwner {
  LLVMTargetDataRef value = nullptr;
  TargetDataOwner() = default;
  ~TargetDataOwner() {
    if (value != nullptr)
      LLVMDisposeTargetData(value);
  }
  TargetDataOwner(const TargetDataOwner &) = delete;
  TargetDataOwner &operator=(const TargetDataOwner &) = delete;
};

struct PassOptionsOwner {
  LLVMPassBuilderOptionsRef value = nullptr;
  PassOptionsOwner() = default;
  ~PassOptionsOwner() {
    if (value != nullptr)
      LLVMDisposePassBuilderOptions(value);
  }
  PassOptionsOwner(const PassOptionsOwner &) = delete;
  PassOptionsOwner &operator=(const PassOptionsOwner &) = delete;
};

// PhaseTimer is intentionally local to this adapter. Detailed timing values
// are diagnostic observations written into task-owned results; they never
// participate in scheduling, native bytes, or program identity.
class PhaseTimer {
public:
  PhaseTimer(bool enabled, std::uint64_t &destination)
      : destination_(enabled ? &destination : nullptr) {
    if (destination_ != nullptr)
      started_ = Clock::now();
  }

  PhaseTimer(const PhaseTimer &) = delete;
  PhaseTimer &operator=(const PhaseTimer &) = delete;
  ~PhaseTimer() { finish(); }

  void finish() {
    if (destination_ == nullptr)
      return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             Clock::now() - started_)
                             .count();
    assert(elapsed >= 0 && "LLVM phase duration must be nonnegative");
    *destination_ = static_cast<std::uint64_t>(elapsed);
    destination_ = nullptr;
  }

private:
  using Clock = std::chrono::steady_clock;
  std::uint64_t *destination_ = nullptr;
  Clock::time_point started_;
};

[[nodiscard]] std::string take_llvm_message(char *message) {
  if (message == nullptr)
    return {};
  std::string result(message);
  LLVMDisposeMessage(message);
  return result;
}

[[nodiscard]] std::string take_llvm_error(LLVMErrorRef error) {
  if (error == LLVMErrorSuccess)
    return {};
  char *message = LLVMGetErrorMessage(error);
  if (message == nullptr)
    return "unknown LLVM pass error";
  std::string result(message);
  LLVMDisposeErrorMessage(message);
  return result;
}

[[nodiscard]] bool select_relocation_model(TargetRelocationModel model,
                                           LLVMRelocMode &llvm_model,
                                           std::string &failure) {
  switch (model) {
  case TargetRelocationModel::PositionIndependent:
    llvm_model = LLVMRelocPIC;
    return true;
  }
  failure = "ThinLTO does not implement target relocation model";
  return false;
}

[[nodiscard]] bool select_code_model(TargetCodeModel model,
                                     LLVMCodeModel &llvm_model,
                                     std::string &failure) {
  switch (model) {
  case TargetCodeModel::Small:
    llvm_model = LLVMCodeModelSmall;
    return true;
  }
  failure = "ThinLTO does not implement target code model";
  return false;
}

[[nodiscard]] bool select_lto_relocation_model(
    TargetRelocationModel model,
    std::optional<llvm::Reloc::Model> &llvm_model,
    std::string &failure) {
  switch (model) {
  case TargetRelocationModel::PositionIndependent:
    llvm_model = llvm::Reloc::PIC_;
    return true;
  }
  failure = "ThinLTO does not implement target relocation model";
  return false;
}

[[nodiscard]] bool select_lto_code_model(
    TargetCodeModel model,
    std::optional<llvm::CodeModel::Model> &llvm_model,
    std::string &failure) {
  switch (model) {
  case TargetCodeModel::Small:
    llvm_model = llvm::CodeModel::Small;
    return true;
  }
  failure = "ThinLTO does not implement target code model";
  return false;
}

// AddressSanitizer is opt-in at function granularity in LLVM. Add the same
// attributes as the ordinary object emitter before summary construction so an
// imported function remains selected for instrumentation in its destination
// backend. The ASan pass itself runs only after cross-module importing.
[[nodiscard]] bool add_address_sanitizer_attributes(LLVMContextRef context,
                                                    LLVMModuleRef module,
                                                    std::string &failure) {
  static constexpr std::string_view sanitize_name = "sanitize_address";
  const unsigned sanitize_kind = LLVMGetEnumAttributeKindForName(
      sanitize_name.data(), sanitize_name.size());
  if (sanitize_kind == 0) {
    failure = "linked LLVM has no sanitize_address function attribute";
    return false;
  }
  LLVMAttributeRef sanitize =
      LLVMCreateEnumAttribute(context, sanitize_kind, 0);
  static constexpr std::string_view frame_pointer = "frame-pointer";
  static constexpr std::string_view frame_pointer_value = "all";
  LLVMAttributeRef retain_frame_pointer = LLVMCreateStringAttribute(
      context, frame_pointer.data(),
      static_cast<unsigned>(frame_pointer.size()), frame_pointer_value.data(),
      static_cast<unsigned>(frame_pointer_value.size()));
  const LLVMAttributeIndex function_attributes =
      static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex);
  for (LLVMValueRef function = LLVMGetFirstFunction(module);
       function != nullptr; function = LLVMGetNextFunction(function)) {
    if (LLVMCountBasicBlocks(function) == 0)
      continue;
    LLVMAddAttributeAtIndex(function, function_attributes, sanitize);
    LLVMAddAttributeAtIndex(function, function_attributes,
                            retain_frame_pointer);
  }
  failure.clear();
  return true;
}

[[nodiscard]] LlvmThinLtoBitcodeResult bitcode_failure(
    std::string_view module_name,
    std::string_view operation,
    std::string reason,
    const LlvmObjectEmissionPhaseTimings &timings) {
  LlvmThinLtoBitcodeResult result;
  result.failure = "in-process LLVM ThinLTO " + std::string(operation) +
      " for '" + std::string(module_name) + "' failed";
  if (!reason.empty())
    result.failure += ": " + reason;
  result.phase_timings = timings;
  return result;
}

[[nodiscard]] LlvmThinLtoResult thinlto_failure(
    std::string_view operation,
    std::string reason,
    const LlvmThinLtoPhaseTimings &timings) {
  LlvmThinLtoResult result;
  result.failure =
      "in-process LLVM ThinLTO " + std::string(operation) + " failed";
  if (!reason.empty())
    result.failure += ": " + reason;
  result.phase_timings = timings;
  return result;
}

// Splits the target profile's comma-separated LLVM feature spelling into the
// exact vector consumed by lto::Config. Empty components are rejected by the
// target profile constructor before this backend is reached and are ignored
// defensively here.
[[nodiscard]] std::vector<std::string> lto_target_features(
    std::string_view feature_string) {
  std::vector<std::string> features;
  std::size_t begin = 0;
  while (begin <= feature_string.size()) {
    const std::size_t comma = feature_string.find(',', begin);
    const std::size_t end = comma == std::string_view::npos
        ? feature_string.size()
        : comma;
    if (end != begin)
      features.emplace_back(feature_string.substr(begin, end - begin));
    if (comma == std::string_view::npos)
      break;
    begin = comma + 1;
  }
  return features;
}

// DefinitionSummary holds the linker facts needed for every occurrence of one
// mangled IR symbol. Draft emits one strong definition for every package-
// qualified symbol. Explicit C exports retain default visibility; all ordinary
// Draft definitions are hidden and may be internalized once their ThinLTO
// references have been resolved.
struct DefinitionSummary {
  std::size_t input_index = 0;
  llvm::GlobalValue::VisibilityTypes visibility =
      llvm::GlobalValue::DefaultVisibility;
  bool can_be_omitted = false;
  bool preserved_artifact_root = false;
};

// One output task owns a SmallVector because LLVM's AddStream callback needs a
// seekable raw_pwrite_stream. The vector and name slot are fixed before any
// backend starts, so concurrent tasks mutate disjoint objects without a lock.
struct ThinLtoOutputSlot {
  llvm::SmallVector<char, 0> bytes;
  std::string module_name;
  bool requested = false;
};

} // namespace

LlvmThinLtoBitcodeResult prepare_constructed_llvm_module_for_thinlto(
    const TargetProfile &target, std::string_view module_name,
    LLVMModuleRef module, LlvmObjectEmissionOptions options) {
  LlvmThinLtoBitcodeResult result;
  const auto fail = [&](std::string_view operation, std::string reason) {
    return bitcode_failure(
        module_name, operation, std::move(reason), result.phase_timings);
  };

  if (module == nullptr)
    return fail("module construction", "caller supplied a null module");
  if (options.optimization != NativeOptimizationLevel::O2)
    return fail("configuration", "package preparation requires O2");

  PhaseTimer initialization_timing(
      options.collect_phase_timings,
      result.phase_timings.target_initialization_nanoseconds);
  initialize_draft_llvm_targets();
  initialization_timing.finish();
  if (LLVMIsMultithreaded() == 0) {
    return fail("initialization",
                "linked LLVM was built without thread support");
  }

  if (options.instrumentation == LlvmNativeInstrumentation::AddressSanitizer) {
    std::string attribute_failure;
    if (!add_address_sanitizer_attributes(
            LLVMGetModuleContext(module), module, attribute_failure)) {
      return fail("AddressSanitizer attribute setup", attribute_failure);
    }
  }

  // LLVM distinguishes summary-bearing ThinLTO input from regular bitcode
  // with the same module flag Clang emits for `-flto=thin`. Draft does not use
  // split LTO units, so the value is zero; the flag still tells InputFile that
  // the attached summary belongs to the ThinLTO path.
  static constexpr std::string_view split_lto_flag = "EnableSplitLTOUnit";
  LLVMMetadataRef split_lto_value = LLVMValueAsMetadata(LLVMConstInt(
      LLVMInt32TypeInContext(LLVMGetModuleContext(module)), 0, 0));
  LLVMAddModuleFlag(
      module,
      LLVMModuleFlagBehaviorError,
      split_lto_flag.data(),
      split_lto_flag.size(),
      split_lto_value);

  PhaseTimer target_validation_timing(
      options.collect_phase_timings,
      result.phase_timings.target_validation_nanoseconds);
  const std::string module_triple = LLVMGetTarget(module);
  const std::string module_layout = LLVMGetDataLayoutStr(module);
  target_validation_timing.finish();
  if (module_triple != target.llvm_triple) {
    return fail("target validation", "module triple '" + module_triple +
                                         "' does not match profile '" +
                                         target.llvm_triple + "'");
  }
  if (module_layout != target.llvm_data_layout) {
    return fail("target validation",
                "module data layout does not match profile '" +
                    target.facts.identity + "'");
  }

  char *verify_message = nullptr;
  PhaseTimer verification_timing(
      options.collect_phase_timings,
      result.phase_timings.ir_verification_nanoseconds);
  const int verification_status =
      LLVMVerifyModule(module, LLVMReturnStatusAction, &verify_message);
  verification_timing.finish();
  if (verification_status != 0)
    return fail("IR verification", take_llvm_message(verify_message));
  if (verify_message != nullptr)
    LLVMDisposeMessage(verify_message);

  LLVMRelocMode relocation_model = LLVMRelocDefault;
  LLVMCodeModel code_model = LLVMCodeModelDefault;
  std::string model_failure;
  if (!select_relocation_model(target.relocation_model, relocation_model,
                               model_failure) ||
      !select_code_model(target.code_model, code_model, model_failure)) {
    return fail("target selection", model_failure);
  }

  PhaseTimer target_machine_timing(
      options.collect_phase_timings,
      result.phase_timings.target_machine_nanoseconds);
  LLVMTargetRef llvm_target = nullptr;
  char *target_message = nullptr;
  if (LLVMGetTargetFromTriple(target.llvm_triple.c_str(), &llvm_target,
                              &target_message) != 0 ||
      llvm_target == nullptr) {
    target_machine_timing.finish();
    return fail("target lookup", take_llvm_message(target_message));
  }
  if (target_message != nullptr)
    LLVMDisposeMessage(target_message);

  TargetMachineOwner machine;
  machine.value = LLVMCreateTargetMachine(
      llvm_target, target.llvm_triple.c_str(), target.llvm_cpu.c_str(),
      target.llvm_feature_string.c_str(), LLVMCodeGenLevelDefault,
      relocation_model, code_model);
  if (machine.value == nullptr) {
    target_machine_timing.finish();
    return fail("target-machine creation", {});
  }
  LLVMSetTargetMachineAsmVerbosity(machine.value, 0);
  TargetDataOwner target_data;
  target_data.value = LLVMCreateTargetDataLayout(machine.value);
  if (target_data.value == nullptr) {
    target_machine_timing.finish();
    return fail("target data-layout creation", {});
  }
  const std::string computed_layout =
      take_llvm_message(LLVMCopyStringRepOfTargetData(target_data.value));
  target_machine_timing.finish();
  if (computed_layout != target.llvm_data_layout) {
    return fail("target validation",
                "LLVM computes data layout '" + computed_layout +
                    "' but profile requires '" + target.llvm_data_layout +
                    "'");
  }

  PassOptionsOwner pass_options;
  pass_options.value = LLVMCreatePassBuilderOptions();
  if (pass_options.value == nullptr)
    return fail("pass option creation", {});
  PhaseTimer prelink_timing(
      options.collect_phase_timings,
      result.phase_timings.o2_optimization_nanoseconds);
  const std::string prelink_failure = take_llvm_error(LLVMRunPasses(
      module, "thinlto-pre-link<O2>", machine.value, pass_options.value));
  prelink_timing.finish();
  if (!prelink_failure.empty())
    return fail("O2 pre-link optimization", prelink_failure);

  // LLVM's C bitcode writer cannot attach a module summary. Build the summary
  // from the already optimized module and serialize both into one in-memory
  // buffer. GenerateHash writes the ordinary ThinLTO module hash required by
  // LLVM's format; Draft does not retain it, name a cache directory, or use it
  // as source/artifact identity.
  PhaseTimer serialization_timing(
      options.collect_phase_timings,
      result.phase_timings.output_copy_nanoseconds);
  llvm::Module *native_module = llvm::unwrap(module);
  // LLVM's direct summary builder requires a real ProfileSummaryInfo object
  // even when the module has no profile metadata; the object represents that
  // absence. A null pointer is not the API's no-profile sentinel.
  llvm::ProfileSummaryInfo profile_summary(*native_module);
  llvm::ModuleSummaryIndex summary = llvm::buildModuleSummaryIndex(
      *native_module,
      [](const llvm::Function &) -> llvm::BlockFrequencyInfo * {
        // A null result asks the summary builder to compute the function's
        // block frequencies locally. The callback itself must be callable;
        // an empty std::function is not LLVM's sentinel for this case.
        return nullptr;
      },
      &profile_summary);
  llvm::SmallVector<char, 0> bitcode;
  llvm::raw_svector_ostream output(bitcode);
  llvm::WriteBitcodeToFile(
      *native_module, output, false, &summary, true);
  result.bitcode.assign(bitcode.data(), bitcode.size());
  serialization_timing.finish();
  if (result.bitcode.empty())
    return fail("bitcode serialization", "LLVM returned an empty buffer");
  result.ok = true;
  return result;
}

LlvmThinLtoResult emit_llvm_thinlto_in_process(
    const TargetProfile &target,
    std::span<const LlvmThinLtoModuleInput> inputs,
    LlvmObjectEmissionOptions options,
    std::size_t worker_count) {
  LlvmThinLtoResult result;
  const auto fail = [&](std::string_view operation, std::string reason) {
    return thinlto_failure(
        operation, std::move(reason), result.phase_timings);
  };
  if (options.optimization != NativeOptimizationLevel::O2)
    return fail("configuration", "whole-artifact emission requires O2");
  if (inputs.empty())
    return fail("input loading", "no package modules were supplied");

  initialize_draft_llvm_targets();
  if (LLVMIsMultithreaded() == 0) {
    return fail("initialization",
                "linked LLVM was built without thread support");
  }

  llvm::lto::Config config;
  config.CPU = target.llvm_cpu;
  config.MAttrs = lto_target_features(target.llvm_feature_string);
  std::string model_failure;
  if (!select_lto_relocation_model(
          target.relocation_model, config.RelocModel, model_failure) ||
      !select_lto_code_model(
          target.code_model, config.CodeModel, model_failure)) {
    return fail("target selection", model_failure);
  }
  config.CGOptLevel = llvm::CodeGenOptLevel::Default;
  config.CGFileType = options.output_kind == LlvmNativeOutputKind::Assembly
      ? llvm::CodeGenFileType::AssemblyFile
      : llvm::CodeGenFileType::ObjectFile;
  config.OptLevel = 2;
  config.VerifyEach = false;
  config.DisableVerify = false;
  config.DefaultTriple = target.llvm_triple;
  config.OverrideTriple = target.llvm_triple;
  // The ordinary O2 pipeline precedes ASan exactly as it did before ThinLTO.
  // Supplying the combined pipeline makes the sanitizer run in each imported
  // backend module rather than instrumenting definitions before importing.
  if (options.instrumentation == LlvmNativeInstrumentation::AddressSanitizer)
    config.OptPipeline = "default<O2>,asan";

  std::mutex diagnostic_mutex;
  std::string llvm_diagnostics;
  config.DiagHandler = [&](const llvm::DiagnosticInfo &diagnostic) {
    std::string rendered;
    llvm::raw_string_ostream stream(rendered);
    llvm::DiagnosticPrinterRawOStream printer(stream);
    diagnostic.print(printer);
    std::lock_guard<std::mutex> lock(diagnostic_mutex);
    if (!llvm_diagnostics.empty())
      llvm_diagnostics += "; ";
    llvm_diagnostics += rendered;
  };

  // ThinLTO backends perform the expensive module-local optimization/codegen
  // work. Bound their pool to the same worker budget as the enclosing compiler
  // graph. The enclosing ThinLTO node is the only runnable Draft task at this
  // point, so its sibling executor workers are sleeping rather than competing
  // for CPU. No FileCache is supplied to LTO::run below.
  llvm::ThreadPoolStrategy parallelism =
      llvm::heavyweight_hardware_concurrency(
          static_cast<unsigned>(worker_count));
  llvm::lto::ThinBackend backend =
      llvm::lto::createInProcessThinBackend(parallelism);
  llvm::lto::LTO lto(std::move(config), std::move(backend));

  struct LoadedInput {
    std::unique_ptr<llvm::lto::InputFile> file;
    std::vector<llvm::lto::SymbolResolution> resolutions;
  };
  std::vector<LoadedInput> loaded(inputs.size());
  std::map<std::string, DefinitionSummary> definitions;
  std::map<std::string, std::size_t> module_indices;

  PhaseTimer loading_timing(
      options.collect_phase_timings,
      result.phase_timings.input_loading_nanoseconds);
  for (std::size_t input_index = 0; input_index < inputs.size(); ++input_index) {
    const LlvmThinLtoModuleInput &input = inputs[input_index];
    if (input.identifier.empty() || input.bitcode.empty()) {
      loading_timing.finish();
      return fail("input loading", "module identity or bitcode is empty");
    }
    if (!module_indices.emplace(input.identifier, input_index).second) {
      loading_timing.finish();
      return fail("input loading", "duplicate module identity '" +
                                       input.identifier + "'");
    }
    llvm::MemoryBufferRef buffer(
        llvm::StringRef(input.bitcode.data(), input.bitcode.size()),
        input.identifier);
    llvm::Expected<std::unique_ptr<llvm::lto::InputFile>> created =
        llvm::lto::InputFile::create(buffer);
    if (!created) {
      loading_timing.finish();
      return fail("input loading", llvm::toString(created.takeError()));
    }
    loaded[input_index].file = std::move(*created);
    // InputFile learns whether a module is ThinLTO only when LTO::add reads
    // the bitcode's summary metadata. Calling isThinLTO here would always see
    // its construction-time false value, even for valid Clang ThinLTO input.
    std::vector<bool> preserved_found(
        input.preserved_symbols.size(), false);
    for (const llvm::lto::InputFile::Symbol &symbol :
         loaded[input_index].file->symbols()) {
      if (symbol.isUndefined())
        continue;
      const std::string name = symbol.getName().str();
      DefinitionSummary definition;
      definition.input_index = input_index;
      definition.visibility = symbol.getVisibility();
      definition.can_be_omitted = symbol.canBeOmittedFromSymbolTable();
      const std::string ir_name = symbol.getIRName().str();
      for (std::size_t preserved_index = 0;
           preserved_index < input.preserved_symbols.size();
           ++preserved_index) {
        if (input.preserved_symbols[preserved_index] == ir_name) {
          definition.preserved_artifact_root = true;
          preserved_found[preserved_index] = true;
        }
      }
      if (!definitions.emplace(name, definition).second) {
        loading_timing.finish();
        return fail("symbol resolution", "multiple definitions of '" +
                                             name + "'");
      }
    }
    for (std::size_t preserved_index = 0;
         preserved_index < input.preserved_symbols.size();
         ++preserved_index) {
      if (!preserved_found[preserved_index]) {
        loading_timing.finish();
        return fail(
            "symbol resolution",
            "preserved symbol '" + input.preserved_symbols[preserved_index] +
                "' is not defined by module '" +
                input.identifier + "'");
      }
    }
  }

  for (std::size_t input_index = 0; input_index < loaded.size(); ++input_index) {
    LoadedInput &input = loaded[input_index];
    input.resolutions.reserve(input.file->symbols().size());
    for (const llvm::lto::InputFile::Symbol &symbol : input.file->symbols()) {
      llvm::lto::SymbolResolution resolution;
      const std::string name = symbol.getName().str();
      const auto definition = definitions.find(name);
      const bool has_definition = definition != definitions.end();
      const bool is_definition = !symbol.isUndefined();
      resolution.Prevailing =
          is_definition && has_definition &&
          definition->second.input_index == input_index;
      // Hidden Draft symbols are final inside this LTO unit and may be imported
      // freely. A default-visible C export remains externally interposable and
      // is therefore preserved rather than assumed final.
      resolution.FinalDefinitionInLinkageUnit =
          has_definition &&
          definition->second.visibility != llvm::GlobalValue::DefaultVisibility;
      const bool public_c_definition = is_definition && has_definition &&
          definition->second.visibility ==
              llvm::GlobalValue::DefaultVisibility;
      // Only explicit C exports are observable by non-LTO objects. Ordinary
      // Draft definitions use hidden visibility even though their external
      // linkage makes them appear in the input symbol table. Marking every
      // such symbol VisibleToRegularObj would preserve every package boundary
      // and defeat the cross-package importing this operation exists to do.
      resolution.VisibleToRegularObj =
          is_definition && has_definition &&
          ((public_c_definition && !definition->second.can_be_omitted) ||
           definition->second.preserved_artifact_root);
      resolution.ExportDynamic =
          public_c_definition && !definition->second.can_be_omitted;
      resolution.LinkerRedefined = false;
      input.resolutions.push_back(resolution);
    }
    if (llvm::Error error =
            lto.add(std::move(input.file), input.resolutions)) {
      loading_timing.finish();
      return fail("input loading", llvm::toString(std::move(error)));
    }
  }
  loading_timing.finish();

  const unsigned maximum_tasks = lto.getMaxTasks();
  if (maximum_tasks == 0)
    return fail("backend planning", "LLVM reported no native tasks");
  std::vector<ThinLtoOutputSlot> output_slots(maximum_tasks);
  llvm::AddStreamFn add_stream =
      [&](unsigned task, const llvm::Twine &module_name)
          -> llvm::Expected<std::unique_ptr<llvm::CachedFileStream>> {
    if (task >= output_slots.size()) {
      return llvm::createStringError(
          std::errc::invalid_argument,
          "ThinLTO returned an out-of-range task identifier");
    }
    ThinLtoOutputSlot &slot = output_slots[task];
    if (slot.requested) {
      return llvm::createStringError(
          std::errc::invalid_argument,
          "ThinLTO requested one task output more than once");
    }
    slot.requested = true;
    slot.module_name = module_name.str();
    auto stream = std::make_unique<llvm::raw_svector_ostream>(slot.bytes);
    return std::make_unique<llvm::CachedFileStream>(std::move(stream));
  };

  PhaseTimer lto_timing(
      options.collect_phase_timings,
      result.phase_timings.thin_link_and_backends_nanoseconds);
  llvm::Error lto_error = lto.run(std::move(add_stream));
  lto_timing.finish();
  if (lto_error) {
    std::string reason = llvm::toString(std::move(lto_error));
    if (!llvm_diagnostics.empty())
      reason += ": " + llvm_diagnostics;
    return fail("thin link or backend", std::move(reason));
  }

  PhaseTimer copy_timing(
      options.collect_phase_timings,
      result.phase_timings.output_copy_nanoseconds);
  std::vector<std::optional<std::string>> output_by_module(inputs.size());
  for (ThinLtoOutputSlot &slot : output_slots) {
    if (!slot.requested)
      continue;
    const auto module = module_indices.find(slot.module_name);
    if (module == module_indices.end()) {
      copy_timing.finish();
      return fail("output publication", "LLVM returned unknown module '" +
                                            slot.module_name + "'");
    }
    std::optional<std::string> &destination =
        output_by_module[module->second];
    if (destination.has_value()) {
      copy_timing.finish();
      return fail("output publication", "LLVM returned multiple outputs for '" +
                                            slot.module_name + "'");
    }
    destination.emplace(slot.bytes.data(), slot.bytes.size());
  }
  for (std::size_t module_index = 0;
       module_index < output_by_module.size(); ++module_index) {
    if (!output_by_module[module_index].has_value())
      continue;
    result.outputs.push_back({
        module_index, std::move(*output_by_module[module_index])});
  }
  copy_timing.finish();
  if (result.outputs.empty())
    return fail("output publication", "LLVM returned no native output");
  result.ok = true;
  return result;
}

} // namespace draft
