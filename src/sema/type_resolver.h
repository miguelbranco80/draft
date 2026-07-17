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
// Imported-package member lookup and `when` selection are intentionally later
// semantic graph operations. A type that depends on either remains incomplete
// instead of receiving a guessed layout.

#pragma once

#include "sema/analyzer.h"
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

// Selection-aware form used by the semantic fixed-point driver. Selected
// member-level `when` regions contribute directly to their owning type scope.
void resolve_package_types(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    SemanticPackage &package,
    const ConditionalSelections &selections,
    DiagnosticSink &diagnostics);

} // namespace draft
