# Compiler command reference

Status: current bootstrap CLI. The executable is named `draftc`; examples below
use the default `build/draftc` CMake output.

Package commands take the package directory itself as their first positional
path. `draftc` canonicalizes that directory and searches upward for the nearest
`draft.workspace`; its containing directory owns workspace-relative imports and
`.draft/` state. If no marker exists, the supplied package is a standalone
workspace. Thus `draftc check project/tools/admin` checks that exact package;
with `project/draft.workspace` present, `import lib/text` resolves from
`project/lib/text` from the package's single path coordinate.

`build` treats its path as a recursive discovery scope and builds every package
with a surface `main` below it. Therefore `draftc build .` is the ordinary
"build every program in this workspace" command; there is no separate
`programs` or required `--all` command. Nested `draft.workspace` directories
are independent workspaces and are not traversed. If discovery finds no program
and a non-executable `--kind` was requested, the path names one exact library
package.

The optional marker is also a small durable operator configuration:

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

Top-level `exclude` rows prune recursive discovery; a named directory that does
not yet exist is simply absent from the tree. `[build]` accepts `target`,
`optimization`, `kind`, `output`, `debug-symbols`, `assertions`, and repeated
`provider`, `provider-summary`, and `runtime-asset` rows. A named program
requires `root`, may override those build values, and may add repeated run
`argument`/`environment` rows plus one `working-directory`. It never lists
source files, changes import rules, downloads dependencies, or invokes a shell.
The three native-input keys alone also accept an exact built-in target selector,
as in `provider[aarch64-macos]`, `provider-summary[x86_64-linux]`, or
`runtime-asset[x86_64-windows]`. Matching rows append after unconditional rows;
unknown selectors are errors even when another target is active. Scalars,
arguments, environment, scripts, and arbitrary expressions cannot be
conditioned.
CLI options replace scalar defaults; the first repeated CLI mapping replaces
the corresponding manifest list. Arguments after `run ... --` replace manifest
arguments. An aggregate `build` resolves this precedence independently for each
discovered root. Named programs may therefore select different targets,
optimization levels, artifact kinds, provider inputs, runtime assets, and
explicit output paths in one command. A CLI build option remains one deliberate
override for every selected root.

All package commands accept
`--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows` where shown.
macOS remains the compatibility default. A manifest target supplies the default
when present, and an explicit CLI target wins. The workspace, selected root, and
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
Target lowering reports `native lowering tasks`, exact dependency edges, the
initial ready-set size, worker slots, and semantic publication waves. MIR,
package-LLVM-unit, and artifact-layout task counts are separate. A package unit
with no live MIR prerequisites appears in `package LLVM units initially ready`;
it can execute alongside another package's MIR rather than waiting at a
workspace-wide phase barrier. `package LLVM unit tasks` may exceed the package
count only for a native-only O0 object build with more than 48 live procedures
in a package.
Semantic closure reports `effect/reference ready waves`, task counts, and
worker slots because package effect-flow tasks and procedure native-reference
tasks share one executor after direct effects publish.
Detailed output also separates closure input preparation, direct effects and
assembly, effect/reference closure, SCC-product publication, denial products,
and interface finalization. SCC counters distinguish exact local component rows
from the compact package effect-closure barriers and report their dependency
edge count.
Selection changes which reuse already completed products perform no body work
and need no special “reuse” counter. All rows remain command-local; none is
evidence of a persistent compiler cache.
Native commands additionally report the user and system CPU accounted to the
remaining Clang assembler/linker operations, other host tools, and validation
executables. In-process LLVM work contributes ordinary parent wall/CPU time.

`--timings=all` adds package/tool-level events, source discovery and I/O,
lexing/parsing, import-graph resolution, and each visible event's exclusive
`self` duration. The `source file tasks`, `source file task waves`, and `source
file worker slots` counters describe whole-file read/lex/parse scheduling;
individual file events are replayed in canonical filename order after those
tasks join, so their durations may overlap. Declaration semantics is divided
into ready-wave selection, task preparation, bounded task execution, and
coordinator publication. Effect closure separates procedure-flow convergence,
SCC construction, transitive effect propagation, and call-site composition.
Each parallel production LLVM package row separates direct module construction,
target validation, one-time verification, target-machine setup, optional
O2/ASan passes, machine-code emission, and output copying. Input preparation and
textual IR parsing appear only when the explicit external-oracle route is
exercised.

Reports are diagnostic observations only: the flag does not enter compiler
configuration, resolved-program identity, manifests, or emitted artifacts.
Durations naturally vary with the host, while counters and phase names describe
the work the command actually performed. Failed commands still print the
completed portion of an enabled report, which makes the option useful for
locating failure-path costs.

## Run Turbo Draft

The default CMake build also produces `build/draftide`: a Draft-hosted terminal
IDE linked to the sibling bootstrap compiler service.

```sh
cmake --build build --target draftide --parallel
build/draftide path/to/workspace \
  [--source package.draft] \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows]
```

Turbo Draft uses the same upward workspace discovery and `draft.workspace`
default program as `draftc`. Without a marker it discovers executable roots in
the standalone directory. `--source` may select the initial direct package
file; otherwise the compiler opens the first target-selected source in bytewise
filename order, and `package.draft` remains only a convention. The target
defaults to the native host. Unless `--target` supplies an explicit override,
selecting a named program applies its effective workspace/program target.
IDE-local root/window selections are not stored in the workspace manifest.

Turbo Draft opens ordinary files; the active buffer and every other dirty
workspace buffer form one in-memory compiler transaction, and saving updates
the same files. File > Open Folder swaps to another validated directory after
explicit dirty-buffer handling. Build and F5 use the complete effective
`[build]` plus matching `[program]` configuration: target, optimization,
artifact kind/output, debug symbols, assertions, providers and summaries, and
runtime assets. F5 runs an executable with the program's arguments,
environment overrides, and working directory; a non-executable artifact is
built but deliberately not launched.
Saving `draft.workspace` affects the next Check, Build, or F5 without reopening
the IDE. Provider-summary files are likewise reread at that foreground
boundary; unchanged structural configuration keeps the retained checked graph.
F1 opens the complete shortcut reference. F6 opens the program-root list and
package/dependency view; F7-F11 toggle other compiler-derived semantic windows.
F12 goes to the exact semantic definition beneath the cursor, Shift-F12 opens
ordered usage locations, and Alt-Left/Alt-Right move through navigation
history. Definitions in compiler-distributed core or dependency sources open
as read-only buffers.

The executable and `draft_compiler_service` shared library must remain
discoverable as siblings. The build records a loader-relative lookup on macOS
and ELF; Windows places the DLL beside the executable. `--smoke` performs a
noninteractive compiler/UI composition check for CI.

## Inspect and compile

```sh
build/draftc lex path/to/file.draft
build/draftc syntax path/to/file.draft
build/draftc check path/to/package \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] [--timings|--timings=all]
build/draftc emit-llvm path/to/package \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] [--timings|--timings=all]
build/draftc emit-c-header path/to/package [-o output.h] \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] [--timings|--timings=all]
build/draftc target [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows]
```

`check` runs the provider-free front end and semantic pipeline. If the program
contains saved `...` expansions, it loads and revalidates them. It never starts
a provider. Target-specific parsed assembly is checked and retained with its
owning procedure bodies. `emit-llvm` additionally lowers MIR and prints one
complete LLVM module per semantic package in package order. Each module
contains that package's globals and source-ordered concrete procedure
definitions. A root module also contains its small entry/validation wrapper,
but not the invariant hosted runtime implementation, which is embedded in the
compiler as a separate exact-target object.

`import core/...` reads immutable source rows embedded in the same compiler
binary. The bundle has a build-time content identity and is parsed directly
from memory; no command searches the checkout, executable directory, current
directory, or an environment variable for core source. The separately embedded
hosted-runtime object remains a backend input rather than Draft source.

## Expand checked source

```sh
build/draftc expand path/to/package --out new-directory \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] [--assertions=off] \
  [--provider name=object|archive|shared-library:/absolute/path]... \
  [--provider-summary name:/absolute/path]... \
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
build/draftc build path/to/package-or-directory [-o output] \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] \
  [--kind executable|object|static-library|dynamic-library|assembly] \
  [-O0|-O2] \
  [--debug-symbols] \
  [--assertions=off] \
  [--provider name=object|archive|shared-library:/absolute/path]... \
  [--provider-summary name:/absolute/path]... \
  [--runtime-asset name:/absolute/file-or-directory]... \
  [--timings|--timings=all]
```

`build` is always provider-free. It compiles handwritten programs directly and
resolved programs from their saved generated Draft source. It rechecks source,
generated expansions, explicit summaries, and linker/deployment paths, but it
does not contact Codex, execute judgments, require validation evidence, or
modify the resolution manifest.

Each `--provider` artifact path is resolved against the command's current
directory and must name a real regular file. Symlinks are rejected so the
linker input cannot be silently retargeted through the mapping; resolve a
build-system convenience symlink with `realpath` before passing a dylib or
shared object. A `shared-library` mapping supplies the link input. ELF
executable and dynamic-library links record `$ORIGIN` as a runtime
search directory so a copied provider can live beside the artifact. Mach-O
still follows the provider's install name and normal `@rpath`/loader rules;
Windows requires the matching DLL beside the executable or otherwise visible
to the loader. The complete vendored example is
[`examples/raylib-asteroids`](../../examples/raylib-asteroids/README.md).

Native builds default to `-O0`, which skips LLVM middle-end optimization and
selects LLVM's fastest no-optimization target-machine level. A native-only O0
object build splits packages larger than 48 artifact-live procedures into fixed
48-procedure LLVM units that can emit concurrently. `-O2` prepares one complete
summary-bearing module per semantic package, then runs one whole-artifact
ThinLTO operation. LLVM may import procedures across package boundaries and
runs its native backends in parallel. Compiler-produced assembly and retained
LLVM text remain package-wide at both levels. Authored package assembly is
copied or assembled exactly as written.

Optimization may change only derived native products. It does not change source
semantics, assertions, resolution pins, synthesis context, or resolved-program
identity. The O0 object partition is canonical and independent of worker count.
`emit-llvm` deliberately prints the canonical pre-optimization package modules;
use `build --kind assembly -O2` to inspect final post-ThinLTO assembly. Draft
currently exposes only `-O0` and `-O2`. There is no LTO mode or granularity flag
and no persistent ThinLTO cache.

Ordinary builds omit LLVM source-location metadata and platform debug
companions. Pass `--debug-symbols` to construct full source locations and, for
linked Mach-O or PE/COFF artifacts, publish the sibling `.dSYM` or `.pdb`.
ELF keeps requested DWARF in the primary artifact and therefore still returns
no companion path. The flag changes derived debug products only; assertions,
optimization, source semantics, and resolution identity remain independent.

`build` recursively discovers every ordinary surface package under the supplied
directory that contains a package-level procedure named `main` and builds all
of them in canonical root-path order. Discovery does not follow
symlinks, enter a directory whose leaf begins with `.`, descend into configured
dependency roots or nested workspaces, enter a
`draft-expanded-source.map` projection, visit a manifest `exclude`, or
select target-inapplicable, test, or benchmark files. Compiler-generated build
and resolution state is already below hidden `.draft`. A malformed discovered package or any selected executable
that fails normal compilation makes the aggregate command fail. Supplying a
narrower package directory narrows discovery to that subtree. If no `main` is
found and `--kind` is object, static-library, dynamic-library, or assembly, the
supplied directory is built as one exact library package.

Each selected root currently receives an independent compiler graph and native
pipeline under its own effective `[build]`, matching `[program]`, and CLI
configuration. Target is resolved before executable inspection because
target-qualified source may contain `main`; named programs whose target differs
from the workspace default are inspected exactly under that target. Shared
imported packages may therefore be analyzed or emitted more than once during
one aggregate command. This is a performance limitation, not a semantic
obstacle to building all executables. Later command-local deduplication may
reuse work keyed by source identity, target, and compiler configuration;
persistent object caching remains a separate later feature with an explicit
invalidation contract.

Default output paths mirror the selected package folder below the target build
namespace. For example, roots `cli` and `tools/admin` produce executables at
`.draft/build/aarch64-macos/packages/cli/cli` and
`.draft/build/aarch64-macos/packages/tools/admin/admin`. The folder path, not
only the package's short name, prevents output collisions. `-o` is accepted
only when exactly one root is selected; an aggregate build with several
discovered roots must use their
independent default paths. Distinct `[program]` `output` values are valid in an
aggregate build; duplicate explicit paths are rejected before compilation so a
later root cannot overwrite an earlier artifact. A workspace-wide `output`
therefore requires a single selected root unless every program replaces it with
a distinct path.

## Build and run one program

```sh
build/draftc run path/to/package [build options] [-- argument...]
```

`run` selects one exact package, builds an executable through the same native
pipeline as `build`, attaches the child directly to the current standard input,
output, and error streams, waits, and returns its exit status. It never invokes
a shell. `--cwd <directory>` selects a working directory and repeated
`--env NAME=value` rows override the inherited environment. If the path is the
workspace directory and `draft.workspace` declares `default`, that named
program is selected. Arguments after `--` are literal program arguments and
replace configured `argument` rows; compiler flags are not recognized after
the separator.

For non-assembly native builds, the native adapter emits one internal linker-
input object per semantic package through the LLVM 22 library linked into
`draftc`. Final executables and libraries also consume the exact target's
compiler-embedded hosted-runtime object. Relocatable `--kind object` omits that
runtime input and leaves its symbols unresolved for the eventual final link.
On Mach-O and ELF it publishes one relocatably linked whole-package-graph
object. COFF has no partial-link equivalent: it publishes a single-package
`.obj` and requires `--kind static-library` for a multi-package or
package-assembly graph. Mapped providers cannot be embedded in that archive; use a
final executable/DLL link or supply them when consuming the Draft object or
archive. Clang, `dsymutil`, and LLVM utilities default to the matching tools
directory selected while building the compiler.
On macOS it additionally uses the Apple linker/SDK and `libtool`; on Linux it
uses `ld.lld`, `llvm-ar`, and the target's libc development files. These
installations are compiler operational prerequisites, not resolution-manifest
inputs. Ordinary builds do not execute a toolchain-version probe.

Windows defaults to `.exe`, `.obj`, `.lib`, and `.dll` names. With
`--debug-symbols`, a linked PE executable or DLL also publishes a deterministic
sibling `.pdb`; a DLL
publishes its import `.lib` as a second companion. The command prints both
companion paths when present. A native Windows compiler build links the official
LLVM 22 development distribution; matching Clang/lld-link/llvm-lib and an x64
Visual Studio/Windows SDK environment are operational prerequisites. Ordinary
`check`, `emit-llvm`, `emit-c-header`, and `build` work on that host. The
bootstrap Windows validation runner, durable resolution-store lock, and Codex
provider subprocess adapters are not yet implemented. Native `test` and `bench`
therefore fail explicitly. Any `resolve` which must write or re-pin a manifest,
including provider-free `--revalidate`, also fails explicitly; active `judge`
runs requiring Codex are unavailable, while `judge --list` works. Provider-free
builds of already resolved source remain ordinary `build` commands.

All completely lowered package LLVM units and package-assembly inputs form one
bounded native ready set. Workers emit only
task-local bytes; after they join, the compiler reports the lowest task-ID
failure or publishes files and linker inputs in canonical artifact-layout
order. `--timings=all` lists these task measurements in that same order and
reports `native object tasks` and `native object workers` counters. Worker
scheduling never changes diagnostics or artifact identity.

Assembly output is a directory containing one `.s` file per semantic package
module, the hosted runtime assembly, and one exact copied source per package
assembly input. Object output performs a relocatable link over the package
graph while deliberately leaving runtime and foreign references unresolved.
Static archives use
deterministic metadata, shared libraries receive a platform install name or
SONAME, and executables add hosted entry glue.

## Resolve synthesis sites

```sh
build/draftc resolve path/to/package [--revalidate] [--build] \
  [--regenerate [site-id]] \
  [-o output] \
  [--kind executable|object|static-library|dynamic-library|assembly] \
  [-O0|-O2] \
  [--debug-symbols] \
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
Foreign-provider mappings, summaries, and runtime assets remain explicit
invocation inputs and are not written to that manifest. Validation and judgment
evidence remain separate and are changed only by their own commands.

The root/target-specific resolution manifest and all generated objects it
references are normal project source and should ordinarily be committed
together. Resolving one executable never replaces a sibling executable's
manifest, and changing targets selects a separate row. A clean checkout
can therefore `check`, `expand`, `test`, or `build` without Codex credentials.
The compiler creates no persistent AST, HIR, MIR, native-object, or incremental
cache; `.draft/generated` is source, not cached compiler state.

## Test and benchmark

```sh
build/draftc test path/to/package \
  [--target aarch64-macos|aarch64-linux|x86_64-linux|x86_64-windows] [-O0|-O2] \
  [--instrument address|...] \
  [--provider name=object|archive|shared-library:/absolute/path]... \
  [--provider-summary name:/absolute/path]... \
  [--runtime-asset name:/absolute/file-or-directory]... \
  [--timings|--timings=all]

build/draftc bench path/to/package [--verify] \
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

Both commands default to `-O0` and accept the same compiler-owned native
pipeline as `build`; O2 uses whole-artifact ThinLTO. The selected level is part of the
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
build/draftc judge path/to/package [selector...] [--list] \
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
