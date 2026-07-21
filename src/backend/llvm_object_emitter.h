// In-process LLVM object and assembly emission for one lowered Draft unit.
//
// This module is the only bootstrap layer that links LLVM's native target
// library. It accepts one complete textual LLVM module--either package-static
// data or one machine function--plus a complete Draft target profile, parses
// and verifies the module in a fresh LLVM context, checks LLVM's target-machine
// data layout against the profile, and returns emitted bytes in memory. It
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

// Selects a compiler-owned LLVM transformation bundle before emission. It is
// deliberately closed: callers cannot smuggle arbitrary pass pipelines across
// the semantic/backend boundary.
enum class LlvmNativeInstrumentation {
  None,
  AddressSanitizer,
};

// Options contain only choices already authorized by a compiler command. They
// are not ambient LLVM flags. Object emission uses LLVM's no-optimization code
// generation level to match Draft's current unoptimized bootstrap contract;
// language-level release assertions remain a separate MIR configuration.
struct LlvmObjectEmissionOptions {
  LlvmNativeOutputKind output_kind = LlvmNativeOutputKind::Object;
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
