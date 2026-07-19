# Compiler command reference

Status: current bootstrap CLI. The executable is named `draftc`; examples below
use the default `build/draftc` CMake output.

All package commands accept `--target aarch64-macos|aarch64-linux` where shown.
macOS remains the compatibility default. Package paths are canonicalized before
the workspace and resolution-store boundaries are established.

All package commands also accept `--timings` or `--timings=all`. Put the option
anywhere after the package directory, alongside the command's other options;
for example, `draftc build examples/hello-world -o hello_world --timings=all`.
The compact form writes a hierarchical wall-clock report and deterministic
work counters to stderr after ordinary command output. It separates
resolution-manifest work, every interface-discovery round, body-surface
compilation, final MIR/LLVM code generation, native object/link/debug-symbol
work, and validation work when those phases run. Native commands additionally
report the user and system CPU accounted to Clang, the linker, other host tools,
and validation executables.

`--timings=all` adds package/tool-level events, source discovery and I/O,
lexing/parsing, import-graph resolution, and each visible event's exclusive
`self` duration. Reports are diagnostic observations only: the flag
does not enter compiler configuration, resolved-program identity, manifests,
or emitted artifacts. Durations naturally vary with the host, while counters
and phase names describe the work the command actually performed. Failed
commands still print the completed portion of an enabled report, which makes
the option useful for locating failure-path costs.

## Inspect and compile

```sh
build/draftc lex path/to/file.draft
build/draftc syntax path/to/file.draft
build/draftc check path/to/package \
  [--target aarch64-macos|aarch64-linux] [--timings|--timings=all]
build/draftc emit-llvm path/to/package \
  [--target aarch64-macos|aarch64-linux] [--timings|--timings=all]
build/draftc emit-c-header path/to/package [-o output.h] \
  [--target aarch64-macos|aarch64-linux] [--timings|--timings=all]
build/draftc target [--target aarch64-macos|aarch64-linux]
```

`check` runs the provider-free front end and semantic pipeline. If the program
contains saved `...` expansions, it loads and revalidates them. It never starts
a provider. `emit-llvm` additionally lowers MIR and prints each package module.

## Build native artifacts

```sh
build/draftc build path/to/package [-o output] \
  [--target aarch64-macos|aarch64-linux] \
  [--kind executable|object|static-library|dynamic-library|assembly] \
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

The native adapter uses the host toolchain. On macOS it expects Clang, the Apple
linker and SDK, `libtool`, and `dsymutil`; on Linux it expects Clang, `ld.lld`,
`llvm-ar`, and the target's libc development files. These installations are
operational prerequisites, not resolution-manifest inputs.

Assembly output is a directory containing one `.s` file per package module and
one exact copied source per package assembly input. Object output performs a
relocatable link over the complete package graph. Static archives use
deterministic metadata, shared libraries receive a platform install name or
SONAME, and executables add hosted entry glue.

## Resolve synthesis sites

```sh
build/draftc resolve path/to/package [--revalidate] \
  [--regenerate [site-id]] \
  [--target aarch64-macos|aarch64-linux] [--assertions=off] \
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

`--regenerate` forces a provider proposal even for fresh pins. With no following
value it selects every current synthesis site; with one exact `site-...`
identity it selects only that site. The selector must match, unrelated fresh
pins remain exact, and a failed attempt leaves the committed manifest unchanged.
It cannot be combined with `--revalidate`.

The Codex adapter discovers `codex` through the user's `PATH` and uses that
installation's ordinary authentication and configuration. If `--model` is
omitted, the adapter also uses the Codex-configured default model. Executable
paths, distribution hashing, and credentials are not Draft program inputs.

`--revalidate` rechecks all saved sites without a provider and writes a fresh
source-resolution manifest. It cannot be combined with a provider model.
External provider, summary, and runtime-asset mappings become content-addressed
resolved-program inputs. Validation and judgment evidence remain separate and
are changed only by their own commands.

## Test and benchmark

```sh
build/draftc test path/to/package \
  [--target aarch64-macos|aarch64-linux] [--instrument address|...] \
  [--provider name=object|archive|shared-library:/absolute/path]... \
  [--provider-summary name:/absolute/path]... \
  [--runtime-asset name:/absolute/file-or-directory]... \
  [--timings|--timings=all]

build/draftc bench path/to/package [--verify] \
  [--target aarch64-macos|aarch64-linux] [--instrument address|...] \
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

The macOS target currently supports `--instrument address`. The compiler owns
the AddressSanitizer IR attribute and flags and lets host Clang link its matching
runtime. Other instrumentation kinds and the Linux Draft sanitizer profile fail
closed with an explicit diagnostic.

## Judge

```sh
build/draftc judge path/to/package [selector...] [--list] \
  [--target aarch64-macos|aarch64-linux] [--assertions=off] \
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
