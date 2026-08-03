# Bootstrap compiler architecture

Status: current architectural overview distilled from the completed first
implementation plan. Language semantics remain owned by the specification.

This document describes the durable phase boundaries and representations. The
original sequencing and acceptance criteria remain intact in
[the historical first implementation plan](../history/first-implementation-plan.md).
Demand-driven semantic completion and deterministic parallel scheduling are
specified in the implemented
[semantic work graph](semantic-work-graph.md); its product boundaries are the
current bootstrap architecture rather than a future migration target.

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

The front end treats one complete selected file as its smallest scheduling
unit. A worker privately reads all of that file, lexes its complete byte stream,
and parses one complete syntax tree; the parser never divides a file into token
or source regions. After the file tasks join, the coordinator moves their
source buffers, trees, and diagnostics into command-owned tables in bytewise
filename order. File IDs and diagnostic order are consequently identical with
one or many workers, and neither `SourceManager` nor the parser requires shared
mutation or locks.

The semantic and native pipeline is complete without the elaborator. The
elaborator closes typed holes in an otherwise ordinary program; it is not the
foundation on which ordinary parsing, checking, or code generation depends.

### Self-hosted frontend replacement boundary

The self-hosted frontend staging source lives under `compiler/` and is compiled
by the C++ bootstrap into the build-tree-only `draftc-next` executable. It owns
source bytes and coordinates, structured frontend diagnostics, token storage,
the complete production lexer including semicolon insertion, and the concrete
syntax tree's append-only node and child-ID tables. It also owns deterministic
direct-child package discovery, exact target/test/benchmark source selection,
bytewise filename ordering, multi-file parsing, assembly-source inventory, and
package-name consistency. `draftc-next` accepts `lex <file.draft>`,
`syntax <file.draft>`, and
`package-syntax <package> --target <selector>`. It now also owns explicit
workspace/dependency/core roots, canonical semantic package identities,
recursive first-discovery package loading, ordered import occurrences, and
cycle, depth, and canonical containment checks. The corresponding
`workspace-syntax` command accepts the root package, workspace and physical
core roots, pinned core identity, target selector, and zero or more explicit
dependency mappings. The subsequent `workspace-declarations` boundary consumes
that closed graph directly. It creates one shared package scope and one child
scope per parsed file, collects ordinary declarations in canonical file/source
order, classifies source-only declaration categories and visibility/flags,
diagnoses direct-scope duplicates, and binds each accepted file-local import
alias to the exact canonical graph edge for its source ImportClause. Its names
and ranges borrow the graph-owned source rather than copying spellings. The
`workspace-public-names` boundary then gives every direct unconditional public
declaration a stable `(Package_Id, Symbol_Id)` reference and gives each import
binding one contiguous source-ordered span of those references. Two-part
`alias.member` lookup first resolves the file-local alias, then returns that
cross-package reference without copying a name or creating a typed proxy.
Imports themselves remain outside package scope, so they cannot re-export
names. `workspace-target-declarations` then records each package-level `when`,
indexes visible local scalar constants as stable source-ordered products, and
evaluates only the values demanded by a condition. An unpublished name becomes
an explicit product edge rather than a recursive evaluator call; forward chains
therefore publish each value once, cycles remain a visible stalled graph, and an
unrelated unsupported constant stays dormant. The supported scalar vocabulary
is the earlier target facts, known feature queries, boolean/string/unsigned
literals, boolean composition, categorical equality, and unsigned comparisons.
Exactly one selected declaration list is appended to the existing package
scope. Initial Symbol IDs remain stable; nested `else when` rows enter a later
deterministic materialization round. Public alias spans are rebuilt from that
selected source table. The subsequent `workspace-interfaces` boundary installs
the production-order predeclared scalar table, demands selected public
declaration products, classifies scalar constants versus type aliases, types
named scalars plus pointer, multi-pointer, slice, fixed-array, tuple, and
ordinary fixed-procedure structures plus declaration-owned `distinct` types,
ordinary named `struct` declarations with natural fields, ordinary `enum`
declarations, and lazily translates their reachable types and values into
package-independent interface IDs. A struct shell receives its defining root
content identity, root-relative package path, and declaration name before field
resolution, so `^Node` recursion is finite while direct by-value cycles remain
product edges. Struct completion publishes one source-order packet of field
names/types, natural byte offsets, total size, and alignment. Enum completion
uses the same nominal identity, retains each exact value as sign plus u128
magnitude, selects or validates the integer backing, and publishes layout equal
to that backing. Distinct, struct, and enum rows preserve the three-part nominal
key; importing and re-exporting copy it unchanged while rebuilding underlying,
member, or backing types locally and independently checking the nominal packet.
It then reconstructs each dependency declaration in the consumer's Type-ID
domain beneath the exact file-local import alias. The current closed subset
accepts named scalar aliases (including forward local aliases and qualified
imported aliases), boolean/unsigned/string constants over the existing evaluator,
structural/distinct/ordinary-struct/ordinary-enum type aliases and globals, and
non-parametric Draft procedures with fixed supported parameters and results.
Enum values currently admit implicit successors, full-width integer literals,
unary sign/grouping, and already-published local or imported unsigned constants.
Positive fixed-array counts may use representable untyped or exact `usize`
values already available through the scalar product graph. Variants, unions,
C enums, packed/bit fields, C or explicit-alignment structs, member
`when`/synthesis/directives, SIMD/parametric
types, foreign/export declarations, procedure contracts, arithmetic count or
enum-value expressions, and general constant/type work remain a diagnosed
interface staging boundary.
Imported constants are
available while building dependent interfaces but not during the earlier
package-`when` phase, whose dependency interfaces do not yet exist.
The staging executable is deliberately absent from the install set until a
coherent public compiler command surface exists.

Replacement proceeds at phase boundaries rather than by mixing C++ and Draft
inside a phase. C++ `draftc lex` and `draftc syntax` remain independent single-
file oracles while non-installed helpers expose the production C++ package and
workspace loaders as phase oracles. Registered differential tests supply every
Draft source under `core`, `compiler`, `examples`, `lib`, and `tools`, every
repository folder-package root, a four-target selection matrix, and phase-
specific malformed, mismatched-name, over-nested, empty, and missing inputs.
The tests also exercise the real frontend workspace graph plus workspace,
dependency, and core mappings; repeated imports; cycles; ambiguous prefixes;
missing and malformed imports; canonical symlink aliasing and escape rejection
when the host permits symlink creation; command-root containment; and the 256-
level import-depth bound. They compare stdout, stderr, and exit status exactly.
Focused same-package Draft tests separately exercise scanning, semicolon
insertion, syntax categories, child structure, parser recovery, filename
selection, and component-aligned dependency prefixes. A further process
differential compares the production C++ declaration collector with the Draft
collector over the real staging graph, source-only category/flag combinations,
package-wide and file-local duplicate diagnostics, all four target file tags,
deferred `when` declarations, and graph failure propagation. Later semantic
work can therefore consume canonical Draft-owned package rows, concrete trees,
semantic identities, import edges, package declaration name sets, and file-
local alias bindings directly; it does not need a compatibility call back into
the C++ parser, workspace loader, or declaration collector. The public-name
differential additionally intersects the production compiler's final accepted
PackageInterface/ImportedSymbol rows with those direct source declarations and
compares defining symbol identities, visibility filtering, per-file aliases,
canonical targets, and member order. It covers the real compiler graph,
workspace/dependency/core boundaries, all four target file selectors, and
earlier graph/declaration failures. Conditional public declarations remain
explicitly absent on both sides of this earlier gate.

The target-declaration differential obtains final condition decisions from the
production semantic product graph, replays them through the production source
collector, and compares those rows with Draft's product evaluator. It checks the
four versioned target fact sets, categorical and unsigned comparisons, guarded
`has_feature`, boolean composition, selected visibility/native flags, stable
symbol prefixes, selected public/import views, and earlier-phase failures. Its
named-value fixture covers forward and chained local dependencies, target-
derived categorical and numeric values, a named feature string, short-circuit
type checking, selected-branch constants feeding nested conditions, and an
unrelated unsupported arithmetic constant which remains dormant. That fixture
also guards the production scheduler rule that a selected declaration frontier
must receive condition, type, layout, and constant product identities before
the package-name barrier can close. Cyclic or demanded out-of-vocabulary
constants fail explicitly without selecting a branch. The real staging graph
exercises both sides of core/c_abi's target branch. The typed-interface
differential then compares canonical
scalar/structural/distinct/ordinary-struct/ordinary-enum graphs, fixed-array
counts, exact natural layouts and enum values, and consumer-local type/value
reconstruction across all four targets.
It includes forward local type/count aliases, imported type/value/count aliases,
all five moved data structures, structural globals and fixed procedures, target
page-size arrays, distinct declarations over scalar, structural, private, and
imported distinct types, plain/grouped/recursive structs, private and transitive
struct re-exports, structs nested in arrays/procedures/other structs,
inferred/explicit/signed/u128 enums, private and transitive enum re-exports,
enums nested in arrays/structs, and explicit specialized-aggregate/C-enum/
unsupported-expression/by-value-cycle failures.
General constant evaluation, remaining nominal and parametric type constructors,
procedure contracts, and body typing are the next semantic product slices.

The staging graph uses physical core source because the C++ bootstrap must
compile `draftc-next` before it can run. The installed bootstrap's immutable
embedded-core selection and transactional complete-file source overrides have
not yet moved to the Draft graph. They are source-provider seams rather than
changes to package identity or import resolution.

Frontend performance experiments use two explicitly requested, non-installed
executables rather than adding observation to either production driver. The C++
`draft-bootstrap-frontend-benchmark` target compiles private copies of the
source, lexer, parser, and concrete-tree modules with benchmark-only raw-scanner
and pre-tokenized-parser entry points. The Draft `draft-frontend-benchmark`
target stages `compiler/syntax/frontend_benchmark_support_bench.draft` under an
ordinary filename; the `_bench.draft` suffix otherwise excludes that support
from `draftc-next` and every normal package build. Both benchmark executables are
also excluded from CMake's default target. Consequently the experiment adds no
clock reads, counters, conditionals, callbacks, code size, or startup work to a
compiler product.

Each executable loads one identical valid source before measurement, then times
raw scanning (including UTF-8 validation), semicolon insertion, complete lexing,
parsing from a token copy prepared before the clock, and combined source-to-tree
construction. One warmup precedes ten samples of a caller-selected in-process
iteration batch. The report uses the median and includes a clock-only row rather
than subtracting an estimated timer cost. Loading, token preparation, result
checksums, destruction, sorting, and TSV rendering are outside timed intervals.
The comparison tool refuses to calculate ratios unless byte, token, node,
iteration, and checksum metadata match across implementations. This is
development evidence for locating costs, not release qualification or a public
CLI contract.

This boundary does not couple the self-hosted frontend to LLVM. LLVM 22 remains
part of the C++ bootstrap and native backend; the Draft source, diagnostic,
syntax, workspace, and semantic-name packages depend only on ordinary core
allocation, filesystem, formatting, and OS facilities. Backend migration can
happen later behind MIR/target interfaces without delaying source-to-qualified-
name and target-selection self-hosting.

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
stack. Parallel semantic and native workers measure into task-owned result
slots; after the synchronous run, the command thread appends completed events
and their
phase children in stable task-ID order. The recorder itself therefore remains
single-threaded, and scheduler completion order cannot change timing row order
or compiler results.

The events reflect the implementation's real orchestration rather than the
conceptual diagram alone. An ordinary handwritten `check` constructs one graph:
interface discovery installs declarations and types, then semantic continuation
checks bodies, effects, denials, and completed interfaces on those same package
rows. A native `build` continues that graph directly through procedure-owned
MIR and package-owned LLVM units, then invokes only the remaining platform
tools. O2 prepares one summary-bearing module per semantic package, then one
workspace ThinLTO product imports and emits their native outputs; a large
native-only O0 object build may use several package units. It does not reload
or recheck handwritten source.
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
Products owned by different packages execute concurrently through the bounded
wave executor. Generic-owner products use the same frozen-prefix/task-append
representation as body products. Authored declaration-type products add only
row-granular patches for collected type and symbol identities. Sibling products
may therefore execute independently even when they target one package; the
coordinator publishes every packet in product-ID order. Name-set and interface
tasks are placed after read-only work and serialized because validation-context
loading can extend the command-owned source table; no worker reads that table
while another worker changes it. Those package barriers move the retained
`CompiledPackage` into the coordinator-owned task slot and restore it during
publication rather than copying its complete semantic tables. The graph must
therefore keep a package barrier out of any ready wave containing another
product for that package; the coordinator checks this invariant before moving
the payload.
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
invalidation, canonical initial product construction, and dependency-ready
package closure. Building its sorted views
costs O((packages + imports) log(packages + imports)); invalidation costs
O(packages + imports), without a persistent cache. The `compiler passes`,
`workspace loads`, `workspace source transitions`, `package body starts`,
`procedure bodies checked`, `external procedure bodies materialized`, and ABI
classification wave/task/worker counters make those distinct operations
visible. `--timings=all` adds package/tool scopes, file discovery and I/O,
lexing/parsing, import-graph resolution, declaration ready-wave execution and
publication, worker time and task count by semantic product kind,
package-closure ready fronts, joined effect/reference work, procedure-flow
subphases, direct-semantic assembly work, and per-package LLVM
parse/verify/optimize/code-generation phases.
Exclusive time remains meaningful inside sequential groups; independently
scheduled child durations may overlap their parent wall time. Child process CPU
is reported separately from parent wall time.

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
effects, denials, metadata/obligation context, native interop, validation,
assembly, and MIR consume the exact selected arenas without concatenating them.
When an operation follows procedure references, it keeps each procedure beside
the local arena which owns its HIR IDs. Procedure products are therefore the
only workspace HIR representation.

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
workers run through the command-owned `WorkExecutor`. Results return before
deterministic publication. Worker count is scheduling policy only: one-worker
and four-worker qualification compares product graphs, diagnostics, semantic
table sizes, and final LLVM bytes. Dynamic discovery still occurs only between
waves, never by concurrent mutation of the semantic graph. A one-worker wave
runs directly on the calling thread. The first parallel wave starts one bounded
pool with an explicit eight-MiB stack per worker; the same sleeping workers
serve complete-file front-end tasks and later semantic, LLVM, assembly, and
artifact graphs until the command ends. The executor retains no syntax or
compiler product between runs, so this is thread reuse rather than semantic
memoization or a cross-command cache.

Automatic executable-root discovery and IDE source inventory borrow that same
executor; neither operation creates a preliminary package-loader pool before
the command or project session reaches compilation.

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
result. Direct subsystem callers return the same live `PackageBodyWorkState`;
there is no second body-state carrier. Pending roots from every package now
enter one workspace-wide frozen ready set. Product IDs are appended in
PackageId/work order, workers may execute the complete independent set
concurrently, and package-local semantic suffixes publish in that same canonical
order after join. A newly materialized external root carries an explicit edge
to the completed consumer body which exposed its demand. Cross-package demand
discovery and retained-product selection repeat
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

The LLVM adapter has one direct ownership operation: emit one package-owned
LLVM unit from immutable semantic inputs and an ordered live-MIR subset. O2,
assembly, and retained-IR requests use one complete unit for the semantic
package. A native-only O0 object build may divide more than 48 live procedures
into fixed 48-procedure units so target-machine work can run concurrently.
Unit zero owns all live globals, relocatable initializer storage, source debug
metadata, and any requested hosted entry point; later units obtain hidden
external declarations for cross-unit procedures and globals on demand.
Invariant hosted runtime implementation is one separately compiled target
object embedded in the compiler distribution; the root artifact layout selects
it by exact target identity rather than reconstructing it inside unit zero.
Procedure MIR is not
reassembled into an owning `MirProgram`; the emitter borrows the immutable
payloads from their product side-table rows in canonical package order. This
gives LLVM the natural package optimization scope while retaining procedure-
granular semantic checking and MIR lowering.

Compiler orchestration publishes one or more `PackageLlvmUnit` products. Each
depends only on the direct-reference and `MirProcedure` products assigned to
that deterministic unit plus shared target, package-interface, ABI, and
reachability facts. For a native command the same task immediately publishes
the requested object or assembly bytes; LLVM text is retained only for the
single complete unit used by an explicit IR consumer or qualification oracle.
One `ArtifactLayout` product per package then publishes every unit in unit-index
order followed by hosted runtime and authored assembly where applicable. MIR,
LLVM-unit construction, and layout do not run as three workspace-wide batches.
One closed native execution graph mirrors the exact semantic edges: a unit
starts when its assigned MIR slots finish, and its layout may finish while
unrelated package MIR is still running. Workers write only fixed private slots.
After the executor joins, the coordinator replays ordinary semantic ready waves
and moves payloads into canonical rows in product order.

Published layouts then form the independent artifact-materialization ready set.
Ordinary package rows borrow their already emitted bytes without copying;
the root's hosted-runtime row borrows immutable embedded bytes; package-assembly
workers own private assembler paths. Relocatable object materialization skips
that runtime row so its references remain for the final consumer. The explicit Clang oracle
is the only later stage that consumes retained LLVM text. The command thread
waits for that set, selects diagnostics by lowest stable task ID, and only then
publishes
files and linker inputs in task-ID order. Parallel
scheduling changes elapsed time, never artifacts or diagnostics. Direct
subsystem tests call the same complete-package emitter as compiler
orchestration.

### Internal representations

- **Surface AST:** lossless enough for diagnostics, structural site identity,
  generated-source maps, and exact grammar-category replacement.
- **Semantic product graph:** an append-only command-local table of explicit
  product kinds, dependencies, and states. Eager target/source/parse/import
  inputs begin complete; semantic tasks move through frozen ready waves.
  Source transitions append successors and supersede unselected generations.
  Payload side tables remain typed and phase-owned rather than entering a
  generic graph value.
- **Declaration semantic products:** package declarations, imports,
  signatures, constants, layouts, and preliminary interfaces have stable IDs
  and remain immutable after publication.
- **Body semantic tables / typed HIR:** the coordinator owns the canonical
  body prefix; each procedure product owns its semantic suffix and HIR arena.
  HIR-local IDs start at zero and remain local through every workspace
  consumer. No HIR is paired with declaration-only tables.
- **Target ABI facet:** one immutable row per source-semantic TypeId, published
  after body closure with explicit target and exact type-producer edges. An
  `Illegal` row is a completed answer. Later consumers read the table through
  the target-matching semantic prefix and never invoke the classifier.
- **Procedure CFG:** explicit branches and scopes, used for return analysis,
  `defer`, branch facts, judgments, and denial summaries.
- **Draft MIR:** a small non-optimizing IR with explicit loads, stores, checks,
  context arguments, calls, aggregate operations, and source locations. One
  artifact-live runtime HIR procedure lowers to one privately verified MIR
  procedure; workspace compilation stores that result only in the side-table
  row owned by its `MirProcedure` product. Package rows retain ordered product
  IDs, not a reconstructed `MirProgram`. All authored bodies are checked first;
  semantic closure publishes direct native-reference rows for every concrete
  runtime body and validates parsed assembly beside direct-effect discovery,
  including non-artifact checks. One explicit artifact closure later selects
  the runtime procedures admitted to the exact native dependency executor. A
  package LLVM unit borrows its completed ordered MIR slots only after their
  executor edges complete. O2 owns one complete package input plus one explicit
  workspace ThinLTO product depending on every such input; native-only O0 may
  own fixed-size internal units. LLVM is an emission/optimization back end
  rather than Draft's semantic model.

HIR storage expressions retain the minimum alignment guaranteed by their exact
occurrence, separately from logical type alignment. Member and index address
formation propagates that power-of-two guarantee through byte offsets. MIR
address instructions preserve it, MIR loads/stores record the clamped access
alignment, and LLVM emits that exact fact. This is what permits direct packed
field access without weakening the alignment promised by ordinary `^T` values.
Bit-field members additionally retain their exact owner-relative bit range in
HIR. MIR uses explicit bit loads/stores against the first containing byte;
assignment lowering never manufactures a typed pointer to the field.

MIR assertion and bounds instructions retain their source-level checks. LLVM
lowering emits the successful comparison in the package control-flow graph and
places the hosted-runtime helper call in a non-returning failure block. This
preserves the runtime handler and diagnostic ABI while exposing the ordinary
path to LLVM so it can prove or combine checks. Assertion operands have already
been evaluated before this branch; assertion-disabled compilation instead
removes their evaluation while lowering HIR to MIR.

LLVM types stay behind numeric, target, ABI, and code-generation adapters. The
front end must not depend on LLVM IR details.

### Native target boundary

One selected versioned profile fixes the triple, LLVM data layout, pointer
width, C ABI rules, CPU features, object format, relocation/code/TLS models,
trap behavior, linker contract, and parsed-assembly capability. Target-specific
classification lives under this boundary rather than leaking into type
checking. The current constructors cover AArch64 macOS, AArch64 GNU/Linux, and
x86-64 GNU/Linux; a profile may provide package assembly without a parsed
inline-assembly grammar.

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
independent ordinary compiler graph. Workspace build defaults, the matching
named-program overrides, and explicit CLI overrides are resolved separately for
each root. Because target-qualified source can declare `main`, named roots with
a different configured target are inspected exactly under that target before
the canonical root list is finalized. Discovery never changes import resolution
or merges multiple executables into one semantic graph.

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
