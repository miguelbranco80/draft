# Semantic work graph implementation plan

Status: active implementation sequence for reaching the
[semantic work graph target state](semantic-work-graph.md). This document tracks
replacement and deletion work; it does not define Draft semantics.

## Rule for every step

Each step must leave one authoritative path for the products it migrates. A new
graph node is not progress if the old package pass still computes the same fact.
Tests must prove product identity, invalidation, diagnostics, and worker-count
determinism from the public compiler entry points. Temporary coexistence is
allowed only between committed steps and must be removed by the named deletion
gate.

## Implementation sequence

1. **Dynamic product graph — complete.** The command-local
   `SemanticProductId`, product kind/state rows, dependency edges, frozen ready
   waves, task-local outcomes, and deterministic publication operation now have
   a sequential scheduling oracle. The existing closed `WorkGraph` remains only
   for already frozen provider and native batches.

2. **Command inputs and package barriers — complete.** The selected target,
   source generations, parsed files, package imports, package name completeness,
   opaque interface `...` sets, and package interface readiness are products.
   Package-interface ordering now runs through the product coordinator. Checked
   source transitions append successor products, supersede only the affected
   interface slice, and retain unrelated current products.

3. **Declaration completion without rounds.** Parse once and retain stable
   syntax identities. Replace `analyze_package_semantics` reconstruction with
   explicit products for conditional selection, constant values, type identity,
   type members, member types, natural layout, and ABI classification. Selected
   `when` branches publish declarations into a deterministic package-name
   barrier. Declaration cycles and inline-layout cycles become precise graph
   diagnostics. Delete provisional semantic rounds and discarded packages.
   Initial collection/import binding and package-branch materialization are now
   append-only; the remaining work is to replace private type/constant/layout
   readiness copies and the final aggregate pass with the named products.

4. **Canonical generic type demand.** Give every concrete type application one
   canonical command-local key and owner task. Cross-package owner evaluation
   appends explicit layout/value dependencies and publishes one immutable result.
   Delete `TypeInstantiationPublisher`, declaration rebuild retries, request-set
   hashes used only to detect progress, and retained instantiated-interface
   side paths.

5. **Procedure-owned checking.** Split package body checking into immutable
   package inputs plus one task-owned result for each authored template or
   concrete procedure instance. A result owns its local semantic arena, typed
   HIR, lexical constants, diagnostics, direct effects, and discovered demands.
   Publish canonical types/instances between waves. Check every authored body
   and symbolic template regardless of reachability. Delete `BodyCheckResult`'s
   package-wide mutable copy, `check_additional_package_instances`, body work
   keys, and the package body-extension/reuse loop.

6. **Synthesis as an explicit wait state.** A body or declaration task may
   report its exact ready `...` set after producing the typed constraint needed
   by the provider. Freeze opaque siblings, run providers independently, check
   proposals privately, merge accepted ordinary source canonically, and create
   the successor source-generation products. Delete speculative enriched
   package copies and any retained-package recheck path.

7. **Flow closure and denials.** Publish direct procedure summaries separately,
   build the concrete direct-call and procedure-value-flow graph, identify SCCs,
   and close each SCC monotonically in condensation order. Check denials only
   against closed summaries. Delete the package-wide effect fixed point and
   imported-effect refresh mutation.

8. **Per-procedure MIR.** Lower each checked concrete procedure into a private
   MIR result without mutating semantic type tables. Publish package static data
   and parsed assembly separately. Delete package-wide MIR lowering and semantic
   type interning from the lowering phase.

9. **Parallel semantic waves.** Run each frozen ready wave with bounded workers.
   Workers write only task slots; the coordinator sorts by stable product ID,
   interns canonical discoveries, publishes diagnostics, and adds graph work.
   Make compile-time resource accounting task-local. Qualify one-worker and
   multi-worker output, diagnostics, generated source, and failure selection as
   identical.

10. **Native product consumption.** Emit independent machine functions from MIR
    products, keep package static data and assembly explicit, and perform symbol,
    section, relocation, and linker-input layout as deterministic publication
    barriers. Start conservatively by emitting every concrete procedure; any
    later demand-only emission must use the roots defined by the target-state
    document.

11. **Final-state deletion and qualification.** Remove `PackageSemanticProgress`,
    declaration/body generations, semantic retry counters, package-wide HIR/MIR
    ownership, closed-graph assumptions in semantic code, and stale docs/tests.
    Update the Draft coding skill to teach only the final compiler behavior.
    Run focused tests, the full suite, sanitizers, both target checks, native
    parity/determinism, resolved-provider fixtures, and a large-source timing
    qualification at several worker counts.

## Completion evidence

Completion requires both positive proof and absence checks:

- every product and barrier named by the target-state table has an owning row,
  transition test, and real compiler consumer;
- no semantic package retry, retained-package recheck, hidden on-demand
  recursion, shared worker mutation, or package-wide MIR loop remains;
- invalid unused authored bodies still fail checking;
- legal recursion closes through SCCs and illegal declaration/layout cycles
  retain exact source diagnostics;
- one-worker and multi-worker runs produce byte-identical outputs and identical
  ordered diagnostics; and
- searches, current documentation, examples, and the embedded coding skill all
  describe only the final architecture.
