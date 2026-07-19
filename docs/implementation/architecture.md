# Bootstrap compiler architecture

Status: current architectural overview distilled from the completed first
implementation plan. Language semantics remain owned by the specification.

This document describes the durable phase boundaries and representations. The
original sequencing and acceptance criteria remain intact in
[the historical first implementation plan](../history/first-implementation-plan.md).

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
compiler is written in a deliberately small C++20 subset and uses a selected
host LLVM toolchain. Its most valuable outputs are the executable language
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
   through the ordinary parser and semantic core. It owns transactional source
   pin handling; independent validation commands own evidence.
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

### Timing observation boundary

The process driver may attach one command-owned timing recorder to the existing
phase option structs. Compiler, validation, and native adapters contribute
nested events and deterministic work counters; none may consult recorded time
or make it part of semantic identity. The recorder is deliberately sequential
and uses an explicit nesting stack, matching the current dependency-ordered
pipeline. A future parallel pipeline must replace that assumption without
letting scheduling change compiler results.

The events reflect the implementation's real orchestration rather than the
conceptual diagram alone. An ordinary handwritten `check` constructs one graph:
interface discovery installs declarations and types, then semantic continuation
checks bodies, effects, denials, and completed interfaces on those same package
rows. A native `build` continues that graph directly through MIR/LLVM, then
invokes the host toolchain; it does not reload or recheck handwritten source.
`--timings` exposes resolution rounds as in-memory source transitions. A
checked complete-file overlay is parsed into the existing workspace graph;
package/root/import IDs remain stable, only the changed package and its
transitive consumers rebuild declaration semantics, and unrelated dependency
rows remain authoritative. The `compiler passes`, `workspace loads`, and
`workspace source transitions` counters make those distinct operations visible.
`--timings=all` adds package/tool scopes, file
discovery and I/O, lexing/parsing, import-graph resolution, and exclusive time;
child process CPU is reported separately from parent wall time.

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

Persistent on-disk shape:

```text
.draft/resolution.json
.draft/generated/<content-hash>.draft
.draft/evidence/<content-hash>.json
```

The bootstrap deliberately has no persistent compiler cache. Parsed syntax,
typed graphs, MIR, native objects, and other derived compiler state live only
for one command or in explicitly requested output artifacts. The generated
Draft objects above are committed program source, not cached compiler state.
`draftc expand --out <directory>` is an explicit read-only projection of those
objects after complete provider-free checking. It writes final source bytes and
generated-to-surface maps transactionally to a new directory; that requested
artifact is neither an input nor persistent compiler state.

`draft resolve` stages changes and atomically commits only a coherent,
compiler-checked program. It does not run tests, benchmarks, or judgments.
With `--build`, the resolver returns its exact final semantic-closure graph and
the driver continues that graph through MIR/LLVM and native emission after the
source commit. No second front-end orchestration or manifest reload occurs.
Interface and body expansions are complete source files, but installing one is
not a workspace reload: the compiler reparses the replacement transactionally,
rejects any package/import-topology change, and reanalyzes the affected graph
closure. Speculative provider proposals use isolated copies of the same
command-local graph operation, so checking a rejected proposal cannot mutate
the authoritative stage and does not perform filesystem discovery again.
`draft build` consumes saved expansions without contacting a provider
or modifying the resolution manifest. It rechecks every program input but
treats the host compiler, linker, and SDK as operational build configuration.

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
  builds already ignore judge execution by definition. The public `draft judge`
  command now uses the provider-neutral judgment boundary and the implemented
  Codex adapter when explicitly configured; without a provider it reports that
  judgment execution is unavailable while ordinary compilation remains valid.
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
