# Draft

Status: Draft 1 language design, revision 2. Syntax is provisional.

This split edition is the Draft language specification. The preserved
[monolithic revision-2 document](../agent-native-systems-language-v2.md) remains
intact.

<a id="section-1"></a>

## 1. Purpose

Draft is an Odin-inspired systems language in which agent synthesis is a typed,
scoped, reproducible compiler operation.

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
- Rich compile-time types with concrete native lowering.
- Native parsed assembly with typed operands.
- Direct C ABI import and export.
- Compiler denials over ordinary resolved names and built-in constructs.
- Runtime and compile-time assertions with explicit failure semantics.
- `...` as a typed synthesis construct in complete surface programs.
- Optional agent judgments as pinned validation evidence, never runtime semantics.
- Deterministic locked builds from pinned, inspectable expansions.
- Semantic compiler context for agents rather than indiscriminate source dumps.

## Specification map

- [Core language (§§3–4)](01-core-language.md) — packages, declarations,
  expressions, constants, and control flow.
- [Types, memory, and runtime (§§5–7)](02-types-memory-runtime.md) — native
  types, layout, storage, context, concurrency, entry, and core libraries.
- [Design context and agent synthesis (§§8–10)](03-agent-synthesis.md) — durable
  documentation, judgments, `...`, resolution, and deterministic builds.
- [Native interop (§§11–12)](04-native-interop.md) — parsed assembly, C ABI,
  foreign imports, exports, and linking.
- [Denials and validation (§§13–14)](05-denials-validation.md) — semantic
  restrictions, tests, instrumentation, and performance evidence.
- [Compiler architecture (§15)](06-compiler.md) — lowering, semantic context
  construction, evidence, and dependency-ordered elaboration.
- [Future ideas (§16)](07-future-ideas.md) — prospective layout, GPU, and
  raw-assembly extensions.

## Bootstrap compiler

The bootstrap compiler is being implemented in a deliberately direct C++20
subset under the rules in [AGENTS.md](AGENTS.md) and the sequencing in
[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md). The checked-in implementation
currently includes source ownership, structured diagnostics, the full lexical
token vocabulary, UTF-8 and literal validation, and Draft semicolon insertion.

Configure, build, and test the current compiler with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DDRAFT_ENABLE_SANITIZERS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`build/draftc lex path/to/file.draft` prints the token stream, including which
semicolons were inserted by the compiler. Later compiler commands will use the
same source, diagnostic, and syntax layers.
