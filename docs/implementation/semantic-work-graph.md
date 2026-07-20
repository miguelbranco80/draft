# Semantic work graph target state

Status: desired end-state architecture, not a description of the current
implementation and not normative language semantics. The specification remains
authoritative, especially the dependency-ordered elaboration rules in
[Compiler architecture](../specification/06-compiler.md#dependency-ordered-elaboration).

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
| Parsed file | Loading, lexing, and parsing are eager and independent per file. |
| Package imports | The acyclic package graph orders interface dependencies. |
| Package name set | All authored declarations plus accepted declaration/member expansions are known before dependent lookup proceeds. |
| Opaque `...` set | Ready sibling sites are frozen, checked privately, and merged transactionally; siblings cannot observe one another. |
| Constant value | Evaluation is requested by dependants; a declaration cycle is an error. |
| Type identity | The nominal or structural identity exists even when later facets are incomplete. |
| Type members | Member names and declared types are complete. |
| Natural layout | Size, alignment, and offsets are complete for the selected target; pointer recursion is allowed and inline recursion is rejected. |
| ABI classification | Calling convention facts are separate from natural layout. |
| Procedure template body | A symbolic parametric body is checked once under its declared constraints. |
| Procedure instance body | A canonical concrete argument tuple owns one independently checkable typed body. |
| Direct effect summary | Facts local to one concrete body, before transitive call and pointer-flow closure. |
| Closed effect SCC | Monotonic closure over one concrete call/flow strongly connected component. |
| Denial result | Checked only after the summaries on which the denial depends are closed. |
| MIR procedure | Lowering is owned by one checked concrete procedure and does not mutate semantic type tables. |
| Machine function | Native emission is independent per concrete function when the target backend permits it. |
| Package static data and assembly | Non-function package output is explicit and separate from function emission. |
| Artifact layout | Symbols, sections, relocations, and link inputs are published in canonical order after independent fragments complete. |

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
the concrete call and procedure-pointer-flow graph. MIR and machine emission
start only from closed semantic products, so lowering never feeds facts back
into type checking.

## Emission reachability

Semantic checking and machine emission are separate questions. The first
correct implementation may emit every concrete procedure. If emission later
becomes demand-only, its conservative roots must include:

- the configured entry point and C exports;
- validation entries selected by the command;
- procedure identities stored in globals, constants, or static data;
- requirements introduced by parsed assembly or generated source; and
- all transitive direct calls and finite procedure-pointer targets.

An uncertain target is emitted, not silently discarded. This optimization must
never weaken whole-program checking.

## Completion criteria

The target state is reached when package retry rounds and retained-package
rechecking are gone; generic instance and type-layout demand are explicit graph
products; procedure workers publish isolated immutable bodies; effects and
denials close through explicit SCCs; MIR is produced per concrete procedure;
and deterministic parallel scheduling is qualified at multiple worker counts.

The existing package-wide semantic tables, sequential body mutation,
declaration fixed-point reconstruction, package-wide HIR/MIR passes, and closed
immutable work-graph executor are migration constraints, not parts of the final
design. The executor may remain useful for already frozen provider and native
ready sets, but the semantic coordinator must support deterministic dynamic
discovery.

The intentionally rejected endpoints are equally important: no node per
expression, no universal incremental-query engine, no callback or future maze,
no shared concurrent semantic mutation, and no cross-process cache.
