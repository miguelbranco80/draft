# Draft

Draft is an experimental systems language built around one question:

> What if agent-generated code were a typed, scoped compiler operation—not a
> text generator bolted onto the side?

The rest of the language is deliberately unsurprising: native code, explicit
memory and layout, folder packages, C interoperation, parsed assembly, a small
core library, no garbage collector, and no package manager.

```draft
package app

import core/console

main :: proc() {
    answer: i64 = ... "Produce the i64 expression 42."
    assert(answer == 42)
    assert(console.println("answer:", answer) == .none)
}
```

`...` produces ordinary Draft source. The compiler checks it in the exact type
and scope of the site, saves the accepted expansion, and can build it later
without Codex. Handwritten programs never need an agent at all.

This is an experiment, not a production language. The syntax is provisional,
the compiler is young, and the interesting details are in the docs. Start with
the [language tour](examples/language-tour/package.draft), then read the
[specification](docs/README.md).

## What is unusual here?

- **Synthesis has a type.** An expression site cannot quietly become a file,
  dependency, command, or unrelated declaration.
- **Generated code stays source.** It is inspectable, diffable, committable,
  and checked by the same compiler as handwritten Draft.
- **Builds can be provider-free.** Accepted expansions are content-addressed
  workspace inputs, not an invisible model cache.
- **Judgment is separate from compilation.** Optional agent claims create
  pinned evidence; they do not change runtime semantics.
- **Low-level work stays low-level.** Draft has explicit allocators, native
  layout, C ABI imports/exports, SIMD, bit fields, assembly, denials, tests, and
  benchmarks.
- **The tooling is written in Draft.** DraftIDE is a small Turbo-style terminal
  IDE backed by the same compiler service.

## Try it

Tagged releases publish self-contained archives for Apple Silicon macOS,
AArch64 Linux, x86-64 Linux, and x86-64 Windows. Extract one, put its `bin`
directory on `PATH`, and check the exact build:

```sh
draftc --version
draftc run share/draft/examples/language-tour
draftide share/draft/examples/language-tour
```

The archive includes Draft’s matching LLVM/Clang tools, embedded `core`, docs,
examples, and the Draft coding skill. It still uses the host platform SDK:
install Xcode Command Line Tools on macOS, normal libc development files on
Linux, or run from a Visual Studio 2022 x64 developer shell on Windows.

To build the C++ bootstrap from source, install LLVM 22 and run:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=/opt/homebrew/opt/llvm@22/lib/cmake/llvm
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On Ubuntu, use `/usr/lib/llvm-22/lib/cmake/llvm`. The same build produces
`build/draftc` and `build/draftide`. See [Building and releasing
Draft](docs/operations/releases.md) for installation, packaging, checksums, and
platform details.

## A normal project

One directory is one package. A `draft.workspace` file is optional; add it only
when a repository needs durable build/run settings or several named programs.

```text
my-workspace/
    draft.workspace
    apps/editor/package.draft
    lib/text/package.draft
```

```sh
draftc check apps/editor
draftc build .
draftc run apps/editor -- document.txt
```

There is no package registry or dependency solver. Download source, keep it in
the workspace, and import it explicitly.

## Read next

- [Documentation map and specification](docs/README.md)
- [Examples by language feature](examples/README.md)
- [Compiler command reference](docs/operations/command-reference.md)
- [Compiler architecture](docs/implementation/architecture.md)
- [DraftIDE](tools/draftide/README.md)
- [Current qualification records](docs/releases/README.md)

Draft is MIT licensed. LLVM components in binary archives retain their upstream
Apache-2.0-with-LLVM-exceptions license; see
[third-party notices](THIRD_PARTY_NOTICES.md).
