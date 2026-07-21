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
identities between them. ABI classification is not yet a product.
Concrete cross-package owner-evaluated type applications now use canonical
command-local demand products with exact requester-layout and transitive-owner
edges; no package is rebuilt to publish their results.
Procedure HIR and semantic append packets are now owned by exact live body
products. Type, symbol, and constant prefixes use read-only overlays, and
declaration-closed semantic inputs are direct immutable views. Owned scopes,
aggregate members, enum values, parametric parameters, and static argument
packs also use canonical-prefix and task-local-suffix views. Aggregate member
offset mutation is explicitly restricted to a task's local suffix. Other
body-mutable semantic side tables are still copied into each task view. Append
IDs still require sequential exact-prefix publication rather than shared-wave
remapping.
Effect closure, denials, and MIR still consume a package-wide HIR compatibility
projection. Those mechanisms are explicit remaining deletion work in the
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
