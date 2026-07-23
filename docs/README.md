# Draft documentation

The root [README](../README.md) is the short language and repository entry
point. This directory separates normative language meaning from target
contracts, implementation mechanics, operational commands, release evidence,
open decisions, and historical records.

The small [project checklist](../TODO.md) records intended work without making
those items part of the language specification or current implementation state.

## Specification

- [Core language](specification/01-core-language.md)
- [Types, memory, and runtime](specification/02-types-memory-runtime.md)
- [Design context and agent synthesis](specification/03-agent-synthesis.md)
- [Native interop](specification/04-native-interop.md)
- [Denials and validation](specification/05-denials-validation.md)
- [Compiler architecture semantics](specification/06-compiler.md)
- [Future ideas](specification/07-future-ideas.md)

The specification is authoritative for Draft language behavior. Future ideas
are explicitly non-normative.

## Targets

- [AArch64 macOS target profile](targets/aarch64-macos.md)
- [AArch64 Linux GNU target profile](targets/aarch64-linux.md)
- [x86-64 Linux GNU target profile](targets/x86-64-linux.md)
- [x86-64 Windows MSVC target profile](targets/x86-64-windows.md)
- [AArch64 parsed assembly profile](targets/aarch64-macos-assembly.md)

Target documents define versioned machine, ABI, and assembly facts. They do not
generalize those facts to every future Draft target.

## Bootstrap implementation

- [Architecture](implementation/architecture.md)
- [Semantic work graph](implementation/semantic-work-graph.md)
- [Semantic work graph completion record](implementation/semantic-work-graph-implementation-plan.md)
- [Front end and semantic core](implementation/semantic-core.md)
- [Elaboration, semantic context, and pins](implementation/elaboration-and-pins.md)
- [Native backend and artifacts](implementation/native-backend-and-artifacts.md)
- [Hosted runtime and core packages](implementation/runtime-and-core.md)
- [Turbo Draft and the embedded compiler service](implementation/turbo-draft.md)
- [Validation, judgments, and evidence](implementation/validation-and-evidence.md)
- [Implementation limits](implementation/implementation-limits.md)

These documents explain the first compiler. They may describe narrower
algorithms or limits than the language permits, but cannot redefine the
specification.

## Operations and releases

- [Compiler command reference](operations/command-reference.md)
- [Continuous integration](operations/continuous-integration.md)
- [Current release notes](releases/README.md)
- [Native host qualification](releases/native-host-qualification.md)
- [Agent acceptance fixture](releases/agent-acceptance.md)

Qualification documents state what exact tests, binaries, models, and inputs
have been exercised. They are evidence, not additional language semantics.

## Decisions and history

- [Open language questions](decisions/language-questions.md)
- [Compiler graph and resolution simplification plan](history/compiler-graph-and-resolution-plan.md)
- [First implementation plan](history/first-implementation-plan.md)
- [Bootstrap implementation decision archive](history/bootstrap-implementation-decisions.md)
- [Parsed assembly staging](history/assembly-staging.md)
- [Bootstrap README implementation snapshot](history/bootstrap-readme-snapshot.md)
- [Specification source history](history/specification-source.md)
- [Documentation migration ledger](history/documentation-migration-ledger.md)
- [Former native-input design](history/native-input-experiment.md)
- [Historical release qualifications](history/releases/README.md)

The archives intentionally retain the original wording and version identities.
Current subsystem documents are organized extractions; the archives make the
reorganization auditable without presenting chronological notes as current
architecture.
