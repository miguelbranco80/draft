// Resolution of source type syntax, parametric parameters, aggregate members,
// and procedure signatures.
//
// Declaration collection gives every package name a stable identity before this
// pass begins. Type resolution can therefore handle forward references and
// recursive pointers without source-order rules. It creates lexical scopes for
// parametric parameters, type members, and procedure parameters; resolves
// builtins and package-local type names; and computes target-natural layouts
// whenever every required member layout is known.
//
// Imported-package member lookup consumes proxy scopes reconstructed from
// canonical package interfaces before this pass begins. `when` selection is a
// staged semantic graph operation; an unselected region remains incomplete.

#pragma once

#include "sema/analyzer.h"
#include "sema/constant.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

namespace draft {

// Mutates the collected SemanticPackage in place. New member/parameter symbols
// and scopes append to existing tables, preserving all IDs assigned by the
// declaration collector. Errors are recoverable and use the canonical invalid
// semantic type so independent declarations can continue resolving.
void resolve_package_types(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    DiagnosticSink &diagnostics);

// Resolves one type node encountered after package signature resolution, such as
// a local declaration annotation. The caller supplies its lexical scope and the
// active compile-time member selections and value/type overlays. A ready
// ConstantValue::Type in those bindings denotes its represented TypeId in the
// same way as a source Type symbol. The operation may intern new structural or
// anonymous types but does not revisit unrelated package declarations.
[[nodiscard]] TypeId resolve_type_syntax(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    const SyntaxTree &tree,
    NodeId type,
    ScopeId scope,
    const ConstantTable &active_constants,
    const std::vector<ConstantTypeBinding> &active_types,
    const TargetFacts &target,
    DiagnosticSink &diagnostics);

// Preserves one integer expression that still mentions compile-time value
// parameters. Body checking uses the same builder as type syntax for explicit
// procedure applications such as `take[N + 1](value)`. A fully concrete or
// unsupported expression returns no value; ordinary constant evaluation then
// remains the caller's fallback.
[[nodiscard]] std::optional<IntegerExpression>
resolve_dependent_integer_expression_syntax(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    const SyntaxTree &tree,
    NodeId expression,
    ScopeId scope,
    TypeId contextual_type,
    const ConstantTable &active_constants,
    DiagnosticSink &diagnostics);

// Resolves one procedure declaration introduced in a lexical statement scope.
// Body checking first declares owner so recursion can find the name, then calls
// this routine to create its optional parametric scope, immutable parameter
// symbols, and canonical procedure type. Active bindings allow a preceding
// lexical type-valued constant or enclosing generic parameter to appear in the
// signature. Package declarations use the same internal path during the
// ordinary signature-resolution pass.
[[nodiscard]] TypeId resolve_local_procedure_signature(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    const SyntaxTree &tree,
    NodeId declaration,
    NodeId procedure,
    ScopeId scope,
    SymbolId owner,
    const ConstantTable &active_constants,
    const std::vector<ConstantTypeBinding> &active_types,
    const TargetFacts &target,
    DiagnosticSink &diagnostics);

// Resolves one named type declaration introduced by a procedure body. The
// declaration owner already exists in the lexical Block scope and nominal
// declarations already own an incomplete TypeId, exactly as package types do
// after declaration collection. Active bindings make an enclosing generic
// procedure concrete while member types, array counts, and representation
// attributes are resolved; the local type itself may still introduce its own
// independent parametric parameters.
[[nodiscard]] TypeId resolve_local_type_declaration(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    const SyntaxTree &tree,
    NodeId declaration,
    NodeId type,
    ScopeId scope,
    SymbolId owner,
    const ConstantTable &active_constants,
    const std::vector<ConstantTypeBinding> &active_types,
    const TargetFacts &target,
    DiagnosticSink &diagnostics);

// Creates or reuses one nominal type application from already resolved,
// consumer-local arguments. Body checking uses this when substituting a
// procedure template signature such as `^Dynamic[T]` after T was inferred.
// Keeping construction in the type resolver preserves the single cache and
// layout path used by source-written applications.
[[nodiscard]] TypeId instantiate_parametric_type_application(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    SymbolId source,
    std::vector<ParametricArgument> arguments,
    SourceRange use_range,
    const TargetFacts &target,
    DiagnosticSink &diagnostics);

// Concretizes an owner-evaluated Array/SIMD count or retained structural alias
// application whose recipe belongs to this package. Procedure body checking
// supplies its exact generic environment through the shared deferred-binding
// records; the type resolver composes nested templates and invokes the normal
// compile-time interpreter. This keeps one evaluator and one structural-type
// construction path across declaration and body phases.
[[nodiscard]] TypeId instantiate_owner_evaluated_type_application(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    TypeId source,
    const std::vector<DeferredElementCountTypeBinding> &type_bindings,
    const std::vector<DeferredElementCountValueBinding> &value_bindings,
    SourceRange use_range,
    const ConstantTable &active_constants,
    const TargetFacts &target,
    DiagnosticSink &diagnostics);

// Selection-aware form used by the semantic fixed-point driver. Selected
// member-level `when` regions contribute directly to their owning type scope.
void resolve_package_types(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    DiagnosticSink &diagnostics);

// Fixed-point form. Values were evaluated by the full compile-time interpreter
// against a prior deterministic semantic graph and are keyed only by stable
// source syntax; this clean rebuild consumes them before computing layouts.
void resolve_package_types(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    const std::vector<ResolvedIntegerExpression> &resolved_integers,
    const TargetFacts &target,
    DiagnosticSink &diagnostics);

// Interface-discovery form. blocked_synthesis contains exact integer-recipe
// syntax sites where the full interpreter reached unresolved synthesis in a
// prior clean graph. Only their generic not-compile-time diagnostics are
// deferred; every unrelated type error remains authoritative.
void resolve_package_types(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    const std::vector<ResolvedIntegerExpression> &resolved_integers,
    const TargetFacts &target,
    const std::vector<SyntaxReference> &blocked_synthesis,
    DiagnosticSink &diagnostics);

} // namespace draft
