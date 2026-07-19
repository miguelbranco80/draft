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

The current CLI uses the canonical parent of the requested root package as the
workspace root. Put an application package in a child such as `project/app/`
when resolution state should belong to `project/.draft/`; passing
`project/app/` then keeps the manifest and generated source inside that project.
This distinction is invisible for handwritten packages until a command needs
persistent resolution inputs.

Recognized direct children are:

```text
*.draft
*.s
*.S
*.asm
name@aarch64-macos.<extension>
name@aarch64-linux.<extension>
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
passes:

```sh
build/draftc lex path/to/file.draft
build/draftc syntax path/to/file.draft
build/draftc check path/to/package --target aarch64-macos
build/draftc check path/to/package --target aarch64-linux
build/draftc expand path/to/package --out /tmp/expanded-source \
  --target aarch64-macos
build/draftc resolve path/to/package --build -o /tmp/program \
  --target aarch64-macos
```

Use `lex` for token/semicolon questions, `syntax` for grammar/recovery, and
`check` for package selection, names, types, constants, denials, layout, and
the provider-free semantic pipeline.

Other useful inspections:

```sh
build/draftc target --target aarch64-macos
build/draftc target --target aarch64-linux
build/draftc emit-llvm path/to/package --target aarch64-macos
build/draftc emit-c-header path/to/package -o /tmp/package.h \
  --target aarch64-macos
```

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

## Native validation

On a matching AArch64 host:

```sh
build/draftc build path/to/package \
  --target aarch64-macos \
  -o /tmp/draft-program
/tmp/draft-program
```

Use `--target aarch64-linux` on matching Linux. Cross-checking can prove the
front end and lowering contract, but current native execution requires the
matching AArch64 host toolchain and runtime.

For a library or artifact-specific task, select the actual kind:

```sh
build/draftc build path/to/package --kind object -o /tmp/package.o
build/draftc build path/to/package --kind static-library -o /tmp/libpackage.a
build/draftc build path/to/package --kind dynamic-library -o /tmp/libpackage.dylib
build/draftc build path/to/package --kind assembly -o /tmp/package-assembly
```

Use `.so` on Linux. Validate generated headers with an independent C compiler
when external ABI is the purpose. A successful Draft build alone does not prove
that a C consumer sees the intended layout and symbols.

## Measure compiler work

Use the driver timing report before changing compiler architecture for speed:

```sh
build/draftc check path/to/package --timings
build/draftc build path/to/package -o /tmp/program --timings=all
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

For resolved programs, distinguish `workspace loads` from `workspace source
transitions`. A checked `...` expansion is reparsed into the existing
command-local graph and reanalyzes only its package plus transitive import
consumers; it is not another workspace load and creates no persistent cache.
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
build/draftc test path/to/package --target aarch64-macos
```

Use `*_bench.draft`, a `bench_` prefix, and exactly one
`^benchmark.Benchmark` for benchmarks:

```sh
build/draftc bench path/to/package --verify --target aarch64-macos
```

Tests and benchmarks form a coherent resolved validation graph. They are not
ordinary build prerequisites, and build never reruns their evidence.

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

Run one or a small regular-expression slice while iterating:

```sh
ctest --test-dir build --output-on-failure \
  -R '^(draft_compiler_tests|draft_parser_tests)$'
```

Native/interop work commonly needs:

```sh
cmake --build build --target \
  draft_native_interop_tests \
  draft_aarch64_abi_tests \
  draft_c_header_tests \
  draft_toolchain_tests \
  --parallel

ctest --test-dir build --output-on-failure \
  -R '^(draft_native_interop_tests|draft_aarch64_abi_tests|draft_c_header_tests|draft_toolchain_tests|draft_target_profile_tests|draft_assembly_tests|draft_compiler_tests)$'
```

On a native AArch64 macOS/Linux host, artifact closure also includes
`draft_native_determinism_tests`, `draft_native_backend_parity_tests`,
`draft_native_conformance_tests`, and `draft_c_client_integration_tests`. The
first compares one-worker and four-worker output; the second exercises every
artifact kind through embedded LLVM and the external Clang oracle. CMake
deliberately omits unsupported native tests rather than reporting false skips.

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
all applications.

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
