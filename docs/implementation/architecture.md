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
independent package-static and single-procedure objects through embedded LLVM,
then invokes only the remaining platform tools; it does not reload or recheck
handwritten source.
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
`workspace loads`, `workspace source transitions`, `package body starts`,
`procedure bodies checked`, `external procedure bodies materialized`, and ABI
classification wave/task/worker counters make those distinct operations
visible. `--timings=all` adds package/tool scopes, file
discovery and I/O, lexing/parsing, import-graph resolution, and exclusive time;
child process CPU is reported separately from parent wall time.

Each package row separates its immutable package-interface payload from its
body-owned semantic tables and constants. Every authored symbolic template and
concrete procedure owns a separate HIR arena and is a live
`ProcedureTemplateBody` or
`ProcedureInstanceBody` product appended before its checker is invoked. Nested
procedures and locally discovered specializations are appended only after the
frozen wave which exposed them joins, with an exact edge to that exposing body.
Each root owns a task-local diagnostic sink, and graph publication merges those
diagnostics in product order. HIR-local IDs begin at zero in each product; every
semantic ID addresses the body package returned beside the product set. Direct
effect, denial, assembly, and MIR tasks consume those arenas without
concatenating them. A deterministic compatibility projection still rewrites
local IDs for metadata, agent obligations, native interop, and validation
consumers which remain package-wide. The projection is a short-lived value
owned by the invoking operation; it is never stored in `BodyCheckResult`, so
procedure products remain the only retained HIR.

The coordinator retains the canonical semantic and constant prefix while one
root checks a private view frozen at exact table counts. `TypeStore` and
`SymbolTable` expose that prefix through non-owning read-only overlays; the task
owns only their newly interned rows and explicit binding additions to canonical
scopes. `ConstantTable` likewise reads package constants through a canonical
prefix and owns only lexical constants. Declaration-closed file scopes, imports,
imported documentation, native bindings, and package conditional regions are
also read directly from the retained prefix and have no body-output fields.
Owned-scope, aggregate/enum, parametric, specialization, imported semantic,
dependent-type recipe, semantic-site, and declaration-denial tables expose the
same canonical prefix followed by a task-owned suffix; their raw task vectors
contain only rows created by that procedure. Imported semantic tables include
symbols, types, concrete procedures, outbound type requests, and
effect/return/write contracts. Recipe tables retain required integer expressions
and deferred element counts, value expressions, and type applications.
Aggregate layout publication, procedure-specialization promotion,
required-integer refinement, and site enrichment address combined tables by
global index but have explicit local-only mutable operations, so a task cannot
rewrite a prefix row. The task returns only its appended types, scopes, symbols,
body side-table rows, lexical constants, and local HIR; no retained table prefix
is copied into it.
Publication rejects a packet whose frozen prefix is not present in the current
generation and adopts packets in product order; it never replaces the package
with a worker-owned successor. Each packet's private suffix IDs are translated
to current canonical IDs. Structural types are interned, while equal procedure
and nominal type specializations discovered by sibling workers redirect their
root, owned scopes, parameters or members, and nominal TypeId to the first
canonical result. Existing type and symbol rows cannot be mutated through an
overlay, which exposed and removed a former constant-evaluation write into
retained declarations.
Constant products now carry their checked static type beside the immutable
value, and the package-interface barrier installs that payload explicitly.

Each published procedure result also retains the exact cross-package procedure
requests it created and canonical indices for its body-level semantic sites.
Current selection follows those product-owned routes rather than scanning all
rows ever appended to the package. A selected HIR symbol scan additionally
recognizes reuse of a concrete import proxy first created by another product;
deactivating that earlier product therefore cannot drop a still-live transitive
demand. Site indices continue to observe package-level loop-range enrichment
without copying or mutating the procedure's immutable HIR.

Every currently ready root is now dispatched from one shared prefix and all
workers run through the bounded closed-wave executor. Results join before
deterministic publication. Worker count is scheduling policy only: one-worker
and four-worker qualification compares product graphs, diagnostics, semantic
table sizes, and final LLVM bytes. Dynamic discovery still occurs only between
waves, never by concurrent mutation of the semantic graph. A one-worker wave
runs directly on the calling thread. Supported POSIX hosts create larger pools
with an explicit eight-MiB stack per worker, matching the syntax-recursion
budget available to the main compiler thread instead of inheriting macOS's
smaller pthread default.

Body selection is an explicit projection over immutable procedure products.
Authored roots are always selected; a current external demand selects its exact
retained owner product; and discovered nested or concrete roots follow their
earlier prerequisite. An unseen external demand appends and checks one product.
Removing a demand changes only the projection: HIR, semantic rows, product IDs,
and diagnostics already published for that package interface remain
inspectable and can be selected again without rechecking. Only selected HIR
feeds transitive demand discovery, metadata, effects, denials, validation,
assembly, MIR, and LLVM, and only selected external instances enter the public
package interface. A changed declaration still replaces its complete
`CompiledPackage`; there is no generation counter, body work key, or
aggregate demand comparison.

Compile-time type preflight and early synthesis discovery run on private copies
and never become a hidden first body pass over the authoritative package.

Workspace packages retain their live `PackageBodyWorkState` after finalization.
Its work rows, procedure results, and semantic-product rows share one append-only
index domain, so an added demand resumes the exact completed prefix. The
compiler no longer reconstructs an extension scheduler from a reduced
`BodyCheckResult`; that transfer object remains only for direct subsystem
callers. Pending roots from every package now enter one workspace-wide frozen
ready set. Product IDs are appended in PackageId/work order, workers may execute
the complete independent set concurrently, and package-local semantic suffixes
publish in that same canonical order after join. A newly materialized external
root carries an explicit edge to the completed consumer body which exposed its
demand. Cross-package demand discovery and retained-product selection repeat
until the current program reaches a fixed point; there is no consumer-first
body executor outside the graph.

After that body fixed point, every TypeId in the source-semantic prefix owns a
separate target ABI-classification product. Declaration types depend on their
package interface; body-created types depend on the exact procedure product
which installed them; all rows also depend on the target product. The pure
classifier runs for every package in one bounded workspace wave and publishes a
TypeId-indexed table in product order. Native validation, C-header emission, and
LLVM lowering are consumers of that table, so ABI meaning is computed once and
cannot drift between front end and backend. MIR reads the table immutably:
compiler-only storage addresses use `rawptr` plus an explicit addressed TypeId,
while actual source pointers retain their canonical semantic type.

The LLVM adapter has direct operations for three explicit ownership units. Its
compatibility operation emits one complete package module. Its static-data
operation emits a complete module containing globals, relocatable initializer
storage, runtime support, and any hosted entry point but no Draft procedure
definition. Its machine-function operation emits a complete module defining
exactly one MIR procedure while declaring package globals and sibling
procedures. Each split unit can therefore enter a private LLVM context and
produce an object independently; no textual fragment linker or shared LLVM
module is required.

Compiler orchestration consumes only the split operations. `PackageStaticData`
owns the static unit, every concrete `MirProcedure` produces one
`MachineFunction`, and one `ArtifactLayout` product per package publishes the
canonical static/function/assembly input sequence. After every selected package
reaches target lowering, those layout rows form one closed native ready set.
Each worker owns an isolated LLVM context or private assembler paths and writes
one result slot. The main thread joins the set, selects diagnostics by lowest
stable task ID, and only then publishes files and linker inputs in task-ID
order. Parallel scheduling changes elapsed time, never artifacts or
diagnostics. The complete-package LLVM operation remains a direct subsystem
test convenience and is not a compiler or native-build path.

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
- **Body semantic generation / typed HIR:** the coordinator owns the canonical
  body prefix; each procedure product owns its semantic suffix and HIR arena.
  Publication joins those rows into the generation consumed by current
  package-wide compatibility passes. No HIR is paired with declaration-only
  tables.
- **Target ABI facet:** one immutable row per source-semantic TypeId, published
  after body closure with explicit target and exact type-producer edges. An
  `Illegal` row is a completed answer. Later consumers read the table through
  the target-matching semantic prefix and never invoke the classifier.
- **Procedure CFG:** explicit branches and scopes, used for return analysis,
  `defer`, branch facts, judgments, and denial summaries.
- **Draft MIR:** a small non-optimizing IR with explicit loads, stores, checks,
  context arguments, calls, aggregate operations, and source locations. One
  checked runtime HIR procedure lowers to one privately verified MIR procedure;
  workspace compilation stores that result only in the side-table row owned by
  its `MirProcedure` product. Package rows retain ordered product IDs, not a
  reconstructed `MirProgram`. Compilation publishes package static-data and
  assembly barriers, then lowers every independent runtime procedure in one
  bounded ready wave. LLVM is an emission/optimization back end rather than
  Draft's semantic model.

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
