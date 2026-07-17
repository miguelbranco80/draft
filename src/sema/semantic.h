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

} // namespace draft
