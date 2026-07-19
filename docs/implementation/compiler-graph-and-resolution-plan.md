# Compiler graph and resolution simplification plan

Status: implemented on 2026-07-19. This is a non-normative architecture and
completion record; the language specification and command reference remain
authoritative for behavior.

## Purpose

Make ordinary compilation and `...` synthesis use one understandable compiler
architecture, while preserving an explicit boundary around agent calls and
generated-source changes. The result should compile quickly, keep generated
Draft inspectable, and require no provider during an ordinary build.

The durable resolved program remains:

```text
authored Draft source
+ .draft/resolution.json
+ .draft/generated/<source-hash>.draft
```

The manifest and referenced generated source are project inputs and are
normally committed to version control. Native objects, transaction staging,
and other derived state are not. Draft will not maintain a persistent compiler
cache.

## Decisions retained

- Generated expansions are ordinary Draft source. They re-enter the parser and
  semantic checker and can never manufacture checked HIR, MIR, or machine code.
- `draftc build` is provider-free and deterministic. It does not contact Codex,
  update generated source, run judgments, or require validation evidence.
- The former `--locked` native build mode was removed in commit `42e6097`.
  Provider-free checked builds are the normal behavior; do not reintroduce a
  second mode or its package-manager terminology.
- Do not add a persistent AST, HIR, MIR, object, or incremental compiler cache.
  Reusing data owned by one command's semantic graph is ordinary in-process
  compiler state, not cross-invocation caching. `.draft/generated` is committed
  source, not a cache.
- `docs` remains durable semantic context for synthesis with no runtime effect.
- `judge` remains an explicit review operation rather than part of ordinary
  compilation.
- Content-addressed generated fragments remain canonical. Complete expanded
  files are a projection for people and tools, not a second source of truth.

## Implemented architecture

### One compiler graph

- Replace repeated interface, body-surface, and final compilations with one
  semantic dependency graph per distinct source selection, owned for the
  duration of a command. Ordinary source, a test selection, and a benchmark
  selection are different graphs because they contain different files and may
  have different imports; no one of those graphs is rebuilt merely to advance
  a compiler phase.
- Represent declaration availability, type and layout completion, synthesis
  obligations, body checking, MIR readiness, and requested validation as
  explicit graph state.
- Schedule a deterministic sorted ready set. The implemented scheduler is
  sequential. Parallel execution remains a possible measured optimization, not
  part of the architecture contract or a source of different results.
- Reuse loaded files, tokens, syntax, declarations, interned types, checked
  expansions, and dependency facts instead of reloading the workspace for each
  conceptual phase.
- Keep handwritten and generated declarations in the same semantic graph once
  generated source has passed its grammar boundary and ordinary checks. A
  complete-file expansion is reparsed transactionally, may not change package
  or import topology, and rebuilds only its package plus transitive consumers.
  Unrelated dependency declarations and types remain live.

### Resolution and build workflow

- Keep `draftc resolve` as the only operation authorized to request or change
  generated source. This is the visible side-effect and review boundary.
- Add `draftc resolve --build` so a successful resolution can continue through
  MIR, native emission, and linking in the same process without rebuilding the
  graph. Plain `resolve` still stops after committing checked source.
- Add explicit `--regenerate`, with an optional site selector, to ask the
  provider to reconsider source that is otherwise fresh.
- Make resolution prove that the completed program parses, types, satisfies
  denials, and is ready for the requested target. Do not automatically run
  tests, benchmarks, or judgments as a condition of saving generated source.
- Keep `test`, `bench`, and `judge` as separate explicit commands. Their
  evidence is associated with the exact resolved program but is not required
  to compile it.

### Pin freshness and provenance

- Define freshness only by facts that determine whether an expansion still
  fits its synthesis obligation: grammar category, expected type, visible
  declarations and bindings, prompt, `docs` and attachments, target and ABI,
  active denials, and relevant compiler semantics.
- Record provider, model, reasoning effort, retry policy, Codex adapter version,
  and other generation settings as provenance only.
- Exclude that provenance from pin freshness and resolved-program identity.
  Changing the default Codex model must not invalidate accepted source.
- Preserve the exact expansion bytes and their content hash as semantic program
  inputs. A deliberate regeneration changes the program only if it accepts
  different Draft source.
- Separate generation policy from genuine compiler configuration. A setting
  that changes Draft typing or target meaning remains a semantic input even
  when a provider happens to receive it too.

### Persistent state and inspection

- Document that `.draft/resolution.json` and every referenced
  `.draft/generated/*.draft` object are normally committed.
- Keep `.draft` transaction staging and native intermediates temporary or
  ignored. Do not create a `.draft/cache` hierarchy or another persistent
  compiler cache under a different name.
- Keep validation and judgment evidence separate from the generated-source
  selection. Evidence may be retained for auditing or release qualification,
  but ordinary builds neither require nor update it.
- Add `draftc expand <package> --out <directory>` to materialize complete source
  files with every `...` replaced, while preserving generated-to-surface source
  maps.

### CLI and input simplification

- Remove low-level Codex distribution-root and executable-path flags from the
  normal language workflow. Provider discovery and credentials belong to the
  adapter or user configuration, not Draft program identity.
- Keep any explicit model selection as generation policy and provenance, not as
  a condition for reusing checked source.
- Treat vendored Draft packages as ordinary source in the workspace. Draft does
  not need a package lockfile or dependency-download mechanism.
- Retain exact content identities only where the selected bytes genuinely are
  program inputs, notably generated source, native objects and libraries,
  foreign-provider summaries, and runtime assets.

## Completion record

1. Synthesis semantics, provider-free build behavior, provenance, and evidence
   separation were updated first (`e73c659`, with the preceding specification
   slice in the same series).
2. Resolution manifest v5 excludes generation provenance from freshness and
   resolved-program identity; focused fixtures prove model changes do not stale
   accepted source.
3. Provider discovery was reduced to ordinary `PATH`/Codex configuration
   (`32fb133`), and explicit regeneration was added (`623ca42`).
4. Version-control guidance and transactional complete-source `expand` output
   landed in `afac184`; validation tests no longer write state into source
   fixtures (`1552891`).
5. Checked graphs gained explicit semantic and target-lowering continuations
   (`0cab695`, `b627d50`), and `resolve --build` continues the returned graph
   into native emission (`c8f8e07`).
6. Checked source replacement became a transactional in-memory workspace
   operation (`52e85c6`). Declaration analysis now rebuilds only affected
   packages and transitive consumers (`5daa62e`); provider-free resolution and
   speculative/authoritative resolver stages use that operation without
   another workspace load (`1ba75f3`).
7. Timing remeasurement showed native Clang/tool invocation dominates a small
   handwritten build. The larger agent acceptance graph now performs one
   ordinary graph construction plus one separately selected typed test-context
   graph, rather than five repeated compiler/workspace passes. Typed validation
   state survives the body-source transition. Parallel front-end scheduling was
   therefore not added: its coordination and timing-recorder complexity are not
   justified by the measured remaining front-end work. The sorted sequential
   ready set is the qualified implementation.

The qualifying timing counters were `compiler passes: 1`, `workspace loads: 1`
for the handwritten hello build. The resolved agent-acceptance build, whose
synthesis context includes test source and `core/testing`, reported two passes,
two loads, and two in-memory source transitions: one ordinary program graph,
one distinct validation-selection graph, and no repeated construction for its
interface or body overlays.

The changes were kept in coherent commits. Public slices updated their owning
specification, implementation document, command reference, Draft coding skill,
and tests as applicable.

## Acceptance checks

- Switching the configured Codex model leaves an accepted expansion fresh and
  leaves resolved-program identity unchanged.
- `--regenerate` can replace one selected site without disturbing unrelated
  fresh sites.
- Provider access disabled at the process boundary cannot prevent `check`,
  `build`, `test`, or `bench` from consuming a complete resolved program.
- Corrupt, missing, stale, or ambiguously associated generated source fails
  with an exact diagnostic and never triggers a provider during `build`.
- Failed or interrupted resolution leaves the previously committed manifest
  authoritative.
- `resolve --build` performs one front-end construction for each distinct
  selected source graph and produces the same native program as a later
  provider-free `build` of the committed source. Interface/body rounds never
  reconstruct the same workspace graph.
- The sorted sequential scheduler produces stable manifests,
  generated-source selections, diagnostics, and native outputs. Any future
  parallel scheduler must prove byte identity against this oracle before use.
- A clean checkout containing authored source plus the committed durable
  `.draft` files builds without provider credentials or network access.
- Repeated commands leave no persistent compiler cache; all cross-invocation
  semantic state is reconstructible from authored and generated source.
