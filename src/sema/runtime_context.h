// Construction of the compiler-managed runtime Context semantic type.
//
// The built-in `context` value exists at every body and synthesis site, even
// when source does not import core/runtime. This small pass selects the exact
// imported/local public Context when available and otherwise creates the
// ABI-identical private type. It runs after type/interface resolution so both
// body checking and early declaration synthesis see one stable type graph.

#pragma once

#include "sema/analyzer.h"
#include "source/diagnostic.h"

namespace draft {

// Idempotently fills package.runtime_context_type and its member symbols.
// Existing imported/local runtime types are reused; a private type is installed
// only for packages that do not expose the ordinary core/runtime declaration.
void ensure_runtime_context_type(
    SemanticPackage &package, DiagnosticSink &diagnostics);

} // namespace draft
