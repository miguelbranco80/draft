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
scopes, symbols, and types; resolves signatures and aggregate layouts; evaluates
arbitrary-precision integers and exactly rounded IEEE floating-point constants;
interprets scalar and aggregate compile-time procedures with locals, assignment,
bounded loops, recursion, switches, parametric values, and type/layout queries;
and selects declaration/member `when` branches through deterministic fixed-point
rounds.
Provider-independent docs, judgments, and synthesis sites retain decoded text,
typed expectations, secure package-relative attachments, and SHA-256 content
identities; public docs cross package-interface boundaries. Procedure effect
summaries compose through local and imported calls, and lexical `deny` regions
enforce `assert`, context, assembly, unchecked access, globals, packages, and
named declarations transitively while rejecting unknown call edges. The single
AArch64 macOS profile is explicit and versioned rather than inferred from the
host. Checked procedure bodies now lower into a target-independent MIR with
explicit locals, addresses, loads/stores, bounds checks, calls, lexical defer
unwinding, and CFG terminators for short-circuit expressions, conditionals,
loops, and switches. A defensive verifier checks every MIR table reference and
block boundary before a native backend may consume it. The first backend emits
deterministic opaque-pointer LLVM IR per package, then a version-gated toolchain
adapter emits AArch64 Mach-O objects and links an executable. Normal builds
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
boundary and retain exact linker names; `examples/c-interop` exercises both.
The hosted entry shim owns one runtime Context across the linked package graph,
captures process arguments, enforces the exact `main` signature, and reports
assertion and bounds failures before trapping. Ordinary procedures see that
Context through the predeclared `context` value. A scope that writes a Context
field (or takes a Context-field address) receives a lexical copy, and ordinary
calls made in that scope receive the copy as their hidden argument. The narrow
`runtime.default_context` and `runtime.call_with_context` bridges let
context-free C callbacks acquire a compatible value and enter an ordinary Draft
callback without exposing Context as a generally legal C aggregate. The hosted
temp allocator is independently owned per pthread, supports explicit group
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
zero exit from 15 handwritten programs spanning the runtime/core facilities,
multi-package generics, parsed and package assembly, C interop, atomics, and
pthreads. A separate end-to-end gate builds a Draft dylib and generated header,
compiles a checked-in C11 client against them, and launches it. These complement
phase-local tests with real AArch64 macOS ABI, linker, and loader conformance.

Synthesis resolution now has a provider-neutral transaction and an explicit
Codex CLI adapter. Declaration and aggregate-member sites form an early opaque
interface stage; dependent bodies are checked only after those expansions are
installed, then statement, expression, and assembly sites run in the later body
stage. Provider requests carry canonical expected and visible-binding type
spellings plus explicit target, SIMD, and parsed-assembly facts; digests remain
verification identities rather than opaque substitutes for usable context.
Proposed and stored source is barred from introducing another `...` or
`judge`. A successful resolution writes exact content-addressed expansion bytes
and one canonical manifest atomically; normal checks and builds reproduce both
stages from pins without contacting a model. Before that manifest rename, the
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

`build/draftc resolve path/to/package --codex-executable /absolute/path/to/codex
--codex-model <model>` invokes Codex only for missing or stale sites. Executable
bytes, the explicit model, fixed non-interactive adapter contract, prompt format,
and output schema form the provider configuration identity stored with each pin.
`build/draftc resolve path/to/package --revalidate` never invokes a provider; it
checks existing generated bytes against current obligations and commits only if
the complete program and its selected tests and benchmarks still succeed.
Development-only resolution may add `--allow-host-toolchain`; release resolution
supplies the pinned roots below. Packages without selected validation need no
native runner.

Pin native inputs during resolution, then reproduce them without a provider:

```sh
build/draftc resolve path/to/package \
  --toolchain-root /absolute/path/to/llvm \
  --sdk-root /absolute/path/to/MacOSX.sdk

build/draftc build path/to/package --locked \
  --toolchain-root /absolute/path/to/llvm \
  --sdk-root /absolute/path/to/MacOSX.sdk
```

The first toolchain layout requires executable `bin/clang`, `bin/ld64.lld`, and
`bin/llvm-ar`. Relocating an unchanged tree preserves its identity; changing
any byte, path, permission, or symlink spelling makes the build fail before a
compiler process starts.
