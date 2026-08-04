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
- [Exercise an installed distribution](#exercise-an-installed-distribution)
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

Package commands take the package directory itself. `draftc` searches upward
for the nearest `draft.workspace`; its directory owns imports and `.draft/`
state. With no marked ancestor, the supplied package is a standalone workspace.
`app` and `lib` remain ordinary directory names. `build <directory>` recursively
discovers every visible package with a surface package-level `main` below that
scope; name a narrower directory to narrow the build. Nested marked workspaces
are independent and are not traversed. Resolution manifests live under
`workspace/.draft/resolutions/`, generated source objects under
`workspace/.draft/generated/`, and evidence under
`workspace/.draft/evidence/`; selecting a child package never creates a nested
package-local store.

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

`draft.workspace` is an optional durable build/run configuration as well as the
workspace boundary. It may name programs, a default program, discovery
exclusions, and compiler/run defaults; it never enumerates source files,
downloads dependencies, or changes language semantics. Turbo Draft uses the
same upward boundary discovery and may use the named default; `--source`
optionally chooses the initial direct-child file. Without it, the compiler uses
the first target-selected source in bytewise filename order; `package.draft`
remains only a convention. F5 checks/builds/runs the Program associated with an
active root-owned buffer and rejects an editing-only file outside that graph. It
honors the Program's effective target, optimization,
artifact/output, debug/assertion,
provider/asset, argument, environment, and working-directory settings. An
explicit IDE target replaces manifest targets, and non-executable artifacts are
built without being launched.
The repository's CMake `draftide` target compiles the IDE application itself at
O2; this is independent of the optimization selected for the Program built by
F5.
Saving `draft.workspace` or a provider-summary file affects the next Check,
Build, or F5 without reopening the IDE. Any parsed manifest change
conservatively invalidates the retained checked graph; provider-summary files
are reread even when the manifest is unchanged, and a changed effective summary
policy also invalidates that graph.

DraftIDE keeps agent work explicit. **Compile > Resolve Synthesis** is the only
IDE command that may fill or update `...` pins; **Compile > Judge Claims** is the
separate evidence command. They save dirty documents in the active root's
reachable graph (including shared package files), show a pending status, and
use the compiler-owned Codex CLI policy and the installed CLI's built-in default
model. Unrelated editing-only documents are untouched. Build, Build All
Programs, and F5 remain provider-free and fail on a missing or stale pin.
Resolve success still requires a later Build/F5.
Results always replace Build Output; compiler/provider errors raise Diagnostics,
while a completed negative judgment raises Build Output for its verdict.

**Compile > Expand Agent Comment** (Ctrl-E) is a separate unsaved editor
experiment, not `...` resolution. The cursor selects the maximal contiguous
same-marker `//?` or `//!` block containing its line. Snapshot reachable
workspace-owned Draft sources with current active/dirty overlays and send the
model the complete active file plus the selected kind, exact range, text, and
line. Treat other scattered annotations as context while making the selected
block the immediate request.

Rewrite only the active file and return exactly one complete replacement. A
rewrite may add imports, package declarations, and local or top-level code, but
must preserve unrelated behavior unless the selected annotation asks otherwise.
Do not edit or create another file or invent an external API. When a required
cross-file interface is unavailable, leave an honest precise TODO, minimal
compiling scaffold/no-op, retained annotation, or preserve existing behavior as
the local context warrants. Treat `//?` as persistent intent which is meant to
stay in the returned file. A `//!` annotation is transient: remove it, keep it,
or turn it into an ordinary comment according to whether the requested work is
complete. DraftIDE intentionally does not enforce these policies, parse the
final result, or make compiler validity an acceptance condition. Its host runs
one provider-free scratch check of the first complete candidate. If that check
has errors, Codex receives the candidate and bounded workspace-relative
diagnostics for exactly one advisory reconsideration; diagnostics may predate
the selected work, and the second result is neither rechecked nor rejected.
Apply the resulting complete file verbatim as one unsaved undo transaction,
write no file or pin, and edit no other file. Check remains the explicit visible
validation operation; the scratch check never replaces semantic Diagnostics.

F5 launches executables directly without a shell. Arguments remain literal and
ordered. Environment rows are `NAME=value` overrides on the inherited
environment with the last occurrence winning. Relative working directories are
workspace-relative; an absent one inherits DraftIDE's working directory. The
child inherits the restored primary terminal, and DraftIDE waits for Enter
after completion so program output remains readable before the TUI resumes.
Packages and Imports is populated from the compiler's target-selected reachable
graph, while Open Documents lists editor-owned documents. Typing runs only
the production lexer over the active complete file; it does not check packages
or refresh semantic views. Check and F5 submit the active file plus every other
dirty file
belonging to that graph as one transactional source-override set. F12 and
Shift-F12 first check pending edits before requesting exact semantic ranges.
F6 presents Compiler Options: the selected root package and target plus the
effective build configuration. Run Configuration separately edits exact argv,
environment, and working-directory values. Packages and Imports is an
expandable structured tree, and Window owns the other read-only semantic
reports. F12 uses the current successful
compiler graph to select an exact definition, Shift-F12 opens ordered usages,
and Alt-Left/Alt-Right traverse app-owned navigation history. A failed latest
check disables semantic jumps even though last-good summary reports remain
available. Compiler-distributed and dependency sources open in read-only
buffers whose document layer rejects mutation and save.

For `lib/turbo_ui`, reserve a classic shadow-capable button footprint with
`button_width(label)`, or an exact one-row flat face with
`compact_button_width(label)`; both measure terminal columns rather than UTF-8
bytes. Lists, trees, and read-only text views share
`Scrollbar_Visibility`; use `.when_needed` for compact panes while remembering
that the last column stays reserved to prevent horizontal jitter. A mutable
byte-owned window title is painted by calling
`begin_window(..., "")` followed immediately by `window_title_bytes`; do not
invent a mutable-slice-to-string conversion.

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
build/draftc --version
build/draftc lex path/to/file.draft
build/draftc syntax path/to/file.draft
build/draftc check path/to/workspace/package --target aarch64-macos
build/draftc check path/to/workspace/package --target aarch64-linux
build/draftc check path/to/workspace/package --target x86_64-linux
build/draftc check path/to/workspace/package --target x86_64-windows
build/draftc expand path/to/workspace/package --out /tmp/expanded-source \
  --target aarch64-macos
```

The version report names the public release, exact source commit, linked LLVM,
embedded core identity, and supported target selectors. Include it with a
compiler bug report; none of its display-only release metadata changes program
identity.

Fail-closed provider-free CI must omit every `resolve` spelling, including
`resolve --build` and `resolve --revalidate`: resolution may contact a provider
or rewrite pins. The consumer commands above reject missing or stale pins.

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
build/draftc emit-llvm path/to/workspace/package --target aarch64-macos
build/draftc emit-c-header path/to/workspace/package -o /tmp/package.h \
  --target aarch64-macos
```

`emit-llvm` prints one complete module per semantic package. Each block contains
that package's artifact-live globals and concrete runtime procedure definitions
and is an independent compiler input. Unreachable authored bodies are still
checked; they simply do not become MIR or native definitions. Module granularity
is an internal compiler invariant; there is no CLI option to change it.

Pass an explicit target for target-sensitive or cross-target work. The omitted
target selects only the compiler binary's native host and does not establish
portability.

`expand` performs a normal provider-free check and writes final selected source
plus generated-to-surface `.draft-map` sidecars. Its destination must not
already exist. It is useful when reviewing `...` expansions or feeding simple
editor tooling, but remains derived output: commit the resolution manifest and
referenced content-addressed generated Draft objects instead.

`resolve --build` is the explicit source-changing plus native-emission workflow.
Resolution commits only after the complete program checks; native lowering then
continues the returned graph without another front-end construction. It does
not implicitly execute tests, benchmarks, or judgments.

The positional path names the exact package for check/test/resolve-like
commands and a recursive search scope for `build`; names such as `app` and
`lib` have no special meaning. `build .` discovers every visible ordinary
package with a surface package-level `main` in the current workspace. Name a
package or subtree to narrow it. Default outputs mirror each root path under
`.draft/build/<target-file-tag>/`; `-o` requires exactly one discovered root.

For one durable workspace configuration, use the closed line-oriented marker:

```text
draft-workspace-v1
default = editor
exclude = build

[build]
target = aarch64-macos
optimization = O0
provider[aarch64-macos] = window=shared-library:build/libwindow.dylib
provider[x86_64-linux] = window=shared-library:build/libwindow.so

[program editor]
root = apps/editor
argument = document.txt
working-directory = .
environment = DRAFT_MODE=development
```

Workspace build defaults apply first, matching program overrides apply second,
and explicit CLI options win. Program `provider`, `provider-summary`, and
`runtime-asset` rows append to workspace rows in source order. Repeated CLI
provider/environment inputs replace their configured lists. Aggregate `build`
performs that merge independently for each discovered root; a CLI option
deliberately overrides every root, while
named programs may retain different targets, optimization, providers, artifact
kinds, runtime assets, and distinct outputs. Target is resolved before
target-qualified `main` discovery. `draftc run apps/editor -- document.txt`
builds one exact executable, inherits the terminal, and passes bytes after `--`
literally; those arguments replace configured `argument` rows. The driver
invokes no shell. Ordinary `draftc build .` still builds every discovered
program rather than silently selecting the configured default.
An `exclude` may name an absent derived directory such as `build`; a fresh
checkout does not have to create excluded paths before discovery.

Only native inputs have a target-qualified manifest form:
`provider[aarch64-macos]`, `provider-summary[x86_64-linux]`, and
`runtime-asset[x86_64-windows]`. A matching row appends after unconditional
rows. Every selector must name a built-in target. Do not invent conditioned
scalars, arguments, environment, scripts, interpolation, or build recipes;
`draft.workspace` remains closed operator policy rather than a CMake replacement.

## Native validation

On a matching native host:

```sh
build/draftc build path/to/workspace/package \
  --target aarch64-macos \
  -O0 \
  -o /tmp/draft-program
/tmp/draft-program
```

Use `--target aarch64-linux` or `--target x86_64-linux` on the matching Linux
architecture. Cross-checking can prove the front end and lowering contract,
but native execution requires the matching host toolchain and runtime.

Native `build`, `resolve --build`, `test`, and `bench` default to `-O0`; pass
`-O2` when the task requires optimized code. Native-only O0 object builds may
split a package above 48 live procedures into fixed internal units. O2 prepares
one summary-bearing module per semantic package, then runs whole-artifact
ThinLTO with cross-package importing and parallel native backends. The choice
may change derived native products, not Draft semantics, assertions, or
resolution pins. Validation evidence distinguishes O0 and O2 policy, so use
`bench -O2` when measuring optimized code. `emit-llvm` remains the canonical
pre-optimization inspection; use `build --kind assembly -O2` to inspect final
post-ThinLTO output. Do not invent O1, O3, size optimization, an
LTO/granularity flag, or arbitrary pass strings. O2 has no persistent ThinLTO
cache.
Ordinary `build` and `resolve --build` also omit source-level debug metadata;
pass `--debug-symbols` only when the requested result will be debugged or its
debug contract is under test. On macOS this publishes a dSYM, on Windows a PDB,
and on Linux it retains DWARF in the primary artifact.

For a library or artifact-specific task, select the actual kind:

```sh
build/draftc build path/to/workspace/package --kind object -o /tmp/package.o
build/draftc build path/to/workspace/package --kind static-library -o /tmp/libpackage.a
build/draftc build path/to/workspace/package --kind dynamic-library -o /tmp/libpackage.dylib
build/draftc build path/to/workspace/package --kind assembly -o /tmp/package-assembly
```

Use `.so` on Linux. Validate generated headers with an independent C compiler
when external ABI is the purpose. A successful Draft build alone does not prove
that a C consumer sees the intended layout and symbols.

## Measure compiler work

Use the driver timing report before changing compiler architecture for speed:

```sh
build/draftc check path/to/workspace/package --timings
build/draftc build path/to/workspace/package -o /tmp/program --timings=all
```

`--timings` reports major wall-clock phases and work counters to stderr.
`--timings=all` adds package/tool scopes, file I/O, lexing/parsing,
import-graph resolution, declaration ready-wave execution/publication,
procedure-flow and effect-closure stages, per-package LLVM
parse/verify/optimize/code-generation stages, and exclusive `self` time. Native
lowering counters expose its exact MIR-to-package-LLVM-unit-to-layout task graph,
including dependency edges, the initial ready set, worker slots, and units
which can start without a MIR prerequisite. Native-only O0 object builds may
report several fixed 48-procedure units for one large package; O2 and retained-
IR builds remain package-wide. Native and validation commands also
distinguish child-process CPU from wall time.
Complete semantic closure reports a joined effect/reference executor: package
flow closure occupies the first task rows while procedure-native-reference work
uses the same ready worker pool.
The earlier direct-semantic executor reports procedure direct-effect tasks and
one parsed-assembly task per package; both consume immutable checked bodies and
may overlap.
Compare phase structure and counters before comparing small durations, which
vary with the host and warm filesystem caches. Timing is diagnostic only and
never changes program identity or output.

Provider-using resolution reports `provider synthesis` for each ready wave and
counts both `synthesis provider ready waves` and actual provider calls. Calls in
one wave may overlap, so compare the enclosing wall time with the call count;
do not add per-worker timing writes to the single-threaded recorder.

Every complete semantic check publishes direct native-reference rows, even when
no artifact is requested. Native lowering additionally reports package
assembly, artifact reachability, live MIR, package LLVM units, and artifact-layout
work. Each live `MirProcedure` payload lives in its workspace product side table
rather than a package MIR program. After reachability, one exact closed executor
lets a package-unit task start when its assigned MIR tasks finish and lets its
layout follow without waiting for unrelated package MIR. A later native ready
set contains one object task per published unit plus any
selected package-assembly tasks. Worker counts are internal/test options, not
CLI flags. Repository determinism tests compare those counters across worker
counts; do not invent `--workers` or `-j`.

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
build/draftc test path/to/workspace/package --target aarch64-macos -O0
```

Use `*_bench.draft`, a `bench_` prefix, and exactly one
`^benchmark.Benchmark` for benchmarks:

```sh
build/draftc bench path/to/workspace/package --verify \
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

On a supported native host, the default build also asks the bootstrap to build
the Draft-written staging driver at O2. It currently implements `lex`, `syntax`,
target-qualified folder-package loading through `package-syntax`, canonical
recursive import-graph loading through `workspace-syntax`, and package
declarations plus file-local import aliases through `workspace-declarations`.
`workspace-public-names` adds direct unconditional public declarations and
two-part qualified lookup through those aliases. `workspace-target-declarations`
selects the package `when` frontier through target facts and demand-driven local
scalar constant products, then rebuilds that public view.
`workspace-interfaces` continues dependency-first through canonical
predeclared/named scalar types, scalar constants, explicitly typed globals,
pointer/multi-pointer/slice/fixed-array/tuple structures, fixed procedure types
and signatures, declaration-owned distinct types, ordinary named natural-layout
structs, ordinary enums, variants, and unions, and consumer-local imported
type/value reconstruction.
Named integer constants, enum values, and array counts share a typed evaluator
for literals, ready local/imported unsigned constants, target numeric facts,
grouping, unary sign/complement, binary `+`, `-`, `*`, `/`, `%`, `&`, `|`, and
`~` (xor), `<<`/`>>`, and explicit casts to concrete unsigned types no wider
than 64 bits. Direct integer `==`, `!=`, `<`, `<=`, `>`, and `>=` expressions
compare in that exact domain and publish booleans. Untyped operations remain
mathematically exact regardless of intermediate width and use infinite-two's-
complement bitwise semantics; concrete unsigned types and explicit integer
casts no wider than 64 bits reduce at their declared width. Shift counts must
be nonnegative, stay below a concrete left width, and fit the one-million-bit
constant resource bound. Named integer constants currently publish only
nonnegative u64 results, enum values must fit their
signed/u128 interface packet, and array counts require a positive result
representable by the target-sized count packet from an untyped value or exact
`usize`. Distinct interface identity uses
the defining content root,
root-relative package path, and original declaration name, and survives
transitive re-export while the underlying graph is rebuilt locally. Nominal
structs use the same persistent identity and retain source-order field names,
translated types, natural byte offsets, total size, and alignment. Pointer
recursion, grouped fields, private/transitive exposure, and struct nesting in
the other supported constructors are covered. Ordinary enum interfaces retain
the same persistent identity, explicit or inferred integer backing/layout, and
source-order names plus exact signed/u128 values. Explicit values use the shared
integer evaluator above, and implicit successors use checked exact arithmetic.
Ordinary variants retain source-order
payload-free/typed alternatives, explicit direct/distinct or inferred
discriminators, common payload offsets, and exact natural layout; pointer
recursion and transitive identity are covered. Ordinary unions retain
source-order grouped fields at byte offset zero, exact maximum-member natural
layout, pointer recursion, and transitive identity. C enums, packed/bit fields,
C or explicitly aligned aggregates, selected/synthesized/member-directive
regions, SIMD/parametric types, foreign/export, procedure contracts,
signed, wider, or non-integer cast destinations, comparison results nested in
the broader unsupported constant-expression vocabulary, negative or wider
named integer publication, and general constant/type forms remain an explicit
staging failure.
The lower `compiler/big_integer` package owns arbitrary-precision signed values
with explicit init/destroy lifetime and is qualified independently against
production C++ `BigInteger`. It is not a `draftc-next` command. The typed
interface evaluator consumes it through one root-owned temporary table and
stable value IDs, then converts only the final result into the consumer's finite
interface packet.
It is a build-tree development artifact, not an installed public command:

```sh
cmake --build build --target \
  draftc_next draft_package_syntax_oracle draft_workspace_syntax_oracle \
  draft_workspace_declarations_oracle \
  draft_workspace_public_names_oracle draft_big_integer_oracle --parallel
build/draftc-next lex compiler/syntax/lexer.draft
build/draftc-next syntax compiler/syntax/parser.draft
build/draftc-next package-syntax compiler/syntax --target aarch64-macos
build/draftc-next workspace-syntax tools/draftc_next \
  --workspace . --core core --core-identity local-core-qualification \
  --target aarch64-macos
build/draftc-next workspace-declarations tools/draftc_next \
  --workspace . --core core --core-identity local-core-qualification \
  --target aarch64-macos
build/draftc-next workspace-public-names tools/draftc_next \
  --workspace . --core core --core-identity local-core-qualification \
  --target aarch64-macos
build/draftc-next workspace-target-declarations tools/draftc_next \
  --workspace . --core core --core-identity local-core-qualification \
  --target aarch64-macos
build/draftc-next workspace-interfaces path/to/supported/root-package \
  --workspace path/to/workspace --core path/to/core \
  --core-identity local-core-qualification --target aarch64-macos
ctest --test-dir build --output-on-failure \
  -R '^draft_self_hosted_(frontend_units|big_integer_differential|lexer_differential|parser_differential|package_syntax_differential|workspace_syntax_differential|workspace_declarations_differential|workspace_public_names_differential|workspace_target_declarations_differential|workspace_interfaces_differential)$'
```

The big-integer differential builds its fixed Draft exerciser with O2 in
process-unique CMake-binary storage. It compares raw stdout/stderr files and
exit status with the production C++ implementation over parsing, canonical
sign and comparison, multi-limb arithmetic, signed division/remainder, shifts,
infinite-two's-complement bitwise operations, formatting, and host conversion
boundaries. Passing this gate qualifies the lower representation; it does not
by itself qualify a semantic consumer. The workspace-interface differential is
the gate proving that arithmetic, bitwise, complement, and shift expressions,
including wide intermediates and trapping/resource-bound failures, narrow
correctly through local, imported, and re-exported scalar, enum, and array
result packets.

The single-file differential tests compare C++ `draftc lex`/`draftc syntax`
with Draft `draftc-next lex`/`draftc-next syntax` over repository Draft sources
plus phase-specific malformed, over-nested, and missing inputs. The package
differential compares a helper around the production C++ package loader with
`draftc-next package-syntax` over repository folder packages, all four target
file tags, and explicit package-name, malformed-source, no-source, empty, and
missing-directory failures. All gates require identical stdout, stderr, and
exit status. The workspace differential additionally compares the production
C++ graph loader with Draft over the real frontend graph and explicit
workspace/dependency/core fixtures covering repeated imports, mapping errors,
cycles, missing or malformed packages, canonical aliasing and escape rejection
when the host permits symlink creation, root containment, and the 256-level
import-depth limit. It compares semantic root/package/import output without
physical paths. The workspace-declarations differential compares package/file
scope IDs, accepted symbols, source-only category/visibility/flags, native
spellings, canonical import targets, exact duplicate/modifier diagnostics, all
four target selectors, and graph-failure propagation. It deliberately stops
before dependency public-interface import and typing. The public-name
differential intersects final production PackageInterface and ImportedSymbol
rows with direct unconditional source declarations, then compares defining
symbol IDs, private-name exclusion, no import re-export, alias locality,
canonical targets, source order, all target file selectors, and earlier-phase
failure propagation. Conditional public declarations and typed interface
payloads remain outside that gate. The target-declaration differential replays
production ConditionalSelections through the source collector, then compares
Draft's target facts, condition order, appended Symbol IDs/categories/flags,
selected public/import names, all four profiles, and earlier failures. It also
checks forward and chained local constants, target-derived named values, named
feature strings, short-circuit validation, selected-branch constants feeding
nested conditions, dormant unrelated constants, and fail-closed demanded
arithmetic/cycles. Arithmetic and aggregate values, imported constants, types,
and compile-time procedures remain outside this target-selection evaluator. The
typed-interface differential then compares canonical reachable
scalar/structural/distinct/ordinary-struct/ordinary-enum/ordinary-variant/
ordinary-union type rows, fixed-array counts, exact natural aggregate layouts
and enum values, declaration classifications, scalar constant payloads, and
imported consumer-local type/value shapes on all four targets. Its supported
fixture includes forward local type/count aliases, qualified imported aliases
and counts, local/forward/imported/target integer arithmetic, concrete unsigned
wrapping, signed division/remainder, arbitrary-precision intermediates narrowing
through scalar/enum/array consumers, unsigned wrapping casts, exact integer
comparisons, imported cast targets, and re-exported comparison booleans,
structural
globals, fixed procedure types/signatures, tuple results, target page-size
arrays, separate same-underlying distinct declarations, private distinct
exposure, distinct-over-distinct, identity-preserving transitive re-export,
plain/grouped/pointer-recursive structs, private and transitive struct exposure,
struct arrays/procedure signatures, inferred/explicit/signed/u128 enums,
private and transitive enum exposure, imported enum values, enum nesting in
supported constructors, payload-free/typed/recursive variants,
distinct-integer discriminators, private/transitive variant exposure, variant
nesting in supported constructors, size-rounding/grouped/recursive and target-
selected unions, private/transitive union exposure, and union nesting. The
oracle first accepts C-layout, aligned, combined C/aligned, and selected-member
union fixtures before the self-hosted command rejects those valid production
forms at its staging boundary; synthesized union members also fail closed. C
enums, invalid enum arithmetic, invalid variants/unions, specialized aggregate
layouts/members, SIMD types, unsupported count operators/types, division by
zero, final enum values beyond the u128 packet, negative/wider named scalar
publication, and local alias or by-value layout cycles must fail at the exact
self-hosting boundary. Do not infer support for the production interface's
remaining nominal/parametric constructors or contracts from this narrow gate.
Keep the bootstrap path until a replacement phase has this oracle; passing
focused Draft tests alone is not replacement evidence.

For a frontend performance investigation, build the two explicitly requested
standalone targets from a Release CMake tree, then run the comparison on one
valid source. They are not default targets or installed commands, and their
benchmark hooks are absent from ordinary `draftc` and `draftc-next` binaries:

```sh
cmake --build build-release --target \
  draft_bootstrap_frontend_benchmark \
  draft_frontend_benchmark --parallel
python3 tools/compare_frontend_benchmarks.py \
  --build-dir build-release \
  --source compiler/syntax/parser.draft \
  --iterations 100
```

The child executables load before timing, perform one warmup and ten samples,
and report raw scanning, semicolon insertion, complete lexing, pre-tokenized
parsing, combined frontend work, and clock overhead. The coordinator requires
matching byte/token/node/checksum metadata before comparing medians. Use a
larger iteration batch when a phase approaches the clock row. Do not subtract
the clock result, add timers to production frontend code, or compare the syntax
CLIs when token/tree rendering is not the operation under investigation.

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

Windows CI obtains the complete pinned LLVM development archive through
`.github/scripts/install-windows-llvm.ps1`. That script verifies the upstream
SHA-256, expands the xz and tar layers explicitly with 7-Zip, and leaves one
immutable prefix for the version-and-digest cache. Do not replace it with the
smaller tool-only installer; the bootstrap also needs headers, CMake exports,
and static LTO/target libraries.

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

## Exercise an installed distribution

When a change touches executable/resource discovery, LLVM tools, the compiler
service, CMake installation, or release automation, passing build-tree tests is
not enough. Build DraftIDE, then run the registered isolated-prefix smoke:

```sh
cmake --build build --parallel
ctest --test-dir build --output-on-failure \
  -R '^draft_distribution_install_smoke$'
```

The smoke requires `draftc --version` and `draftide --version` to report
`toolchain: bundled`, builds and launches installed `examples/hello`, and starts
installed DraftIDE in noninteractive mode. It must not use checkout-relative
core source, the build-tree compiler service, or ambient LLVM tools.

For archive work, create the host package with CPack, extract it, and repeat
`tests/distribution_smoke_test.cmake` with `DRAFT_ROOT` naming the extracted
top-level directory. The exact commands and platform requirements are in
[`docs/operations/releases.md`](../../../../docs/operations/releases.md).
Ordinary CI runs the installed-prefix test on every native host. A `v*` tag
runs the separate workflow which qualifies all four extracted archives before
publication.

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
The CMake test graph handles the one mixed-process exception explicitly:
DraftIDE remains ordinary Draft code but loads the instrumented C++ compiler
service, so its service-backed tests preload the compiler-selected ASan runtime
on ELF. Do not add an ambient `LD_PRELOAD` to all generated-program tests.

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
