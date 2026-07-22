// In-process LLVM-C adapter for deterministic native emission.
//
// See llvm_object_emitter.h for the phase, ownership, and semantic firewall.
// This implementation deliberately uses small local RAII owners around opaque
// C handles so every error path releases LLVM resources without introducing a
// compiler-wide wrapper framework.

#include "backend/llvm_object_emitter.h"

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>

#include <mutex>
#include <string>
#include <string_view>

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
    if (value != nullptr) LLVMContextDispose(value);
  }
  ContextOwner(const ContextOwner &) = delete;
  ContextOwner &operator=(const ContextOwner &) = delete;
};

struct MemoryBufferOwner {
  LLVMMemoryBufferRef value = nullptr;
  MemoryBufferOwner() = default;
  ~MemoryBufferOwner() {
    if (value != nullptr) LLVMDisposeMemoryBuffer(value);
  }
  MemoryBufferOwner(const MemoryBufferOwner &) = delete;
  MemoryBufferOwner &operator=(const MemoryBufferOwner &) = delete;
};

struct ModuleOwner {
  LLVMModuleRef value = nullptr;
  ModuleOwner() = default;
  ~ModuleOwner() {
    if (value != nullptr) LLVMDisposeModule(value);
  }
  ModuleOwner(const ModuleOwner &) = delete;
  ModuleOwner &operator=(const ModuleOwner &) = delete;
};

struct TargetMachineOwner {
  LLVMTargetMachineRef value = nullptr;
  TargetMachineOwner() = default;
  ~TargetMachineOwner() {
    if (value != nullptr) LLVMDisposeTargetMachine(value);
  }
  TargetMachineOwner(const TargetMachineOwner &) = delete;
  TargetMachineOwner &operator=(const TargetMachineOwner &) = delete;
};

struct TargetDataOwner {
  LLVMTargetDataRef value = nullptr;
  TargetDataOwner() = default;
  ~TargetDataOwner() {
    if (value != nullptr) LLVMDisposeTargetData(value);
  }
  TargetDataOwner(const TargetDataOwner &) = delete;
  TargetDataOwner &operator=(const TargetDataOwner &) = delete;
};

struct PassOptionsOwner {
  LLVMPassBuilderOptionsRef value = nullptr;
  PassOptionsOwner() = default;
  ~PassOptionsOwner() {
    if (value != nullptr) LLVMDisposePassBuilderOptions(value);
  }
  PassOptionsOwner(const PassOptionsOwner &) = delete;
  PassOptionsOwner &operator=(const PassOptionsOwner &) = delete;
};

// LLVM messages and LLVM errors use distinct disposal functions. Convert each
// immediately at its API boundary so no raw allocated message survives a
// return path or is accidentally freed by the wrong function.
[[nodiscard]] std::string take_llvm_message(char *message) {
  if (message == nullptr) return {};
  std::string result(message);
  LLVMDisposeMessage(message);
  return result;
}

[[nodiscard]] std::string take_llvm_error(LLVMErrorRef error) {
  if (error == LLVMErrorSuccess) return {};
  char *message = LLVMGetErrorMessage(error);
  if (message == nullptr) return "unknown LLVM pass error";
  std::string result(message);
  LLVMDisposeErrorMessage(message);
  return result;
}

// Target registration mutates LLVM process-global registries and must finish
// before any worker performs lookup. std::call_once gives every later worker a
// happens-before edge; after this function the registered metadata is read-only.
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

[[nodiscard]] bool select_relocation_model(
    TargetRelocationModel model,
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

[[nodiscard]] bool select_code_model(
    TargetCodeModel model,
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
[[nodiscard]] LLVMCodeGenOptLevel select_code_generation_level(
    NativeOptimizationLevel optimization) {
  switch (optimization) {
  case NativeOptimizationLevel::O0: return LLVMCodeGenLevelNone;
  case NativeOptimizationLevel::O2: return LLVMCodeGenLevelDefault;
  }
  return LLVMCodeGenLevelNone;
}

// Prefixes an LLVM-owned reason with the logical module label while keeping
// physical build paths out of diagnostics and deterministic test output.
[[nodiscard]] LlvmObjectEmissionResult emission_failure(
    std::string_view module_name,
    std::string_view operation,
    std::string reason) {
  LlvmObjectEmissionResult result;
  result.failure = "in-process LLVM " + std::string(operation) + " for '" +
      std::string(module_name) + "' failed";
  if (!reason.empty()) result.failure += ": " + reason;
  return result;
}

} // namespace

std::string_view native_optimization_level_name(
    NativeOptimizationLevel level) {
  switch (level) {
  case NativeOptimizationLevel::O0: return "O0";
  case NativeOptimizationLevel::O2: return "O2";
  }
  return "unknown";
}

std::string_view linked_llvm_version() {
  return DRAFT_LLVM_VERSION;
}

std::string linked_llvm_tool_path(std::string_view tool) {
  std::string path = DRAFT_LLVM_TOOLS_DIRECTORY;
  if (!path.empty() && path.back() != '/') path.push_back('/');
  path.append(tool);
  return path;
}

LlvmObjectEmissionResult emit_llvm_object_in_process(
    const TargetProfile &target,
    std::string_view module_name,
    std::string_view llvm_ir,
    LlvmObjectEmissionOptions options) {
  initialize_native_llvm();
  if (LLVMIsMultithreaded() == 0) {
    return emission_failure(
        module_name,
        "initialization",
        "linked LLVM was built without thread support");
  }

  LLVMRelocMode relocation_model = LLVMRelocDefault;
  LLVMCodeModel code_model = LLVMCodeModelDefault;
  std::string model_failure;
  if (!select_relocation_model(
          target.relocation_model, relocation_model, model_failure) ||
      !select_code_model(target.code_model, code_model, model_failure)) {
    return emission_failure(module_name, "target selection", model_failure);
  }

  ContextOwner context;
  context.value = LLVMContextCreate();
  if (context.value == nullptr) {
    return emission_failure(module_name, "context creation", {});
  }
  MemoryBufferOwner input;
  input.value = LLVMCreateMemoryBufferWithMemoryRangeCopy(
      llvm_ir.data(),
      llvm_ir.size(),
      std::string(module_name).c_str());
  if (input.value == nullptr) {
    return emission_failure(module_name, "IR buffer creation", {});
  }

  ModuleOwner module;
  char *parse_message = nullptr;
  if (LLVMParseIRInContext2(
          context.value, input.value, &module.value, &parse_message) != 0) {
    return emission_failure(
        module_name, "IR parsing", take_llvm_message(parse_message));
  }
  // LLVMParseIRInContext2 leaves input ownership with this adapter. Dispose it
  // now rather than retaining a duplicate module-sized allocation through code
  // generation.
  LLVMDisposeMemoryBuffer(input.value);
  input.value = nullptr;

  const std::string module_triple = LLVMGetTarget(module.value);
  if (module_triple != target.llvm_triple) {
    return emission_failure(
        module_name,
        "target validation",
        "module triple '" + module_triple + "' does not match profile '" +
            target.llvm_triple + "'");
  }
  const std::string module_layout = LLVMGetDataLayoutStr(module.value);
  if (module_layout != target.llvm_data_layout) {
    return emission_failure(
        module_name,
        "target validation",
        "module data layout does not match profile '" +
            target.facts.identity + "'");
  }

  char *verify_message = nullptr;
  if (LLVMVerifyModule(
          module.value, LLVMReturnStatusAction, &verify_message) != 0) {
    return emission_failure(
        module_name, "IR verification", take_llvm_message(verify_message));
  }
  if (verify_message != nullptr) LLVMDisposeMessage(verify_message);

  LLVMTargetRef llvm_target = nullptr;
  char *target_message = nullptr;
  if (LLVMGetTargetFromTriple(
          target.llvm_triple.c_str(), &llvm_target, &target_message) != 0 ||
      llvm_target == nullptr) {
    return emission_failure(
        module_name,
        "target lookup",
        take_llvm_message(target_message));
  }
  if (target_message != nullptr) LLVMDisposeMessage(target_message);

  TargetMachineOwner machine;
  machine.value = LLVMCreateTargetMachine(
      llvm_target,
      target.llvm_triple.c_str(),
      target.llvm_cpu.c_str(),
      target.llvm_feature_string.c_str(),
      select_code_generation_level(options.optimization),
      relocation_model,
      code_model);
  if (machine.value == nullptr) {
    return emission_failure(module_name, "target-machine creation", {});
  }
  LLVMSetTargetMachineAsmVerbosity(machine.value, 0);

  // The module and TargetProfile already agree, but LLVM's selected backend
  // could still compute a different layout after a toolchain upgrade. Reject
  // that distribution mismatch before it produces ABI-incompatible bytes.
  TargetDataOwner target_data;
  target_data.value = LLVMCreateTargetDataLayout(machine.value);
  if (target_data.value == nullptr) {
    return emission_failure(module_name, "target data-layout creation", {});
  }
  char *target_layout_message =
      LLVMCopyStringRepOfTargetData(target_data.value);
  const std::string target_layout = take_llvm_message(target_layout_message);
  if (target_layout != target.llvm_data_layout) {
    return emission_failure(
        module_name,
        "target validation",
        "LLVM " + std::string(linked_llvm_version()) +
            " computes data layout '" + target_layout +
            "' but profile requires '" + target.llvm_data_layout + "'");
  }

  // O2 is one stable compiler-owned pipeline over the complete semantic-package
  // module. Run it before instrumentation so sanitizer checks describe the
  // optimized memory operations which will actually reach code generation.
  // O0 intentionally creates no pass manager and leaves the verified module
  // byte-for-byte in its lowered form.
  if (options.optimization == NativeOptimizationLevel::O2 ||
      options.instrumentation == LlvmNativeInstrumentation::AddressSanitizer) {
    PassOptionsOwner pass_options;
    pass_options.value = LLVMCreatePassBuilderOptions();
    if (pass_options.value == nullptr) {
      return emission_failure(module_name, "pass option creation", {});
    }
    LLVMPassBuilderOptionsSetVerifyEach(pass_options.value, 1);

    if (options.optimization == NativeOptimizationLevel::O2) {
      const std::string optimization_failure = take_llvm_error(LLVMRunPasses(
          module.value,
          "default<O2>",
          machine.value,
          pass_options.value));
      if (!optimization_failure.empty()) {
        return emission_failure(
            module_name, "O2 optimization", optimization_failure);
      }
    }

    if (options.instrumentation ==
        LlvmNativeInstrumentation::AddressSanitizer) {
      const std::string instrumentation_failure = take_llvm_error(LLVMRunPasses(
          module.value,
          "asan",
          machine.value,
          pass_options.value));
      if (!instrumentation_failure.empty()) {
        return emission_failure(
            module_name,
            "ASan instrumentation",
            instrumentation_failure);
      }
    }
  }

  MemoryBufferOwner output;
  char *emission_message = nullptr;
  const LLVMCodeGenFileType output_kind =
      options.output_kind == LlvmNativeOutputKind::Assembly
      ? LLVMAssemblyFile
      : LLVMObjectFile;
  if (LLVMTargetMachineEmitToMemoryBuffer(
          machine.value,
          module.value,
          output_kind,
          &emission_message,
          &output.value) != 0 ||
      output.value == nullptr) {
    return emission_failure(
        module_name,
        options.output_kind == LlvmNativeOutputKind::Assembly
            ? "assembly emission"
            : "object emission",
        take_llvm_message(emission_message));
  }
  if (emission_message != nullptr) LLVMDisposeMessage(emission_message);

  LlvmObjectEmissionResult result;
  result.ok = true;
  result.bytes.assign(
      LLVMGetBufferStart(output.value), LLVMGetBufferSize(output.value));
  return result;
}

} // namespace draft
