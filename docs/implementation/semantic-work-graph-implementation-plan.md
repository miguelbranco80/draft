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
   a sequential scheduling oracle. The existing closed `WorkGraph` remains a
   bounded executor for already frozen provider, procedure-body, and native
   batches; it does not own dynamic semantic identity or discovery.

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

   Complete workspace compilation now retains one append-only declaration
   package and schedules authored declaration types, non-parametric nominal
   layouts, package/member conditional choices, and final named constants as
   individual products. Forward type aliases and type-valued constant uses add
   exact `SymbolId` edges; aggregate layout adds exact type-facet edges; a
   selected package branch appends only its new declarations and conditions.
   Blocked attempts own disposable package copies and diagnostics, while the
   coordinator publishes successful snapshots, structural constant types,
   layouts, selections, and values in `SemanticProductId` order. The
   `PackageNameSet` barrier closes only after those indexed products complete.
   Imported interface nominals are immutable upstream inputs, and symbolic
   parametric templates intentionally receive member-type products but no
   fictitious concrete layout product.

   `TypeStore` retains identity, members, member types, and natural layout as
   separate one-way facets. Natural layout is produced by the single pure
   `sema/type_product` contract, which distinguishes exact waits from overflow;
   declaration resolution no longer publishes an authored nominal layout in
   product mode. Tests at the public compiler boundary prove a forward
   declaration edge, a full-interpreter layout-call edge, a separate nominal-
   layout edge, constant-to-member-facet edges, stable structural-type
   publication across mixed waves, source-generation supersession, and full
   core/generic pipeline composition.

   Member-condition selection now has an explicit reachable frontier and exact
   continuation edges: a nested `else when` becomes a product only after its
   predecessor selects that syntax, and the owning member packet cannot publish
   before every selected condition completes. Member identity and member typing
   are now separate `TypeMembers` and `TypeMemberTypes` products: the first
   publishes source-order symbols, and the second fills those exact symbols only
   after the namespace is complete. Declaration-owned compile-time calls also
   retain exact procedure-product edges even when a sequential task snapshot
   already contains the completed signature.

   Interface-synthesis discovery now uses the same declaration products as
   complete compilation. A declaration, member, condition, named constant, or
   declaration-owned integer recipe reports a synthesis wait; the coordinator
   blocks that exact product on one package `OpaqueSynthesisSet`, exhausts
   independent ready work, and withholds PackageInterface. Product-driven tests
   prove source-order sibling discovery, dependency-delayed sites, declaration
   anchors, typed integer boundaries, and the explicit graph edge. The current
   sequential provider-context oracle replays only the stopped products over
   their completed published prerequisites; step 6 replaces that composition
   with task-owned typed constraint results.

   Rejecting direct semantic test and subsystem entry points now use a small
   package-local sequential product coordinator. This migration exposed and
   corrected three hidden aggregate assumptions: ordinary constants now produce
   constant rather than declaration edges for inferred types; non-evaluating
   `type_of` waits on an unfinished procedure signature; and full-interpreter
   value arguments preserve their concrete integer identity. A long constant
   chain now completes through product edges instead of tripping the legacy
   recursive evaluator-depth limit.

   This step remains open in two narrower places. The explicit direct Discover-
   mode overload and its early metadata tests still use
   `discover_package_declarations`; step 6 replaces that provider-surface
   compatibility composition with task-owned synthesis constraints. ABI
   classification must become an explicit product at the body/declaration
   boundary established by step 5 rather than being attached prematurely to a
   declaration-only `TypeStore`.

4. **Canonical generic type demand — complete.** Every concrete cross-package
   owner-evaluated type application has one command-local key formed from its
   owner package, owner template SymbolId, and ordered owner-local canonical
   arguments. Requester natural layouts are explicit prerequisites. Each owner
   task either publishes one immutable `InterfaceTypeGraph` or adds exact edges
   to nested owner demands; a consumer imports completed results into only its
   retried private declaration attempt. The coordinator appends owner semantic
   state and results in product order. `TypeInstantiationPublisher`, recursive
   owner publication, declaration rebuild retries, progress hashes, and the
   `PackageInterface::instantiated_types` side path are deleted.

5. **Procedure-owned checking.** Split package body checking into immutable
   package inputs plus one task-owned result for each authored template or
   concrete procedure instance. A result owns its local semantic arena, typed
   HIR, lexical constants, diagnostics, direct effects, and discovered demands.
   Publish canonical types/instances between waves. Check every authored body
   and symbolic template regardless of reachability. Delete `BodyCheckResult`'s
   package-wide mutable copy, `check_additional_package_instances`, body work
   keys, and the package body-extension/reuse loop.

   The first reconstruction seam is complete. Package-level authored roots and
   concrete specializations are now invoked through fresh `BodyChecker`
   instances over a deterministic append-only publication prefix rather than
   one checker retaining a hidden growing instance work list. A concrete
   `ParametricInstanceRecord` retains the complete exact type/value environment,
   including substitutions inherited by nested templates, while its concrete
   procedure scope retains static-pack bindings and element parameters. A later
   root can therefore be checked without the transient checker which discovered
   it. Nested procedure declarations now publish their own later roots as well;
   each retains the enclosing concrete environment and pack-capture boundary,
   so no `check_procedure` recursion hides another body.

   Compiler body work is now represented by live `ProcedureTemplateBody` and
   `ProcedureInstanceBody` rows. Authored roots are appended before invocation;
   nested and locally instantiated roots are appended only after the exposing
   frozen wave joins and carry an exact parent-product dependency. Each root
   receives a private diagnostic sink and publication preserves product order.
   Recoverable invalid HIR completes its scheduling row while invalidating the
   package result, so another invalid unused body is still checked.

   Compiler orchestration no longer calls or exposes
   `check_additional_package_instances`; both clean and extension work now use
   the same explicit item-at-a-time body state and live product publisher.

   Root invocation is worker-owned as well. The coordinator retains the
   canonical package and gives the worker a private view frozen at explicit
   TypeStore, SymbolTable, side-table, and ConstantTable counts. TypeStore,
   SymbolTable, and ConstantTable prefixes are non-owning read-only overlays; a
   task owns only new rows, explicit additions to existing scopes, and lexical
   constants. Declaration-closed file, import, documentation, native-binding,
   and package-condition tables are direct immutable views and have been
   deleted from the task append packet. The worker has no mutable alias to
   `PackageBodyWorkState` and returns only a
   `ProcedureBodySemanticAppend`, not a complete successor. The coordinator
   rejects a stale prefix and appends the packet in product order. A focused
   test proves the work cursor cannot advance before publication and canonical
   state remains unchanged while the task is in flight.

   HIR ownership is now product-local: every exact root starts an empty arena,
   publishes one permanent `ProcedureBodyHirResult`, and never receives earlier
   HIR as input. `BodyCheckResult` no longer stores a package-wide copy;
   remaining package-wide consumers explicitly build, use, and discard one
   deterministic ID-rewritten projection.

   Constant products now retain their checked static TypeId beside the value;
   package-interface finalization installs the immutable pair and no body task
   may repair a retained declaration symbol as a side effect.

   Every body-mutable semantic table now exposes a canonical prefix plus a
   task-local suffix; no retained prefix row is copied into a procedure task.
   Imported rows cover symbols, types, concrete procedures, outbound type
   requests, and effect/return/write contracts. Recipe rows cover required
   integer expressions and deferred element counts, value expressions, and type
   applications. Aggregate offset publication, procedure-specialization
   promotion, required-integer refinement, and semantic-site enrichment have
   explicit local-only mutable operations. All roots in one ready wave now read
   one frozen prefix. Publication translates task-private suffix IDs, interns
   structural types, and canonicalizes equal procedure and nominal type
   specializations in stable work order after the wave joins. The isolated
   workers run through the bounded executor, and public compiler tests qualify
   identical product graphs, diagnostic order, semantic table sizes, and LLVM
   bytes with one and four workers. The consumer-first external-demand loop,
   body work key, and extension/rebuild paths remain as the deletion boundary
   for this step.

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
