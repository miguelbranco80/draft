// Append-only declaration selection plus provisional type/constant discovery.
//
// Initial collection and interface binding run once. Package-level `when`
// regions retain their lexical declaration context; each ready selection
// appends only its chosen branch to the authoritative declaration generation.
// Until type, constant, and layout facets become independent semantic products,
// discovery evaluates those facts on private copies. One final authoritative
// type/constant pass emits real diagnostics. No provisional copy may be returned
// or paired with later HIR.

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

// Collects declarations once, appends each newly selected package-level branch,
// and probes type/constant readiness until no product changes. One final pass
// resolves the authoritative selected package. A conditional that depends on
// unavailable synthesis remains an explicit unresolved site.
[[nodiscard]] SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    DiagnosticSink &diagnostics);

// Workspace-aware form. Every source import must have a matching dependency
// interface. Binding occurs once on the authoritative declaration generation;
// private readiness probes copy those already bound interface rows.
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
