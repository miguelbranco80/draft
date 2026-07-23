# Semantic work graph

Status: implemented bootstrap architecture, not normative language semantics.
The specification remains authoritative, especially the dependency-ordered
elaboration rules in
[Compiler architecture](../specification/06-compiler.md#dependency-ordered-elaboration).
The completed replacement and deletion sequence is retained in the
[implementation record](semantic-work-graph-implementation-plan.md).

## Objective

Draft compilation should be **eager in discovery, demand-driven in completion,
and parallel in independent ready work**. One command owns one evolving semantic
graph for each selected source program. Parsing discovers declarations and
potential dependencies early; later work completes only the semantic products
whose prerequisites are ready. The graph continues through typed bodies, effect
closure, MIR, and native emission without rebuilding enriched state.

This design is for fundamental compiler speed and maintainability. It does not
depend on a persistent cache, and it must remain understandable as stable IDs,
flat tables, explicit state enums, loops, and switches rather than becoming a
generic query framework.

## End-state invariants

- The graph is command-local. No AST, semantic, MIR, object, or incremental
  compiler cache survives into another invocation.
- Work is monotonic: a product moves from absent, to blocked on named
  prerequisites, to complete or diagnosed. Completed immutable products are not
  re-entered and enriched in place.
- The graph is dynamic rather than one closed DAG. Checking can discover
  canonical generic instances, layout demands, procedure-flow edges, and ready
  synthesis obligations. New nodes and edges are published between frozen work
  waves.
- Cycles have explicit meanings. Illegal declaration or inline-layout cycles are
  diagnosed. Legal recursive procedures and effect propagation are solved as
  strongly connected components. No hidden recursive `ensure_*` call or broad
  package retry loop acts as the scheduler.
- Concurrency changes elapsed time only. Semantic identity, diagnostics,
  generated source, MIR, objects, and executables are identical for every worker
  count.
- Every selected authored procedure and every symbolic parametric template is
  checked even when unreachable from `main`. Demand-driven scheduling must not
  turn invalid unused source into accepted source.
- Source and generated-source locations survive every product and diagnostic.

Draft's semantics fit this model: imports are acyclic; `when` selects parsed
branches; parametrics and packs have explicit canonical arguments; nested
procedures do not capture runtime state; global initialization is compile-time;
and `...` produces ordinary Draft source through a transactional, grammar-bound
interface. None requires unrestricted macro expansion or an ambient mutable
compile-time world.

## Semantic products and barriers

The graph models products, not arbitrary compiler functions. A task should be
large enough to own a coherent result; there is no node for each token,
expression, or name lookup.

| Product or barrier | Required meaning |
| --- | --- |
| Target profile | One immutable set of semantic target facts precedes every target-dependent type, ABI, MIR, and native product. |
| Source generation | One selected authored/generated source set owns the product slice derived from those exact bytes. |
| Parsed file | Loading, lexing, and parsing are eager and independent per file. |
| Package imports | The acyclic package graph orders interface dependencies. |
| Package name set | All authored declarations plus accepted declaration/member expansions are known before dependent lookup proceeds. |
| Package interface | The package's exported names, concrete generic results, and closed effect/return/write contracts are immutable consumer inputs. |
| Opaque `...` set | Ready sibling sites are frozen, checked privately, and merged transactionally; siblings cannot observe one another. |
| Constant value | Evaluation is requested by dependants; a declaration cycle is an error. |
| Type identity | The nominal or structural identity exists even when later facets are incomplete. |
| Type members | The selected source-order member names and stable identities are complete. |
| Type member types | Every stable member identity has its declared type and associated compile-time member value where applicable. |
| Natural layout | Size, alignment, and offsets are complete for the selected target; pointer recursion is allowed and inline recursion is rejected. |
| ABI classification | Calling convention facts are separate from natural layout. |
| Procedure template body | A symbolic parametric body is checked once under its declared constraints. |
| Procedure instance body | A canonical concrete argument tuple owns one independently checkable typed body. |
| Direct effect summary | Facts local to one concrete body, before transitive call and pointer-flow closure. |
| Closed effect SCC | Monotonic closure over one concrete call/flow strongly connected component. |
| Denial result | Checked only after the summaries on which the denial depends are closed. |
| Direct native-reference summary | One checked concrete runtime body records direct calls, procedure values and escapes, globals, foreign edges, and uncertain indirect targets without producing MIR. |
| Artifact reachability | One command-selected root closure separates the complete checked procedure set from the procedure/global subset required by the requested artifact. |
| MIR procedure | Lowering is owned by one checked concrete procedure and does not mutate semantic type tables. |
| Package assembly | Captured and parsed assembly is an explicit package product consumed by MIR and native layout. |
| Package LLVM module | One complete module consumes the package's ordered artifact-live MIR products and owns only its artifact-live globals and concrete definitions. |
| Artifact layout | The package module and assembly link inputs are published in canonical order after their products complete. |

Type readiness is deliberately faceted: identity, members, member types, natural
layout, and ABI classification are distinct facts. A pointer often needs only
identity; an inline field needs layout. This removes the need for broad generic
layout retries while preserving legal recursive types.

A body task may finish as complete, error, blocked on explicit semantic product
IDs, or blocked on a frozen set of synthesis sites. `...` may therefore obtain a
type constraint skeleton before provider work starts, then return accepted
source to the ordinary parser and checker without creating privileged HIR.

## Ownership and scheduling

Each worker owns isolated output: a procedure-local semantic arena, typed HIR,
direct facts and effects, diagnostics, and newly discovered demands. Workers do
not append concurrently to shared package symbol, type, HIR, or diagnostic
tables.

For each wave, the coordinator:

1. freezes the canonically ordered ready set;
2. executes independent tasks with a bounded worker count;
3. collects task-local results;
4. sorts results by stable semantic ID;
5. deterministically interns and publishes canonical types and instances;
6. appends discovered nodes and dependency edges;
7. publishes diagnostics in canonical semantic and source order; and
8. repeats until the selected graph is complete or diagnosed.

Compile-time resource limits are task-local or otherwise deterministically
accounted. A shared schedule-sensitive instruction or memory budget is invalid.
Stable integer IDs and canonical structural keys are preferred; cryptographic
hashes belong only where bytes cross a persistent trust or identity boundary.

Procedure-flow and denial analysis use two visible stages: independently
computed direct summaries, followed by SCC discovery and monotonic closure over
the concrete call and procedure-pointer-flow graph. MIR and package emission
start only from closed semantic products, so lowering never feeds facts back
into type checking.

One immutable `ProcedureEffectAnalysis` is prepared per package before its
direct ready wave. It owns the selected HIR-row projection, terminal
native/imported contracts, dense SymbolId lookup, and procedure-leaf paths for
each completed TypeId. Direct workers borrow this context instead of rebuilding
package-sized lookup data. Flow closure makes one mutable summary-table copy;
non-recursive transfers run once, and later HIR rediscovery is limited to rows
that actually consumed a local returned-procedure or pointer-write contract.
Only recursive flow SCCs iterate internally. Transitive effect closure interns
each complete semantic effect once and propagates insertion-ordered 32-bit IDs
through membership bit sets; full strings, paths, and nested callback summaries
are materialized only at the public closed-summary boundary.

Detailed command timing observes this boundary without entering semantic data.
The interface graph reports aggregate ready-wave selection, task preparation,
bounded execution, and deterministic publication across all dynamic waves.
Effect closure reports contract-table setup, procedure-flow convergence, final
SCC construction, transitive effect propagation, and call-site composition for
each package. Worker measurements stay in task-owned slots and are replayed by
the coordinator in product order.

## Emission reachability

Status: implemented for provider-free native lowering.

Semantic checking and machine emission are separate questions. Every selected
authored body and symbolic parametric template is checked first. Each checked
concrete runtime body then publishes one compact direct native-reference row.
Only after those rows and package assembly are complete does one workspace
product compute the artifact projection rooted by:

- the configured entry point and C exports;
- validation entries selected by the command;
- procedure identities stored in reachable globals, constants, or static data;
- the explicit foreign/export C-ABI boundary used by package assembly;
- all transitive direct calls and finite procedure-pointer targets.

Package assembly cannot name a private Draft symbol, so its Draft-side roots
are the same explicit exports already included above. Parsed inline assembly is
inside its owning live procedure. An uncertain indirect call is retained as an
inspectable summary fact. Every concrete Draft procedure value that can supply
such a call is itself an exact relocation edge at the value's originating body
or global, so that definition is retained without guessing that every unrelated
procedure is live.

The closure maps its stable procedure/global identities back to package-local
body and SymbolId rows. MIR and LLVM consume only that projection. The complete
body, direct-effect, closed-effect, and denial products remain present, so this
optimization cannot weaken whole-program checking or denial enforcement.

## Implemented boundary

Package retry rounds and retained-package rechecking are gone. Generic instance
and type-layout demand are explicit graph products; procedure workers publish
isolated immutable bodies; effects and denials close through explicit SCCs; MIR
is produced per artifact-live concrete procedure after a workspace reachability
product; LLVM emission consumes one completed ordered live MIR/global set per
semantic package; and deterministic parallel scheduling is qualified at one and
four semantic workers.

The bootstrap retains package-owned canonical semantic tables because stable
SymbolId, ScopeId, and TypeId values need one publication domain. Workers never
mutate those tables concurrently: each ready task reads a frozen prefix and
returns a task-owned suffix and exact row patches, which the coordinator
publishes in product order. The bounded executor runs only a frozen ready set;
the semantic coordinator owns dynamic discovery and publication between waves.
There is no body work-key retention, declaration snapshot chain, aggregate HIR
or MIR owner, or closed immutable semantic DAG.

The intentionally rejected endpoints are equally important: no node per
expression, no universal incremental-query engine, no callback or future maze,
no shared concurrent semantic mutation, and no cross-process cache.
