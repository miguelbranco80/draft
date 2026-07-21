// In-process LLVM object and assembly emission for one Draft package module.
//
// This module is the only bootstrap layer that links LLVM's native target
// library. It accepts one complete textual LLVM module for one semantic package
// plus a complete Draft target profile, parses and verifies the module in a
// fresh LLVM context, checks LLVM's target-machine data layout against the
// profile, and returns emitted bytes in memory. It
// performs no filesystem I/O, process launch, package scheduling, linking, or
// diagnostic publication.
//
// Every invocation owns an isolated LLVM context, module, target machine, and
// output buffer. Calls may therefore execute concurrently as long as the linked
// LLVM distribution reports thread support. LLVM's AArch64 registry is process
// global and initialized exactly once; it contains immutable target metadata
// after initialization. The adapter uses LLVM's C API to keep version-sensitive
// C++ implementation types out of Draft headers and the rest of the compiler.
//
// LLVM may choose instruction encodings, register allocation, and object-file
// details only within the ABI, layout, feature, relocation, and code-model facts
// fixed by TargetProfile. A disagreement is a compiler/toolchain error, never a
// reason to reinterpret Draft source.
// Relevant specification: docs/specification/04-native-interop.md sections
// 11-12 and docs/specification/06-compiler.md "Native lowering and summaries".

#pragma once

#include "target/profile.h"

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
// fastest no-optimization code generation. O2 runs LLVM's default O2 module
// pipeline and selects its matching default code-generation level. The enum is
// deliberately closed: arbitrary pass strings and the accidental optimization
// menus of individual LLVM releases are not part of Draft's command contract.
//
// Optimization changes derived native bytes only. It does not enter source,
// target, resolved-program, synthesis, or semantic-product identity, and it
// cannot change package-module granularity.
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
// are not ambient LLVM flags. Optimization transforms a complete package module
// immediately before object or assembly emission; language-level assertion
// removal remains a separate MIR configuration.
struct LlvmObjectEmissionOptions {
  LlvmNativeOutputKind output_kind = LlvmNativeOutputKind::Object;
  NativeOptimizationLevel optimization = NativeOptimizationLevel::O0;
  LlvmNativeInstrumentation instrumentation =
      LlvmNativeInstrumentation::None;
};

// bytes contains exactly one object file or one textual assembly file on
// success. failure is empty on success and owns a stable adapter-prefixed reason
// on failure. No LLVM reference or pointer escapes this value.
struct LlvmObjectEmissionResult {
  bool ok = false;
  std::string bytes;
  std::string failure;
};

// Returns the LLVM distribution version linked into draftc. This is build
// evidence, not a runtime probe, semantic target fact, resolved-program input,
// or cache key.
[[nodiscard]] std::string_view linked_llvm_version();

// Returns one executable path from the exact LLVM distribution selected by
// CMake. The caller supplies a basename such as "clang" or "dsymutil". This is
// operational compiler configuration and may be overridden by an embedding
// caller; it never enters Draft program identity.
[[nodiscard]] std::string linked_llvm_tool_path(std::string_view tool);

// Emits one already-lowered module synchronously. module_name is a logical
// diagnostic label only and must not contain a physical checkout path. The
// input bytes and target are borrowed for the call and never retained.
[[nodiscard]] LlvmObjectEmissionResult emit_llvm_object_in_process(
    const TargetProfile &target,
    std::string_view module_name,
    std::string_view llvm_ir,
    LlvmObjectEmissionOptions options);

} // namespace draft
