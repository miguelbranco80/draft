# Draft compiler engineering rules

These rules apply to the entire repository. They govern the bootstrap compiler,
runtime, core packages, tests, tools, and implementation documentation.

The desired style is direct, data-oriented, heavily explained, and deliberately
unfancy. Optimize for a future compiler engineer being able to open one module,
understand the data it owns, follow the transformations, and change it without
learning a private framework.

## Source of truth

- The Draft specification is authoritative. Do not change language behavior to
  make an implementation shortcut convenient.
- Read the relevant specification section before implementing or reviewing a
  language feature. Cite the section in comments where the implementation rule
  is not obvious.
- `docs/implementation/architecture.md` describes architecture, while
  `docs/history/first-implementation-plan.md` preserves sequencing; neither is
  authoritative for semantics.
- When the specification is genuinely ambiguous, isolate the choice, document
  it as an implementation decision, and surface it for review. Do not silently
  invent semantics in a low-level helper.

## Repository Draft coding skill

- For any task that authors, reviews, debugs, or tests `.draft` source, changes
  a public `core/` API, or decides whether the current language/library supports
  an operation, load and follow
  [`.agents/skills/write-draft-code/SKILL.md`](.agents/skills/write-draft-code/SKILL.md).
- The skill is an applied coding guide, not semantic authority. The
  specification and current core source win if a skill reference is stale;
  repair that reference in the same coherent change.
- Keep the skill's main file concise and route detail through its focused
  `references/` files. Do not copy the complete specification into the skill.
- After materially changing the skill, run the skill creator's structural
  validator and forward-test realistic Draft tasks with fresh agents. A
  syntactically valid skill that causes agents to invent APIs or mishandle
  ownership is not complete.

## Governing style

### Write the direct version

- Represent the problem being solved directly. Prefer an enum, a plain struct,
  an array, and an explicit loop over a class hierarchy or generic framework.
- Make control flow visible. Prefer ordinary calls, loops, and `switch` statements
  to callbacks, fluent pipelines, implicit registration, or hidden dispatch.
- Make state transitions visible. A reader should be able to identify which
  function changes compiler state, which data changes, and why.
- Keep the first correct implementation simple even when it repeats a small
  amount of code. Extract an abstraction after the common semantic operation is
  understood, not merely because two fragments look similar today.
- Prefer semantic compression: preserve small concrete operations, then compose
  them into higher-level operations. Do not erase the useful lower-level pieces.
- Avoid speculative flexibility. Implement the current Draft 1 requirement and
  leave a clear seam only where the specification already identifies variation,
  such as target profiles or synthesis providers.
- Do not introduce a framework to implement one feature.

### Keep code local and readable

- A coherent operation may remain in one moderately long function when splitting
  it would force the reader through a chain of tiny wrappers. Split at meaningful
  data, lifetime, phase, or reuse boundaries—not at arbitrary line counts.
- Keep closely related data and operations in the same module.
- Prefer explicit intermediate variables with semantic names over dense nested
  expressions.
- Prefer straightforward loops over clever iterator, range, or functional
  compositions when the loop exposes the algorithm more clearly.
- Avoid boolean parameters whose meaning is unclear at a call site. Use an enum
  or separate named operation when the distinction is semantic.
- Avoid generic names such as `Manager`, `Processor`, `Helper`, `Util`, `Thing`,
  or `Data` unless the surrounding domain makes the responsibility exact.
- Do not add pass-through getters and setters around plain compiler data. Use a
  function when it enforces an invariant or performs a meaningful operation.

## Comments are a primary deliverable

The repository intentionally prefers abundant, extensive comments. Comments
must transfer understanding, not merely increase line count.

### Required file and module comments

Every nontrivial header or implementation file starts with a comment explaining:

- the module's purpose and the compiler phase it belongs to;
- its important inputs and outputs;
- the data or resources it owns;
- the invariants it establishes or assumes;
- the modules it is intentionally allowed to depend on;
- any relevant Draft specification sections.

The comment may be several paragraphs. A new engineer should understand why the
file exists before reading individual declarations.

### Required type comments

Every important struct, enum, tagged variant, ID, table, IR node, and state object
has an extensive comment covering, as applicable:

- what one value represents;
- valid and invalid states;
- ownership, lifetime, mutability, and pointer stability;
- relationships between indices, IDs, parallel arrays, and side tables;
- the phase in which fields become available;
- whether equality or ordering is semantic, structural, or incidental;
- whether the value may be serialized or included in a content hash.

Comments on fields should explain units, sentinel values, index domains, and
non-obvious coupling. Do not comment `line` with “the line”; explain whether it
is zero- or one-based and which source buffer it addresses.

### Required function comments

Every nontrivial function has a contract comment explaining:

- what operation it performs and why it belongs at this layer;
- preconditions and assumptions;
- returned result or state changes;
- ownership and lifetime effects;
- diagnostics or failure behavior;
- determinism or ordering requirements;
- the algorithm and its important invariants when they are not immediate.

For a multi-stage function, add comments before each stage describing the state
transition. For a subtle branch, explain why the branch exists and what would go
wrong without it.

### What good comments explain

- Why semicolon insertion is suppressed at this token boundary.
- Why a generated declaration cannot be visible to another site in the same
  opaque completeness set.
- Why a type is interned before its layout is complete.
- Why a list is sorted before hashing or serialization.
- Why a context argument physically moves around an ABI-mandated return slot.
- Why an apparently redundant check preserves a denial or safety guarantee.
- Why a source range points to generated source rather than the surface site.

### Comments to avoid

- Do not translate obvious syntax into English: `index += 1` does not need
  “increment index.”
- Do not preserve dead implementation history. Explain the current reason; use
  version control for archaeology.
- Do not make vague claims such as “for safety,” “for performance,” or “magic.”
  State the concrete invariant, cost, or mechanism.
- Do not use a comment to excuse opaque code. Rewrite the code, then document
  the remaining semantic reason.
- Never leave a comment inconsistent with the code. Updating nearby comments is
  part of every behavioral change.

### Specification examples in comments

Use short examples when they make a grammar, typing, layout, or lowering rule
clearer. Include both the source-level case and the important internal result
when useful. Keep examples small enough to verify mentally.

## Modularization and dependency direction

Modularization means cohesive compiler subsystems with narrow, explicit
interfaces. It does not mean a class or file for every noun.

The intended dependency direction is:

```text
base
  <- source
  <- syntax
  <- workspace
  <- sema
  <- elaborator
  <- ir
  <- target
  <- backend/llvm
  <- driver
```

Some stages exchange stable IDs or read-only interfaces, but lower layers must
not call back into higher layers. In particular:

- `base` knows nothing about Draft syntax or LLVM.
- `syntax` knows grammar and source ranges but not resolved types.
- `sema` owns language meaning and must not depend on LLVM IR.
- `elaborator` consumes semantic obligations and returns source expansions; it
  cannot directly manufacture checked HIR or MIR.
- `ir` makes Draft runtime operations explicit without encoding one LLVM
  version's accidental representation.
- target modules own layout and ABI choices.
- `backend/llvm` is an adapter and must not become the semantic authority.
- the Codex adapter lives behind the provider boundary and cannot leak model SDK
  types, prompts, or network behavior into `sema`.

Each module should expose the smallest useful public header. Do not create a
general-purpose dumping ground. A helper belongs with the data and invariant it
serves.

## Data representation and ownership

- Prefer stable integer IDs into explicit tables for declarations, types,
  symbols, files, packages, scopes, blocks, and IR values.
- Prefer contiguous storage and phase-owned arenas for compiler graphs that die
  together. Document every arena's lifetime.
- Prefer plain structs for records and explicit tagged variants for alternatives.
- Avoid webs of individually allocated polymorphic objects.
- Owning and non-owning pointers must be visually and contractually clear.
- Do not use `shared_ptr` by default. Shared ownership requires a written
  lifetime argument showing why one clear owner or an arena is insufficient.
- Use RAII for actual resources such as files, mappings, LLVM handles, temporary
  directories, locks, and subprocesses. “Direct code” does not mean leaking or
  hand-waving resource lifetime.
- Avoid mutable global state. Pass phase context explicitly. Immutable process-
  wide tables are acceptable when initialization and determinism are obvious.
- Do not cache until the cache key, invalidation rule, ownership, and
  determinism consequences are documented.

## C++ subset

Use C++20 as a practical systems language, not as an invitation to use every
language feature.

### Preferred tools

- Plain structs, enums, free functions, namespaces, and explicit composition.
- `std::string`, `std::string_view`, `std::vector`, `std::span`,
  `std::optional`, `std::variant`, and `std::unique_ptr` where their ownership
  and lifetime are clear.
- Small project-specific `Result`, ID, arena, and diagnostic types whose
  semantics are documented.
- `const` wherever it communicates a real invariant.

### Restricted tools

- Build without C++ exceptions and RTTI. Report user errors through diagnostics
  and recoverable internal failures through explicit result values.
- Do not use inheritance or virtual dispatch unless a real runtime boundary
  requires open-ended behavior and a function table or tagged switch is worse.
  Document any exception to this rule.
- Do not write template metaprogramming. Templates are acceptable for standard
  containers and small, obvious, locally documented utilities.
- Do not overload operators except for value types where the operation has one
  unsurprising mathematical or identity meaning.
- Do not use macros for control flow, ownership, declaration generation, or
  hidden registration. Restrict macros to build/platform facts, assertions, and
  unavoidable LLVM/C interoperation.
- Do not use C++ modules, coroutines, reflection tricks, expression-template
  libraries, dependency-injection frameworks, or automatic serialization
  frameworks.
- Use `auto` when the type is syntactically obvious or an iterator type is
  irrelevant. Spell out types that carry semantic meaning at the point of use.

## Error handling and invariants

- Invalid Draft source produces a structured diagnostic with an exact source
  range. It must not crash the compiler.
- Impossible compiler states use assertions with comments describing the
  invariant. Do not turn user-controlled invalid input into an assertion.
- Never ignore an error result. Handle it, propagate it, or deliberately convert
  it into a diagnostic at a documented boundary.
- Do not silently recover by changing program meaning. Error recovery exists to
  produce more diagnostics, not to create code for a different program.
- Keep diagnostic construction near the semantic decision that detects the
  error, while keeping rendering separate.
- Preserve original and generated source locations through every IR stage.

## Determinism and inspectability

- Never depend on hash-table iteration, filesystem enumeration, pointer values,
  thread scheduling, locale, host paths, or ambient environment order.
- Sort data explicitly before canonical serialization, hashing, diagnostics,
  interface emission, or manifest output. Comment the semantic sort key.
- Use physical filesystem paths only for I/O and diagnostics. They must not
  enter semantic identity.
- Every cache and manifest input must be enumerable and explainable.
- Concurrency may change scheduling, never results. First write and test the
  deterministic sequential algorithm; parallelize only a clearly independent
  ready set.
- Generated source and semantic dumps must be readable enough to debug without
  attaching a debugger to the compiler.

## Performance

- Correct language behavior, deterministic output, and useful diagnostics come
  first.
- Know the asymptotic behavior of core operations. Avoid accidental quadratic
  scans over tokens, declarations, types, dependency edges, or IR instructions.
- Prefer data layouts that make the common traversal simple and contiguous.
- Measure before adding a complex optimization. Record the benchmark and the
  reason when complexity is retained for performance.
- Keep an obviously correct slow path or focused test oracle when implementing
  a subtle optimized algorithm.
- Do not sacrifice semantic clarity for insignificant compile-time wins.

## Tests

- Every language rule needs positive and negative tests.
- Every diagnostic test checks the source range and the important message, not
  only that compilation failed.
- Layout and ABI tests compare Draft's computation against fixed target facts
  and, where appropriate, an independent C/Clang oracle.
- Parser tests cover recovery and malformed input, not only valid examples.
- Constant evaluation tests cover boundaries, traps, signedness, rounding,
  infinities, NaNs, and subnormals.
- Synthesis tests use deterministic fake providers before exercising Codex.
- Provider-free build tests run with provider access disabled.
- Add regression tests before or with bug fixes.
- Prefer small tests that isolate one rule, plus a smaller number of complete
  end-to-end programs that exercise stage composition.
- Keep tests safe under simultaneous builds from multiple Git worktrees. Write
  scratch only below the current CMake binary directory or a process-unique
  temporary directory; never use a fixed shared path or write derived state
  into the source checkout. Integration scripts must receive their scratch
  root explicitly rather than deriving one from a repository-global location.

## Change discipline for agents

### Pre-release final-state discipline

Draft is still being built and has no released compatibility contract. When a
repository change replaces a schema, path layout, command contract, fixture,
or internal representation, leave only the intended final state:

- Do not add legacy readers, migration code, dual writes, deprecated aliases,
  fallback paths, or compatibility adapters unless the user explicitly asks
  to preserve a released external contract.
- Update every current producer, consumer, test, example, document, CI entry,
  and skill reference in the same coherent change. Search the repository for
  the replaced spelling or shape and remove stale current-state references.
- Delete obsolete repository-owned fixtures and generated state, then
  regenerate only the artifacts required by the final implementation. Do not
  carry an old manifest or evidence row forward merely to avoid rebuilding it.
- Keep historical documents historical. They may describe an earlier state,
  but current code must not retain a compatibility path solely because that
  state once existed.

This rule does not authorize deleting unrelated user work in a dirty checkout.
Resolve ownership first, and restrict cleanup to state that is demonstrably an
obsolete product of the repository behavior being replaced.

### Commits

- Keep the work in a sequence of reasonably small, coherent commits. Each
  commit should have one related purpose that can be explained without an
  "and" joining unrelated changes.
- Commit a completed, verified slice as soon as it is coherent. Do not leave
  completed current work uncommitted at handoff.
- Do not mix unrelated behavior, refactoring, generated artifacts,
  repository-wide documentation reorganization, or agent-rule changes in one
  commit. Documentation that specifies or explains the same semantic change may
  remain with that implementation when separating it would make either commit
  incomplete.
- Before committing, inspect the staged diff, run the narrow relevant checks,
  and use an imperative commit message that states the outcome. Each commit
  should build and pass its relevant tests when practical.
- Preserve commits and work authored by the user or another agent. Do not
  rewrite, squash, or discard them unless the user explicitly requests it.

### Documentation updates

Documentation is part of the change, not a later cleanup task. Update the
current document that owns a changed contract in the same coherent commit as
the code when practical. Use [docs/README.md](docs/README.md) as the complete
map and follow this routing:

- Language syntax, typing, evaluation, memory, runtime meaning, agent
  constructs, interoperation, denials, or validation semantics belong in the
  relevant `docs/specification/` section. An implementation shortcut or current
  limitation must not be written as normative language behavior.
- Machine layout, ABI, object-format, platform-runtime, and parsed-assembly
  facts belong in the relevant versioned `docs/targets/` profile.
- Compiler phase boundaries, representations, algorithms, ownership,
  elaboration, lowering, runtime/core implementation, external inputs, and
  evidence mechanics belong in the matching `docs/implementation/` subsystem
  document. Record a narrower temporary capability in
  `docs/implementation/implementation-limits.md` as well as at the relevant
  subsystem seam when readers need both contexts.
- Public commands, flags, output kinds, provider mappings, and operator
  workflows belong in `docs/operations/command-reference.md`.
- Exact completed qualification, exercised toolchains/models, acceptance
  fixtures, and release-selection facts belong in `docs/releases/`. Update
  these only after the claimed checks have actually run; they are evidence, not
  plans.
- Unresolved semantic choices belong in
  `docs/decisions/language-questions.md`. Once resolved, put the authoritative
  result in the specification and the concrete mechanism in implementation or
  target documentation rather than leaving the decision only in a discussion
  note.
- Runnable feature coverage belongs in `examples/README.md` and in the example
  itself. Update the feature map when an example is added, removed, renamed, or
  materially broadened.
- `README.md` remains the short public entry point. Update it when the language
  overview, supported target, getting-started path, or documentation map changes,
  but do not turn it into an implementation-status journal.
- `docs/history/` preserves chronology and provenance. Do not edit an archive
  to describe current behavior except to repair an archival or migration error;
  update the current specification, target, implementation, operations, or
  release document instead.

Keep the repository Draft coding skill synchronized with those owning
documents and source APIs:

- Syntax, typing, evaluation, runtime meaning, and broadly applicable coding
  patterns update `references/language.md`; ownership or lifetime changes also
  update `references/memory-and-ownership.md`.
- Public `core/` additions, removals, signatures, errors, ownership, target
  availability, and conspicuous new gaps update `references/core-library.md`
  and the ownership guide when relevant.
- Target, C ABI, native linking, assembly, or artifact changes update
  `references/interop-and-targets.md`.
- `docs`, `judge`, `...`, resolution, denials, tests, benchmarks, or validation
  semantics update `references/agent-features.md`.
- CLI, package layout, test practice, examples, documentation routing, or
  completion workflow changes update `references/workflow-and-testing.md`.
- Change `SKILL.md` itself when routing, universal workflow, or trigger scope
  changes. Regenerate `agents/openai.yaml` when its user-facing metadata no
  longer describes the skill.

When adding, moving, or renaming a document, update `docs/README.md` and every
affected link in the same commit. If a code change genuinely changes no public,
target, implementation, operational, qualification, or example contract, say
so in the completion summary rather than making a token documentation edit.

Before editing:

1. Read this file, the relevant specification sections, and the module header.
2. Identify the data representation, phase boundary, and invariants involved.
3. Look for an existing direct operation before adding a new abstraction.

While editing:

1. Implement the smallest coherent semantic change.
2. Keep unrelated refactors out of the patch.
3. Add or update the extensive comments required above as part of the code—not
   as a cleanup pass left for later.
4. Add tests at the same time as behavior.

Before declaring completion:

1. Run the narrow tests, then the relevant broader suite.
2. Run formatting, warnings, and enabled sanitizers.
3. Re-read the changed code linearly and remove unnecessary indirection.
4. Audit every nearby comment for continued truth.
5. Confirm deterministic ordering and explicit error handling.
6. Confirm the owning current documents and documentation maps remain accurate.
7. Summarize the changed behavior, tests, documentation, and any remaining
   limitation.

## Definition of straightforward

Straightforward does not mean primitive, monolithic, uncommented, or careless.
For this compiler it means:

- the representation resembles the language concept;
- the code says what phase it is performing;
- ownership and state changes are explicit;
- abstractions earn their existence by compressing understood semantics;
- comments preserve the reasoning that code alone cannot express;
- a new engineer can trace a Draft program from token to native instruction
  without passing through hidden machinery.
