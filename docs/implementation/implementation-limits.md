# Bootstrap implementation limits

These are explicit crash-safety and bounded-context contracts of the bootstrap compiler. They are not general Draft language limits unless the normative specification says otherwise.

## Recursive implementation resource limits

Status: bootstrap crash-safety contract; not a Draft language limit.

The direct recursive implementation has separate budgets for graphs that are
independent in the language. Parsed declarations, members, types, expressions,
and statements share a 512-level syntax-nesting budget. After parsing, an
acyclic package-import chain is limited to 256 loaded levels, and the type
resolver independently limits forward declaration dependencies to 256 active
declarations. Compile-time constant bindings have their own 256-binding
dependency budget; compile-time procedure calls retain the separate 64-call
recursion budget plus the existing execution-step and value-size limits.

These guards report stable source diagnostics before the host C++ stack becomes
the accidental limit. Cycle diagnostics remain distinct: each graph checks its
visited or active state before applying the acyclic-depth bound. Structural
walks over HIR, MIR, interface types, effects, and constants either follow the
already bounded syntax/type shape or install a visited/cycle row before
following children.

## Semantic product scheduling boundary

Status: implemented bootstrap architecture; not a Draft language limit.

The command-local semantic product graph owns target/source/parsed inputs,
package name and interface barriers, opaque synthesis waits, declarations,
constants, faceted type completion, procedure HIR, effect closure, denials,
per-procedure MIR, package assembly, package LLVM units, artifact layout, and
source-generation transitions. Collection and import binding happen
once per source generation; selected package branches append into the retained
canonical tables. Declaration tasks use frozen patch-enabled views, generic and
body tasks return append packets, and constant/condition tasks return isolated
values or selections. The coordinator publishes every result in stable product
order. Interface-synthesis discovery uses these same products; there is no
aggregate declaration/constant discovery path. Aggregate-member conditions have
independent products and exact dynamically discovered continuation edges.
Member-name and member-type readiness are separate products with stable member
identities between them.

Every selected source file is read, lexed, and parsed as one independent task
using a private source island. Publication into the command SourceManager
remains serialized by canonical filename so FileIds and diagnostics are
deterministic; the parser itself is not subdivided within one file.

After the workspace body fixed point, every canonical
source-semantic TypeId has one target-specific `TypeAbiClassification` product.
Declaration-baseline rows depend on the package interface, while a type first
published by a procedure depends on that exact procedure product. All packages
share one bounded classification wave; payload and graph publication remain in
product order. Native validation, C-header emission, and LLVM lowering consume
the published table and never rerun the classifier.
Concrete cross-package owner-evaluated type applications now use canonical
command-local demand products with exact requester-layout and transitive-owner
edges; no package is rebuilt to publish their results.
Procedure HIR and semantic append packets are now owned by exact live body
products. Type, symbol, and constant prefixes use read-only overlays, and
declaration-closed semantic inputs are direct immutable views. Owned-scope,
aggregate/enum, parametric, specialization, imported semantic, and
dependent-type recipe, semantic-site, and declaration-denial records also use
canonical-prefix and task-local-suffix views. Imported rows cover symbols,
types, concrete procedures, outbound type requests, and effect/return/write
contracts. Recipe rows cover required integer expressions and deferred element
counts, value expressions, and type applications. Aggregate offset,
procedure-specialization, required-integer, and semantic-site mutation are
explicitly restricted to a task's local suffix. No retained semantic table is
copied into a body task. Ready body tasks share one frozen prefix; deterministic
publication remaps task suffix IDs, interns structural types, and canonicalizes
equal procedure and nominal type specializations. The bootstrap driver invokes
the isolated tasks through its command-owned `WorkExecutor`; task-indexed
diagnostics and products publish only after the synchronous run. Interface
waves also use that
executor: different package owners run concurrently, and generic-owner tasks
return `SemanticTaskAppend` packets so same-package demands are independent.
Authored declaration-type products return the same append packet plus exact
patches for their collected `TypeId`/`SymbolId`, so independent same-package
declarations also share a ready wave. Name-set and interface tasks are isolated
after read-only work because validation loading may extend the shared source
table. One-worker runs stay on the command thread. The first larger run starts a
bounded pool with an explicit eight-MiB worker stack so authored syntax
recursion has the same practical budget in sequential and parallel execution.
Later compiler and backend graphs reuse those workers; the executor retains no
semantic table, task result, or artifact between runs.

Automatic build-root discovery and the IDE's source inventory also borrow this
executor, so a command/session has no preliminary package-loader pool. The
scheduler validates and constructs each run's reverse dependency rows in one
pass rather than rebuilding that orchestration data before execution. Edgeless
graphs skip cycle traversal, and a singleton ready front runs directly without
installing shared scheduler state or waking the pool.

Workspace packages retain the live body publication state after finalization,
so a newly demanded external specialization appends to the existing work and
product prefix rather than reconstructing an extension scheduler. Authored,
current external, and prerequisite-reachable bodies form an explicit selected
projection. Removed demands leave completed products immutable but exclude them
from transitive discovery, metadata, effects, interfaces, lowering, and native
emission. Exact product-owned outbound requests and a selected-HIR proxy scan
keep transitive demands live even when their original discovering product is no
longer selected. There is no aggregate demand-set comparison, demand-removal
rebuild, or declaration-generation body work key. All packages now contribute
their pending procedure products to one workspace-wide frozen ready set. New
external owner bodies depend on the exact completed consumer product which
requested them; package-local semantic suffixes still publish in PackageId/work
order after the ready wave returns. No semantic-wave payload imposes a
package-local execution chain; only package name/interface barriers which may
load validation source are serialized after read-only task slots join. Those
barriers may invoke the complete-file scheduler synchronously on the
coordinator, so the command executor is never re-entered from one of its own
workers.
Final effect/interface closure likewise uses dependency-ready package fronts.
Preparation and flow closure run as package tasks; direct effects and denials
from every package in the front share procedure waves. Completed SCC products
retain exact local edges, and one package effect-closure barrier owns the full
component set consumed by importers. This avoids repeating every imported SCC
on every consumer component while preserving dependency invalidation.
Diagnostics publish only after join in PackageId and phase order, and an invalid
dependency never unlocks its consumer.
Parsed assembly consumes the same checked-body frontier as direct effects and
runs in that ready executor rather than waiting for a workspace-wide native
phase. It is therefore available to `check` as well as later artifact commands.
O2 package LLVM tasks publish summary-bearing bitcode rather than native bytes.
One package-less `WorkspaceThinLto` product depends on every package unit, runs
LLVM's internally parallel backends, and publishes native buffers back to
package artifact layouts in canonical order. This is command-local memoization
of completed graph products, not a cross-process cache; no ThinLTO cache is
configured.
Per-package phase and declaration-generation counters are deleted. Source
transitions supersede exact product rows; body initialization and closure reuse
are derived from retained product/payload invariants rather than a parallel
phase label.
MIR lowering reads the shared `TypeStore` immutably. Compiler-only addresses
use MIR-local addressed-type metadata, so there is no unclassified post-ABI
type suffix. Direct effects, denials, parsed assembly, and MIR consume
authoritative procedure-owned HIR arenas and publish live products; they no
longer use a package-wide HIR compatibility projection.
Direct-effect workers share one immutable package lookup context rather than
reconstructing terminal contracts and type paths per procedure. Flow closure
copies that context once, executes acyclic transfers once, and re-enters only
HIR rows which consumed a local procedure-value flow contract; legal recursive
flow components retain their explicit finite fixed point.
Metadata/obligation context, native interop, and validation discovery also
resolve only the exact selected procedure products. Each completed MIR product
owns its procedure
payload directly in the workspace side table; the compiler retains neither a
package-wide HIR copy nor a reconstructed package MIR program. The standalone
HIR projection and package MIR container/lowering pass have been deleted; direct
subsystem tests use the same procedure-product lowering and package LLVM emitter
as compiler orchestration.
Definite-initialization diagnostics and agent loop-range facts are produced by
the isolated body task before its semantic suffix is published; package
finalization no longer needs a temporary HIR projection.

LLVM and native lowering now follow artifact-live semantic products. Every
authored body is checked, but each concrete runtime procedure first publishes a
direct native-reference summary during complete semantic closure, including a
non-artifact check. Parsed package assembly is also retained during that
closure. One workspace reachability product later closes from the authored root
`main`, exports, or selected validation entries through calls, procedure values,
and globals. Object, archive, and assembly output keep an authored `main` live
even when only executable output contributes the hosted C `main`/`wmain`
wrapper.
Only its live rows receive independently owned MIR products. O2, assembly, and
retained-IR requests publish one complete `PackageLlvmUnit` per package. A
native-only O0 object request divides a package with more than 48 live
procedures into canonical 48-procedure units. The threshold and partition are
independent of worker count; unit zero owns all live globals and package entry
wrappers, while later units use ordinary hidden external declarations for
cross-unit references. MIR, LLVM units, and layouts share one exact closed
executor, so a unit waits only for its assigned MIR rather than a package or
workspace barrier. Each native unit publishes matching bytes and discards LLVM
text unless the single complete unit has an explicit IR consumer. Every package
publishes a deterministic `ArtifactLayout` over its ordered units and assembly.
The artifact planner consumes only that layout; no package-wide MIR container
or alternative persistent per-function LLVM representation remains in compiler
state.

Every package LLVM task constructs its module through the single direct LLVM
C-API builder, including root/validation wrappers and source debug metadata,
and continues that same task-private module into verification, optimization,
and native emission. An explicit inspection or qualification request prints
that already-built module; the external textual adapter may parse the retained
text as an independent LLVM/toolchain consumer oracle. There is no second
Draft-to-LLVM textual emitter. The hosted runtime is a separately compiled
object embedded for each exact target. There is no production print/reparse
boundary and no alternate-emitter fallback for unsupported operations.

## Self-hosted frontend staging limits

Status: build-tree source, lexer, parser, folder-package, canonical import-graph,
declaration-name, unconditional qualified-name, target-conditional declaration,
and scalar/structural/distinct/ordinary-struct/ordinary-enum/ordinary-variant/
ordinary-union typed-interface replacement; not a Draft language or public CLI
limit.

On a supported native host, the build graph uses C++ `draftc` to produce an O2
Draft executable named `draftc-next`. Its accepted commands are
`draftc-next lex <file.draft>`, `draftc-next syntax <file.draft>`, and
`draftc-next package-syntax <package> --target <selector>`. Its
`workspace-syntax` qualification command additionally takes explicit workspace
and physical-core roots, a pinned core identity, the target, and optional
dependency prefix/root/identity mappings. It is not installed or included in
release archives. `workspace-declarations` accepts the same graph arguments and
additionally emits shared package declarations plus file-local aliases bound to
canonical import targets. `workspace-public-names` additionally emits direct
unconditional public symbols, contiguous per-alias imported-name spans, and
stable target `(Package_Id, Symbol_Id)` references used by two-part qualified
lookup. `workspace-target-declarations` selects package `when` branches through
target categorical/numeric facts, known `has_feature` queries,
boolean/string/unsigned literals, boolean composition, supported comparisons,
and demand-driven local scalar constant products over that same vocabulary.
Forward and chained names publish once through explicit product edges; cycles
stall visibly; constants which no condition demands remain dormant. It appends
the chosen source declarations without rebuilding the unconditional Symbol-ID
prefix, then rebuilds the public alias spans. `workspace-interfaces` continues
dependency-first through a deliberately closed typed subset: every package gets
the production-order predeclared scalar table; demanded public declarations
classify type aliases versus boolean/unsigned/string constants; named scalars
and the structural `^T`, `[^]T`, `[]T`, `[N]T`, `(T, U)`, and ordinary fixed
`proc` forms plus declaration-owned `distinct T` type aliases, explicit globals,
ordinary named natural-layout `struct` declarations, ordinary `enum`,
`variant`, and `union` declarations, and procedure signatures; reachable
types and constants are rewritten into canonical package-interface IDs; and
each imported declaration is reconstructed in the consumer Type-ID domain
beneath its file-local alias. One shared arbitrary-precision integer evaluator
accepts literals, local/imported unsigned constants, target numeric facts,
grouping, unary sign, and binary `+`, `-`, `*`, `/`, and `%`. Untyped operations
remain mathematically exact regardless of intermediate width, while concrete
unsigned types no wider than 64 bits wrap at their declared width. Named
interface constants currently publish only nonnegative u64 results. Fixed-array
counts consume the same evaluator and require a positive result representable
by the target-sized count packet from an untyped value or exact `usize`.
Forward local type/count aliases and qualified imported type/value aliases are
supported. Distinct interface rows retain the defining root content identity,
root-relative package path, and original declaration name across direct and
transitive imports; their underlying type is rebuilt in each consumer. Nominal
structs retain that same identity plus source-order field names/types, natural
byte offsets, size, and alignment; a consumer memoizes the nominal shell before
recursive import and verifies the packet by recomputing natural layout. Pointer
recursion is supported, while direct by-value cycles fail as graph cycles.
Ordinary enums retain the same identity, exact source-order alternative names
and signed/u128 values, explicit or inferred integer backing, and backing
layout. Their explicit values consume the shared exact integer evaluator;
implicit successors retain checked sign/magnitude arithmetic.
Ordinary variants retain the same identity, source-order alternative names and
payload types, explicit or smallest-fitting unsigned discriminator, common
payload offsets, and exact natural layout. Payload-free alternatives use
`void`; pointer recursion is supported while by-value cycles remain graph
cycles. Ordinary unions retain the same identity, source-order field names and
types, zero byte offsets, and maximum-member natural layout; grouped fields and
pointer recursion are supported. C enums, packed/bit fields, C or explicitly
aligned aggregates, selected/synthesized/directive members, SIMD types,
parametric forms, foreign/export declarations, procedure contracts, bitwise/
shift/cast/comparison integer expressions, negative or wider named scalar
publication, and general constant/type forms fail explicitly at this interface
boundary.
The `compiler/big_integer` package implements and differentially qualifies the
arbitrary-precision values now consumed by this evaluator:
parsing, sign/magnitude arithmetic, division, shifts, infinite-two's-complement
bitwise operations, fixed-width conversion, and decimal formatting against the
production C++ `BigInteger`. One root evaluation owns a temporary value table,
uses stable IDs across table growth, and projects only its final result into the
finite scalar, enum, or array interface packet before destruction. The remaining
u64/u128 limits above are publication boundaries, not intermediate arithmetic
limits.
Imported constants remain unavailable to the earlier package-`when` selector.
Deeper nominal member lookup, full type/body checking, elaboration, MIR, LLVM,
artifacts, and linking have not moved to the staging driver. The C++ driver
remains the public compiler and compatibility oracle.

The Draft lexer has the same token spellings, ranges, semicolon insertion,
diagnostic rendering, stdout/stderr bytes, and exit behavior as the bootstrap
over its registered differential corpus. The Draft parser likewise has the
same concrete node hierarchy, token spans, recovery diagnostics, nesting-limit
failure, stdout/stderr bytes, and exit behavior over its own registered corpus.
The Draft package loader likewise matches production package discovery, target
and validation suffix selection, bytewise file ordering, source loading,
multi-file parsing, assembly inventory, package-name diagnostics, and process
behavior across repository packages and explicit four-target/failure fixtures.
The Draft workspace loader additionally matches explicit root validation,
canonical semantic identity, recursive first-discovery order, import occurrence
order, mapping disambiguation, alias deduplication, containment, cycle handling,
and the package-depth bound against the production C++ loader. This is package-
graph compatibility evidence. The declaration differential additionally proves
package/file scope construction, source-only symbol classification, visibility
and native/parametric flags, package-wide and file-local duplicate behavior,
and exact import-alias-to-graph-edge binding against the production C++
collector. It does not prove dependency interface import or typed semantics.
The public-name differential additionally compares each unconditional source
public symbol with the production compiler's final accepted PackageInterface,
then compares every file-local alias member with production ImportedSymbol
binding. It proves visibility filtering, no import re-export, stable defining
symbol references, canonical targets, alias locality, and source ordering for
that earlier view. Conditional public names are deliberately filtered out of
that earlier command's oracle and Draft result; the later target-declaration
command rebuilds them after selection. Neither view is yet a serialized or
typed package interface. The typed-interface differential then compares
canonical reachable
scalar/structural/distinct/ordinary-struct/ordinary-enum/ordinary-variant/
ordinary-union type rows, including array element counts, struct fields/layouts,
enum alternatives/backing values, variant discriminator/payload layouts, union
field/overlay layouts, declaration classifications, constant payloads, and
consumer-local imported type/value shapes with production on all four targets.
Its focused fixture covers forward local type and array-count dependencies,
qualified imported type/value/count aliases, local/forward/imported/target
integer arithmetic, concrete unsigned wrapping, signed quotient/remainder, and
checked u128 multiplication,
pointer/multi-pointer/slice/array/tuple/procedure type
aliases, structural globals and procedure signatures, tuple results, and
target-dependent page-size arrays, distinct declarations with identical and
structural underlyings, private distinct exposure, identity-preserving
transitive re-export, plain/grouped/pointer-recursive structs, private and
transitive struct exposure, nested struct arrays/signatures,
inferred/explicit/signed/u128 enums, enum exposure/re-export, imported enum
constants, and enums nested in other supported constructors. The focused
fixture also covers payload-free/typed/recursive variants, explicit/inferred
and distinct-integer discriminators, private/transitive variant exposure, and
variant nesting. Size-rounding/grouped/recursive and target-selected unions,
private/transitive union exposure, and union nesting are covered as well. The
oracle additionally verifies that C-layout, explicitly aligned, combined C and
aligned, and selected-member unions are valid production inputs before the
self-hosted command rejects them at its staging boundary; synthesized union
members have their own fail-closed fixture.
Separate fixtures require C enums, invalid enum arithmetic, invalid
variants/unions, specialized aggregate layouts, SIMD types, unsupported
array-count operators/types, arithmetic division by zero, a final enum value
beyond the u128 packet, negative/wider named scalar publication, and local alias
or by-value layout cycles to fail at the exact self-hosting boundary. This
evidence applies only to that
closed subset, not to the full production PackageInterface vocabulary or
serialization.

The target-declaration differential replays the production compiler's published
condition selections through its source collector and compares Draft's selected
condition order, stable symbol suffix, source classification/flags, and rebuilt
public/import view on all four profiles. The real compiler graph and explicit
expression fixtures cover both selected branches, guarded architecture feature
queries, earlier failures, local forward/chained scalar constants, target-
derived named values, selected-branch dependencies feeding nested conditions,
short-circuit validation, and dormant unrelated constants. Demanded arithmetic
and cyclic constants exercise the explicit fail-closed staging boundary.
Embedded-core package rows and complete-file source overrides still use the
bootstrap path; the staging graph currently accepts physical core source. Each
later phase or source-provider transition must add its own focused tests and
complete process-level oracle before the corresponding C++ path can be removed.

`compiler_syntax.Token_List`, the syntax tree's typed node/child tables, and the
workspace graph's concrete root/package/import/diagnostic/visit tables currently
implement contiguous allocation directly. The bootstrap rejects an embedded
`core/array.Dynamic[T]` consumed across different source files of one package
with a pointer type-identity diagnostic, including when the generic argument is
explicit. This is a bootstrap generic-instantiation limitation, not missing
array semantics or a Draft 1 ownership rule. The direct tables preserve the
intended data layout, stable integer IDs, and explicit allocator lifetime; they
can return to ordinary core containers after that compiler limitation is
repaired and qualified.

`compiler_sema` uses ordinary `core/array.Dynamic` owners but deliberately keeps
the complete initial name-set representation and its operations in one source
file. Splitting that package while the same bootstrap limitation remains would
make its imported generic field types disagree across modules. This is a source-
organization constraint on the staging compiler, not a semantic phase boundary.

## Native host and instrumentation limits

Status: four hosted target profiles; three complete native test harnesses plus
one Windows native build/launch smoke gate.

The bootstrap compiler runs and executes its complete native integration suite
on AArch64 macOS, AArch64 GNU/Linux, and x86-64 GNU/Linux. The x86-64 Windows
host builds the bootstrap and launches every ordinary example, target package
assembly, foreign-provider executable, and an independent C client/Draft DLL
pair. It links a selected LLVM 22 library (LLVM-C on Windows) for ordinary
package-module object emission. Matching
Clang/`ld.lld`/`llvm-ar`/`dsymutil`, the Apple linker, `libtool`, SDK, and system
runtime remain ordinary tooling prerequisites rather than Draft program inputs.
The Windows bootstrap still lacks validation-process execution, durable
resolution/evidence-store locking, and provider-backed Codex subprocess
support. Ordinary provider-free checking, compilation, linking, core/runtime
use, and execution do not depend on those operations. Any resolve that must
write or re-pin a manifest fails explicitly until the resolution lock is
implemented. COFF whole-graph object output is limited to a single native input
because the format has no relocatable partial link; multi-input consumers use a
`.lib`.
Other Linux libcs/distribution contracts
and other architectures still have no target profile. Parsed inline assembly
remains AArch64-only; x86-64
supports ordinary native code and target-qualified package assembly but rejects
selected `asm` constructs.

AddressSanitizer is qualified only for the macOS target. Linux and every other
instrumentation request remain fail-closed until the compiler pass, runtime,
deployment, execution environment, and evidence identity are specified and
tested together. Requested Linux ELF debug information is embedded in the
primary artifact; split debug packages are not implemented.
