# Documentation migration ledger

This ledger records the 2026 documentation reorganization. It exists so future
editors can verify that moving information into a logical subsystem did not
silently delete original language, implementation, command, or qualification
detail.

## Whole-file moves

| Original path | Canonical destination |
|---|---|
| `01-core-language.md` | `docs/specification/01-core-language.md` |
| `02-types-memory-runtime.md` | `docs/specification/02-types-memory-runtime.md` |
| `03-agent-synthesis.md` | `docs/specification/03-agent-synthesis.md` |
| `04-native-interop.md` | `docs/specification/04-native-interop.md` |
| `05-denials-validation.md` | `docs/specification/05-denials-validation.md` |
| `06-compiler.md` | `docs/specification/06-compiler.md` |
| `07-future-ideas.md` | `docs/specification/07-future-ideas.md` |
| `AARCH64_ASSEMBLY_PROFILE.md` | `docs/targets/aarch64-macos-assembly.md` |
| `IMPLEMENTATION_STATUS.md` | `docs/releases/first-compiler-qualification.md` |
| `TOOLCHAIN_DISTRIBUTION.md` | `docs/releases/aarch64-macos-toolchain.md` |
| `examples/agent-acceptance/README.md` | `docs/releases/agent-acceptance.md` |

The moved files retain their original content. Navigation and cross-document
links were repaired, and specification sections 01, 02, 03, and 06 additionally
received the observable-rule promotions recorded below. The qualification
introduction now points at the canonical architecture and history documents.

## README

The purpose, design principles, and specification map remain in the root
`README.md`. The complete former `Bootstrap compiler` section is preserved
in `docs/history/bootstrap-readme-snapshot.md`. Its command-oriented material
is additionally organized in `docs/operations/command-reference.md`, while
current evidence belongs to the qualification report and architecture belongs
to the implementation documents.

The former reference to a missing monolithic revision-2 file is retained as an
archival fact in `docs/history/specification-source.md`, not as a broken link.

## Implementation plan

The complete original plan is preserved verbatim in
`docs/history/first-implementation-plan.md`. Its durable Goal, Architecture,
internal representations, target boundary, synthesis boundary, and
provider-independent agent-construct sections are extracted into
`docs/implementation/architecture.md`. The stages and first-release acceptance
test remain historical sequencing.

## Implementation decisions

The complete original 1,147-line decision file is preserved in
`docs/history/bootstrap-implementation-decisions.md`. Organized documents
copy its substantive sections as follows:

| Original decision | Organized destination |
|---|---|
| End-of-line `^` | `docs/decisions/language-questions.md` |
| Contextual `c` | `docs/implementation/semantic-core.md` |
| Initial AArch64 macOS profile | `docs/targets/aarch64-macos.md` |
| Recursive implementation resource limits | `docs/implementation/implementation-limits.md` |
| Parsed assembly staging | `docs/history/assembly-staging.md` |
| Relocatable aggregate constants | `docs/implementation/native-backend-and-artifacts.md` |
| Initial locked native input contract, toolchain through foreign inputs | `docs/implementation/locked-builds.md` |
| Procedure-flow and external-audit summaries embedded in that contract | `docs/implementation/semantic-core.md` |
| Synthesis request/context and scheduling embedded in that contract | `docs/implementation/elaboration-and-pins.md` |
| Validation-only context embedded in that contract | `docs/implementation/validation-and-evidence.md` |
| Native artifact ownership and visibility | `docs/implementation/native-backend-and-artifacts.md` |
| Initial hosted runtime Context layout | `docs/implementation/runtime-and-core.md` |
| Initial core memory facilities | `docs/implementation/runtime-and-core.md` |
| Nominal generic inference and transitive interfaces | `docs/implementation/semantic-core.md` |
| Unique dependent-value inference | `docs/implementation/semantic-core.md` |
| Owner-evaluated generic layout constants | `docs/implementation/semantic-core.md` |
| Procedure-dependent generic procedure arguments | `docs/implementation/semantic-core.md` |
| Shift-count validation domain | `docs/implementation/semantic-core.md` |
| Conditional context discovery | `docs/implementation/semantic-core.md` |
| Hosted process views and core threads | `docs/implementation/runtime-and-core.md` |
| Initial compiler-backed atomic interface | `docs/implementation/runtime-and-core.md` |
| Shared evidence attempt storage | `docs/implementation/validation-and-evidence.md` |
| Shared Codex runtime for synthesis and judgment | `docs/implementation/elaboration-and-pins.md` |
| Conditional judgment-manifest publication | `docs/implementation/validation-and-evidence.md` |
| Offline locked judgment evidence gate | `docs/implementation/validation-and-evidence.md` |
| Judgment discovery and partial selection | `docs/implementation/validation-and-evidence.md` |
| Resolution-profile judgment scheduling | `docs/implementation/validation-and-evidence.md` |
| Typed branch and loop-range facts | `docs/implementation/elaboration-and-pins.md` |
| Native source correlation sidecar | `docs/implementation/native-backend-and-artifacts.md` |
| Native Mach-O debug companions | `docs/implementation/native-backend-and-artifacts.md` |
| Canonical CLI workspace roots | `docs/implementation/locked-builds.md` |
| Validation instrumentation request boundary | `docs/implementation/validation-and-evidence.md` |
| Runtime assertion build mode | `docs/implementation/semantic-core.md` |
| Authenticated synthesis overlays in validation graphs | `docs/implementation/elaboration-and-pins.md` |
| Selected self-contained AArch64 distribution | `docs/releases/selected-aarch64-distribution.md` |

Observable rules were also promoted to their normative or target destinations:
`c`, provisional newline `^`, and conditional context discovery to
`docs/specification/01-core-language.md`; nominal/dependent inference,
Context address copying, and atomic ordering to
`docs/specification/02-types-memory-runtime.md`; provider-free versus selected
provider reuse to `docs/specification/03-agent-synthesis.md`; shared
branch/loop facts to `docs/specification/06-compiler.md`; and versioned
Context, pthread-storage, atomic-width, and C-enum ABI facts to
`docs/targets/aarch64-macos.md`.

The oversized original “Initial locked native input contract” is deliberately
split at paragraph boundaries. Lines 139–215 go to locked builds, 217–242 to
semantic effect/audit summaries, 244–283 and 300–420 to elaboration, and
285–298 to validation context. The chronological archive remains the complete
textual fallback for auditing that split; two historical Markdown links were
retargeted after the move.

## Operational repository files

`AGENTS.md` remains at the repository root because agents discover repository
rules there; it is operational configuration rather than reader documentation.
The agent-acceptance explanation and reproduction commands moved to
`docs/releases/agent-acceptance.md`; its paths still name the checked-in
`examples/agent-acceptance` fixture.

## No-loss rule

Before deleting either historical snapshot, every paragraph, code fence,
command, version identity, digest, limit, ABI fact, test count, and stated
limitation must have a named canonical destination. De-duplication is allowed
only after that destination is recorded here and all links and anchors pass.
