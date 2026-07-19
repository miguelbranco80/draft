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

- Each distinct source selection has one semantic dependency graph owned for the
  duration of a command. Ordinary source, a test selection, and a benchmark
  selection are different graphs because they contain different files and may
  have different imports; no one of those graphs is rebuilt merely to advance
  a compiler phase.
- Explicit progress states represent interface discovery, semantic closure,
  validation discovery, and target lowering; package rows own the declarations,
  types, obligations, checked bodies, MIR, and LLVM products available at each
  state.
- A command-local adjacency index records imports by consumer and consumers by
  dependency. A sequential Kahn traversal uses a PackageId-ordered min-heap, so
  dependency scheduling is deterministic and O((packages + imports) log
  packages). Source invalidation walks the reverse adjacency rows in O(packages
  + imports). Parallel execution remains a possible measured optimization, not
  part of the architecture contract or a source of different results.
- The graph reuses loaded files, tokens, syntax, declarations, interned types,
  checked expansions, and dependency facts instead of reloading the workspace
  for each conceptual phase.
- Handwritten and generated declarations share the same semantic graph once
  generated source has passed its grammar boundary and ordinary checks. A
  complete-file expansion is reparsed transactionally, may not change package
  or import topology, and rebuilds only its package plus transitive consumers.
  Unrelated dependency declarations and types remain live.

### Resolution and build workflow

- `draftc resolve` is the only operation authorized to request or change
  generated source. This is the visible side-effect and review boundary.
- `draftc resolve --build` lets a successful resolution continue through
  MIR, native emission, and linking in the same process without rebuilding the
  graph. Plain `resolve` still stops after committing checked source.
- Explicit `--regenerate`, with an optional site selector, asks the provider to
  reconsider source that is otherwise fresh.
- Resolution proves that the completed program parses, types, satisfies
  denials, and is ready for the requested target. It does not automatically run
  tests, benchmarks, or judgments as a condition of saving generated source.
- `test`, `bench`, and `judge` remain separate explicit commands. Their
  evidence is associated with the exact resolved program but is not required
  to compile it.

### Pin freshness and provenance

- Freshness includes only facts that determine whether an expansion still
  fits its synthesis obligation: grammar category, expected type, visible
  declarations and bindings, prompt, `docs` and attachments, target and ABI,
  active denials, and relevant compiler semantics.
- Pins record provider and model in named provenance fields. Their configuration
  identity covers other configured generation policy. For the Codex adapter it
  hashes the explicit/default model choice, timeout, adapter retry budget,
  prompt and output-schema contracts, and fixed adapter version/process policy.
  Draft exposes no reasoning-effort option today; if one is added, it belongs
  in this provenance identity rather than freshness.
- That provenance is excluded from pin freshness and resolved-program identity.
  Changing the default Codex model must not invalidate accepted source.
- The exact expansion bytes and their content hash remain semantic program
  inputs. A deliberate regeneration changes the program only if it accepts
  different Draft source.
- Generation policy remains separate from genuine compiler configuration. A
  setting that changes Draft typing or target meaning remains a semantic input
  even when a provider happens to receive it too.

### Persistent state and inspection

- `.draft/resolution.json` and every referenced
  `.draft/generated/*.draft` object are normally committed.
- `.draft` transaction staging and native intermediates remain temporary or
  ignored. Do not create a `.draft/cache` hierarchy or another persistent
  compiler cache under a different name.
- Validation and judgment evidence remain separate from the generated-source
  selection. Evidence may be retained for auditing or release qualification,
  but ordinary builds neither require nor update it.
- `draftc expand <package> --out <directory>` materializes complete source
  files with every `...` replaced, while preserving generated-to-surface source
  maps.

### CLI and input simplification

- Low-level Codex distribution-root and executable-path flags are absent from the
  normal language workflow. Provider discovery and credentials belong to the
  adapter or user configuration, not Draft program identity.
- Explicit model selection is generation policy and provenance, not a condition
  for reusing checked source.
- Vendored Draft packages are ordinary source in the workspace. Draft does
  not need a package lockfile or dependency-download mechanism.
- Exact content identities remain only where the selected bytes genuinely are
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
   ordinary graph construction plus separately selected typed test- and
   benchmark-context graphs, rather than repeated compiler/workspace passes for
   interface and body stages. Typed validation state survives the body-source
   transition. Parallel front-end scheduling was therefore not added: its
   coordination and timing-recorder complexity are not justified by the
   measured remaining front-end work. The sorted sequential ready set is the
   qualified implementation.
8. A final complexity audit replaced whole-edge rescans and shifting sorted
   vectors with one explicit adjacency index and PackageId-ordered min-heap
   (`83cd919`).
   The provider-free process acceptance now shadows Codex with a failing
   sentinel and runs `check`, native `build`, `test`, and `bench` from the same
   committed generated source. It also rejects any `.draft/cache` creation
   (`39f9a4d`).

The qualifying timing counters were `compiler passes: 1`, `workspace loads: 1`
for the handwritten hello build. The resolved agent-acceptance build, whose
synthesis context includes both validation roles, reported three passes, three
loads, and two in-memory source transitions: one ordinary program graph, one
test-selection graph, one benchmark-selection graph, and no repeated
construction for their interface or body overlays.

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
  `.draft` files builds for the manifest's selected target without provider
  credentials or network access. Existing generated source may be revalidated
  provider-free to select another supported target.
- Repeated commands leave no persistent compiler cache; all cross-invocation
  semantic state is reconstructible from authored and generated source.

## Automated evidence

- `draft_resolver_tests` covers provenance-independent freshness and identity,
  selective regeneration, bounded rejected proposals, provider-free reuse,
  cancellation, validation-context graphs, and failure without commit.
- `draft_resolution_store_tests` and `draft_resolution_overlay_tests` cover
  missing, corrupt, stale, duplicate, mismatched, interrupted, and
  transaction-injected persistent inputs.
- `draft_compiler_tests` covers selective in-memory invalidation and exact
  semantic/lowering continuation; `draft_driver_timings` checks the public
  phase and graph-work counters.
- `draft_resolve_build_reuses_final_graph` compares the assembly tree emitted by
  same-process `resolve --build` with a later offline build byte for byte.
- `draft_provider_free_resolved_program_consumers` places a failing `codex`
  sentinel first on `PATH`, target-qualifies a copied committed manifest using
  existing source, and runs `check`, `build`, `test`, and `bench`. It also checks
  that repeated commands create no `.draft/cache` directory.
- `draft_native_determinism_tests` retains the native byte-identity oracle for
  repeated links. The complete CTest suite composes these focused contracts with
  parser, semantic, target, ABI, evidence, driver, and native integration tests.
