// Type checking and typed-HIR construction for procedure bodies.
//
// This phase consumes the final selected package semantic graph: package names,
// signatures, constants, and layouts are already stable. It appends lexical
// block/local symbols, checks runtime expressions and statements with expected
// types, records body-level judgment/synthesis sites, and emits structured HIR.
// It does not perform ABI lowering, storage placement, or LLVM construction.

#pragma once

#include "sema/analyzer.h"
#include "sema/constant.h"
#include "sema/hir.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

#include <cstddef>

namespace draft {

struct BodyCheckResult {
  bool ok = false;
  HirProgram program;
  std::size_t checked_procedures = 0;
};

// Checks every package procedure definition in stable declaration order.
// Foreign declarations and standalone procedure types have no body and are
// skipped. Errors in one body do not prevent independent bodies from producing
// recoverable HIR.
[[nodiscard]] BodyCheckResult check_package_bodies(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const ConditionalSelections &selections,
    SemanticPackage &package,
    const ConstantTable &constants,
    DiagnosticSink &diagnostics);

} // namespace draft
