# Compiler graph and resolution simplification plan

Status: implemented on 2026-07-19. This is a non-normative architecture and
completion record; the language specification and command reference remain
authoritative for behavior. Its package-interface scheduling record describes
the completed v140 foundation; current replacement work is tracked by the
[semantic work graph implementation plan](semantic-work-graph-implementation-plan.md).

## Purpose

Make ordinary compilation and `...` synthesis use one understandable compiler
architecture, while preserving an explicit boundary around agent calls and
generated-source changes. The result should compile quickly, keep generated
Draft inspectable, and require no provider during an ordinary build.

The durable resolved program remains:

```text
authored Draft source
+ .draft/resolutions/<target-identity>/{workspace|packages/<root>}/resolution.json
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
- Each package has an immutable declaration generation and a separate
  body-owned semantic generation plus procedure-owned HIR arenas. The body work
  key combines that
  declaration generation with the exact canonical set of cross-package generic
  specializations demanded by consumers. Equal keys reuse bodies, monotonic
  additions check only new specializations, and removals rebuild from the clean
  declaration baseline. Diagnostic preflight and early compile-time synthesis
  checks use disposable copies rather than enriching authoritative state.
- Each authored template or concrete procedure is an explicit live body
  product. Initial roots depend on the package interface; nested procedures and
  locally discovered specializations are added after the exposing frozen wave
  and depend on that exact body row. Each product permanently owns its local HIR
  arena and semantic append packet. The coordinator retains canonical state,
  validates an exact frozen prefix, and appends each packet rather than adopting
  a complete worker successor. TypeStore and SymbolTable prefixes are read-only
  overlays; semantic side tables and constants are still copied, and IDs are
  not yet remapped across independent results, so invocation remains a
  sequential oracle. Remaining read-only views and deterministic shared-wave
  publication are required before body waves become parallel, after which the
  preceding body-key retention mechanism can be deleted.
- A command-local adjacency index records imports by consumer and consumers by
  dependency. It is built once with each source-selection graph and retained
  through source transitions, semantic closure, and lowering. A sequential Kahn
  traversal uses a PackageId-ordered min-heap, so dependency scheduling is
  deterministic. Building its sorted identity and reverse-adjacency views costs
  O((packages + imports) log(packages + imports)); source invalidation walks the
  completed reverse rows in O(packages + imports). This dependency-ordered
  semantic traversal remains sequential. Parallel execution is used for the
  immutable provider-call ready set during resolution and the closed independent
  native-object ready set after target lowering. In both cases workers write
  task-indexed slots and the owning thread publishes in stable order, so
  scheduling cannot change compiler results.
- The graph reuses loaded files, tokens, syntax, declarations, interned types,
  checked expansions, and dependency facts instead of reloading the workspace
  for each conceptual phase.
- Handwritten and generated declarations share the same semantic graph once
  generated source has passed its grammar boundary and ordinary checks. A
  complete-file expansion is reparsed transactionally and may not change
  package or import topology. Declaration/member replacement rebuilds its
  package plus transitive consumers. Body-category replacement rebuilds only
  its containing package's declaration IDs, retains consumer body generations,
  and invalidates their closure products. Unrelated dependency declarations,
  body HIR, and types remain live.

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
  prompt and output-schema contracts, embedded Draft coding-skill digest, and
  fixed adapter version/process policy.
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

- Each selected root/target manifest below `.draft/resolutions/` and every referenced
  `.draft/generated/*.draft` object are normally committed.
- `.draft` transaction staging and native intermediates remain temporary or
  ignored. Do not create a `.draft/cache` hierarchy or another persistent
  compiler cache under a different name.
- Validation and judgment evidence remain separate from the generated-source
  selection. Evidence may be retained for auditing or release qualification,
  but ordinary builds neither require nor update it.
- `draftc expand <workspace> --root <package> --out <directory>` materializes complete source
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
7. Timing remeasurement showed native object/tool work dominates a small
   handwritten build. The larger agent acceptance graph now performs one
   ordinary graph construction plus separately selected typed test- and
   benchmark-context graphs, rather than repeated compiler/workspace passes for
   interface and body stages. Typed validation state survives the body-source
   transition. Parallel package-semantic traversal was therefore not added: its
   coordination and timing-recorder complexity are not justified by the
   measured remaining work. The sorted sequential package ready set is the
   qualified implementation; provider waiting is a separate task-local boundary.
8. A final complexity audit replaced whole-edge rescans and shifting sorted
   vectors with one explicit adjacency index and PackageId-ordered min-heap
   (`83cd919`), then retained that index across graph continuations
   (`aa1150f`).
   The provider-free process acceptance now shadows Codex with a failing
   sentinel and runs `check`, native `build`, `test`, and `bench` from the same
   committed generated source. It also rejects any `.draft/cache` creation
   (`39f9a4d`).
9. A target-independent work graph established stable integer task IDs, sorted
   dependencies, cycle validation, bounded workers, task-indexed outcomes, and
   transitive failure propagation (`030fd77`). Native lowering then freezes its
   canonical module/assembly task plan before any object operation (`cd4aef8`).
10. A narrow LLVM 22 C-API adapter added isolated per-call contexts, exact target
    and layout validation, deterministic object/assembly buffers, and concurrent
    call qualification (`bf2e512`). Embedded shared LLVM became the ordinary
    path; the former Clang IR subprocess remains only a qualification oracle,
    and runtime `clang --version` probing was removed (`5d42229`).
11. The command-owned timing recorder gained stable post-join task events
    (`c06b510`), subprocess launch moved to multithread-safe `posix_spawnp`
    (`5405283`), and native tasks began bounded parallel execution followed by
    lowest-ID diagnostics and ordered publication (`435f781`). One-worker and
    four-worker builds are byte-identical for every artifact kind.
12. A real native parity gate now exercises embedded LLVM and external Clang on
    the same graphs for all artifact kinds and launches both executables
    (`36041e3`). Qualification also repaired debug-location propagation across
    multi-instruction ABI expansions so LLVM retains valid DWARF (`b3aa10f`).
13. Validation evidence now names the LLVM distribution linked into the
    bootstrap rather than a removed ambient version probe (`59b1ff5`). Optimized
    Linux qualification also made assertion-only invariants warning-clean under
    NDEBUG and bound the independent C-client oracle to that selected LLVM's
    Clang (`fd137b1`).
14. The complete Draft coding skill is embedded in the compiler and materialized
    once only for a provider-using command (`225477d`). Synthesis now freezes each
    semantic ready set, runs independent provider calls in bounded work-graph
    waves, checks responses sequentially, and repeats only rejected sites in
    correction waves. The Codex runtime uses multithread-safe `posix_spawnp` and
    one request directory per call.
15. Semantic ownership was split into immutable declaration generations and
    body-owned semantic generations. Cross-package procedure
    specializations now use canonical demand-set work keys: retained dependencies
    are reused during body proposals, new demands extend only missing bodies,
    and removals rebuild exact state. This removed retained-table body replay,
    including duplicate static-pack declarations, without introducing a
    persistent cache or a second compiler graph. Exact body products now retain
    their own HIR arenas while a compatibility projection serves package-wide
    consumers still awaiting migration.

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
- Rechecking a body proposal never re-enters an enriched semantic package. A
  dependency with an equal declaration+demand work key retains its exact HIR;
  an added external specialization checks only that new body, and removing the
  demand leaves no stale concrete symbol or interface row.
- The sorted sequential package-semantic scheduler produces stable manifests,
  generated-source selections, and diagnostics.
- A synthesis ready wave may invoke only independent opaque-set sites in
  parallel. Proposal checking remains single-threaded, lowest-site provider
  failures are stable, and only rejected sites enter a correction wave.
- The native ready set may run concurrently only after complete lowering.
  Lowest-ID failures, task-indexed result slots, and ordered publication keep
  native diagnostics and artifacts equal to the one-worker oracle.
- A clean checkout containing authored source plus the committed durable
  `.draft` files builds for the manifest's selected target without provider
  credentials or network access. Existing generated source may be revalidated
  provider-free to select another supported target.
- Repeated commands leave no persistent compiler cache; all cross-invocation
  semantic state is reconstructible from authored and generated source.

## Automated evidence

- `draft_resolver_tests` covers provenance-independent freshness and identity,
  selective regeneration, bounded rejected proposals, overlapping initial and
  correction waves, accepted-site removal, concurrent interface opacity,
  canonically ordered parallel failures, provider-free reuse, cancellation,
  validation-context graphs, and failure without commit.
- `draft_codex_cli_tests` exercises concurrent real process launches, isolated
  request directories, and the one shared embedded-skill materialization.
- `draft_resolution_tests`, `draft_resolution_store_tests`, and
  `draft_resolution_overlay_tests` cover missing, corrupt, stale, duplicate,
  mismatched, interrupted, and transaction-injected persistent inputs.
- `draft_compiler_tests` covers selective in-memory invalidation, immutable
  declaration/body ownership, equal-key body reuse, generic-demand extension,
  local-instance promotion, exact demand removal, and semantic/lowering
  continuation. `draft_resolver_tests` reproduces a body `...` proposal which
  requests the same dependency static-pack instance before and after expansion.
  `draft_driver_timings` checks the public phase and graph-work counters.
- `draft_resolve_build_reuses_final_graph` compares the assembly tree emitted by
  same-process `resolve --build` with a later offline build byte for byte, then
  proves an intentionally failing backend continuation leaves the successful
  source transaction committed and provider-free checkable.
- `draft_provider_free_resolved_program_consumers` places a failing `codex`
  sentinel first on `PATH`, target-qualifies a copied committed manifest using
  existing source, and runs `check`, `build`, `test`, and `bench`. It also checks
  that repeated commands create no `.draft/cache` directory.
- `draft_work_graph_tests` covers true concurrent overlap, dependency order,
  stable slots, cycles, and failure propagation. `draft_native_object_tasks_tests`
  covers the canonical closed task plan.
- `draft_llvm_object_emitter_tests` covers target rejection, object format,
  repeated bytes, isolated concurrent emission, and AddressSanitizer pass output.
- `draft_native_determinism_tests` compares one-worker and four-worker artifact
  trees and stable parallel failures. `draft_native_backend_parity_tests`
  exercises every artifact kind through both embedded LLVM and the retained
  Clang oracle. The complete CTest suite composes these focused contracts with
  parser, semantic, target, ABI, evidence, driver, and native integration tests.
