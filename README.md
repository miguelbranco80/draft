# Draft

Status: Draft 1 language design, revision 2. Syntax is provisional.

This split edition is the Draft language specification. The preserved
[monolithic revision-2 document](../agent-native-systems-language-v2.md) remains
intact.

<a id="section-1"></a>

## 1. Purpose

Draft is an Odin-inspired systems language in which agent synthesis is a typed,
scoped, reproducible compiler operation.

It targets native executables, static and dynamic libraries, embedded systems,
kernels, codecs, databases, games, and other software where layout, allocation,
calling conventions, assembly, generated machine code, and build determinism
matter.

The programmer writes the architecture, public interfaces, types, constraints,
and selected algorithms. The compiler can synthesize expressions, procedure
bodies, and declarations that fit those boundaries. Synthesized code becomes
ordinary inspectable native source and passes the same compiler checks as
handwritten code.

<a id="section-2"></a>

## 2. Design principles

- Native performance with predictable memory and ABI layout.
- Fast parsing, checking, and compilation.
- Simple Odin-like declarations and folder packages.
- Explicit library imports with no implicitly injected package declarations.
- Manual memory management through explicit allocators and scoped context.
- Private-by-default package declarations with explicit `pub`.
- One procedure abstraction for all callables.
- Static nested procedures with explicit runtime state and no hidden captures.
- Rich compile-time types with concrete native lowering.
- Native parsed assembly with typed operands.
- Direct C ABI import and export.
- Compiler denials over ordinary resolved names and built-in constructs.
- Runtime and compile-time assertions with explicit failure semantics.
- `...` as a typed synthesis construct in complete surface programs.
- Optional agent judgments as pinned validation evidence, never runtime semantics.
- Deterministic locked builds from pinned, inspectable expansions.
- Semantic compiler context for agents rather than indiscriminate source dumps.

## Specification map

- [Core language (§§3–4)](01-core-language.md) — packages, declarations,
  expressions, constants, and control flow.
- [Types, memory, and runtime (§§5–7)](02-types-memory-runtime.md) — native
  types, layout, storage, context, concurrency, entry, and core libraries.
- [Design context and agent synthesis (§§8–10)](03-agent-synthesis.md) — durable
  documentation, judgments, `...`, resolution, and deterministic builds.
- [Native interop (§§11–12)](04-native-interop.md) — parsed assembly, C ABI,
  foreign imports, exports, and linking.
- [AArch64 parsed assembly profile](AARCH64_ASSEMBLY_PROFILE.md) — the exact
  closed inline instruction and operand grammar for the first target.
- [Denials and validation (§§13–14)](05-denials-validation.md) — semantic
  restrictions, tests, instrumentation, and performance evidence.
- [Compiler architecture (§15)](06-compiler.md) — lowering, semantic context
  construction, evidence, and dependency-ordered elaboration.
- [Future ideas (§16)](07-future-ideas.md) — prospective layout, GPU, and
  raw-assembly extensions.

## Bootstrap compiler

The bootstrap compiler is being implemented in a deliberately direct C++20
subset under the rules in [AGENTS.md](AGENTS.md) and the sequencing in
[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md). The checked-in implementation
currently includes source ownership, structured diagnostics, the full lexical
token vocabulary, UTF-8 and literal validation, Draft semicolon insertion, a
complete surface syntax tree and recovering parser, and deterministic
target-qualified folder-package loading. The semantic foundation now loads an
acyclic, explicitly rooted package graph; derives public package interfaces;
binds file-local imports without leaking package-local IDs; assigns stable
scopes, symbols, and types; rejects ignored declaration modifiers and limits
`thread_local` to package variables and parametric lists to types/procedures;
resolves signatures, aggregate layouts, and nested value-parametric nominal
applications across package interfaces; preserves typed dependent integer
expressions such as `[N + 1]T`, `Buffer[N + M]`, and explicit
`callee[N + 1]` applications, including explicit integer casts, through
specialization and canonical interface ordinals; evaluates
arbitrary-precision integers and exactly rounded IEEE floating-point constants;
validates that every enum has a real all-zero member and that every tagged-union
alternative fits its chosen discriminator;
interprets scalar and aggregate compile-time procedures with locals, assignment,
bounded loops, recursion, switches, procedure-valued callees, saved-argument
lexical `defer`, parametric values, and type/layout queries;
uses the same full interpreter for concrete array/SIMD lengths, alignments, enum
values, and nominal value arguments after a deterministic signature/layout
fixed point, so those required constants may also call procedures and branch;
preserves exact concrete integer identity at layout boundaries, requiring
`usize` rather than accepting a same-width `u64` or narrower cast by accident;
enforces positional array, named struct, and single-field raw-union literals
with zero-filled omissions consistently at compile time and runtime;
and selects declaration/member `when` branches through deterministic fixed-point
rounds.
Provider-independent docs, judgments, and synthesis sites retain decoded text,
typed expectations, secure package-relative attachments, and SHA-256 content
identities; public docs cross package-interface boundaries. Procedure effect
summaries compose through local and imported calls, and lexical `deny` regions
enforce `assert`, context, assembly, unchecked access, globals, packages, and
named declarations transitively while rejecting unknown call edges. The single
AArch64 macOS profile is explicit and versioned rather than inferred from the
host. Compile-time evaluation includes byte-exact string indexing and all
half-open string slice forms, with the same exact `usize` and bounds rules as
runtime views. Checked procedure bodies now lower into a target-independent
MIR with
explicit locals, addresses, loads/stores, bounds checks, calls, lexical defer
unwinding, and CFG terminators for short-circuit expressions, conditionals,
loops, and switches. Switch labels are folded constants with duplicate and
exhaustiveness checks, including IEEE equality for signed zero and rejection of
unmatchable NaN labels; non-integer scalar subjects lower through ordered
equality branches after one subject evaluation. A defensive verifier checks
every MIR table reference and block boundary before a native backend may consume
it. Checked integer division, remainder, shifts, and numeric conversions use
explicit MIR control flow so LLVM poison cannot replace Draft's specified trap
or signed-minimum remainder behavior. The first backend emits
deterministic opaque-pointer LLVM IR per package with hermetic source locations;
generated operations retain both their authored synthesis site and generated
coordinate. A version-gated toolchain adapter then emits AArch64 Mach-O objects
and links them in reproducible mode without discarding the content-derived UUID.
Normal builds
require the pinned LLVM/Clang 22.1.x distribution; local bring-up can explicitly
permit another host Clang without changing the target profile. Parsed AArch64
assembly now has structural inputs, outputs, clobbers, and instruction rows; the
closed v2 scalar, memory, selection, conversion, NEON, and barrier vocabulary
validates register and flag dataflow plus declared effects before lowering to
volatile inline assembly. The
`examples/assembly` package exercises that path through a native executable.
Selected package `.s`, `.S`, and `.asm` files are also captured as exact build
inputs, assembled with preprocessing explicitly disabled, and linked in
canonical package/filename order; `examples/external-assembly` exercises the
separate C-ABI symbol boundary. An unresolved assembly synthesis site is kept
as a typed obligation during checking and precisely rejects native lowering.
Scalar/pointer `c proc` imports and exports are checked at a separate C ABI
boundary and retain exact linker names. A `c proc` has neither implicit Draft
context nor runtime `assert`; it may call ordinary Draft only through the
explicit checked context bridge. `examples/c-interop` exercises both ABIs.
The hosted entry shim owns one runtime Context across the linked package graph,
captures process arguments, enforces the exact `main` signature, and reports
assertion and bounds failures before trapping. Ordinary procedures see that
Context through the predeclared `context` value. A scope that writes a Context
field (or takes a Context-field address) receives a lexical copy, and ordinary
calls made in that scope receive the copy as their hidden argument. The narrow
`runtime.default_context` and `runtime.call_with_context` bridges let
context-free C callbacks acquire a compatible value and enter an ordinary Draft
callback without exposing Context as a generally legal C aggregate. The
`examples/nested-procedures` program covers lexical recursion, escaping
procedure pointers, enclosing compile-time parameters and constants, lexical
type aliases/nominals/local generics with specialization-specific layouts,
qualified/applied/grouped/tuple type-value aliases,
hidden Context propagation, and collision-free native identities. Runtime
parameters, locals, and iteration bindings from an enclosing invocation are
rejected as captures and must be passed explicitly. Procedure parameters are
immutable value bindings: mutation requires an explicit local copy or an
explicit pointer, multi-pointer, or slice parameter. The hosted temp allocator is
independently owned per pthread, supports explicit group
reset, and is destroyed at thread exit; a spawned child replaces rather than
shares the parent's temporary provider state. The
compiler-distributed `core/runtime`, `core/c`, `core/option`, `core/result`,
`core/memory`, `core/heap`, `core/array`, `core/map`, `core/io`, `core/testing`,
`core/benchmark`, `core/time`, `core/os`, `core/atomic`, and `core/thread`
packages are ordinary inspectable Draft source. `examples/core-runtime` checks
the Context import, layout, and callback
paths; `examples/core-memory` checks cross-package typed `new`/`free`, explicit
allocators and alignment, temporary allocation, bump arenas, owned buffers and
strings, resize preservation, and Darwin virtual memory; and
`examples/core-array` checks inferred nominal generics, ownership operations,
transitively exported nominal types, test records, benchmark records, streams,
and the Darwin monotonic clock through a nine-package graph.
`examples/core-map` additionally executes the allocator-explicit, open-addressed
hash map with stored hashes, tombstones, rehashing, and public string key
operations.
`examples/core-os` verifies stable process argument/environment views and fixed
descriptor I/O wrappers. `examples/core-thread` executes a pthread with an
independent copy of the spawning Context, observes that copy through Draft TLS,
joins it, and exercises the fixed AArch64 Darwin mutex/condition layouts.
`examples/core-atomic` exercises compiler-owned integer and pointer atomics and
every memory-order spelling through LLVM IR and native execution;
`examples/core-atomic-thread` proves the same operations across two pthreads.
`examples/validation` is selected only by `draft test` or `draft bench`; those
commands prove exact `^testing.Test`/`^benchmark.Benchmark` signatures, generate
an isolated native harness in canonical order, and execute it without a shell.
Each run appends content-addressed evidence keyed by the resolved validation
graph, definitions, target, compiler/toolchain, runner environment, artifact,
and policy. Failed attempts revoke prior passing evidence for only that key;
locked builds can verify required active evidence without rerunning it.

On Apple hosts, the test suite also compiles, links, launches, and requires a
zero exit from 17 handwritten programs spanning the runtime/core facilities,
multi-package generics, parsed and package assembly, C interop, atomics, and
pthreads. A separate end-to-end gate builds a Draft dylib and generated header,
compiles a checked-in C11 client against them, and launches it. These complement
phase-local tests with real AArch64 macOS ABI, linker, and loader conformance.

Synthesis resolution now has a provider-neutral transaction and an explicit
Codex CLI adapter. Declaration and aggregate-member sites form an early opaque
interface stage; dependent bodies are checked only after those expansions are
installed, then statement, expression, and assembly sites run in the later body
stage. Provider requests carry canonical expected and visible-binding type
spellings plus complete referenced type graphs, layouts, and members, along
with explicit target, SIMD, and parsed-assembly facts; digests remain
verification identities rather than opaque substitutes for usable context.
Visible imported aliases carry compact public interfaces with constants,
generic constraints, native bindings, and typed effect contracts; those
interfaces are also stale-pin inputs.
Usable visible names remain compact rows. A separate bounded declaration
closure starts from exact prompt mentions and resolved references in the
enclosing checked procedure, follows helper procedures transitively, and
supplies each selected comment-free definition with its type, constant value,
source coordinate, and digest. A transitive nested helper may therefore explain
another definition without being falsely advertised as directly visible at the
site.
They also carry stable package/source coordinates and a comment-free canonical
token view of the enclosing procedure or type declaration, all covered by the
site input digest. Body sites additionally carry typed outer-to-inner `if`,
`switch`, and loop-entry decisions; a default switch row names the complete
explicit label set that did not match, while conditional/clause and iteration
loops name the decision or iterable that admitted the current iteration. These
are historical control-flow facts, not claims that a mutable expression would
re-evaluate identically later in the body. Lexically active `deny` selectors are
sent and hashed in outer-to-inner order, and parametric sites carry ordered
type/value constraints.
Package-wide and structurally dominating judgment claims can guide synthesis
without being treated as passing verdicts. Package documentation reaches every
site, and enclosing-declaration docs reach sites inside that declaration, with
exact attachment bytes copied into the isolated request. Proposed and stored
source is barred from introducing another `...` or `judge`.
Body-stage requests also receive separately checked test/benchmark procedure
signatures, exact validation-state layouts, and portable typed facts for names
their HIR bodies actually reference. Those validation-only imports and names
remain labeled context and never join the generated program's visible scope.
Interface declaration/member requests retain syntax-only validation rows so an
early pin does not stale merely because its generated declaration later makes
the validation graph type-checkable. A successful
resolution writes exact content-addressed expansion bytes and one canonical
manifest atomically; normal checks and builds reproduce both stages from pins
without contacting a model. Before that manifest rename, the
resolver compiles selected typed Test and Benchmark definitions against the
in-memory candidate and asks the driver to execute the ordinary native
validation harness. A failed or unavailable required validation leaves the
previous manifest authoritative. Completed attempts still append their
immutable audit/revocation history, but only a passed attempt named by the final
manifest is selected for locked reuse.

The first locked native-build contract is also active. Resolution can pin one
explicit LLVM tree and macOS SDK by canonical content-tree identity. A locked
build carries that verified manifest snapshot through compilation, re-hashes
both physical roots, invokes the pinned Clang and Mach-O linker by absolute
path, supplies the SDK explicitly, disables Clang configuration discovery, and
uses a minimal child environment with no ambient tool, header, library, or SDK
search path. Logical foreign providers outside the target's built-in set require
explicit object, archive, or dylib mappings; resolution pins those exact bytes.
Named runtime assets may likewise be files or directory trees outside the Draft
distribution. They are verified as complete external identity inputs and
returned to embedding build systems for deployment, but they are never inferred
as linker operands or silently copied into an unspecified output layout.

Configure, build, and test the current compiler with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DDRAFT_ENABLE_SANITIZERS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`build/draftc lex path/to/file.draft` prints the token stream, including which
semicolons were inserted by the compiler. `build/draftc syntax
path/to/file.draft` prints the parsed grammar tree. `build/draftc target` prints
the selected profile's key ABI, LLVM, and assembly facts. `build/draftc check
path/to/package-directory` runs package loading, compile-time selection, type and
layout resolution, imported public-interface checking, and procedure-body HIR
checking without an agent. `build/draftc check examples/packages/app` exercises
the current multi-package path. `build/draftc emit-llvm examples/hello` prints
the package module without invoking external tools. `build/draftc build
examples/hello` uses the pinned native toolchain; `--allow-host-toolchain` is an
explicit development-only escape hatch. All commands use the same
dependency-ordered source, semantic, HIR, and MIR pipeline.

Interface resolution also promotes a normally body-level synthesis site when a
package constant or declaration-level `when` executes that procedure at compile
time. The constant interpreter stops at unresolved source, and the ordinary
body checker builds the site's exact expected type, enclosing procedure, and
lexical context before the provider runs. Its checked expansion is installed in
an interface round; only then can the constant select dependent declarations.
Direct `when ...` conditions take the same path with an exact `bool` obligation.
Offline builds reproduce those rounds from pinned expansion bytes.

The same scheduler owns integer recipes used by types and layouts. Direct array
and SIMD counts, generic value arguments, alignment attributes, and explicitly
backed enum values receive their exact required integer type. If a recipe calls
a compile-time procedure, only the reached body is checked early. A package
with a pending public layout withholds its interface, so consumers cannot bind a
partial type before the checked expansion is installed.

Aggregate-member synthesis is a narrower completeness boundary than a package
declaration. An early compile-time procedure that already type-checks against
the incomplete member graph may publish its site in the same opaque round. If
that check needs a pending member, the compiler discards the speculative graph
and retries after the member expansion. Package declaration sites remain prior
dependencies because they may introduce any missing package name.

`draftc build` defaults to an executable. `--kind object`, `--kind
static-library`, `--kind dynamic-library`, and `--kind assembly` select a
relocatable object, archive, dylib, or collision-free directory of assembly
sources. `draftc emit-c-header examples/c-library -o library.h` emits the C API
for explicit root-package exports. The `examples/c-library` fixture builds as a
no-`main` dylib and its checked-in C client exercises aggregate and callback ABI
compatibility.

For example, an arbitrary provider is selected explicitly and never inferred
from a host library name:

```sh
build/draftc build examples/foreign-provider --allow-host-toolchain \
  --provider custom_math=object:/absolute/path/to/provider.o
```

Pass the same `--provider` row to `draftc resolve` to record its content identity
and to later builds to supply a relocated matching file. Unmapped, duplicate,
unused, stale, or attempts to remap target-owned providers are errors.

Runtime assets use a separate `name:path` mapping. The logical name and exact
content tree enter the resolved-program identity; the physical path does not.
The same complete set is required by manifest-bearing build, test, and benchmark
commands, and may be relocated without changing identity:

```sh
build/draftc resolve path/to/package \
  --runtime-asset unicode-tables:/absolute/assets/unicode

build/draftc build path/to/package --allow-host-toolchain \
  --runtime-asset unicode-tables:/relocated/assets/unicode
```

`build/draftc resolve path/to/package --codex-distribution-root /absolute/codex-root
--codex-executable /absolute/codex-root/bin/codex --codex-model <model>` invokes
Codex only for missing or stale sites. The complete non-followed distribution
tree, root-relative launcher, explicit model, fixed non-interactive adapter
contract, prompt format, and output schema form the provider configuration
identity stored with each pin. The tree is reverified before and after execution.
An explicitly changed provider, model, or adapter configuration regenerates an
otherwise type-fresh site; provider-free offline builds continue to reuse it.
Each Codex call has a five-minute per-attempt deadline and at most two attempts;
timed-out children are killed and reaped before resolution can fail.
`build/draftc resolve path/to/package --revalidate` never invokes a provider; it
checks existing generated bytes against current obligations and commits only if
the complete program and its selected tests and benchmarks still succeed.
Development-only resolution may add `--allow-host-toolchain`; release resolution
supplies the pinned roots below. Packages without selected validation need no
native runner.

Use `--judge` to run every authored judgment after synthesis checking and native
Test/Benchmark validation, but before the resolver's one final manifest commit.
`--judge-select <selector>` repeats the same operation for an exact site,
package, or `<package>:<anchor>` selection. Passing judgment attempts are
durable immediately; their rows become visible only when every selected verdict
passes and the complete resolution transaction commits. A failure leaves the
previous manifest authoritative. Ordinary resolution preserves judgment rows
only when the complete resolved-program digest is unchanged; otherwise it drops
them as stale. This profile also works for wholly handwritten programs with no
synthesis sites.

`build/draftc judge path/to/package` accepts the same explicit
`--codex-distribution-root`, `--codex-executable`, and `--codex-model` triple.
It compiles the complete resolved program before making a provider call,
evaluates every current judgment in canonical package/site order, and durably
records every pass or fail. Only an all-pass aggregate updates
`resolution.json`; that update replaces all judgment rows for the checked
program while preserving synthesis pins, external inputs, and native validation
evidence. The manifest is replaced only if the exact snapshot compiled before
the model calls is still current, so concurrent resolution cannot attach a
verdict to a different program. A failing aggregate never selects its rows and
revokes any prior active evidence for the same exact claim keys.

The provider-neutral judgment API also supports an ordered set of independent
validators and exact requested artifact bytes. It invokes every validator for
each selected site and records one evidence object that passes only when all
rows pass. Offline verification is given the same policy identity, validator
order, and artifact digests, so it can reject mismatched evidence without a
provider. Repeat `--judge-validator <identity>:<model>` to configure those
ordered Codex slots and repeat `--judge-artifact <kind>:<path>` to attach exact
files. `resolve` accepts the same flags for its precommit judgment profile.
Locked `build` uses `--judge-validator <identity>` and
`--judge-artifact <kind>:<sha256>` with `--require-judgment-evidence`, so offline
verification receives only expected identities and digests, never ambient
artifact paths.

`draftc judge path/to/package --list` prints each exact stable `site-...`
identity with its package, anchor, source file, and occurrence without
configuring Codex. Positional selectors (or `--select <selector>`) accept that
exact identity, a package path/identity, or `<package>:<anchor>`. Multiple
selectors form a de-duplicated union. A partial successful run replaces only
the selected sites' rows; unrelated judgment and native evidence remains
selected. For example:

```sh
build/draftc judge path/to/root codec/jpeg:decode \
  --codex-distribution-root /absolute/codex-root \
  --codex-executable /absolute/codex-root/bin/codex \
  --codex-model <model>
```

For an independent two-validator/object policy:

```sh
build/draftc judge path/to/root \
  --codex-distribution-root /absolute/codex-root \
  --codex-executable /absolute/codex-root/bin/codex \
  --judge-validator security:<model-a> \
  --judge-validator correctness:<model-b> \
  --judge-artifact object:/absolute/app.o
```

The all-sites locked gate still requires complete coverage. Partial runs are
therefore useful for iteration but do not satisfy
`--require-judgment-evidence` until every current site has an active selected
row.

Pin native inputs during resolution, then reproduce them without a provider:

```sh
build/draftc resolve path/to/package \
  --toolchain-root /absolute/path/to/llvm \
  --sdk-root /absolute/path/to/MacOSX.sdk \
  --runtime-asset unicode-tables:/absolute/assets/unicode

build/draftc build path/to/package --locked \
  --toolchain-root /absolute/path/to/llvm \
  --sdk-root /absolute/path/to/MacOSX.sdk \
  --runtime-asset unicode-tables:/relocated/assets/unicode \
  --require-judgment-evidence
```

`--require-judgment-evidence` makes a locked build require one exact active,
manifest-selected passing object for every current judgment. Verification
matches the compiled typed claim, resolved program, target, compiler, package,
one-validator/no-artifact policy, and immutable attempt digest. It never starts
Codex or any other provider. A missing row, stale context, changed compiler, or
later failing attempt rejects the build. As with test and benchmark evidence,
the flag is rejected outside `--locked` mode.

The first toolchain layout requires executable `bin/clang`, `bin/ld64.lld`, and
`bin/llvm-ar`. Relocating an unchanged tree preserves its identity; changing
any byte, path, permission, or symlink spelling makes the build fail before a
compiler process starts. Runtime-asset roots use the same file-kind, permission,
byte, and safe-symlink identity and are also rechecked before a compiler process
starts.
