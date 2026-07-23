// Target-independent native reference facts and artifact reachability.
//
// Semantic checking proves every selected authored body even when no executable
// path can reach it. Native emission has a different root set: an executable
// entry, an explicit C export, a validation harness entry, or another
// artifact-owned root. This module keeps those meanings separate. It extracts
// one compact direct-reference row from each checked concrete runtime body and
// closes only the rows needed by one native artifact.
//
// Inputs are immutable checked HIR, the corresponding direct procedure-flow
// summary, package semantic tables, and package global initializers. Outputs
// own package-qualified identities and canonical input indices; they retain no
// SourceManager pointers and do not mutate semantic state. Package-local
// SymbolIds remain only as a cheap bridge back to compiler-owned body/global
// tables. Cross-package edges always use PackageIdentity plus linker-level
// declaration name.
//
// The traversal is deliberately independent from effect closure. Denials and
// effects may use the same checked call/value facts, but their transitive
// contract is not an optimization call graph. Foreign calls terminate in an
// explicit foreign edge. An unknown indirect target remains inspectable but
// does not manufacture references to every Draft procedure: any Draft function
// pointer which can actually reach live machine code already appears as a
// relocation-bearing procedure value in a live body or global initializer.
//
// This is an IR-boundary operation: it depends on semantic HIR and produces the
// exact procedure/global set from which MIR products may be scheduled. It has
// no LLVM, object-format, linker, filesystem, or persistent-cache dependency.
//
// Relevant specification: docs/specification/06-compiler.md, "Native lowering
// and summaries"; docs/specification/04-native-interop.md, "C exports".

#pragma once

#include "sema/body_checker.h"
#include "sema/effect.h"
#include "sema/semantic.h"
#include "workspace/workspace.h"

#include <cstddef>
#include <string>
#include <vector>

namespace draft {

// NativeSymbolIdentity is the command-independent identity of one Draft
// procedure or package global at the native boundary. name is the actual
// package linker component: ordinary declarations use their source name,
// nested procedures use Symbol::linkage_name, and concrete public instances
// use their canonical instance name. Procedure and global domains are kept in
// separate tables, so kind is intentionally not encoded here.
struct NativeSymbolIdentity {
  PackageIdentity package;
  std::string name;

  bool operator==(const NativeSymbolIdentity &) const = default;
};

// NativeForeignReference records one terminal call edge. provider is the
// explicit Draft provider name; linker_name_spelling retains the semantic
// binding spelling because decoding belongs to the interop/backend boundary.
// Empty fields are permitted only for already-diagnosed malformed source and
// never reach a successful native plan.
struct NativeForeignReference {
  std::string provider;
  std::string linker_name_spelling;

  bool operator==(const NativeForeignReference &) const = default;
};

// NativeProcedureReferenceSummary is one checked concrete runtime body's
// complete direct native-reference row. body_index addresses the owning
// PackageBodyWorkState::procedures table. local_symbol addresses the same
// package's SemanticPackage and is never compared across packages.
//
// direct_calls records statically named and finite indirect call targets.
// procedure_values is broader: taking, storing, comparing, returning, or
// passing a procedure identity also requires its definition because emitted
// code contains a relocation to that identity. escaped_procedure_values is the
// inspectable subset passed through a foreign or unknown call boundary.
// referenced_globals records every package/global Symbol expression. Vectors
// are de-duplicated in first HIR/effect discovery order; artifact closure uses
// identity-sorted lookup and therefore never depends on traversal order.
struct NativeProcedureReferenceSummary {
  NativeSymbolIdentity procedure;
  SymbolId local_symbol;
  std::size_t body_index = 0;
  std::vector<NativeSymbolIdentity> direct_calls;
  std::vector<NativeSymbolIdentity> procedure_values;
  std::vector<NativeSymbolIdentity> escaped_procedure_values;
  std::vector<NativeSymbolIdentity> referenced_globals;
  std::vector<NativeForeignReference> foreign_calls;
  bool has_unknown_call_target = false;
};

// NativeGlobalReferenceSummary describes one defined package global. Global
// initialization is compile-time-only in Draft, so the only native dependency
// retained here is a procedure relocation nested anywhere in its immutable
// initializer. local_symbol addresses the owning SemanticPackage.
struct NativeGlobalReferenceSummary {
  NativeSymbolIdentity global;
  SymbolId local_symbol;
  std::vector<NativeSymbolIdentity> procedure_values;
};

// NativeReachabilityInput is one closed, deterministic artifact graph. The
// procedure/global vectors use compiler publication order. Roots may arrive in
// command order; closure canonicalizes lookup without reordering the owned
// input rows returned through result indices.
struct NativeReachabilityInput {
  std::vector<NativeProcedureReferenceSummary> procedures;
  std::vector<NativeGlobalReferenceSummary> globals;
  std::vector<NativeSymbolIdentity> procedure_roots;
  std::vector<NativeSymbolIdentity> global_roots;
};

// NativeReachabilityResult returns canonical input indices, never copied
// procedure/global payloads. live_* indices are strictly increasing. An
// unknown-target row is separately retained for diagnostics and qualification;
// its body remains live, while no unrelated definition is guessed into the
// artifact. failure is nonempty only when identities are duplicated or an
// internal Draft reference/root has no corresponding definition.
struct NativeReachabilityResult {
  bool ok = false;
  std::vector<std::size_t> live_procedures;
  std::vector<std::size_t> live_globals;
  std::vector<std::size_t> unknown_target_procedures;
  std::string failure;
};

// Extracts one concrete body's direct native references. direct must describe
// the same procedure SymbolId as body. Foreign/imported identities are resolved
// from semantic binding tables; malformed mismatches produce an empty
// procedure identity and are rejected later by reachability validation.
[[nodiscard]] NativeProcedureReferenceSummary
collect_native_procedure_references(
    const PackageIdentity &package,
    const SemanticPackage &semantic,
    const ProcedureBodyHirResult &body,
    std::size_t body_index,
    const DirectProcedureEffectSummary &direct);

// Extracts every defined package global in package-scope declaration order.
// Foreign variables are absent because they own no Draft storage. Procedure
// identities nested in aggregate initializers are retained recursively.
[[nodiscard]] std::vector<NativeGlobalReferenceSummary>
collect_native_global_references(
    const PackageIdentity &package,
    const SemanticPackage &semantic,
    const ConstantTable &global_initializers);

// Computes the least artifact set closed over exact procedure/global edges.
// Duplicate definitions and missing internal roots/edges are compiler errors,
// not conservative liveness guesses. The operation is deterministic and
// O((entities + edges) log entities) through sorted identity lookup.
[[nodiscard]] NativeReachabilityResult compute_native_reachability(
    const NativeReachabilityInput &input);

} // namespace draft
