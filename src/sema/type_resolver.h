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
// active compile-time member selections. The operation may intern new structural
// or anonymous types but does not revisit unrelated package declarations.
[[nodiscard]] TypeId resolve_type_syntax(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    const SyntaxTree &tree,
    NodeId type,
    ScopeId scope,
    DiagnosticSink &diagnostics);

// Resolves one procedure declaration introduced in a lexical statement scope.
// Body checking first declares owner so recursion can find the name, then calls
// this routine to create its optional parametric scope, immutable parameter
// symbols, and canonical procedure type. Package declarations use the same
// internal path during the ordinary signature-resolution pass.
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
    DiagnosticSink &diagnostics);

// Selection-aware form used by the semantic fixed-point driver. Selected
// member-level `when` regions contribute directly to their owning type scope.
void resolve_package_types(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    DiagnosticSink &diagnostics);

} // namespace draft
