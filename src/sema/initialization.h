// Conservative definite-initialization checking over typed HIR.

#pragma once

#include "sema/analyzer.h"
#include "sema/hir.h"
#include "source/diagnostic.h"

namespace draft {

// Reports reads that are provably from an automatic local declared with `---`
// and not subsequently initialized. Body checking calls this on one task-owned
// procedure arena before publication; no package-wide HIR view is required.
// The analysis never rejects a merely uncertain read: address escape and
// divergent control flow move a local to an explicit Maybe state.
[[nodiscard]] bool check_definite_initialization(
    const SemanticPackage &package,
    const HirProgram &hir,
    DiagnosticSink &diagnostics);

} // namespace draft
