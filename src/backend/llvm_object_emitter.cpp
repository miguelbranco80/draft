// In-process LLVM-C adapter for deterministic native emission.
//
// See llvm_object_emitter.h for the phase, ownership, and semantic firewall.
// This implementation deliberately uses small local RAII owners around opaque
// C handles so every error path releases LLVM resources without introducing a
// compiler-wide wrapper framework.

#include "backend/llvm_object_emitter.h"

#include "backend/llvm_module_emission.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include <cassert>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#ifndef DRAFT_LLVM_VERSION
#error "DRAFT_LLVM_VERSION must name the LLVM distribution linked into draftc"
#endif
#ifndef DRAFT_LLVM_TOOLS_DIRECTORY
#error "DRAFT_LLVM_TOOLS_DIRECTORY must name the linked LLVM tool directory"
#endif

namespace draft {
namespace {

struct ContextOwner {
  LLVMContextRef value = nullptr;
  ContextOwner() = default;
  ~ContextOwner() {
    if (value != nullptr)
      LLVMContextDispose(value);
  }
  ContextOwner(const ContextOwner &) = delete;
  ContextOwner &operator=(const ContextOwner &) = delete;
};

struct MemoryBufferOwner {
  LLVMMemoryBufferRef value = nullptr;
  MemoryBufferOwner() = default;
  ~MemoryBufferOwner() {
    if (value != nullptr)
      LLVMDisposeMemoryBuffer(value);
  }
  MemoryBufferOwner(const MemoryBufferOwner &) = delete;
  MemoryBufferOwner &operator=(const MemoryBufferOwner &) = delete;
};

struct ModuleOwner {
  LLVMModuleRef value = nullptr;
  ModuleOwner() = default;
  ~ModuleOwner() {
    if (value != nullptr)
      LLVMDisposeModule(value);
  }
  ModuleOwner(const ModuleOwner &) = delete;
  ModuleOwner &operator=(const ModuleOwner &) = delete;
};

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

// AddressSanitizer's module pass instruments only functions carrying LLVM's
// sanitize_address attribute. Add that opt-in directly to parsed definitions
// so the production adapter does not rewrite or rescan textual IR. The frame
// pointer string attribute is part of Draft's selected diagnostic profile and
// remains attached even at O2.
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

// PhaseTimer writes one sequential adapter duration into the task-owned result.
// It reads the steady clock only when detailed timing was explicitly requested.
// finish is idempotent so error branches can close a phase before returning;
// the destructor protects future early exits from silently losing a measure.
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

// LLVM messages and LLVM errors use distinct disposal functions. Convert each
// immediately at its API boundary so no raw allocated message survives a
// return path or is accidentally freed by the wrong function.
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

// Target registration mutates LLVM process-global registries and must finish
// before any worker performs lookup. std::call_once gives every later worker a
// happens-before edge; after this function the registered metadata is
// read-only.
void initialize_native_llvm() {
  static std::once_flag initialized;
  std::call_once(initialized, []() {
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmParser();
    LLVMInitializeAArch64AsmPrinter();
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmParser();
    LLVMInitializeX86AsmPrinter();
  });
}

[[nodiscard]] bool select_relocation_model(TargetRelocationModel model,
                                           LLVMRelocMode &llvm_model,
                                           std::string &failure) {
  switch (model) {
  case TargetRelocationModel::PositionIndependent:
    llvm_model = LLVMRelocPIC;
    return true;
  }
  failure = "in-process LLVM does not implement target relocation model";
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
  failure = "in-process LLVM does not implement target code model";
  return false;
}

// LLVM exposes separate middle-end and target-machine optimization controls.
// Keeping their pairing in this adapter prevents a caller from accidentally
// running O2 IR passes while asking the instruction selector for O0 behavior,
// or vice versa. The pass spelling is selected below only for O2; O0 performs
// no middle-end transformation at all.
[[nodiscard]] LLVMCodeGenOptLevel
select_code_generation_level(NativeOptimizationLevel optimization) {
  switch (optimization) {
  case NativeOptimizationLevel::O0:
    return LLVMCodeGenLevelNone;
  case NativeOptimizationLevel::O2:
    return LLVMCodeGenLevelDefault;
  }
  return LLVMCodeGenLevelNone;
}

// Prefixes an LLVM-owned reason with the logical module label while keeping
// physical build paths out of diagnostics and deterministic test output.
[[nodiscard]] LlvmObjectEmissionResult
emission_failure(std::string_view module_name, std::string_view operation,
                 std::string reason) {
  LlvmObjectEmissionResult result;
  result.failure = "in-process LLVM " + std::string(operation) + " for '" +
                   std::string(module_name) + "' failed";
  if (!reason.empty())
    result.failure += ": " + reason;
  return result;
}

} // namespace

std::string_view native_optimization_level_name(NativeOptimizationLevel level) {
  switch (level) {
  case NativeOptimizationLevel::O0:
    return "O0";
  case NativeOptimizationLevel::O2:
    return "O2";
  }
  return "unknown";
}

std::string_view linked_llvm_version() { return DRAFT_LLVM_VERSION; }

std::string linked_llvm_tool_path(std::string_view tool) {
  std::string path = DRAFT_LLVM_TOOLS_DIRECTORY;
  if (!path.empty() && path.back() != '/')
    path.push_back('/');
  path.append(tool);
  return path;
}

LlvmObjectEmissionResult emit_llvm_object_in_process(
    const TargetProfile &target, std::string_view module_name,
    std::string_view llvm_ir, LlvmObjectEmissionOptions options) {
  LlvmObjectEmissionResult result;
  const auto fail = [&](std::string_view operation, std::string reason) {
    LlvmObjectEmissionResult failed =
        emission_failure(module_name, operation, std::move(reason));
    failed.phase_timings = result.phase_timings;
    return failed;
  };

  PhaseTimer input_timing(options.collect_phase_timings,
                          result.phase_timings.input_preparation_nanoseconds);
  ContextOwner context;
  context.value = LLVMContextCreate();
  if (context.value == nullptr) {
    input_timing.finish();
    return fail("context creation", {});
  }
  MemoryBufferOwner input;
  input.value = LLVMCreateMemoryBufferWithMemoryRangeCopy(
      llvm_ir.data(), llvm_ir.size(), std::string(module_name).c_str());
  input_timing.finish();
  if (input.value == nullptr) {
    return fail("IR buffer creation", {});
  }

  ModuleOwner module;
  char *parse_message = nullptr;
  PhaseTimer parsing_timing(options.collect_phase_timings,
                            result.phase_timings.ir_parsing_nanoseconds);
  const int parse_status = LLVMParseIRInContext2(context.value, input.value,
                                                 &module.value, &parse_message);
  parsing_timing.finish();
  if (parse_status != 0) {
    return fail("IR parsing", take_llvm_message(parse_message));
  }
  // LLVMParseIRInContext2 leaves input ownership with this adapter. Dispose it
  // now rather than retaining a duplicate module-sized allocation through code
  // generation.
  LLVMDisposeMemoryBuffer(input.value);
  input.value = nullptr;

  LlvmObjectEmissionResult emitted = emit_constructed_llvm_module_in_process(
      target, module_name, module.value, options);
  // Text preparation and parsing are properties of this compatibility/oracle
  // entry point. The common constructed-module operation records every phase
  // after construction, so merge the two disjoint timing prefixes without
  // weakening its direct-path measurements.
  emitted.phase_timings.input_preparation_nanoseconds =
      result.phase_timings.input_preparation_nanoseconds;
  emitted.phase_timings.ir_parsing_nanoseconds =
      result.phase_timings.ir_parsing_nanoseconds;
  return emitted;
}

LlvmObjectEmissionResult emit_constructed_llvm_module_in_process(
    const TargetProfile &target, std::string_view module_name,
    LLVMModuleRef module, LlvmObjectEmissionOptions options) {
  LlvmObjectEmissionResult result;
  const auto fail = [&](std::string_view operation, std::string reason) {
    LlvmObjectEmissionResult failed =
        emission_failure(module_name, operation, std::move(reason));
    failed.phase_timings = result.phase_timings;
    return failed;
  };

  if (module == nullptr) {
    return fail("module construction", "caller supplied a null module");
  }

  PhaseTimer initialization_timing(
      options.collect_phase_timings,
      result.phase_timings.target_initialization_nanoseconds);
  initialize_native_llvm();
  initialization_timing.finish();
  if (LLVMIsMultithreaded() == 0) {
    return fail("initialization",
                "linked LLVM was built without thread support");
  }

  LLVMRelocMode relocation_model = LLVMRelocDefault;
  LLVMCodeModel code_model = LLVMCodeModelDefault;
  std::string model_failure;
  if (!select_relocation_model(target.relocation_model, relocation_model,
                               model_failure) ||
      !select_code_model(target.code_model, code_model, model_failure)) {
    return fail("target selection", model_failure);
  }

  if (options.instrumentation == LlvmNativeInstrumentation::AddressSanitizer) {
    std::string attribute_failure;
    if (!add_address_sanitizer_attributes(LLVMGetModuleContext(module), module,
                                          attribute_failure)) {
      return fail("AddressSanitizer attribute setup", attribute_failure);
    }
  }

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
  if (verification_status != 0) {
    return fail("IR verification", take_llvm_message(verify_message));
  }
  if (verify_message != nullptr)
    LLVMDisposeMessage(verify_message);

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
      target.llvm_feature_string.c_str(),
      select_code_generation_level(options.optimization), relocation_model,
      code_model);
  if (machine.value == nullptr) {
    target_machine_timing.finish();
    return fail("target-machine creation", {});
  }
  LLVMSetTargetMachineAsmVerbosity(machine.value, 0);

  // The module and TargetProfile already agree, but LLVM's selected backend
  // could still compute a different layout after a toolchain upgrade. Reject
  // that distribution mismatch before it produces ABI-incompatible bytes.
  TargetDataOwner target_data;
  target_data.value = LLVMCreateTargetDataLayout(machine.value);
  if (target_data.value == nullptr) {
    target_machine_timing.finish();
    return fail("target data-layout creation", {});
  }
  char *target_layout_message =
      LLVMCopyStringRepOfTargetData(target_data.value);
  const std::string target_layout = take_llvm_message(target_layout_message);
  target_machine_timing.finish();
  if (target_layout != target.llvm_data_layout) {
    return fail("target validation",
                "LLVM " + std::string(linked_llvm_version()) +
                    " computes data layout '" + target_layout +
                    "' but profile requires '" + target.llvm_data_layout + "'");
  }

  // O2 is one stable compiler-owned pipeline over the complete semantic-package
  // module. Run it before instrumentation so sanitizer checks describe the
  // optimized memory operations which will actually reach code generation.
  // The complete input module was verified once above. Ordinary compilation
  // does not ask LLVM to repeat that whole-module check after every pass: that
  // diagnostic mode changes an O2 pipeline from useful validation into work
  // proportional to the number of passes. O0 intentionally creates no pass
  // manager and leaves the verified module byte-for-byte in its lowered form.
  if (options.optimization == NativeOptimizationLevel::O2 ||
      options.instrumentation == LlvmNativeInstrumentation::AddressSanitizer) {
    PassOptionsOwner pass_options;
    pass_options.value = LLVMCreatePassBuilderOptions();
    if (pass_options.value == nullptr) {
      return fail("pass option creation", {});
    }
    if (options.optimization == NativeOptimizationLevel::O2) {
      PhaseTimer optimization_timing(
          options.collect_phase_timings,
          result.phase_timings.o2_optimization_nanoseconds);
      const std::string optimization_failure = take_llvm_error(LLVMRunPasses(
          module, "default<O2>", machine.value, pass_options.value));
      optimization_timing.finish();
      if (!optimization_failure.empty()) {
        return fail("O2 optimization", optimization_failure);
      }
    }

    if (options.instrumentation ==
        LlvmNativeInstrumentation::AddressSanitizer) {
      PhaseTimer instrumentation_timing(
          options.collect_phase_timings,
          result.phase_timings.asan_instrumentation_nanoseconds);
      const std::string instrumentation_failure = take_llvm_error(
          LLVMRunPasses(module, "asan", machine.value, pass_options.value));
      instrumentation_timing.finish();
      if (!instrumentation_failure.empty()) {
        return fail("ASan instrumentation", instrumentation_failure);
      }
    }
  }

  MemoryBufferOwner output;
  char *emission_message = nullptr;
  const LLVMCodeGenFileType output_kind =
      options.output_kind == LlvmNativeOutputKind::Assembly ? LLVMAssemblyFile
                                                            : LLVMObjectFile;
  PhaseTimer emission_timing(
      options.collect_phase_timings,
      result.phase_timings.machine_code_emission_nanoseconds);
  const int emission_status = LLVMTargetMachineEmitToMemoryBuffer(
      machine.value, module, output_kind, &emission_message, &output.value);
  emission_timing.finish();
  if (emission_status != 0 || output.value == nullptr) {
    return fail(options.output_kind == LlvmNativeOutputKind::Assembly
                    ? "assembly emission"
                    : "object emission",
                take_llvm_message(emission_message));
  }
  if (emission_message != nullptr)
    LLVMDisposeMessage(emission_message);

  PhaseTimer output_copy_timing(options.collect_phase_timings,
                                result.phase_timings.output_copy_nanoseconds);
  result.ok = true;
  result.bytes.assign(LLVMGetBufferStart(output.value),
                      LLVMGetBufferSize(output.value));
  output_copy_timing.finish();
  return result;
}

} // namespace draft
