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

## Semantic product migration limit

Status: temporary bootstrap architecture limit; not a Draft language limit.

The command-local semantic product graph currently owns target/source/parsed
inputs, package name and interface barriers, opaque interface synthesis waits,
authored declaration types, non-parametric nominal layouts, conditional
choices, named constants during complete compilation, and their
source-generation transitions. Collection and import binding happen once per
source generation; selected package branches append into that retained table.
Ready tasks use private package copies and the coordinator publishes their
results deterministically. Interface-synthesis discovery still uses the
aggregate declaration/constant path. Aggregate-member conditions now have
independent products and exact dynamically discovered continuation edges.
Member-name and member-type readiness are separate products with stable member
identities between them. After the workspace body fixed point, every canonical
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
the isolated tasks through its bounded closed-wave executor; task-indexed
diagnostics and products publish only after join. Other package semantic waves
remain sequential where their payloads still use package snapshots. One-worker
runs avoid thread creation; larger pools on the supported POSIX hosts use an
explicit eight-MiB worker stack so authored syntax recursion has the same
practical budget in sequential and parallel execution.
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
order after the whole worker set joins. Package loops whose payloads still use
package snapshots remain sequential.
MIR lowering still interns address-only pointer types into the shared
`TypeStore`. These form an unclassified suffix after the completed semantic ABI
prefix and cannot appear in a source C signature. Removing that suffix mutation
belongs to the per-procedure MIR migration; it is not a second ABI path.
Effect closure, denials, and MIR still consume a package-wide HIR compatibility
projection. Each operation builds and discards that view from authoritative
procedure products; the compiler retains no package-wide HIR copy. Migrating
those consumers away from the temporary projection is explicit remaining work
in the
[semantic work graph implementation plan](semantic-work-graph-implementation-plan.md),
not alternate final architecture paths.

## Native host and instrumentation limits

Status: explicit two-target bootstrap boundary.

The bootstrap compiler runs and executes its complete native integration suite
on both AArch64 macOS and AArch64 GNU/Linux. It links a selected LLVM 22 library
for ordinary package-object emission. Matching Clang/`ld.lld`/`llvm-ar`/
`dsymutil`, the Apple linker, `libtool`, SDK, and system runtime remain ordinary
tooling prerequisites rather than Draft program inputs. Draft currently emits
only AArch64 machine code; x86-64 hosts can build and sanitize the bootstrap
compiler, but cannot execute Draft's native integration programs.

AddressSanitizer is qualified only for the macOS target. Linux and every other
instrumentation request remain fail-closed until the compiler pass, runtime,
deployment, execution environment, and evidence identity are specified and
tested together. Linux ELF debug information is embedded in the primary
artifact; split debug packages are not implemented.
