# Draft Package Workflow and Testing

This guide turns a Draft coding task into repository changes with proportionate
validation. The repository-wide contract remains [`AGENTS.md`](../../../../AGENTS.md).
The current CLI is documented in the
[`command reference`](../../../../docs/operations/command-reference.md), and
the example map is [`examples/README.md`](../../../../examples/README.md).

## Contents

- [Orient before editing](#orient-before-editing)
- [Package and file layout](#package-and-file-layout)
- [Choose the owning layer](#choose-the-owning-layer)
- [Write a runnable package](#write-a-runnable-package)
- [Front-end validation](#front-end-validation)
- [Native validation](#native-validation)
- [Measure compiler work](#measure-compiler-work)
- [Draft tests and benchmarks](#draft-tests-and-benchmarks)
- [Compiler regression tests](#compiler-regression-tests)
- [Build the bootstrap and run CTest](#build-the-bootstrap-and-run-ctest)
- [Sanitizers](#sanitizers)
- [Examples](#examples)
- [Documentation routing](#documentation-routing)
- [Completion and commits](#completion-and-commits)

## Orient before editing

1. Read root `AGENTS.md`.
2. Read the relevant normative specification section.
3. Read every imported core package and its comments.
4. Find the closest compiling example and focused test.
5. For target/native work, read the selected versioned target profile and
   implementation subsystem document.
6. Run `git status --short` and preserve unrelated user/agent changes.

Do not infer language support from what LLVM, C++, the host OS, or a familiar
standard library could theoretically do. Distinguish:

- a Draft language rule;
- a current bootstrap/backend limitation;
- a missing core API;
- application-specific policy.

That distinction decides where the change and documentation belong.

## Package and file layout

One directory is one package, compilation unit, namespace, visibility boundary,
and synthesis-context boundary. Every selected `.draft` file declares the same
short package name.

The current CLI takes the workspace directory explicitly. A single-root command
defaults to package `.`, or accepts one normalized workspace-relative
`--root`; `app` and `lib` are ordinary directory names. `build <workspace>`
without selectors discovers all visible packages with a surface package-level
`main`, while repeated `--root` options select an exact subset. Root/target
manifests live under `project/.draft/resolutions/`, and generated source objects
are shared by content identity under `project/.draft/generated/`. Test and
benchmark evidence is also workspace-owned under `project/.draft/evidence/`;
selecting a child root never creates a nested package-local `.draft` store.

Recognized direct children are:

```text
*.draft
*.s
*.S
*.asm
name@aarch64-macos.<extension>
name@aarch64-linux.<extension>
name@x86_64-linux.<extension>
name@x86_64-windows.<extension>
```

Nested directories are separate packages or non-source content; they do not
join the parent's source set. Files are processed in bytewise filename order.
Never depend on filesystem enumeration order.

Imports are unquoted paths and file-local:

```draft
package app

import core/console
import core/memory
import library/parser as parser
```

Another source file repeats the imports it uses. Imports do not re-export.

`*_test.draft` participates in `draft test`; `*_bench.draft` participates in
`draft bench`. Both are excluded from ordinary package compilation. A
conventional `package.draft` is useful but has no privileged semantics.

Turbo Draft opens a workspace rather than inferring one from the shell's
current directory. Without a `--root` override, `<workspace>/draft.project`
contains a `draft-project-v1` header, required workspace-relative `root`, and
optional direct-child `source` (default `package.draft`). This file selects the
IDE's initial runnable package; it does not enumerate sources, alter imports, or
replace the compiler's `.draft/` resolution and evidence state. F5 always
checks/builds/runs the root associated with the active editor buffer.
Files is populated from the compiler's target-selected reachable workspace
graph, while Buffers lists open documents. Check and F5 submit the active buffer
plus every other dirty buffer belonging to that graph as one transactional
source-override set; the active buffer also chooses the syntax-span result.

## Choose the owning layer

Before adding an API, answer:

- Is it reusable language-independent policy? Put it in an ordinary package.
- Is it a small broadly useful substrate absent from `core`? Add the narrowest
  core operation with portable contract, target implementation, tests, and
  comments.
- Is it one OS/ABI implementation? Put it in an exact target-tagged source or
  package assembly file behind a shared Draft-facing interface.
- Is it a new language meaning? Update the specification and implement all
  affected phases; do not simulate it in a helper.
- Is it only a current compiler capability gap? Record it as an implementation
  limitation, not normative semantics.

Do not expand core to avoid a small local helper. Do not keep a generally
necessary platform primitive hidden in one example merely to avoid designing
its honest error and ownership contract.

## Write a runnable package

A hosted executable root defines exactly one package-level, non-parametric
ordinary `main`:

```draft
package hello

import core/console

main :: proc() {
    error := console.println("hello from Draft")
    if error != .none {
        return
    }
}
```

`main` may return nothing or the permitted integer status type described in the
runtime specification. Keep setup and cleanup explicit. For a library root,
omit `main` and use `pub` and/or `export` according to the intended consumer.

Comments are a deliverable. Explain module purpose, owned state, lifetimes,
invariants, error behavior, ordering, target assumptions, and non-obvious
algorithm choices. Avoid narrating individual operators.

## Front-end validation

Run from the repository root, increasing scope only after the narrow step
passes. For handwritten programs or `...` programs that already have fresh
saved expansions for the selected target, use the provider-free sequence:

```sh
build/draftc lex path/to/file.draft
build/draftc syntax path/to/file.draft
build/draftc check path/to/workspace --root package --target aarch64-macos
build/draftc check path/to/workspace --root package --target aarch64-linux
build/draftc check path/to/workspace --root package --target x86_64-linux
build/draftc check path/to/workspace --root package --target x86_64-windows
build/draftc expand path/to/workspace --root package --out /tmp/expanded-source \
  --target aarch64-macos
build/draftc resolve path/to/workspace --root package --build -o /tmp/program \
  --target aarch64-macos
```

For fresh source containing `...`, `check`, `expand`, and plain `build` must
fail until a pin exists; they never contact a synthesis provider. After any
useful `lex` or `syntax` inspection, run `resolve` separately for every target
you intend to consume. `resolve --build` is the shortest first-run path when
you also want the current host artifact. Subsequent `check`, `expand`, and
plain `build` commands use only the saved target-scoped expansion.

Use `lex` for token/semicolon questions, `syntax` for grammar/recovery, and
`check` for package selection, names, types, constants, denials, layout, and
the provider-free semantic pipeline.

Other useful inspections:

```sh
build/draftc target --target aarch64-macos
build/draftc target --target aarch64-linux
build/draftc target --target x86_64-linux
build/draftc target --target x86_64-windows
build/draftc emit-llvm path/to/workspace --root package --target aarch64-macos
build/draftc emit-c-header path/to/workspace --root package -o /tmp/package.h \
  --target aarch64-macos
```

`emit-llvm` prints one complete module per semantic package. Each block contains
that package's globals and all concrete runtime procedure definitions and is an
independent compiler input. Module granularity is an internal compiler
invariant; there is no CLI option to change it.

Pass an explicit target for target-sensitive work. A default-macOS check alone
does not establish portability.

`expand` performs a normal provider-free check and writes final selected source
plus generated-to-surface `.draft-map` sidecars. Its destination must not
already exist. It is useful when reviewing `...` expansions or feeding simple
editor tooling, but remains derived output: commit the resolution manifest and
referenced content-addressed generated Draft objects instead.

`resolve --build` is the explicit source-changing plus native-emission workflow.
Resolution commits only after the complete program checks; native lowering then
continues the returned graph without another front-end construction. It does
not implicitly execute tests, benchmarks, or judgments.

The positional path is always the workspace. Single-root commands default to
root `.`, while `--root package/path` selects a child package; names such as
`app` and `lib` have no special meaning. `build <workspace>` without selectors
discovers every visible ordinary package with a surface package-level `main`.
Use repeated `--root` options for an exact subset. Default outputs mirror each
root path under `.draft/build/<target-file-tag>/`; `-o` requires exactly one
selected root.

## Native validation

On a matching native host:

```sh
build/draftc build path/to/workspace --root package \
  --target aarch64-macos \
  -O0 \
  -o /tmp/draft-program
/tmp/draft-program
```

Use `--target aarch64-linux` or `--target x86_64-linux` on the matching Linux
architecture. Cross-checking can prove the front end and lowering contract,
but native execution requires the matching host toolchain and runtime.

Native `build`, `resolve --build`, `test`, and `bench` default to `-O0`; pass
`-O2` when the task requires optimized code. O2 runs within each complete
semantic-package LLVM module, while packages remain independently emitted. It
may change derived object or assembly bytes, not Draft semantics, assertions,
resolution pins, or module granularity. Validation evidence distinguishes O0
and O2 policy, so use `bench -O2` when measuring optimized code. `emit-llvm`
remains the canonical pre-optimization inspection; use
`build --kind assembly -O2` to inspect optimized native output. Do not invent
O1, O3, size optimization, LTO, arbitrary pass, or granularity flags.

For a library or artifact-specific task, select the actual kind:

```sh
build/draftc build path/to/workspace --root package --kind object -o /tmp/package.o
build/draftc build path/to/workspace --root package --kind static-library -o /tmp/libpackage.a
build/draftc build path/to/workspace --root package --kind dynamic-library -o /tmp/libpackage.dylib
build/draftc build path/to/workspace --root package --kind assembly -o /tmp/package-assembly
```

Use `.so` on Linux. Validate generated headers with an independent C compiler
when external ABI is the purpose. A successful Draft build alone does not prove
that a C consumer sees the intended layout and symbols.

## Measure compiler work

Use the driver timing report before changing compiler architecture for speed:

```sh
build/draftc check path/to/workspace --root package --timings
build/draftc build path/to/workspace --root package -o /tmp/program --timings=all
```

`--timings` reports major wall-clock phases and work counters to stderr.
`--timings=all` adds package/tool scopes, file I/O, lexing/parsing,
import-graph resolution, and exclusive `self` time; native and validation
commands also distinguish child-process CPU from wall time. Compare
phase structure and counters before comparing small durations, which vary with
the host and warm filesystem caches. Timing is diagnostic only and never
changes program identity or output.

Provider-using resolution reports `provider synthesis` for each ready wave and
counts both `synthesis provider ready waves` and actual provider calls. Calls in
one wave may overlap, so compare the enclosing wall time with the call count;
do not add per-worker timing writes to the single-threaded recorder.

Native lowering reports package-assembly, MIR, package-LLVM, and artifact-layout
semantic waves separately. Each concrete `MirProcedure` payload lives in its
workspace product side-table row rather than a package MIR program. After every
selected package's MIR is ready, one workspace-wide wave runs one package-LLVM
task per package; each task borrows its package's procedures in canonical order
and emits the complete module. A later native ready set contains one module-
object task per package plus any selected package-assembly tasks. Worker counts
are internal/test options, not CLI flags. Repository determinism tests compare
those counters across worker counts; do not invent `--workers` or `-j`.

For resolved programs, distinguish `workspace loads` from `workspace source
transitions`. A checked `...` expansion is reparsed into the existing
command-local graph. A declaration/member expansion reanalyzes its package and
transitive import consumers. A body expansion reanalyzes only its containing
package, retains completed dependency and consumer bodies, and recomputes
affected selection and closure. It is not another workspace load and creates no
persistent cache. The semantic counters distinguish successor source
transitions, exact procedure checks, workspace-wide frozen ready waves, and
newly materialized cross-package specializations. Selection-only reuse performs
no BodyChecker work and has no separate reuse counter. Effect, denial, metadata,
or obligation closure may still be recomputed after a selected dependency
changes. Counts are command totals and include separately selected
validation-context graphs when those are needed.
An additional compiler pass is expected only for a genuinely different source
selection, such as the typed test or benchmark graph used as synthesis context.

## Draft tests and benchmarks

Put executable positive behavior in `*_test.draft`:

```draft
package parser

import core/testing

test_empty_input :: proc(test: ^testing.Test) {
    result := parse(empty_input)
    testing.expect(test, result.is_empty)
}
```

Discovery requires a defined, package-level, non-parametric ordinary procedure
with a `test_` prefix, exactly one `^testing.Test` argument, and no result.

Run:

```sh
build/draftc test path/to/workspace --root package --target aarch64-macos -O0
```

Use `*_bench.draft`, a `bench_` prefix, and exactly one
`^benchmark.Benchmark` for benchmarks:

```sh
build/draftc bench path/to/workspace --root package --verify \
  --target aarch64-macos -O2
```

Tests and benchmarks form a coherent resolved validation graph. They are not
ordinary build prerequisites, and build never reruns their evidence. Their
content-addressed evidence belongs to the selected workspace, while its key
binds the executable root, validation procedure, target, resolved program, and
native optimization policy. Omitted optimization is O0; benchmark optimized
code explicitly with O2.

## Compiler regression tests

Invalid Draft programs, exact diagnostic ranges, parser recovery, ABI
classification, deterministic serialization, and backend contracts belong in
the C++ tests under [`tests/`](../../../../tests).

Tests are standalone executables, not a third-party test framework. Existing
files use a local `TestState`, focused `test_*` functions, `EXPECT`, and a
`main()` that returns failure when checks accumulated.

For a language rejection, check:

- the important diagnostic message;
- the exact source range or rendered line/column;
- diagnostic count where meaningful;
- recovery state when recovery is under test.

User-controlled invalid source must become a structured diagnostic, never an
assertion. Add a regression before or with every bug fix.

For layout/ABI, compare target facts and, where useful, an independent
C/Clang oracle. For toolchain behavior, prefer deterministic fake toolchains
for argument/artifact contracts and reserve real tools for native integration.

## Build the bootstrap and run CTest

Configure a warning-clean debug build:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLLVM_DIR=/opt/homebrew/opt/llvm@22/lib/cmake/llvm \
  -DDRAFT_WARNINGS_AS_ERRORS=ON \
  -DDRAFT_ENABLE_SANITIZERS=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Use `/usr/lib/llvm-22/lib/cmake/llvm` on the qualified Ubuntu layout. LLVM 22 is
a bootstrap compiler component; it is not a Draft source-package dependency.

List registered tests without executing:

```sh
ctest --test-dir build -N
```

Run the exhaustive example integration slice with:

```sh
ctest --test-dir build -L examples --output-on-failure
```

`examples/qualification.tsv` must classify every tracked Draft/assembly package
directory. Its rows drive all four target frontend checks, every ordinary
native example execution, special C-library, foreign-provider, and vendored
raylib link gates,
and all example-owned Draft tests and benchmarks. Windows consumes the same
ordinary executable inventory through
`tests/driver_windows_native_smoke_test.cmake`; its validation-process runner
does not yet execute the test/benchmark rows. Do not add an example source
package without adding its explicit final-state classification.

Run one or a small regular-expression slice while iterating:

```sh
ctest --test-dir build --output-on-failure \
  -R '^(draft_compiler_tests|draft_parser_tests)$'
```

Native/interop work commonly needs:

```sh
cmake --build build --target \
  draft_mir_tests \
  draft_llvm_ir_tests \
  draft_llvm_object_emitter_tests \
  draft_native_object_tasks_tests \
  draft_compiler_tests \
  draft_native_interop_tests \
  draft_aarch64_abi_tests \
  draft_x86_64_abi_tests \
  draft_win64_abi_tests \
  draft_c_header_tests \
  draft_toolchain_tests \
  --parallel

ctest --test-dir build --output-on-failure \
  -R '^(draft_mir_tests|draft_native_interop_tests|draft_aarch64_abi_tests|draft_x86_64_abi_tests|draft_win64_abi_tests|draft_c_header_tests|draft_toolchain_tests|draft_target_profile_tests|draft_assembly_tests|draft_llvm_ir_tests|draft_llvm_object_emitter_tests|draft_native_object_tasks_tests|draft_compiler_tests)$'
```

On the macOS/Linux native hosts, artifact closure also includes
`draft_native_determinism_tests`, `draft_native_backend_parity_tests`,
`draft_native_conformance_tests`, `draft_c_client_integration_tests`, the
foreign-provider and raylib link/run gates, and the example test/benchmark
matrix. The first compares one-worker and four-worker output; the second
exercises every artifact kind through embedded LLVM and the external Clang
oracle. CMake deliberately omits unsupported native tests rather than reporting
false skips. Windows
instead requires its driver-level example, COFF artifact, foreign-provider,
and C-client/DLL smoke script in CI.

Run that native closure directly on a supported host with:

```sh
ctest --test-dir build --output-on-failure \
  -R '^(draft_native_determinism_tests|draft_native_backend_parity_tests|draft_native_conformance_tests|draft_c_client_integration_tests|draft_foreign_provider_example|draft_raylib_asteroids_example|draft_example_validation_matrix)$'
```

There is currently no repository formatter or lint command. Do not invent one.
Compilation with the strict warning set, tests, and sanitizers are the
mechanical gates; still reread comments and formatting manually.

## Sanitizers

Linux x86-64 CI sanitizes the C++ bootstrap with ASan and UBSan:

```sh
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
cmake -S . -B build-sanitized \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++ \
  -DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm \
  -DDRAFT_WARNINGS_AS_ERRORS=ON \
  -DDRAFT_ENABLE_SANITIZERS=ON
cmake --build build-sanitized --parallel
ctest --test-dir build-sanitized --output-on-failure
```

This instruments the C++ compiler, not generated Draft programs. Generated
Draft `--instrument address` is currently qualified on macOS only. Other
required instrumentation, including Linux Draft ASan, fails closed.

## Examples

Use an example for readable, runnable, positive end-to-end behavior. Extend the
closest focused example when that remains coherent; create a new directory when
the feature or application deserves its own narrative and executable.

Every new or materially broadened example updates `examples/README.md` with:

- what it demonstrates;
- important target or toolchain requirements;
- build/run commands if they differ from the common path;
- whether it is pedagogical, conformance-oriented, or an application.

Keep intentionally invalid input in compiler tests, not a runnable examples
directory. `examples/language-tour` is the readable language starting point;
`examples/runtime-checks` is dense native conformance, not the style model for
all applications. `examples/raylib-asteroids` is the complete vendored
shared-library application pattern: its `game` package owns provider-free
logic, its focused binding owns the C ABI, and its `app` package owns native
resource lifetime. For agent constructs, `examples/judgment-tour` is the
provider-free placement tour, `examples/agent-pending` is the smallest live
Codex transaction, and `examples/agent-judgment-mix` demonstrates judgments
guiding all five synthesis grammar categories.

## Documentation routing

Update the document that owns the changed contract:

- language semantics → `docs/specification/`;
- target layout, ABI, object format, runtime, assembly facts →
  `docs/targets/`;
- compiler representations, algorithms, lowering, core/runtime mechanisms,
  and current limitations → `docs/implementation/`;
- commands, flags, artifact/provider workflow →
  `docs/operations/command-reference.md`;
- actually completed qualification evidence → `docs/releases/`;
- unresolved language choices → `docs/decisions/language-questions.md`;
- runnable feature coverage → example plus `examples/README.md`;
- public overview/target/getting-started map → root `README.md`.

Do not rewrite `docs/history/` to describe current behavior. When adding,
moving, or renaming a document, update `docs/README.md` and every affected link.
If no owned contract changed, say that in the completion summary rather than
making a token documentation edit.

Keep this skill synchronized too:

- syntax/typing/runtime meaning → `references/language.md` and, when relevant,
  `memory-and-ownership.md`;
- public core API → `core-library.md` and its ownership guidance;
- target/ABI/assembly/interop → `interop-and-targets.md`;
- `docs`, `judge`, `...`, denials, or validation → `agent-features.md`;
- commands, repository test practice, examples, or docs routing → this file.

## Completion and commits

Before declaring completion:

1. Run the narrow check and the relevant broader suite.
2. Run strict warnings and enabled sanitizers in proportion to the change.
3. Re-read changed code linearly and remove unnecessary indirection.
4. Audit every nearby comment for continued truth.
5. Confirm ownership, deterministic ordering, target selection, and explicit
   error handling.
6. Inspect `git diff --check`, `git diff`, and the staged diff.
7. Update owning docs, examples map, and skill references when their contracts
   changed.

Make reasonably small coherent commits. A commit should have one purpose that
does not need an “and” to explain it. Keep directly corresponding tests and
contract documentation with the behavior when separation would make either
commit incomplete. Do not mix unrelated refactoring, generated artifacts, or
agent-rule changes. Commit each verified slice when it is complete.
