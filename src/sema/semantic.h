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
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

namespace draft {

struct SemanticAnalysisResult {
  bool ok = false;
  SemanticPackage package;
  ConditionalSelections selections;
  ConstantTable constants;
};

// Runs semantic rounds until no new declaration-level conditional becomes
// ready, then performs one diagnostic final round. The returned package contains
// only selected declarations. Member-level conditionals are retained as typed
// incomplete aggregate sites until their selection pass is added.
[[nodiscard]] SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    DiagnosticSink &diagnostics);

} // namespace draft
