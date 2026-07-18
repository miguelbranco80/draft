// Target-owned semantic type validation.
//
// Draft structural types are interned before native lowering.  This pass is the
// narrow boundary that rejects a structurally meaningful type when the selected
// target has no physical representation for it.  Keeping that decision here
// prevents LLVM's permissive type syntax from silently enlarging Draft 1.
//
// Inputs are the immutable semantic TypeStore and selected TargetFacts; output
// is diagnostics only. The module owns no type state, performs no lowering, and
// depends only on semantic/source data. See 02-types-memory-runtime.md,
// "Target profile" and "Pointers, procedures, and views".

#pragma once

#include "sema/constant.h"
#include "sema/type.h"
#include "source/diagnostic.h"

namespace draft {

// Checks every concrete target-dependent type currently interned in the store.
// It is intentionally safe to run more than once: accepted rows are silent and
// canonical interning means one rejected row produces one diagnostic per pass.
// Compiler orchestration calls it once after package semantics and again after
// body checking, when local and monomorphized types may have been appended.
[[nodiscard]] bool validate_target_types(
    const TypeStore &types,
    const TargetFacts &target,
    DiagnosticSink &diagnostics);

} // namespace draft
