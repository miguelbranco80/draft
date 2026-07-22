// In-process LLVM emission contracts for all implemented target profiles.
//
// The tests use minimal valid modules to isolate parsing, profile verification,
// deterministic object bytes, assembly output, and concurrent context
// ownership. Native integration tests later exercise the compiler's complete
// generated IR, linker contracts, runtime behavior, debug information, and
// sanitizers.

#include "backend/llvm_object_emitter.h"
#include "base/work_graph.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct TestState {
  int failures = 0;
};

#define EXPECT(state, condition)                                                \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << __FILE__ << ':' << __LINE__                                  \
                << ": expectation failed: " #condition << '\n';                \
      ++(state).failures;                                                       \
    }                                                                           \
  } while (false)

// Builds the smallest object-producing module while retaining the exact target
// triple and layout as inputs. The exported function avoids platform-specific
// leading-underscore assumptions in the test itself.
std::string minimal_module(const draft::TargetProfile &target) {
  return
      "target triple = \"" + target.llvm_triple + "\"\n"
      "target datalayout = \"" + target.llvm_data_layout + "\"\n"
      "define i32 @draft_emitter_value() {\n"
      "entry:\n"
      "  ret i32 42\n"
      "}\n";
}

// Object magic is an independent, cheap assertion that the requested target
// format reached LLVM instead of merely returning nonempty diagnostic text.
void test_target_object_and_assembly(TestState &state) {
  for (const draft::TargetProfile &target : {
           draft::make_aarch64_macos_profile(),
           draft::make_aarch64_linux_profile(),
           draft::make_x86_64_linux_profile()}) {
    const std::string module = minimal_module(target);
    const draft::LlvmObjectEmissionResult first =
        draft::emit_llvm_object_in_process(target, "minimal", module, {});
    const draft::LlvmObjectEmissionResult second =
        draft::emit_llvm_object_in_process(target, "minimal", module, {});
    EXPECT(state, first.ok);
    EXPECT(state, first.failure.empty());
    EXPECT(state, first.bytes == second.bytes);
    EXPECT(state, first.bytes.size() >= 4);
    if (target.facts.object_format == "macho" && first.bytes.size() >= 4) {
      EXPECT(state, static_cast<unsigned char>(first.bytes[0]) == 0xcfU);
      EXPECT(state, static_cast<unsigned char>(first.bytes[1]) == 0xfaU);
      EXPECT(state, static_cast<unsigned char>(first.bytes[2]) == 0xedU);
      EXPECT(state, static_cast<unsigned char>(first.bytes[3]) == 0xfeU);
    }
    if (target.facts.object_format == "elf" && first.bytes.size() >= 4) {
      EXPECT(state, static_cast<unsigned char>(first.bytes[0]) == 0x7fU);
      EXPECT(state, first.bytes[1] == 'E');
      EXPECT(state, first.bytes[2] == 'L');
      EXPECT(state, first.bytes[3] == 'F');
    }

    draft::LlvmObjectEmissionOptions assembly_options;
    assembly_options.output_kind = draft::LlvmNativeOutputKind::Assembly;
    const draft::LlvmObjectEmissionResult assembly =
        draft::emit_llvm_object_in_process(
            target, "minimal", module, assembly_options);
    EXPECT(state, assembly.ok);
    EXPECT(state, assembly.bytes.find("draft_emitter_value") !=
        std::string::npos);
  }
}

// O2 must be a real module pipeline, not only a more aggressive instruction
// selector setting. An unreferenced internal definition is intentionally
// observable in O0 assembly and must disappear under LLVM's default O2 global
// cleanup. Repeating O2 also proves the selected pass pipeline remains
// deterministic for both supported object formats.
void test_o0_and_o2_pipelines(TestState &state) {
  for (const draft::TargetProfile &target : {
           draft::make_aarch64_macos_profile(),
           draft::make_aarch64_linux_profile(),
           draft::make_x86_64_linux_profile()}) {
    const std::string module =
        "target triple = \"" + target.llvm_triple + "\"\n"
        "target datalayout = \"" + target.llvm_data_layout + "\"\n"
        "define i64 @draft_optimized_value(i64 %input) {\n"
        "entry:\n"
        "  %slot = alloca i64, align 8\n"
        "  store i64 %input, ptr %slot, align 8\n"
        "  %loaded = load i64, ptr %slot, align 8\n"
        "  %same = add i64 %loaded, 0\n"
        "  ret i64 %same\n"
        "}\n"
        "define internal i64 @draft_unused_value() {\n"
        "entry:\n"
        "  ret i64 99\n"
        "}\n";

    draft::LlvmObjectEmissionOptions o0;
    o0.output_kind = draft::LlvmNativeOutputKind::Assembly;
    const draft::LlvmObjectEmissionResult unoptimized =
        draft::emit_llvm_object_in_process(target, "o0", module, o0);
    EXPECT(state, unoptimized.ok);
    EXPECT(state, unoptimized.bytes.find("draft_optimized_value") !=
        std::string::npos);
    EXPECT(state, unoptimized.bytes.find("draft_unused_value") !=
        std::string::npos);

    draft::LlvmObjectEmissionOptions o2 = o0;
    o2.optimization = draft::NativeOptimizationLevel::O2;
    const draft::LlvmObjectEmissionResult optimized =
        draft::emit_llvm_object_in_process(target, "o2", module, o2);
    const draft::LlvmObjectEmissionResult optimized_again =
        draft::emit_llvm_object_in_process(target, "o2", module, o2);
    EXPECT(state, optimized.ok);
    EXPECT(state, optimized.failure.empty());
    EXPECT(state, optimized.bytes == optimized_again.bytes);
    EXPECT(state, optimized.bytes != unoptimized.bytes);
    EXPECT(state, optimized.bytes.find("draft_optimized_value") !=
        std::string::npos);
    EXPECT(state, optimized.bytes.find("draft_unused_value") ==
        std::string::npos);
  }
}

// Parse and profile failures must remain explicit result values. Invalid input
// is treated as a compiler/backend boundary error and never reaches an LLVM
// assertion or host process.
void test_invalid_ir_and_target_mismatch(TestState &state) {
  const draft::TargetProfile macos = draft::make_aarch64_macos_profile();
  const draft::LlvmObjectEmissionResult invalid =
      draft::emit_llvm_object_in_process(
          macos, "broken", "this is not LLVM IR\n", {});
  EXPECT(state, !invalid.ok);
  EXPECT(state, invalid.failure.find("IR parsing") != std::string::npos);
  EXPECT(state, invalid.failure.find("broken") != std::string::npos);

  const draft::TargetProfile linux = draft::make_aarch64_linux_profile();
  const draft::LlvmObjectEmissionResult mismatch =
      draft::emit_llvm_object_in_process(
          linux, "mac-module", minimal_module(macos), {});
  EXPECT(state, !mismatch.ok);
  EXPECT(state, mismatch.failure.find("module triple") != std::string::npos);
  EXPECT(state, mismatch.failure.find(macos.llvm_triple) != std::string::npos);
}

// AddressSanitizer is a compiler-owned native validation profile. The in-process
// adapter must run LLVM's instrumentation pipeline, not merely copy the function
// attribute into an otherwise ordinary object. The undefined report symbol in
// the object string table is direct evidence that a checked load was inserted.
void test_address_sanitizer_pipeline(TestState &state) {
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  const std::string module =
      "target triple = \"" + target.llvm_triple + "\"\n"
      "target datalayout = \"" + target.llvm_data_layout + "\"\n"
      "define i64 @draft_asan_load(ptr %address) sanitize_address {\n"
      "entry:\n"
      "  %value = load i64, ptr %address, align 8\n"
      "  ret i64 %value\n"
      "}\n";
  draft::LlvmObjectEmissionOptions options;
  options.instrumentation = draft::LlvmNativeInstrumentation::AddressSanitizer;
  const draft::LlvmObjectEmissionResult emitted =
      draft::emit_llvm_object_in_process(target, "asan-load", module, options);
  EXPECT(state, emitted.ok);
  EXPECT(state, emitted.failure.empty());
  EXPECT(state, emitted.bytes.find("asan_report_load8") != std::string::npos);

  // Optimization precedes instrumentation, so the combined pipeline must
  // retain the checked load and instrument the final operation as well.
  options.optimization = draft::NativeOptimizationLevel::O2;
  const draft::LlvmObjectEmissionResult optimized =
      draft::emit_llvm_object_in_process(
          target, "asan-load-o2", module, options);
  EXPECT(state, optimized.ok);
  EXPECT(state, optimized.failure.empty());
  EXPECT(state, optimized.bytes.find("asan_report_load8") != std::string::npos);
}

struct ParallelEmissionContext {
  const draft::TargetProfile *target = nullptr;
  std::string module;
  std::vector<draft::LlvmObjectEmissionResult> results;
};

// The work-graph operation writes only its stable result slot. A fresh LLVM
// context and target machine per call must make four simultaneous emissions
// safe and byte-identical.
bool emit_parallel_module(
    void *opaque,
    draft::WorkTaskId task,
    std::string &failure) {
  auto &context = *static_cast<ParallelEmissionContext *>(opaque);
  draft::LlvmObjectEmissionOptions options;
  options.optimization = draft::NativeOptimizationLevel::O2;
  context.results[task] = draft::emit_llvm_object_in_process(
      *context.target,
      "parallel-module",
      context.module,
      options);
  if (!context.results[task].ok) {
    failure = context.results[task].failure;
    return false;
  }
  return true;
}

// This is a focused O2 thread-safety qualification, not the native scheduler
// integration itself. It proves separate pass managers, contexts, and target
// machines obey the scheduler operation contract before the toolchain uses
// them for concurrent semantic-package modules.
void test_parallel_emission_isolated_contexts(TestState &state) {
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  ParallelEmissionContext context;
  context.target = &target;
  context.module = minimal_module(target);
  context.results.resize(4);
  draft::WorkGraph graph;
  graph.tasks.resize(context.results.size());
  draft::WorkGraphRunOptions options;
  options.worker_count = context.results.size();
  const draft::WorkGraphRunResult run = draft::run_work_graph(
      graph, options, emit_parallel_module, &context);
  EXPECT(state, run.ok);
  for (std::size_t index = 1; index < context.results.size(); ++index) {
    EXPECT(state, context.results[index].bytes == context.results[0].bytes);
  }
}

} // namespace

int main() {
  TestState state;
  EXPECT(state, !draft::linked_llvm_version().empty());
  test_target_object_and_assembly(state);
  test_o0_and_o2_pipelines(state);
  test_invalid_ir_and_target_mismatch(state);
  test_address_sanitizer_pipeline(state);
  test_parallel_emission_isolated_contexts(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " LLVM object emitter expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all LLVM object emitter tests passed\n";
  return EXIT_SUCCESS;
}
