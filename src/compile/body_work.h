// Canonical work identities for demand-driven procedure body specialization.
//
// The workspace compiler discovers generic procedure calls while checking
// consumer bodies, but the executable specialization belongs to the package
// which defines the template. This module owns the package-independent packet
// carried across that boundary, its deterministic canonicalization, and the one
// conversion into owner-local TypeIds. It does not schedule packages, mutate a
// CompiledPackage, check HIR, or know about resolution/provider policy.
//
// Demands are command-lifetime semantic work, not a persistent cache. Their
// canonical order makes independent consumer discovery deterministic. Each
// unseen demand materializes one retained body product; later source selections
// select or deselect that immutable product without comparing aggregate work
// keys or rebuilding a package. Materialization is permitted only into a
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
// before lookup, so product identity is independent of consumer discovery order
// and never trusts a truncated name as equality.
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

// Imports portable type graphs into one body-owned semantic generation and
// returns BodyChecker seeds in the same canonical order. owner may append
// interned types; callers must never pass the immutable declaration baseline.
[[nodiscard]] std::vector<ProcedureInstantiationSeed>
materialize_procedure_demands(
    const std::vector<ProcedureInstantiationDemand> &demands,
    SemanticPackage &owner, DiagnosticSink &diagnostics);

} // namespace draft
