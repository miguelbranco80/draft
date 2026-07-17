// Compile-time validation and typing of package and thread-local initializers.

#pragma once

#include "sema/constant.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

namespace draft {

// Writes successfully checked initializer values to initializers in SymbolId
// order. Package constants remain a separate input: initialized mutable storage
// must not leak into constant evaluation or a public interface. Variables
// without an initializer remain absent and use their ordinary zero value in
// native emission. Foreign variables are declarations, not local storage, and
// are intentionally skipped.
[[nodiscard]] bool check_global_initializers(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const TargetFacts &target,
    const ConstantTable &constants,
    ConstantTable &initializers,
    DiagnosticSink &diagnostics);

} // namespace draft
