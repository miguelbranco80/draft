# Draft compiler: first implementation plan

Status: high-level proposal for the bootstrap compiler.

## Goal

Build Draft first as a complete ordinary systems language on AArch64 macOS,
then activate its agent operations without changing the parser, type system, or
native pipeline:

```text
complete handwritten packages -----------------------> native executable
surface packages with `...` -> checked expansions ---^
```

The compiler must build complete programs with no Codex configuration, provider
credentials, network access, synthesis pins, or judgment evidence. The first
compiler is written in a deliberately small C++20 subset and uses one exactly
pinned LLVM 22.1.x release. Its most valuable outputs are the executable language
behavior, conformance tests, target profile, manifest formats, and canonical
semantic representations that the later self-hosted compiler can reproduce.

## Architecture

The compiler has four main layers:

1. **Front end** -- loads folder packages, lexes and parses complete surface
   syntax, and preserves source locations, documentation, judgments, denials,
   and synthesis sites in the AST.
2. **Semantic core** -- resolves names, checks types and control flow, evaluates
   constants, computes layouts, instantiates parametric declarations, and builds
   canonical package interfaces and dependency summaries.
3. **Elaborator** -- schedules semantically ready `...` sites, constructs their
   bounded context, accepts provider proposals, and sends each expansion back
   through the ordinary parser and semantic core. It owns transactional pin and
   evidence handling; it does not bypass language checks.
4. **Native back end** -- lowers the complete typed program through a small
   Draft MIR to LLVM IR, emits a Mach-O object, links the runtime and program,
   and produces an AArch64 macOS executable.

The semantic and native pipeline is complete without the elaborator. The
elaborator closes typed holes in an otherwise ordinary program; it is not the
foundation on which ordinary parsing, checking, or code generation depends.

```text
 Source/package loader
          |
          v
 Lexer -> parser -> surface AST
                       |
                       v
             semantic dependency graph
                /              \
               v                v
       ordinary checking   synthesis obligation
                                |
                       context -> provider
                                |
                       generated Draft source
                                |
                         parser + checker
                \              /
                 v            v
                resolved semantic graph
                         |
                    typed HIR/CFG
                         |
                    Draft MIR
                         |
                  LLVM -> object -> link

 Pin store <-> elaborator       Target profile + runtime -> lowering/linking
```

### Internal representations

- **Surface AST:** lossless enough for diagnostics, structural site identity,
  generated-source maps, and exact grammar-category replacement.
- **Semantic graph / typed HIR:** declarations have stable IDs; types are
  interned; names are resolved; package interfaces and synthesis context are
  derived here.
- **Procedure CFG:** explicit branches and scopes, used for return analysis,
  `defer`, branch facts, judgments, and denial summaries.
- **Draft MIR:** a small non-optimizing IR with explicit loads, stores, checks,
  context arguments, calls, aggregate operations, and source locations. LLVM is
  an emission/optimization back end rather than Draft's semantic model.

LLVM types stay behind numeric, target, ABI, and code-generation adapters. The
front end must not depend on LLVM IR details.

### AArch64 macOS target boundary

One versioned target profile fixes the triple, LLVM data layout, pointer width,
Darwin C ABI rules, CPU features, object format, relocation/code/TLS models,
trap behavior, linker contract, and parsed-assembly dialect. Target-specific
ABI classification lives under this boundary rather than leaking into type
checking.

The first runtime supplies `runtime.Context`, failure entries, TLS/context
establishment, `main` entry glue, and the smallest allocator/OS support needed
by end-to-end tests.

### Synthesis and pin boundary

Codex is the first synthesis and judging provider. It is reached through a small
versioned adapter protocol so that provider execution details do not enter the
language's semantics. A request contains a canonical obligation and
content-addressed semantic context; a response contains only proposed Draft
syntax and provider metadata. The compiler owns retries, checking, program
identity, and commits.

Proposed on-disk shape:

```text
.draft/resolution.json
.draft/generated/<content-hash>.draft
.draft/evidence/<content-hash>.json
.draft/cache/...
```

`draft resolve` stages changes and atomically commits only a coherent validated
program. `draft build` consumes pins without contacting a provider, and
`draft build --locked` rejects every unpinned or stale input.

### Agent constructs before provider execution

All three agent-facing surface constructs exist in the initial front end and
semantic model:

- **`docs` is real metadata, not a discarded no-op.** The compiler parses its
  attachment group, validates and hashes attached files and folders, associates
  it with the package or declaration, and includes public documentation in the
  canonical package interface. It simply has no runtime lowering.
- **`judge` is a typed, anchored, zero-width construct.** The compiler checks
  placement and reachability, assigns its stable identity, records its claim and
  attachments, and makes the appropriate semantic/CFG facts available. Ordinary
  builds already ignore judge execution by definition. The initial `draft judge`
  command may report that no judging provider is configured until the Codex
  adapter lands.
- **`...` is a typed unresolved obligation, not a fake value or empty body.**
  The parser records its grammar category and attachments; semantic analysis
  derives the expected type, visible bindings, active denials, target facts, and
  dependency edges. Checking can proceed wherever the hole does not withhold
  required declarations, layout, or control-flow facts. Native emission rejects
  an unresolved required site precisely. A complete program with no `...` sites
  compiles normally.

This means the provider-independent compiler already produces exactly the
information Codex later needs. Adding Codex supplies proposals and verdicts; it
does not retrofit agent awareness into the language core.

## Implementation sequence

### 0. Foundations

- Pin Clang/LLVM and define the C++ subset, formatting, sanitizers, and build.
- Freeze the first target profile and implementation-owned file formats.
- Establish diagnostic snapshots and lexer/parser/sema/compile test harnesses.

### 1. Packages and syntax

- Source manager, UTF-8 lexer, semicolon insertion, parser, AST, diagnostics.
- Folder packages, file-local imports, target-qualified file selection.
- Parse every Draft 1 construct, even when a later phase initially reports it
  as not implemented.

### 2. Semantic core

- Symbols, scopes, visibility, type interning, expected-type propagation.
- Constants, layouts, parametric declarations, `when`, control-flow checking.
- Canonical package interfaces, semantic dependency graph, denials and summaries.
- Semantic nodes, identities, attachments, obligations, anchors, and CFG facts
  for `docs`, `judge`, and `...`, without invoking a provider.

### 3. Complete agent-free language

- Typed HIR/CFG and Draft MIR.
- AArch64 macOS layout and calling convention, hidden context argument.
- LLVM IR/object emission, runtime ABI, entry shim, initial core packages, and
  system linking sufficient to exercise every language feature.
- Lower every Draft 1 construct that can occur in a complete handwritten
  program, including native interop and parsed assembly.
- Compile, link, and run representative handwritten multi-package programs.
- Verify that `docs` and `judge` have no runtime footprint and that unresolved
  synthesis sites fail with complete typed obligations.

### 4. End-to-end synthesis

- Synthesis-site identity and grammar-specific expansion parsing.
- Readiness scheduling, canonical context construction, and the Codex adapter.
- Content-addressed generated source, transactional manifests, stale detection.
- Demonstrate `resolve`, ordinary offline build, and byte-for-byte repeatable
  locked build of the same resolved program.

### 5. Harden the first solid release

- Tests, complete denial composition, source maps, judgment execution, and
  validation evidence.
- Crash-safe transactions, malformed-input tests, deterministic serialization,
  sanitizer-clean test suite, and compiler self-consistency checks.
- Explicit diagnostics for unsupported targets, profiles, provider operations,
  and unavailable validation instrumentation.

After that release, extend the core libraries, validation and benchmark tooling,
output kinds, and target profiles without changing the agent-free foundation.

## First release acceptance test

A small multi-package program contains expression, body, and declaration
synthesis sites. `draft resolve` produces checked inspectable expansions and a
manifest, builds and runs the native executable, and commits only after tests
pass. With provider access disabled, `draft build --locked` rebuilds the same
program successfully. Changing any relevant source, interface, attachment,
target, compiler, provider configuration, or generated expansion makes the
correct pin stale and produces a precise diagnostic.
