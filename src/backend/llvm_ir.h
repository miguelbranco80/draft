// Deterministic textual LLVM IR emission from verified Draft MIR.

#pragma once

#include "backend/source_correlation.h"
#include "mir/mir.h"
#include "sema/analyzer.h"
#include "sema/constant.h"
#include "source/diagnostic.h"
#include "target/profile.h"
#include "validation/discovery.h"
#include "workspace/workspace.h"

#include <string>
#include <vector>

namespace draft {

struct LlvmIrOptions {
  PackageIdentity package;
  // One module in every final artifact owns the compiler runtime definitions.
  // Executables additionally own the hosted C `main`; libraries do not.
  bool emit_runtime_support = false;
  bool emit_program_entry = false;
  // A validation executable has a compiler-owned entry point. The entries are
  // already signature- and layout-checked; LLVM emission only materializes
  // isolated state and calls their stable package symbols in canonical order.
  ValidationKind validation_kind = ValidationKind::None;
  std::vector<ValidationEntry> validation_entries;
};

struct LlvmIrResult {
  bool ok = false;
  std::string text;
  // These rows are collected at the same point as the LLVM debug markers, so
  // the sidecar and instruction metadata cannot silently drift apart.
  std::vector<SourceCorrelationEntry> source_correlations;
};

// The emitter produces opaque-pointer LLVM IR and never invokes a toolchain.
// Object emission and linking are separate adapters that must verify the pinned
// LLVM distribution. The textual form is intentionally testable without LLVM
// headers or libraries on the bootstrap compiler's build machine.
[[nodiscard]] LlvmIrResult emit_llvm_ir(
    const TargetProfile &target,
    const SourceManager &sources,
    const LlvmIrOptions &options,
    const SemanticPackage &semantic,
    const ConstantTable &global_initializers,
    const MirProgram &mir,
    DiagnosticSink &diagnostics);

} // namespace draft
