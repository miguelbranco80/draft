// Fixed-point orchestration of declaration collection, type resolution,
// compile-time constants, and declaration-level `when` selection.
//
// Individual semantic passes stay simple and inspectable: collection never
// evaluates expressions, and constant evaluation never mutates syntax. This
// module composes them into deterministic rounds. Provisional rounds write into
// temporary diagnostics; once selections stop changing, one final round builds
// the returned semantic graph and emits each real diagnostic exactly once.

#pragma once

#include "sema/analyzer.h"
#include "sema/constant.h"
#include "sema/interface.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

namespace draft {

struct SemanticAnalysisResult {
  bool ok = false;
  SemanticPackage package;
  ConditionalSelections selections;
  // Named constants participate in language evaluation and interface export.
  ConstantTable constants;
  // Global initializers are closed object-file values.  They live separately
  // because a mutable variable must never become a language constant merely
  // because its initial contents were known at compile time.
  ConstantTable global_initializers;
  // Only interface-synthesis discovery populates this source-order set. Each
  // ID names a package procedure whose body participated in constant or `when`
  // evaluation and encountered unresolved synthesis. The compiler checks these
  // bodies immediately to build early obligations; complete semantic analysis
  // rejects unresolved synthesis instead and leaves the set empty.
  std::vector<SymbolId> compile_time_synthesis_procedures;
};

// Runs semantic rounds until no new declaration-level conditional becomes
// ready, then performs one diagnostic final round. The returned package contains
// only selected declarations and member regions. A conditional that depends on
// an unavailable generic instantiation remains an explicit unresolved site.
[[nodiscard]] SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    DiagnosticSink &diagnostics);

// Workspace-aware form. Every source import must have a matching dependency
// interface. Binding occurs in every provisional fixed-point round because the
// imported constants and types can select `when` declarations.
[[nodiscard]] SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    const AvailablePackageImports &imports,
    DiagnosticSink &diagnostics);

// Interface discovery form used by the compiler's dependency scheduler. In
// Discover mode, constant execution may stop at `...` and returns the exact
// package procedures whose ordinary body check must publish those obligations.
// Every other semantic rule and diagnostic remains identical to the rejecting
// form above.
[[nodiscard]] SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    const AvailablePackageImports &imports,
    CompileTimeSynthesisMode synthesis_mode,
    DiagnosticSink &diagnostics);

} // namespace draft
