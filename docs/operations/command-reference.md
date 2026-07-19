# Compiler command reference

Status: current bootstrap CLI. The executable is named `draftc`; examples below
use the default `build/draftc` CMake output.

All package commands accept `--target aarch64-macos|aarch64-linux` where shown.
macOS remains the compatibility default. Package paths are canonicalized before
the workspace and resolution-store boundaries are established.

## Inspect and compile

```sh
build/draftc lex path/to/file.draft
build/draftc syntax path/to/file.draft
build/draftc check path/to/package [--target aarch64-macos|aarch64-linux]
build/draftc emit-llvm path/to/package [--target aarch64-macos|aarch64-linux]
build/draftc emit-c-header path/to/package [-o output.h] \
  [--target aarch64-macos|aarch64-linux]
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
  [--runtime-asset name:/absolute/file-or-directory]...
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
build/draftc resolve path/to/package [--revalidate] [--judge] \
  [--target aarch64-macos|aarch64-linux] [--assertions=off] \
  [--instrument address|lifetime|undefined-operation|allocator-poisoning|race]... \
  [--judge-select selector]... \
  [--judge-validator identity:model]... \
  [--judge-artifact kind:/absolute/path]... \
  [--codex-distribution-root /absolute/codex-root \
   --codex-executable /absolute/codex --codex-model model] \
  [--provider name=object|archive|shared-library:/absolute/path]... \
  [--provider-summary name:/absolute/path]... \
  [--runtime-asset name:/absolute/file-or-directory]...
```

`resolve` constructs typed obligations and calls the configured provider only
for sites that cannot reuse a valid saved expansion. The provider returns Draft
source, which is parsed and checked through the ordinary pipeline. The compiler
commits generated objects and the manifest only after the complete candidate
program and its selected precommit validation pass.

`--revalidate` rechecks all saved sites without a provider and writes a fresh
manifest/evidence selection. It cannot be combined with provider models or a
judgment profile. External provider, summary, and runtime-asset mappings become
content-addressed resolved-program inputs.

## Test and benchmark

```sh
build/draftc test path/to/package \
  [--target aarch64-macos|aarch64-linux] [--instrument address|...] \
  [--provider name=object|archive|shared-library:/absolute/path]... \
  [--provider-summary name:/absolute/path]... \
  [--runtime-asset name:/absolute/file-or-directory]...

build/draftc bench path/to/package [--verify] \
  [--target aarch64-macos|aarch64-linux] [--instrument address|...] \
  [--provider name=object|archive|shared-library:/absolute/path]... \
  [--provider-summary name:/absolute/path]... \
  [--runtime-asset name:/absolute/file-or-directory]...
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
  [--codex-distribution-root /absolute/codex-root \
   --codex-executable /absolute/codex --codex-model model] \
  [--provider name=object|archive|shared-library:/absolute/path]... \
  [--provider-summary name:/absolute/path]...
```

`--list` prints stable judgment site identities without configuring a provider.
A selector may be an exact `site-...` identity, a package identity/path, or
`package:anchor`; several selectors form a de-duplicated union. Each configured
validator receives the same typed claim and exact requested artifact bytes. A
completed all-pass run atomically updates the selected judgment rows; failures
remain in append-only evidence history but do not publish a passing selection.

Judgment evidence is inspection and qualification data. Ordinary `build` does
not run a provider and does not require a judgment result.
