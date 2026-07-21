// Conservative loop facts for body-level agent sites.
//
// Body checking first constructs complete typed HIR. This small follow-up pass
// then computes the loop facts that are safe to expose to an elaborator. It is
// intentionally separate from ordinary typing: these facts affect provider
// context and pin staleness, never whether a Draft program is accepted or how
// its runtime code is lowered.

#pragma once

#include "sema/analyzer.h"
#include "sema/hir.h"
#include "workspace/package.h"

namespace draft {

// Clears and recomputes SemanticSite::loop_ranges for the procedure-local HIR
// arena and task-local semantic-site suffix supplied by body checking. The
// analysis is monotone: merges and loop backedges can only remove facts.
// Consequently malformed/recoverable HIR yields less context, never an
// optimistic range assertion. Running before publication keeps every HIR ID in
// its owning arena and publishes the derived facts with the same semantic row.
void infer_agent_loop_ranges(
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const HirProgram &hir);

} // namespace draft
