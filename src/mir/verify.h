// Structural and type-table verification for Draft MIR.

#pragma once

#include "mir/mir.h"
#include "sema/type.h"
#include "source/diagnostic.h"

namespace draft {

// Verification is an internal compiler boundary, but it reports ordinary
// diagnostics instead of aborting so a malformed lowering is observable in
// tests and in release builds. It checks table references, block termination,
// result definitions, basic instruction arity, and procedure return shape.
[[nodiscard]] bool verify_mir_procedure(
    const MirProcedure &procedure,
    const TypeStore &types,
    DiagnosticSink &diagnostics);

} // namespace draft
