// Deterministic textual LLVM IR emission from verified Draft MIR.

#pragma once

#include "interop/c_abi.h"
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
  // Debug information is an operational artifact choice, not part of Draft
  // program meaning. Ordinary fast builds leave this false so module
  // construction does not allocate source-location metadata that the user did
  // not request. Qualification and debugger-oriented builds opt in explicitly.
  bool emit_debug_information = false;
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
};

// Emits one complete LLVM module for one semantic package. procedures is the
// canonical artifact-live package-local definition order: the module defines
// every listed global and concrete procedure together with their relocatable
// constants and (when requested) process support and the hosted entry point.
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
    const CAbiTable &abi,
    const ConstantTable &global_initializers,
    std::span<const SymbolId> globals,
    std::span<const MirProcedure *const> procedures,
    DiagnosticSink &diagnostics);

} // namespace draft
