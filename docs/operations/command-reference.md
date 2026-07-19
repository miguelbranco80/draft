# Compiler command reference

Status: bootstrap command and operational reference.

This document collects the concrete build, inspection, artifact, provider,
runtime-asset, synthesis, judgment, and locked-build command forms from the
completed first-compiler README. Architecture and semantic rules are documented
separately.

## Current command surface

The production audit implements the specified runtime-assertion build mode.
`--assertions=off` removes the condition and message before either operand is
lowered and is accepted consistently by `build`, `resolve`, and `judge`.
Test and benchmark validation always overrides this release choice to
assertions on.

The driver exposes `lex`, `syntax`, `target`, `check`, `emit-llvm`,
`emit-c-header`, ordinary and locked `build` for executable, object, static
library, dynamic library, and assembly-bundle kinds, `test`,
`bench --verify`, `resolve`, and the provider-backed `judge` command.
Locked builds may require matching evidence with
`--require-test-evidence`, `--require-benchmark-evidence`, and
`--require-judgment-evidence`; verification does not execute validation or
contact a provider.

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
Successful executable and dynamic-library CLI builds print both the primary
artifact and its `.dSYM` path; every native build also writes
`draft-source-correlation.json` in the isolated build directory.


`draftc build` defaults to an executable. `--kind object`, `--kind
static-library`, `--kind dynamic-library`, and `--kind assembly` select a
relocatable object, archive, dylib, or collision-free directory of assembly
sources. `draftc emit-c-header examples/c-library -o library.h` emits the C API
for explicit root-package exports. The `examples/c-library` fixture builds as a
no-`main` dylib and its checked-in C client exercises aggregate and callback ABI
compatibility.

Every native build also writes `draft-source-correlation.json` in its isolated
build directory. The canonical sidecar names each source-addressable emitted
MIR instruction or terminator by package, canonical MIR procedure ordinal, and
operation ordinal; it retains the procedure spelling for presentation. Each
row then records both its generated coordinate and its authored coordinate.
Synthesized operations also retain the stable synthesis-site identity. Coverage
and sampling-profile adapters can use the returned sidecar digest without
parsing LLVM metadata or exposing physical checkout paths. The sidecar is
derived output; it does not change the resolved program that it describes.

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
  --instrument address \
  --toolchain-root /absolute/path/to/llvm \
  --sdk-root /absolute/path/to/MacOSX.sdk \
  --runtime-asset unicode-tables:/absolute/assets/unicode

build/draftc build path/to/package --locked \
  --instrument address \
  --toolchain-root /absolute/path/to/llvm \
  --sdk-root /absolute/path/to/MacOSX.sdk \
  --runtime-asset unicode-tables:/relocated/assets/unicode \
  --require-test-evidence --require-benchmark-evidence \
  --require-judgment-evidence
```

`--require-judgment-evidence` makes a locked build require one exact active,
manifest-selected passing object for every current judgment. Verification
matches the compiled typed claim, resolved program, target, compiler, package,
one-validator/no-artifact policy, and immutable attempt digest. It never starts
Codex or any other provider. A missing row, stale context, changed compiler, or
later failing attempt rejects the build. As with test and benchmark evidence,
the flag is rejected outside `--locked` mode.

The first toolchain layout requires executable `bin/clang`, `bin/ld`,
`bin/ld-classic`, `bin/llvm-ar`, and `bin/dsymutil`. Its address profile also
requires `bin/llvm-symbolizer` and
`lib/clang/22/lib/darwin/libclang_rt.asan_osx_dynamic.dylib`. Every entry must be a thin
AArch64 Mach-O image. Before hashing, the compiler recursively verifies that
each non-system dylib dependency, ID, and runpath stays inside the selected
tree. Relocating an unchanged tree preserves its identity; changing any byte,
path, permission, or symlink spelling makes the build fail before a compiler
process starts. [The AArch64 macOS toolchain document](../releases/aarch64-macos-toolchain.md) records
the selected LLVM 22.1.8 and Apple linker distribution. Runtime-asset roots use
the same file-kind, permission, byte, and safe-symlink identity and are also
rechecked before a compiler process starts.
