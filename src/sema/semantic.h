// Append-only declaration selection plus terminal type/constant discovery.
//
// Initial collection and interface binding run once. Package-level `when`
// regions retain their lexical declaration context; each ready selection
// appends only its chosen branch to the authoritative declaration generation.
// Until type, constant, and layout facets become independent semantic products,
// blocked discovery attempts evaluate those facts on private copies. The first
// no-progress attempt becomes the authoritative semantic package and contributes
// its type diagnostics; constant validation then runs against that exact package.
// Type resolution is never replayed merely to recreate an equivalent final row.

#pragma once

#include "sema/analyzer.h"
#include "sema/constant.h"
#include "sema/interface.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "workspace/package.h"

#include <cstddef>

namespace draft {

struct SemanticAnalysisResult {
  bool ok = false;
  SemanticPackage package;
  ConditionalSelections selections;
  // Named constants participate in language evaluation and interface export.
  ConstantTable constants;
  // Global initializers are closed object-file values.  They live separately
  // because a mutable variable must never become a language constant merely
  // because its initial contents were known at compile time.
  ConstantTable global_initializers;
  // Only interface-synthesis discovery populates this source-order set. Each
  // ID names a package procedure whose body participated in constant or `when`
  // evaluation and encountered unresolved synthesis. The compiler checks these
  // bodies immediately to build early obligations; complete semantic analysis
  // rejects unresolved synthesis instead and leaves the set empty.
  std::vector<SymbolId> compile_time_synthesis_procedures;
};

// PackageDeclarationDiscovery owns the selected declaration/type graph before
// final constant validation, storage initialization, and target checks. It is
// the payload of PackageNameSet while ConstantValue products are scheduled.
// terminal is false only when branch materialization could not produce a
// coherent name set. blocked_integer_synthesis is phase-private dependency
// evidence retained for the final discovery-mode check; it is never serialized.
struct PackageDeclarationDiscovery {
  bool terminal = false;
  bool discovery_ok = false;
  SemanticPackage package;
  ConditionalSelections selections;
  // Filled monotonically with local ConstantValue product publication after
  // discovery. Imported ready constants are upstream PackageInterface inputs;
  // the product-aware finalizer installs them under consumer-local proxies.
  // Direct semantic clients leave this empty and use finish_package_semantics.
  ConstantTable published_constants;
  std::vector<ResolvedIntegerExpression> resolved_integers;
  std::vector<SyntaxReference> blocked_integer_synthesis;
  std::vector<SymbolId> compile_time_synthesis_procedures;
  std::size_t unresolved_conditionals = 0;
};

// Performs only eager, source-order discovery: collect authored declarations,
// allocate nominal identities, install the consumer package identity, and bind
// already completed import interfaces. The returned payload is nonterminal and
// owns the authoritative append-only declaration table. It contains no member,
// signature, constant, layout, or conditional-selection products yet.
[[nodiscard]] PackageDeclarationDiscovery begin_package_declaration_discovery(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const AvailablePackageImports &imports,
    DiagnosticSink &diagnostics);

// Closes the PackageNameSet barrier after graph products have published all
// selected declarations, declaration types, member types, and natural layouts.
// The operation performs no semantic evaluation. It validates readiness,
// installs the compiler-defined runtime Context, and advances terminal exactly
// once. A false result has emitted an integration diagnostic and leaves the
// payload nonterminal.
[[nodiscard]] bool finish_package_declaration_discovery(
    PackageDeclarationDiscovery &discovery,
    DiagnosticSink &diagnostics);

// Performs collection, interface binding, append-only package `when`
// materialization, and terminal type discovery. It deliberately stops before
// final all-constant evaluation so the compiler coordinator can schedule each
// named constant as its own product.
[[nodiscard]] PackageDeclarationDiscovery discover_package_declarations(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    const AvailablePackageImports &imports,
    CompileTimeSynthesisMode synthesis_mode,
    DiagnosticSink &diagnostics);

// Completes the legacy aggregate constant/storage checks against one terminal
// discovery payload. Direct semantic clients use this composition while the
// workspace coordinator replaces the aggregate constant stage with individual
// products. The discovery payload is consumed exactly once.
[[nodiscard]] SemanticAnalysisResult finish_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    CompileTimeSynthesisMode synthesis_mode,
    PackageDeclarationDiscovery discovery,
    DiagnosticSink &diagnostics);

// Finalizes a discovery payload whose named constants were already published
// by individual semantic products. It combines those immutable local values
// with ready imported-interface constants, then validates conditions and
// storage without invoking aggregate local constant evaluation again.
[[nodiscard]] SemanticAnalysisResult finish_package_semantics_from_products(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    CompileTimeSynthesisMode synthesis_mode,
    PackageDeclarationDiscovery discovery,
    DiagnosticSink &diagnostics);

// Collects declarations once, appends each newly selected package-level branch,
// and probes type/constant readiness until no product changes. The terminal
// probe is retained as the authoritative selected package instead of being
// discarded and reconstructed. A conditional that depends on unavailable
// synthesis remains an explicit unresolved site.
[[nodiscard]] SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    DiagnosticSink &diagnostics);

// Workspace-aware form. Every source import must have a matching dependency
// interface. Binding occurs once on the authoritative declaration generation;
// private readiness probes copy those already bound interface rows.
[[nodiscard]] SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    const AvailablePackageImports &imports,
    DiagnosticSink &diagnostics);

// Interface discovery form used by the compiler's dependency scheduler. In
// Discover mode, constant execution may stop at `...` and returns the exact
// package procedures whose ordinary body check must publish those obligations.
// Every other semantic rule and diagnostic remains identical to the rejecting
// form above.
[[nodiscard]] SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    const AvailablePackageImports &imports,
    CompileTimeSynthesisMode synthesis_mode,
    DiagnosticSink &diagnostics);

} // namespace draft
