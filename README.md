# Draft

Status: Draft 1 language design, revision 2. Syntax is provisional.

This split edition is the canonical Draft language specification in this
repository. The origin of the former monolithic-document reference is recorded
in [specification history](docs/history/specification-source.md).

<a id="section-1"></a>

## 1. Purpose

Draft is an Odin-inspired systems language in which agent synthesis is a typed,
scoped compiler operation that produces inspectable Draft source.

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
- Explicit `--assertions=off` release builds that remove both runtime-assertion
  operands during MIR lowering while retaining ordinary parsing and checking.
- `...` as a typed synthesis construct in complete surface programs.
- Optional agent judgments as pinned validation evidence, never runtime semantics.
- Provider-free builds from pinned, inspectable expansions.
- Semantic compiler context for agents rather than indiscriminate source dumps.

## Specification map

- [Core language (§§3–4)](docs/specification/01-core-language.md) — packages, declarations,
  expressions, constants, and control flow.
- [Types, memory, and runtime (§§5–7)](docs/specification/02-types-memory-runtime.md) — native
  types, layout, storage, context, concurrency, entry, and core libraries.
- [Design context and agent synthesis (§§8–10)](docs/specification/03-agent-synthesis.md) — durable
  documentation, judgments, `...`, resolution, and provider-free builds.
- [Native interop (§§11–12)](docs/specification/04-native-interop.md) — parsed assembly, C ABI,
  foreign imports, exports, and linking.
- [AArch64 parsed assembly profile](docs/targets/aarch64-macos-assembly.md) — the exact
  closed inline instruction and operand grammar for the first target.
- [Denials and validation (§§13–14)](docs/specification/05-denials-validation.md) — semantic
  restrictions, tests, instrumentation, and performance evidence.
- [Compiler architecture (§15)](docs/specification/06-compiler.md) — lowering, semantic context
  construction, evidence, and dependency-ordered elaboration.
- [Future ideas (§16)](docs/specification/07-future-ideas.md) — prospective layout, GPU, and
  raw-assembly extensions.


## Documentation

The complete documentation map is in [docs/README.md](docs/README.md). In
particular:

- [Examples and feature map](examples/README.md)
- [Bootstrap compiler architecture](docs/implementation/architecture.md)
- [Elaboration, semantic context, and pins](docs/implementation/elaboration-and-pins.md)
- [AArch64 macOS target profile](docs/targets/aarch64-macos.md)
- [AArch64 Linux target and qualification](docs/targets/aarch64-linux.md)
- [x86-64 Linux target profile](docs/targets/x86-64-linux.md)
- [x86-64 Windows target profile](docs/targets/x86-64-windows.md)
- [Compiler command reference](docs/operations/command-reference.md)
- [Native host qualification](docs/releases/native-host-qualification.md)
- [Historical first implementation plan](docs/history/first-implementation-plan.md)

Repository engineering rules remain in [AGENTS.md](AGENTS.md).

## Build the bootstrap compiler

The C++ bootstrap links LLVM 22 through its C API. On Apple Silicon, install
Homebrew `llvm@22`; on Ubuntu, install the matching `llvm-22-dev` distribution.
Draft programs do not acquire a package dependency from this bootstrap build
component: LLVM is part of the compiler implementation and distribution.

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLLVM_DIR=/opt/homebrew/opt/llvm@22/lib/cmake/llvm \
  -DDRAFT_ENABLE_SANITIZERS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Use `/usr/lib/llvm-22/lib/cmake/llvm` for the standard Ubuntu LLVM 22 layout.
`LLVM_DIR` may instead name an exact downloaded or locally built LLVM 22 CMake
package; no ambient `llvm-config` lookup decides the linked backend version.

The bootstrap is a direct C++20 implementation targeting AArch64 macOS,
AArch64 GNU/Linux, x86-64 GNU/Linux, and x86-64 Windows/MSVC. All four build
and execute on matching native hosts; the Linux x86-64 CI row additionally runs
the bootstrap under address and undefined-behavior sanitizers. Windows links
the official LLVM-C 22 development distribution and uses its matching Clang,
lld-link, and llvm-lib tools plus the Windows SDK.
It implements the complete ordinary-language pipeline independently of Codex;
provider-backed synthesis and judgment live behind compiler-owned typed,
content-addressed boundaries. Detailed capability and evidence claims belong to
the linked qualification report rather than this overview.

The same CMake build produces `build/draftide`, a Turbo-style terminal IDE whose
application, editor, project interaction, syntax-colored UI, and Build/Run
policy are written in Draft. During bootstrap it calls the C++ compiler library
through a narrow opaque C ABI:

```sh
build/draftide .
```

The repository's `draft.project` selects `examples/turbo-editor` initially;
`--root` remains an explicit override. F5 checks, builds, and runs the selected
root, while F6 opens its root selector and package/dependency view.

See [Turbo Draft](tools/draftide/README.md) for controls and
[the implementation boundary](docs/implementation/turbo-draft.md) for the
ownership design.
