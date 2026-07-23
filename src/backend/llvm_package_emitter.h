// Direct in-process construction and emission of one Draft package LLVM unit.
//
// This is the native backend's package-owned emission boundary. It consumes
// immutable artifact-live semantic products plus an ordered MIR subset,
// constructs a fresh LLVM module
// through LLVM's API, optionally prints that module for an explicit `emit-llvm`
// request, and optionally continues the same module into object or assembly
// bytes. Context, module, builder, and every LLVM value remain task-local and
// are destroyed before the operation returns.
//
// The value-only interface intentionally contains no LLVM handles. Semantic
// code therefore cannot depend on LLVM representation details, while package
// tasks can run concurrently in isolated contexts. Relevant specification:
// docs/specification/06-compiler.md "Native lowering and summaries".

#pragma once

#include "backend/llvm_package.h"
#include "backend/llvm_object_emitter.h"
#include "interop/c_abi.h"
#include "mir/mir.h"
#include "sema/analyzer.h"
#include "sema/constant.h"
#include "source/diagnostic.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace draft {

// One package-unit task's operational requests. retain_llvm_text is an inspection
// choice and native_options selects a derived artifact; neither changes Draft
// semantics or package identity. O2 and retained-text callers supply the whole
// package; native-only O0 callers may supply one deterministic subset. When
// both outputs are absent the builder still
// constructs and verifies its direct module, which is useful for focused
// backend tests.
struct LlvmPackageEmissionOptions {
  LlvmIrOptions module;
  bool retain_llvm_text = false;
  std::optional<LlvmObjectEmissionOptions> native_options;
  bool collect_phase_timings = false;
};

// Direct construction timing is separate from the target-machine subphases in
// LlvmObjectEmissionResult. Zero means timing was not requested, construction
// was not reached, or the measured operation fit below one clock tick.
struct LlvmPackageEmissionPhaseTimings {
  std::uint64_t module_construction_nanoseconds = 0;
  std::uint64_t llvm_text_printing_nanoseconds = 0;
};

// llvm_text owns the pre-optimization module only when explicitly retained.
// native owns the exact object/assembly result when requested. Backend source
// errors are published into the supplied DiagnosticSink; native.adapter
// failures remain in native.failure for the coordinator to diagnose once.
struct LlvmPackageEmissionResult {
  bool ok = false;
  std::string llvm_text;
  LlvmObjectEmissionResult native;
  LlvmPackageEmissionPhaseTimings phase_timings;
};

[[nodiscard]] LlvmPackageEmissionResult emit_llvm_package_direct(
    const TargetProfile &target, const SourceManager &sources,
    const LlvmPackageEmissionOptions &options, const SemanticPackage &semantic,
    const CAbiTable &abi, const ConstantTable &global_initializers,
    std::span<const SymbolId> globals,
    std::span<const MirProcedure *const> procedures,
    DiagnosticSink &diagnostics);

} // namespace draft
