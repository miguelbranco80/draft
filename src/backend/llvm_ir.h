// Deterministic textual LLVM IR emission from verified Draft MIR.

#pragma once

#include "backend/source_correlation.h"
#include "interop/aarch64_abi.h"
#include "mir/mir.h"
#include "sema/analyzer.h"
#include "sema/constant.h"
#include "source/diagnostic.h"
#include "target/profile.h"
#include "validation/discovery.h"
#include "workspace/workspace.h"

#include <span>
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

// Emits one complete LLVM module for one semantic package. procedures is the
// canonical package-local MIR product order: the module defines every listed
// concrete procedure together with the package's globals, relocatable
// constants, and (when requested) its process support and hosted entry point.
//
// The span borrows procedure-owned MIR payloads. Keeping those payloads in
// their semantic-product side-table slots preserves the compiler's fine-grain
// lowering graph; LLVM emission merely observes the completed package set and
// does not reconstruct an owning package-wide MIR program. Every pointer must
// remain valid until this synchronous call returns.
[[nodiscard]] LlvmIrResult emit_llvm_package_module(
    const TargetProfile &target,
    const SourceManager &sources,
    const LlvmIrOptions &options,
    const SemanticPackage &semantic,
    const Aarch64CAbiTable &abi,
    const ConstantTable &global_initializers,
    std::span<const MirProcedure *const> procedures,
    DiagnosticSink &diagnostics);

} // namespace draft
