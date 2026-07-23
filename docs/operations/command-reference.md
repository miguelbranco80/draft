# Compiler command reference

Status: current bootstrap CLI. The executable is named `draftc`; examples below
use the default `build/draftc` CMake output.

All package commands take an explicit workspace directory as their first
positional path. The workspace is canonicalized before package selection and
the resolution-store boundary are established. A single-root command accepts
`--root <package>` as a normalized path relative to that workspace and defaults
to `--root .`; no directory name such as `app` has special meaning. Thus
`draftc check project` compiles the package in `project/`, while
`draftc check project --root tools/admin` compiles the package in
`project/tools/admin/` and permits imports such as `lib` from `project/lib/`.

All package commands accept
`--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows` where shown.
macOS remains the compatibility default. The workspace, selected root, and
target determine persistent namespaces. Generated source objects are shared by
content identity, while manifests and derived build artifacts are isolated as:

```text
<workspace>/.draft/generated/<hash>.draft
<workspace>/.draft/resolutions/<target-identity>/workspace/resolution.json
<workspace>/.draft/resolutions/<target-identity>/packages/<root>/resolution.json
<workspace>/.draft/evidence/<hash>.json
<workspace>/.draft/build/<target-file-tag>/workspace/<artifact>
<workspace>/.draft/build/<target-file-tag>/packages/<root>/<artifact>
```

Do not ignore the complete `.draft` directory in a Draft project that commits
resolved programs. Generated source and target/root manifests are the inputs
that make later `check`, `build`, `test`, and `bench` commands provider-free;
evidence is durable validation history. A workspace at the repository root can
ignore only disposable and interrupted-transaction state with:

```gitignore
/.draft/build/
/.draft/staging/
/.draft/tmp/
/.draft/evidence/*.tmp-*
```

The Draft compiler repository has a different Git concern: it contains several
nested example workspaces, so its own `.gitignore` uses recursive equivalents.
These repository-maintenance rules are not a convention imposed on user
projects, and `draftc` never reads `.gitignore` to decide package discovery.

All package commands also accept `--timings` or `--timings=all`. Put the option
anywhere after the package directory, alongside the command's other options;
for example, `draftc build examples/hello --timings=all`.
The compact form writes a hierarchical wall-clock report and deterministic
work counters to stderr after ordinary command output. It separates
resolution-manifest work, interface discovery and in-memory source transitions,
body-semantic continuation, provider-synthesis ready waves, MIR/LLVM code
generation, native object/link/debug-symbol work, and validation work when those
phases run.
Body continuation reports `package body starts`, `procedure bodies checked`,
`procedure body ready waves`, and `external procedure bodies materialized`.
They distinguish package body-work initialization, exact body tasks actually
run, workspace-wide frozen worker waves, and previously unseen cross-package
specializations.
The following `ABI classification` phase reports `ABI classification ready
waves`, `ABI classifications`, and `ABI classification worker slots`. These are
the number of newly classified semantic type rows, the bounded workers actually
used, and the single workspace-wide wave which owns them; later C and LLVM
consumers do not repeat this work.
Selection changes which reuse already completed products perform no body work
and need no special “reuse” counter. All rows remain command-local; none is
evidence of a persistent compiler cache.
Native commands additionally report the user and system CPU accounted to the
remaining Clang assembler/linker operations, other host tools, and validation
executables. In-process LLVM work contributes ordinary parent wall/CPU time.

`--timings=all` adds package/tool-level events, source discovery and I/O,
lexing/parsing, import-graph resolution, and each visible event's exclusive
`self` duration. Reports are diagnostic observations only: the flag does not
enter compiler configuration, resolved-program identity, manifests,
or emitted artifacts. Durations naturally vary with the host, while counters
and phase names describe the work the command actually performed. Failed
commands still print the completed portion of an enabled report, which makes
the option useful for locating failure-path costs.

## Run Turbo Draft

The default CMake build also produces `build/draftide`: a Draft-hosted terminal
IDE linked to the sibling bootstrap compiler service.

```sh
cmake --build build --target draftide --parallel
build/draftide path/to/workspace \
  [--root package/path] [--source package.draft] \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows]
```

Turbo Draft discovers executable roots without a manifest. Optional
`<workspace>/draft.project` requires `root = package/path` and may specify
`source = package.draft` to select the initial row; `--root` and `--source` are
explicit overrides. The target defaults to the native host. The manifest is
initial IDE operator configuration, not a source/package list or dependency
manifest.

Turbo Draft opens ordinary files; the active buffer and every other dirty
project buffer form one in-memory compiler transaction, and saving updates the
same files. File > Open Workspace swaps to another validated directory after
explicit dirty-buffer handling. F5 checks, builds, and runs the selected root.
F6 opens the root list and package/dependency view; F7-F11 toggle other
compiler-derived semantic windows. F12 cycles roots, and Shift-F12 targets.

The executable and `draft_compiler_service` shared library must remain
discoverable as siblings. The build records a loader-relative lookup on macOS
and ELF; Windows places the DLL beside the executable. `--smoke` performs a
noninteractive compiler/UI composition check for CI.

## Inspect and compile

```sh
build/draftc lex path/to/file.draft
build/draftc syntax path/to/file.draft
build/draftc check path/to/workspace [--root package/path] \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] [--timings|--timings=all]
build/draftc emit-llvm path/to/workspace [--root package/path] \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] [--timings|--timings=all]
build/draftc emit-c-header path/to/workspace [--root package/path] [-o output.h] \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] [--timings|--timings=all]
build/draftc target [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows]
```

`check` runs the provider-free front end and semantic pipeline. If the program
contains saved `...` expansions, it loads and revalidates them. It never starts
a provider. `emit-llvm` additionally lowers MIR and prints one complete LLVM
module per semantic package in package order. Each module contains that
package's globals and source-ordered concrete procedure definitions.

## Expand checked source

```sh
build/draftc expand path/to/workspace [--root package/path] --out new-directory \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] [--assertions=off] \
  [--provider name=object|archive|shared-library:/absolute/path]... \
  [--provider-summary name:/absolute/path]... \
  [--runtime-asset name:/absolute/file-or-directory]... \
  [--timings|--timings=all]
```

`expand` compiles and authenticates the resolved program without a provider,
then writes every selected Draft and assembly source file under a new output
tree. Generated fragments are already substituted in the copied Draft bytes.
`draft-expanded-source.map` records safe output-root names and semantic package
root identities; every source has a `.draft-map` sidecar recording any
generated-to-surface intervals. The output directory must not exist, preventing
stale files from a previous graph from surviving in the new projection.

This tree is an explicitly requested inspection artifact, not a cache or a
second source of truth. Continue to commit the selected
`.draft/resolutions/<target-identity>/{workspace|packages/<root>}/resolution.json`
and every referenced `.draft/generated/<hash>.draft` object. Do not commit native
intermediates, transaction staging, or an expanded tree unless the project has
a separate reason to version that derived view.

## Build native artifacts

```sh
build/draftc build path/to/workspace [--root package/path]... [-o output] \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] \
  [--kind executable|object|static-library|dynamic-library|assembly] \
  [-O0|-O2] \
  [--assertions=off] \
  [--provider name=object|archive|shared-library:/absolute/path]... \
  [--provider-summary name:/absolute/path]... \
  [--runtime-asset name:/absolute/file-or-directory]... \
  [--timings|--timings=all]
```

`build` is always provider-free. It compiles handwritten programs directly and
resolved programs from their saved generated Draft source. It rechecks source,
generated expansions, foreign artifacts, summaries, and runtime assets, but it
does not contact Codex, execute judgments, require validation evidence, or
modify the resolution manifest.

Each `--provider` artifact path is absolute and names a real regular file;
symlinks are rejected so hashing and later identity checks never depend on link
retargeting. Resolve a build-system convenience symlink with `realpath` before
passing a dylib or shared object. A `shared-library` mapping supplies the link
input. ELF executable and dynamic-library links record `$ORIGIN` as a runtime
search directory so a copied provider can live beside the artifact. Mach-O
still follows the provider's install name and normal `@rpath`/loader rules;
Windows requires the matching DLL beside the executable or otherwise visible
to the loader. The complete vendored example is
[`examples/raylib-asteroids`](../../examples/raylib-asteroids/README.md).

Native builds default to `-O0`, which skips LLVM middle-end optimization and
selects LLVM's fastest no-optimization target-machine level. `-O2` runs LLVM's
default O2 pipeline independently over each complete semantic-package module,
then uses the matching default target-machine optimization level. The choice
applies equally to object and compiler-produced assembly output. Authored
package assembly is copied or assembled exactly as written.

Optimization may change only derived native bytes. It does not change source
semantics, assertions, package-module granularity, resolution pins, synthesis
context, or resolved-program identity. `emit-llvm` deliberately prints the
canonical pre-optimization package modules; use `build --kind assembly -O2` to
inspect optimized native assembly. Draft currently exposes only `-O0` and
`-O2`, and performs no cross-package LTO.

Without `--root`, `build` recursively discovers every ordinary surface package
under the workspace that contains a package-level procedure named `main` and
builds all of them in canonical root-path order. Discovery does not follow
symlinks, enter a directory whose leaf begins with `.`, descend into configured
core or dependency roots, enter a `draft-expanded-source.map` projection, or
select target-inapplicable, test, or benchmark files. Compiler-generated build
and resolution state is already below hidden `.draft`. A malformed discovered package or any selected executable
that fails normal compilation makes the aggregate command fail. Repeated
`--root` options replace discovery with exactly that subset; this is also how a
library package without `main` is selected for object, archive, dynamic-library,
or assembly output.

Each selected root currently receives an independent compiler graph and native
pipeline. Shared imported packages may therefore be analyzed or emitted more
than once during one aggregate command. This is a performance limitation, not a
semantic obstacle to building all executables. Later command-local deduplication
may reuse work keyed by source identity, target, and compiler configuration;
persistent object caching remains a separate later feature with an explicit
invalidation contract.

Default output paths mirror the selected package folder below the target build
namespace. For example, roots `cli` and `tools/admin` produce executables at
`.draft/build/aarch64-macos/packages/cli/cli` and
`.draft/build/aarch64-macos/packages/tools/admin/admin`. The folder path, not
only the package's short name, prevents output collisions. Duplicate explicit
roots are rejected. `-o` is accepted only when exactly one root is selected;
an aggregate build with several discovered or explicit roots must use their
independent default paths.

For non-assembly native builds, the native adapter emits one internal linker-
input object per semantic package through the LLVM 22 library linked into
`draftc`. On Mach-O and ELF, `--kind object` publishes one relocatably linked
whole-graph object. COFF has no partial-link equivalent: it publishes a
single-input `.obj` and requires `--kind static-library` for a multi-input
Draft-owned graph. Mapped providers cannot be embedded in that archive; use a
final executable/DLL link or supply them when consuming the Draft object or
archive. Clang, `dsymutil`, and LLVM utilities default to the matching tools
directory selected while building the compiler.
On macOS it additionally uses the Apple linker/SDK and `libtool`; on Linux it
uses `ld.lld`, `llvm-ar`, and the target's libc development files. These
installations are compiler operational prerequisites, not resolution-manifest
inputs. Ordinary builds do not execute a toolchain-version probe.

Windows defaults to `.exe`, `.obj`, `.lib`, and `.dll` names. A linked PE
executable or DLL also publishes a deterministic sibling `.pdb`; a DLL
publishes its import `.lib` as a second companion. The command prints both
companion paths when present. A native Windows compiler build links the official
LLVM-C 22 development distribution; matching Clang/lld-link/llvm-lib and an x64
Visual Studio/Windows SDK environment are operational prerequisites. Ordinary
`check`, `emit-llvm`, `emit-c-header`, and `build` work on that host. The
bootstrap Windows validation runner, durable resolution-store lock, and Codex
provider subprocess adapters are not yet implemented. Native `test` and `bench`
therefore fail explicitly. Any `resolve` which must write or re-pin a manifest,
including provider-free `--revalidate`, also fails explicitly; active `judge`
runs requiring Codex are unavailable, while `judge --list` works. Provider-free
builds of already resolved source remain ordinary `build` commands.

All completely lowered package modules and package-assembly inputs form one
bounded native ready set. Workers emit only
task-local bytes; after they join, the compiler reports the lowest task-ID
failure or publishes files and linker inputs in canonical artifact-layout
order. `--timings=all` lists these task measurements in that same order and
reports `native object tasks` and `native object workers` counters. Worker
scheduling never changes diagnostics or artifact identity.

Assembly output is a directory containing one `.s` file per semantic package
module and one exact copied source per package assembly input.
Object output performs a relocatable link over the complete package graph.
Static archives use
deterministic metadata, shared libraries receive a platform install name or
SONAME, and executables add hosted entry glue.

## Resolve synthesis sites

```sh
build/draftc resolve path/to/workspace [--root package/path] [--revalidate] [--build] \
  [--regenerate [site-id]] \
  [-o output] \
  [--kind executable|object|static-library|dynamic-library|assembly] \
  [-O0|-O2] \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] [--assertions=off] \
  [--model model] \
  [--provider name=object|archive|shared-library:/absolute/path]... \
  [--provider-summary name:/absolute/path]... \
  [--runtime-asset name:/absolute/file-or-directory]... \
  [--timings|--timings=all]
```

`resolve` constructs typed obligations and calls the configured provider only
for sites that cannot reuse a valid saved expansion. The provider returns Draft
source, which is parsed and checked through the ordinary pipeline. The compiler
commits generated objects and the manifest only after checking the complete
candidate program. It does not run tests, benchmarks, or judgments.
Independent sites in one semantic ready set are called concurrently, up to four
Codex processes. Proposal checking remains sequential in canonical package and
obligation order after each wave joins; only rejected sites enter the next
correction wave. Timings report `provider synthesis` wall time plus ready-wave
and call counters.

`--build` continues the final checked graph through MIR, LLVM, and the native
adapter after a successful resolution commit. It accepts the same `-o` and
`--kind` artifact choices and `-O0`/`-O2` optimization selection as `build`;
those options are rejected without `--build`. The compiler does not rerun its
front end between resolving and emitting the artifact. A later ordinary build
of the committed source at the same optimization level produces the same native
program. If native emission fails, the already checked source transaction
remains committed.

`--regenerate` forces a provider proposal even for fresh pins. With no following
value it selects every current synthesis site; with one exact `site-...`
identity it selects only that site. The selector must match, unrelated fresh
pins remain exact, and a failed attempt leaves the committed manifest unchanged.
It cannot be combined with `--revalidate`.

The Codex adapter discovers `codex` through the user's `PATH` and uses that
installation's ordinary authentication and configuration. If `--model` is
omitted, the adapter also uses the Codex-configured default model. Executable
paths, distribution hashing, and credentials are not Draft program inputs.
The compiler binary contains the complete Draft coding skill used by synthesis;
users do not install or locate a separate skill directory. It is materialized
once only when the command actually needs Codex, then shared read-only through
the otherwise separate request directory for each site. Its digest is recorded
as generation provenance and does not invalidate accepted source.

`--revalidate` rechecks all saved sites without a provider and writes a fresh
source-resolution manifest. It cannot be combined with a provider model.
External provider, summary, and runtime-asset mappings become content-addressed
resolved-program inputs. Validation and judgment evidence remain separate and
are changed only by their own commands.

The root/target-specific resolution manifest and all generated objects it
references are normal project source and should ordinarily be committed
together. Resolving one executable never replaces a sibling executable's
manifest, and changing targets selects a separate row. A clean checkout
can therefore `check`, `expand`, `test`, or `build` without Codex credentials.
The compiler creates no persistent AST, HIR, MIR, native-object, or incremental
cache; `.draft/generated` is source, not cached compiler state.

## Test and benchmark

```sh
build/draftc test path/to/workspace [--root package/path] \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] [-O0|-O2] \
  [--instrument address|...] \
  [--provider name=object|archive|shared-library:/absolute/path]... \
  [--provider-summary name:/absolute/path]... \
  [--runtime-asset name:/absolute/file-or-directory]... \
  [--timings|--timings=all]

build/draftc bench path/to/workspace [--root package/path] [--verify] \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] [-O0|-O2] \
  [--instrument address|...] \
  [--provider name=object|archive|shared-library:/absolute/path]... \
  [--provider-summary name:/absolute/path]... \
  [--runtime-asset name:/absolute/file-or-directory]... \
  [--timings|--timings=all]
```

These commands compile an isolated native validation harness, execute it, and
append a typed evidence attempt. A passing attempt becomes active; a semantic
failure revokes that exact key. Benchmark validation uses one warmup and ten
process-isolated samples. `--verify` is a readable CI/release spelling; the
benchmark still executes and records fresh evidence.

Both commands default to `-O0` and accept the same compiler-owned `-O2`
package-module pipeline as `build`. The selected level is part of the
validation policy identity, so O0 and O2 runs produce distinct evidence keys;
it remains absent from resolved-program identity. In particular, pass `-O2`
when the recorded benchmark should measure optimized code.

Evidence is stored only below the explicit workspace's `.draft/evidence`
directory. Its content-addressed object and state keys bind the selected root,
resolved program, target, toolchain, environment, native optimization level,
and policy; selecting a child package never creates a second `.draft`
directory below that package.

The macOS target currently supports `--instrument address`. The compiler owns
the AddressSanitizer IR attributes and in-process LLVM pass, then lets the
matching LLVM Clang driver link its runtime. Other instrumentation kinds and the
Linux Draft sanitizer profile fail closed with an explicit diagnostic.

## Judge

```sh
build/draftc judge path/to/workspace [--root package/path] [selector...] [--list] \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] [--assertions=off] \
  [--judge-validator identity:model]... \
  [--judge-artifact kind:/absolute/path]... \
  [--model model] \
  [--provider name=object|archive|shared-library:/absolute/path]... \
  [--provider-summary name:/absolute/path]... \
  [--timings|--timings=all]
```

`--list` prints stable judgment site identities without configuring a provider.
A selector may be an exact `site-...` identity, a package identity/path, or
`package:anchor`; several selectors form a de-duplicated union. Each configured
validator receives the same typed claim and exact requested artifact bytes. A
completed run records one independent evidence attempt per selected judgment;
failures remain in append-only evidence history and revoke only their exact
active keys. `--model` selects the ordinary single-validator model and cannot be
combined with explicit `--judge-validator` rows.

Judgment evidence is inspection and qualification data. Ordinary `build` does
not run a provider and does not require a judgment result.
