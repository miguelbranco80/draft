// Lexical and transitive enforcement of Draft compiler denials.
//
// This phase resolves each selector through ordinary semantic scopes, walks HIR
// with an explicit active-denial stack, and compares direct operations and
// composed procedure summaries against that stack. Declaration-region contracts
// begin active at the declared procedure entry; expression and statement
// regions add restrictions only for their nested HIR. Unknown call edges reject
// every active denial, as required by the language.
//
// Denial checking does not alter HIR or optimize the call graph. It consumes the
// conservative EffectSummaryResult and emits source-located diagnostics at the
// violating operation plus a note at the selector that established the policy.
//
// Relevant specification: docs/specification/05-denials-validation.md section 13.

#pragma once

#include "sema/analyzer.h"
#include "sema/effect.h"
#include "sema/hir.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace draft {

// A deny selector is resolved once through the ordinary symbol table before it
// is compared with either HIR effects or the usable synthesis context. Keeping
// this representation public to semantic compiler passes prevents those two
// consumers from inventing subtly different name-based interpretations.
enum class ResolvedDenialKind {
  Symbol,
  ImportedPackage,
  RuntimeAssert,
  RawStringData,
  Context,
  ContextField,
  Assembly,
  Unchecked,
};

struct ResolvedDenialSelector {
  ResolvedDenialKind kind = ResolvedDenialKind::Symbol;
  SymbolId symbol;
  std::string root_identity;
  std::string root_relative_path;
  std::string field;
  SourceRange range;
};

// Resolves every selector child of one deny syntax node in source order.
// scope is the lexical scope outside the governed region, exactly as it was at
// semantic checking time. Invalid syntax and unknown names are diagnosed.
[[nodiscard]] std::vector<ResolvedDenialSelector> resolve_denial_selectors(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    SyntaxReference denial,
    ScopeId scope,
    DiagnosticSink &diagnostics);

// Returns true when no new denial error was emitted. selected_indices names the
// exact canonical procedure-product projection whose direct summaries were
// closed into effects. HIR-local IDs never cross procedure arenas.
[[nodiscard]] bool check_procedure_denials(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    std::span<const ProcedureBodyHirResult> procedures,
    std::span<const std::size_t> selected_indices,
    const EffectSummaryResult &effects,
    DiagnosticSink &diagnostics);

// Standalone subsystem form which checks every supplied procedure product.
[[nodiscard]] bool check_procedure_denials(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const SemanticPackage &package,
    std::span<const ProcedureBodyHirResult> procedures,
    const EffectSummaryResult &effects,
    DiagnosticSink &diagnostics);

} // namespace draft
