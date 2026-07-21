// Deterministic publication of one procedure body's isolated semantic output.
//
// A body worker allocates TypeId, ScopeId, SymbolId, and side-table indices in
// the domain formed by its frozen canonical prefix plus its private suffix.
// Another result from the same frozen wave may be published first, so those
// suffix numbers are not canonical positions. This module translates every
// process-local reference, interns equal structural types, appends the owned
// rows, and rewrites the procedure-local HIR and discovered work roots to the
// resulting canonical package generation.
//
// The operation is deliberately a plain deterministic publisher, not a query
// system or a concurrent mutation layer. Workers never call it. The package
// coordinator invokes it in stable product order after all work in a frozen
// wave has joined. It depends on semantic representations and HIR, but never on
// parsing, workspace scheduling, MIR, LLVM, or provider behavior.

#pragma once

#include "sema/body_checker.h"

namespace draft {

// Publishes task.semantic into package/constants and rewrites every semantic ID
// retained by task.program and task.discovered_work. The packet may have been
// produced from an earlier prefix of the same package generation. A malformed
// or non-prefix packet is diagnosed and leaves the caller with an invalid
// compilation result; source errors are not reported at this layer.
[[nodiscard]] bool publish_body_task_semantics(
    SemanticPackage &package,
    ConstantTable &constants,
    ProcedureBodyTaskResult &task,
    DiagnosticSink &diagnostics);

} // namespace draft
