// In-process LLVM object and assembly emission for one Draft package-owned unit.
//
// This module is the only bootstrap layer that links LLVM's native target
// library. Its textual oracle accepts one complete LLVM module for one semantic
// package; its constructed-module seam may receive a deterministic O0 subset.
// Both receive a complete Draft target profile, verify the module in a
// fresh LLVM context, checks LLVM's target-machine data layout against the
// profile, and returns emitted bytes in memory. It
// performs no filesystem I/O, process launch, package scheduling, linking, or
// diagnostic publication.
//
// Every invocation owns an isolated LLVM context, module, target machine, and
// output buffer. Calls may therefore execute concurrently as long as the linked
// LLVM distribution reports thread support. LLVM's native target registries
// are process global and initialized exactly once; they contain immutable
// target metadata after initialization. The adapter uses LLVM's C API to keep
// version-sensitive C++ implementation types out of Draft headers and the rest
// of the compiler.
//
// LLVM may choose instruction encodings, register allocation, and object-file
// details only within the ABI, layout, feature, relocation, and code-model
// facts fixed by TargetProfile. A disagreement is a compiler/toolchain error,
// never a reason to reinterpret Draft source. Relevant specification:
// docs/specification/04-native-interop.md sections 11-12 and
// docs/specification/06-compiler.md "Native lowering and summaries".

#pragma once

#include "target/profile.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace draft {

// Selects which target-machine buffer the adapter returns. This changes only
// artifact representation; both paths consume the same verified module and
// target facts. Enum order is incidental and is never serialized.
enum class LlvmNativeOutputKind {
  Object,
  Assembly,
};

// NativeOptimizationLevel selects one complete compiler-owned LLVM pipeline.
// O0 preserves the canonical lowered module and asks the target machine for its
// fastest no-optimization code generation. Production O2 uses package pre-link
// pipelines plus one workspace ThinLTO operation and selects LLVM's matching
// default code-generation level. The enum is deliberately closed: arbitrary
// pass strings and the accidental optimization menus of individual LLVM
// releases are not part of Draft's command contract.
//
// Optimization changes derived native bytes only. It does not enter source,
// target, resolved-program, synthesis, or semantic-product identity. O2 keeps
// one input module per semantic package and one whole-artifact ThinLTO
// decision; native-only O0 may use deterministic internal units so target-
// machine work can run concurrently.
enum class NativeOptimizationLevel {
  O0,
  O2,
};

// Returns the exact public command spelling without the leading dash. The
// result is process-lifetime static storage and is also suitable for derived-
// artifact and validation-policy identities; callers must not treat it as a
// semantic program or target fact.
[[nodiscard]] std::string_view native_optimization_level_name(
    NativeOptimizationLevel level);

// Selects a compiler-owned LLVM transformation bundle before emission. It is
// deliberately closed: callers cannot smuggle arbitrary pass pipelines across
// the semantic/backend boundary.
enum class LlvmNativeInstrumentation {
  None,
  AddressSanitizer,
};

// Options contain only choices already authorized by a compiler command. They
// are not ambient LLVM flags. Optimization transforms the supplied LLVM unit
// immediately before object or assembly emission; language-level assertion
// removal remains a separate MIR configuration.
struct LlvmObjectEmissionOptions {
  LlvmNativeOutputKind output_kind = LlvmNativeOutputKind::Object;
  NativeOptimizationLevel optimization = NativeOptimizationLevel::O0;
  LlvmNativeInstrumentation instrumentation =
      LlvmNativeInstrumentation::None;
  // Phase timing performs steady-clock reads only for --timings=all. The
  // resulting observations travel back through the task-owned result and are
  // replayed by the command thread; the worker never touches TimingRecorder.
  bool collect_phase_timings = false;
};

// Wall durations for the sequential operations inside one isolated LLVM
// package-unit task. Zero means the diagnostic measurement was disabled, the
// operation was not reached, or a tiny operation completed below one clock
// tick. These values never enter artifact identity or influence LLVM behavior;
// they exist solely so the native coordinator can reconstruct nested
// --timings=all rows after parallel workers join.
struct LlvmObjectEmissionPhaseTimings {
  std::uint64_t target_initialization_nanoseconds = 0;
  std::uint64_t input_preparation_nanoseconds = 0;
  std::uint64_t ir_parsing_nanoseconds = 0;
  std::uint64_t target_validation_nanoseconds = 0;
  std::uint64_t ir_verification_nanoseconds = 0;
  std::uint64_t target_machine_nanoseconds = 0;
  std::uint64_t o2_optimization_nanoseconds = 0;
  std::uint64_t asan_instrumentation_nanoseconds = 0;
  std::uint64_t machine_code_emission_nanoseconds = 0;
  std::uint64_t output_copy_nanoseconds = 0;
};

// bytes contains exactly one object file or one textual assembly file on
// success. failure is empty on success and owns a stable adapter-prefixed reason
// on failure. phase_timings contains only diagnostic wall durations requested
// by the caller. No LLVM reference or pointer escapes this value.
struct LlvmObjectEmissionResult {
  bool ok = false;
  std::string bytes;
  std::string failure;
  LlvmObjectEmissionPhaseTimings phase_timings;
};

// Returns the LLVM distribution version linked into draftc. This is build
// evidence, not a runtime probe, semantic target fact, resolved-program input,
// or cache key.
[[nodiscard]] std::string_view linked_llvm_version();

// Returns one executable path from the exact LLVM distribution shipped beside
// the running compiler, falling back to the CMake-selected development tree
// only when no installed tool exists. The caller supplies a basename such as
// "clang" or "dsymutil". This is operational compiler configuration and may be
// overridden by an embedding caller; it never enters Draft program identity.
[[nodiscard]] std::string linked_llvm_tool_path(std::string_view tool);

// Reports whether the running process has the installed sibling tool layout.
// Version and release-smoke commands use this to distinguish a relocatable
// archive from a source-tree build without exposing a host path as identity.
[[nodiscard]] bool linked_llvm_tools_are_distributed();

// Emits one already-lowered module synchronously. module_name is a logical
// diagnostic label only and must not contain a physical checkout path. The
// input bytes and target are borrowed for the call and never retained.
[[nodiscard]] LlvmObjectEmissionResult emit_llvm_object_in_process(
    const TargetProfile &target,
    std::string_view module_name,
    std::string_view llvm_ir,
    LlvmObjectEmissionOptions options);

} // namespace draft
