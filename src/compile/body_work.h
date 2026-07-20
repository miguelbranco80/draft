// Canonical work identities for demand-driven procedure body specialization.
//
// The workspace compiler discovers generic procedure calls while checking
// consumer bodies, but the executable specialization belongs to the package
// which defines the template. This module owns the package-independent packet
// carried across that boundary, its deterministic set operations, and the one
// conversion into owner-local TypeIds. It does not schedule packages, mutate a
// CompiledPackage, check HIR, or know about resolution/provider policy.
//
// Demands are command-lifetime semantic work, not a persistent cache. Their
// canonical order makes an exact declaration-generation+demand-set key stable
// under consumer discovery order. Materialization is permitted only into a
// body-owned SemanticPackage; package declaration baselines remain immutable.
// This is the concrete compiler mechanism for specification section 10's
// semantic dependency order and section 15's ordinary checking of generated
// source.

#pragma once

#include "base/sha256.h"
#include "sema/body_checker.h"
#include "sema/interface.h"
#include "source/diagnostic.h"

#include <string>
#include <vector>

namespace draft {

// One package-independent generic argument crossing from a checked consumer to
// the package which owns the template body. type is a self-contained interface
// graph for either a type argument or the declared type of a value argument.
// Value arguments additionally carry their exact compile-time value. No
// consumer-local TypeId enters the owner's declaration baseline.
struct ProcedureInstantiationDemandArgument {
  bool is_type = true;
  InterfaceTypeGraph type;
  ConstantValue value;
};

// ProcedureInstantiationDemand is the stable work key and materialization
// packet for one externally requested concrete procedure. digest covers the
// complete canonical packet; instance_name uses its shortened spelling only as
// native symbol identity. Demands are sorted by instance name and full digest
// before comparison, so package reuse is independent of consumer discovery
// order and never trusts a truncated name as equality.
struct ProcedureInstantiationDemand {
  std::string public_template_name;
  std::string instance_name;
  Sha256Digest digest;
  std::vector<ProcedureInstantiationDemandArgument> arguments;
  std::vector<InterfaceTypeGraph> pack_types;
};

// Sorts and deduplicates one package's complete ready demand set. Equal packets
// requested by several consumers collapse to one work item. A shortened native
// name collision between different full digests is diagnosed and returns
// false; no arbitrary packet is selected.
[[nodiscard]] bool canonicalize_procedure_demands(
    std::vector<ProcedureInstantiationDemand> &demands,
    DiagnosticSink &diagnostics);

// Compares canonical demand sets by their collision-resistant semantic keys.
// Both inputs must already have passed canonicalize_procedure_demands.
[[nodiscard]] bool
same_procedure_demands(const std::vector<ProcedureInstantiationDemand> &left,
                       const std::vector<ProcedureInstantiationDemand> &right);

// Computes a monotonic extension between canonical sets. true means every old
// demand remains and added receives only new packets in canonical order. false
// means work was removed or changed, so the owner must rebuild from its
// declaration baseline to avoid retaining stale executable bodies.
[[nodiscard]] bool added_procedure_demands(
    const std::vector<ProcedureInstantiationDemand> &previous,
    const std::vector<ProcedureInstantiationDemand> &current,
    std::vector<ProcedureInstantiationDemand> &added);

// Imports portable type graphs into one body-owned semantic generation and
// returns BodyChecker seeds in the same canonical order. owner may append
// interned types; callers must never pass the immutable declaration baseline.
[[nodiscard]] std::vector<ProcedureInstantiationSeed>
materialize_procedure_demands(
    const std::vector<ProcedureInstantiationDemand> &demands,
    SemanticPackage &owner, DiagnosticSink &diagnostics);

} // namespace draft
