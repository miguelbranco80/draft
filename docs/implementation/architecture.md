# Bootstrap compiler architecture

Status: current architectural overview distilled from the completed first
implementation plan. Language semantics remain owned by the specification.

This document describes the durable phase boundaries and representations. The
original sequencing and acceptance criteria remain intact in
[the historical first implementation plan](../history/first-implementation-plan.md).
The desired endpoint for demand-driven semantic completion and deterministic
parallel scheduling is specified separately in the
[semantic work graph target state](semantic-work-graph.md); that document is a
target architecture rather than a claim about the current bootstrap.

## Goal

Build Draft first as a complete ordinary systems language on AArch64 macOS,
then activate its agent operations without changing the parser, type system, or
native pipeline:

```text
complete handwritten packages -----------------------> native executable
surface packages with `...` -> checked expansions ---^
```

The compiler must build complete programs with no Codex configuration, provider
credentials, network access, synthesis pins, or judgment evidence. The first
compiler is written in a deliberately small C++20 subset and links a selected
LLVM 22 distribution through a narrow C-API adapter. Its most valuable outputs
are the executable language behavior, conformance tests, target profile,
manifest formats, and canonical semantic representations that the later
self-hosted compiler can reproduce.

## Architecture

The compiler has four main layers:

1. **Front end** -- loads folder packages, lexes and parses complete surface
   syntax, and preserves source locations, documentation, judgments, denials,
   and synthesis sites in the AST.
2. **Semantic core** -- resolves names, checks types and control flow, evaluates
   constants, computes layouts, instantiates parametric declarations, and builds
   canonical package interfaces and dependency summaries.
3. **Elaborator** -- schedules semantically ready `...` sites, constructs their
   bounded context, accepts provider proposals, and sends each expansion back
   through the ordinary parser and semantic core. It owns transactional source
   pin handling; independent validation commands own evidence.
4. **Native back end** -- lowers the complete typed program through a small
   Draft MIR to LLVM IR, verifies the selected target contract at the in-process
   LLVM boundary, emits native objects, and performs the final platform link.

The semantic and native pipeline is complete without the elaborator. The
elaborator closes typed holes in an otherwise ordinary program; it is not the
foundation on which ordinary parsing, checking, or code generation depends.

```text
 Source/package loader
          |
          v
 Lexer -> parser -> surface AST
                       |
                       v
             semantic dependency graph
                /              \
               v                v
       ordinary checking   synthesis obligation
                                |
                       context -> provider
                                |
                       generated Draft source
                                |
                         parser + checker
                \              /
                 v            v
                resolved semantic graph
                         |
                    typed HIR/CFG
                         |
                    Draft MIR
                         |
                  LLVM -> object -> link

 Pin store <-> elaborator       Target profile + runtime -> lowering/linking
```

### Timing observation boundary

The process driver may attach one command-owned timing recorder to the existing
phase option structs. Compiler, validation, and native adapters contribute
nested events and deterministic work counters; none may consult recorded time
or make it part of semantic identity. Sequential phases use an explicit nesting
stack. Parallel native workers measure into task-owned result slots; after the
join, the command thread appends those completed events in stable task-ID order.
The recorder itself therefore remains single-threaded, and scheduler completion
order cannot change timing row order or compiler results.

The events reflect the implementation's real orchestration rather than the
conceptual diagram alone. An ordinary handwritten `check` constructs one graph:
interface discovery installs declarations and types, then semantic continuation
checks bodies, effects, denials, and completed interfaces on those same package
rows. A native `build` continues that graph directly through MIR/LLVM, emits
independent package objects through embedded LLVM, then invokes only the
remaining platform tools; it does not reload or recheck handwritten source.
`--timings` exposes resolution rounds as in-memory source transitions. A
checked complete-file overlay is parsed into the existing workspace graph;
package/root/import IDs remain stable. Target selection, source generations,
parsed files, per-package imports, package name sets, opaque interface
synthesis sets, named constants in complete compilation, and package interfaces
have stable command-local product IDs.
The interface coordinator freezes dependency-ready waves and publishes
task-local diagnostics and package payloads in product-ID order. Each named
constant consumes already published values or reports exact constant/type-facet
blockers; task-local structural type values are interned by the coordinator.
Ready imported constants remain dependency-interface inputs and are installed
under consumer-local proxy IDs rather than duplicated as consumer products.
The package interface waits for every constant product and validates storage
against their immutable table without recursively reevaluating them. An
unresolved declaration/member set leaves an explicit synthesis product waiting
and keeps the name/interface products of every dependent package blocked. An
accepted overlay appends successor products and marks the affected former interface
slice superseded; unrelated products remain authoritative.

A declaration/member expansion rebuilds the changed package and transitive
consumers because their interfaces may have changed. A statement/expression/
assembly expansion rebuilds declarations only for its containing package—the
grammar boundary proves that it cannot alter a package interface—while
transitive consumers retain checked HIR and invalidate only effect/obligation
closure. One command-local adjacency index is built with the workspace graph,
retained across every continuation, and supports import lookup, reverse source
invalidation, canonical initial product construction, and the body/closure
traversals not yet migrated to semantic products. Building its sorted views
costs O((packages + imports) log(packages + imports)); invalidation costs
O(packages + imports), without a persistent cache. The `compiler passes`,
`workspace loads`, `workspace source transitions`, `package body checks`,
`package body extensions`, and `package body reuses` counters make those
distinct operations visible. `--timings=all` adds package/tool scopes, file
discovery and I/O, lexing/parsing, import-graph resolution, and exclusive time;
child process CPU is reported separately from parent wall time.

Each package row separates an immutable declaration generation from its
body-owned semantic tables and constants. Every authored symbolic template and
concrete procedure owns a separate HIR arena and is a live
`ProcedureTemplateBody` or
`ProcedureInstanceBody` product appended before its checker is invoked. Nested
procedures and locally discovered specializations are appended only after the
frozen wave which exposed them joins, with an exact edge to that exposing body.
Each root owns a task-local diagnostic sink, and graph publication merges those
diagnostics in product order. HIR-local IDs begin at zero in each product; every
semantic ID addresses the body package returned beside the product set. A
deterministic compatibility projection rewrites local IDs and concatenates the
arenas once for effect, denial, and MIR consumers which remain package-wide.

The coordinator still moves exclusive ownership of the current semantic and
constant prefix into one root task, which returns a worker-owned successor for
explicit adoption. This transfer avoids an accidental full-package copy per
body, but the current sequential oracle must adopt one complete semantic
successor before invoking the next root. Replacing that successor with local
semantic discoveries and deterministic canonical publication is the remaining
boundary before body waves may run in parallel.

The transitional body work key is the declaration generation plus the exact
canonical set of concrete generic procedures demanded by consumer packages.
Equal keys reuse the complete body result; added demands append and check only
new specializations; a removed or changed demand rebuilds from declarations so
stale executable procedures cannot survive. Compile-time type preflight and
early synthesis discovery run on private copies and never become a hidden first
body pass over the authoritative package. The procedure-local arena migration
deletes this retained-package mechanism rather than preserving it as a cache.

After every selected package has reached target lowering, the backend derives a
closed native work graph in canonical package/module/assembly order. Package
modules have already expressed imported symbols as external declarations, so
their object tasks are independent and form one bounded ready set. Each worker
owns an isolated LLVM context or private assembler paths and writes one result
slot. The main thread joins the set, selects diagnostics by lowest stable task
ID, and only then publishes files and linker inputs in task-ID order. Thus
parallel scheduling changes elapsed time, never artifacts or diagnostics.

### Internal representations

- **Surface AST:** lossless enough for diagnostics, structural site identity,
  generated-source maps, and exact grammar-category replacement.
- **Semantic product graph:** an append-only command-local table of explicit
  product kinds, dependencies, and states. Eager target/source/parse/import
  inputs begin complete; semantic tasks move through frozen ready waves.
  Source transitions append successors and supersede unselected generations.
  Payload side tables remain typed and phase-owned rather than entering a
  generic graph value.
- **Declaration semantic generation:** package declarations, imports,
  signatures, constants, layouts, and preliminary interfaces have stable IDs
  and remain immutable after publication.
- **Body semantic generation / typed HIR:** a declaration-prefix copy owns
  lexical rows, concrete procedure instances, body sites, imported effect
  closure, and HIR together. No HIR is paired with declaration-only tables.
- **Procedure CFG:** explicit branches and scopes, used for return analysis,
  `defer`, branch facts, judgments, and denial summaries.
- **Draft MIR:** a small non-optimizing IR with explicit loads, stores, checks,
  context arguments, calls, aggregate operations, and source locations. LLVM is
  an emission/optimization back end rather than Draft's semantic model.

LLVM types stay behind numeric, target, ABI, and code-generation adapters. The
front end must not depend on LLVM IR details.

### AArch64 macOS target boundary

One versioned target profile fixes the triple, LLVM data layout, pointer width,
Darwin C ABI rules, CPU features, object format, relocation/code/TLS models,
trap behavior, linker contract, and parsed-assembly dialect. Target-specific
ABI classification lives under this boundary rather than leaking into type
checking.

The first runtime supplies `runtime.Context`, failure entries, TLS/context
establishment, `main` entry glue, and the smallest allocator/OS support needed
by end-to-end tests.

### Synthesis and pin boundary

Codex is the first synthesis and judging provider. It is reached through a small
versioned adapter protocol so that provider execution details do not enter the
language's semantics. A request contains a canonical obligation and
content-addressed semantic context; a response contains only proposed Draft
syntax and provider metadata. The compiler owns retries, checking, program
identity, and commits.

Persistent on-disk shape:

```text
.draft/resolutions/<target-identity>/workspace/resolution.json
.draft/resolutions/<target-identity>/packages/<root-path>/resolution.json
.draft/generated/<content-hash>.draft
.draft/evidence/<content-hash>.json
.draft/build/<target-file-tag>/workspace/<artifact>
.draft/build/<target-file-tag>/packages/<root-path>/<artifact>
```

The workspace-directory package and child-package rows are disjoint even when
a child is literally named `workspace`. Manifests and derived artifacts are
selected by exact root and target. Generated expansion objects are the one
shared content-addressed pool because their digest already supplies collision-
free identity. Evidence is likewise owned by this one workspace directory; its
typed keys bind a resolved program and target rather than relying on a
package-local physical path. Automatic executable discovery is a
workspace-layer operation:
it deterministically scans ordinary visible directories for surface
package-level `main` declarations, then sends every selected root through an
independent ordinary compiler graph. It never changes import resolution or
merges multiple executables into one semantic graph.

The first aggregate implementation runs those root graphs sequentially and may
repeat shared dependency analysis and code generation. A later optimization may
deduplicate immutable package work by exact source identity, target, and compiler
configuration. Persistent object reuse is intentionally deferred until its key,
invalidation, ownership, and determinism contract are specified; neither future
optimization may change discovery, diagnostics, or artifact identity.

The bootstrap deliberately has no persistent compiler cache. Parsed syntax,
typed graphs, MIR, native objects, and other derived compiler state live only
for one command or in explicitly requested output artifacts. The generated
Draft objects above are committed program source, not cached compiler state.
`draftc expand --out <directory>` is an explicit read-only projection of those
objects after complete provider-free checking. It writes final source bytes and
generated-to-surface maps transactionally to a new directory; that requested
artifact is neither an input nor persistent compiler state.

`draft resolve` stages changes and atomically commits only a coherent,
compiler-checked program. It does not run tests, benchmarks, or judgments.
With `--build`, the resolver returns its exact final semantic-closure graph and
the driver continues that graph through MIR/LLVM and native emission after the
source commit. No second front-end orchestration or manifest reload occurs.
Interface and body expansions are complete source files, but installing one is
not a workspace reload: the compiler reparses the replacement transactionally,
rejects any package/import-topology change, and reanalyzes the affected graph
closure. Speculative provider proposals use isolated copies of the same
command-local graph operation, so checking a rejected proposal cannot mutate
the authoritative stage and does not perform filesystem discovery again.
`draft build` consumes saved expansions without contacting a provider
or modifying the resolution manifest. It rechecks every program input but
treats the host compiler, linker, and SDK as operational build configuration.

### Agent constructs before provider execution

All three agent-facing surface constructs exist in the initial front end and
semantic model:

- **`docs` is real metadata, not a discarded no-op.** The compiler parses its
  attachment group, validates and hashes attached files and folders, associates
  it with the package or declaration, and includes public documentation in the
  canonical package interface. It simply has no runtime lowering.
- **`judge` is a typed, anchored, zero-width construct.** The compiler checks
  placement and reachability, assigns its stable identity, records its claim and
  attachments, and makes the appropriate semantic/CFG facts available. Ordinary
  builds already ignore judge execution by definition. The public `draft judge`
  command now uses the provider-neutral judgment boundary and the implemented
  Codex adapter when explicitly configured; without a provider it reports that
  judgment execution is unavailable while ordinary compilation remains valid.
- **`...` is a typed unresolved obligation, not a fake value or empty body.**
  The parser records its grammar category and attachments; semantic analysis
  derives the expected type, visible bindings, active denials, target facts, and
  dependency edges. Checking can proceed wherever the hole does not withhold
  required declarations, layout, or control-flow facts. Native emission rejects
  an unresolved required site precisely. A complete program with no `...` sites
  compiles normally.

This means the provider-independent compiler already produces exactly the
information Codex later needs. Adding Codex supplies proposals and verdicts; it
does not retrofit agent awareness into the language core.
