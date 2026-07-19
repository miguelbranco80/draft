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
- Locked-build tests run with provider access disabled.
- Add regression tests before or with bug fixes.
- Prefer small tests that isolate one rule, plus a smaller number of complete
  end-to-end programs that exercise stage composition.

## Change discipline for agents

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
6. Summarize the changed behavior, tests, and any remaining limitation.

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
