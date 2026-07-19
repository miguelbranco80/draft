# Draft: Compiler architecture

Part of the [Draft language specification](../../README.md).

[← Denials and validation](05-denials-validation.md) · [Next: Future ideas →](07-future-ideas.md)

<a id="section-15"></a>

## 15. Compiler architecture

### Compile-time facilities

The compiler provides constant evaluation, `when` selection, target and type
introspection, parametric types and procedures, layout queries, and
`static_assert`.

Compile-time procedures produce values, not syntax trees. `when` selects among
already parsed branches, and parametric declarations substitute only their
declared type and value parameters. Draft 1 has no macro expander, AST construction
API, textual substitution, or general compile-time declaration generation.
The `...` construct is the sole syntax-synthesis facility; its output is pinned
as inspectable ordinary source under the rules in [section 9](03-agent-synthesis.md#section-9).

### Native lowering and summaries

The compiler lays out static and thread-local package storage and emits only
zero or compile-time constant initializers. For hosted executables it validates
the root package's `main` contract and emits the target entry shim. Executable
startup constructs the root runtime `Context`, and ordinary procedure
signatures and calls lower with the one hidden runtime-context pointer; C
procedure signatures do not.
Package interfaces record referenced calls and globals, context-field access,
built-in use including `unchecked`, `assert`, and assembly, and denial contracts
for separate compilation and link-time composition.

### Synthesis context construction

The compiler builds agent context from the compiler-checked semantic graph
available at the site's elaboration stage. A request contains:

```text
grammar category and expected type
visible local bindings and declarations
enclosing declaration skeleton and parametric parameters and constraints
canonical interfaces of relevant packages
definitions and layouts of relevant user, runtime, and imported core types
permitted context fields, excluding fields removed by active denials
active denials
target features, ABI, calling convention, assembly forms, registers, and operands
attached prompts, documents, files, and folders
relevant tests, judgments, and benchmarks
```

`option.Option`, `result.Result`, `testing.Test`, and `benchmark.Benchmark` are
ordinary core declarations when their packages are imported. The context
builder follows semantic references and includes their canonical definitions
exactly as it does for user-defined types.

Context construction begins with the synthesis site, expected type, visible
names, and constraints, then follows referenced declarations to a bounded
semantic closure. It sends compact interfaces first and expands selected
definitions on demand. Denied names and context fields are absent from the
usable symbol set; their semantic identities remain diagnostic constraints.
Every definition supplied on demand is content-hashed and added to the site's
input hash. Canonical visible-binding facts, the enclosing declaration skeleton,
and parametric parameters and constraints are included even when they do not
originate in a separately hashable file.

The agent proposes syntax from this context. The normal parser, resolver, type
checker, and denial checker accept an expansion before it enters the working
program. Tests, benchmarks, and selected judgments remain authoritative and run
only after the required expansions form one coherent resolved program.

### Shared body-site control facts

Body synthesis and procedure-body judgments receive the compiler's typed
control-flow and data-flow facts at their source position, including visible
bindings, branch refinements, active denials, and available context fields.
These are compile-time facts, not observations of runtime values.

Branch facts describe the structured decisions that admitted control to the
site, in outer-to-inner source order; they do not promise that a mutable source
expression would evaluate the same way again. A conditional fact records its
polarity. A switch-case fact records the matching authored labels, while a
default fact records the complete explicit label set that failed to match.
Loop facts are conservative: mutation or address escape invalidates affected
bindings, and array/slice iteration relates the index to the captured length.
Clause-loop ranges are supplied only for the canonical
`index = 0; index < upper; index += 1` form when `upper` is independent of
`index` and the body neither mutates nor exposes the index address. Nested
procedures do not inherit outer runtime facts, and active denials redact facts
whose values or declarations may not be supplied.

### Judgment context and evidence

A selected judgment receives canonical source and the typed semantic graph for
its anchored package, type, or procedure region; relevant generated source;
attached context; requested target artifacts; and, for a procedure-body site,
the shared control facts above. The compiler limits further context to the
anchored region and its semantic dependencies.

Every surface `judge` receives a stable manifest identity from its package,
enclosing semantic region, grammar category, and structural position. Its
evidence key covers that identity, claim, and attachments; the resolved-program
identity; anchored facts, canonical inspected scope, and semantic dependencies;
requested-artifact hashes; target; and compiler, judge, model, and execution
configuration. The same rule covers wholly handwritten code.

Judgment evidence records the key, every constituent validator identity,
verdict, and rationale, and the policy's aggregate verdict. Judgment execution
and evidence are optional unless an explicit judgment or resolution-validation
profile requests them. `draft build` neither contacts a judge nor requires
evidence in order to compile an already resolved program.

### Staged declaration and interface elaboration

A synthesis site whose output contributes symbols, interface declarations, or
type layout runs before code that depends on that output, regardless of
visibility. This includes private package declarations and public or private
type members. Expression and procedure-body sites normally run in the later
body stage, including bodies of `pub` procedures. A body required by a `when`
condition, type or layout computation, or constant initializer joins the early
compile-time dependency stage instead.

During first resolution, regeneration, and revalidation, declaration or member
sites contributing to the same enclosing package or type form one opaque
completeness set. Their output names need not be declared in advance, and each
expansion is checked without generated outputs from other sites in the set;
existing pins never create semantic edges unavailable on a clean resolution.
Declarations that must be generated together belong in one site. After every
expansion in the set is checked and merged, with conflicts rejected, dependent
lookup and later body sites may continue.

At this stage the available semantic graph consists of parsed declaration
skeletons, completed imports, prerequisite generated interfaces, and facts that
do not depend on the pending expansion. Dependent bodies remain suspended until
the interface and layout are complete.

### Dependency-ordered elaboration

```text
1. Select the target profile and parse packages, imports, declaration skeletons,
   and synthesis sites.
2. Build the semantic dependency graph for imports, `when`, constants, types and
   layouts, and procedure bodies.
3. Load valid pins. Ordinary resolution synthesizes a missing or stale site when
   its obligation and prerequisites are known; `--revalidate` instead stages an
   existing stale expansion under [section 10](03-agent-synthesis.md#section-10).
4. Parse, resolve, type-check, and enforce denials on the expansion before adding
   it to the working semantic graph. Recompute newly ready nodes and repeat.
5. Complete package interfaces, layouts, constants, storage, calling conventions,
   and runtime-body checking. Unresolvable generated-interface cycles are errors.
6. Form one coherent resolved program with one expansion at every required site.
7. In resolution or explicit validation mode, build requested target artifacts
   and run selected tests, benchmarks, and judgments against that program.
8. Commit successful resolution under
   [section 10](03-agent-synthesis.md#section-10). An explicit `draft judge` run
   pins judgment evidence without changing generated source.
9. Build modes verify required pins and evidence, then lower the resolved program
   to LLVM and native code without contacting a model or rerunning validation.
```

Ready nodes from independent packages or declarations may run concurrently;
concurrency is only a scheduling optimization over the dependency graph.

A resolver may internally retry or compare several proposals for one site,
sequentially or concurrently. Proposal count, ordering, scoring, tie-breaking,
and temporary builds are implementation choices. Only the committed resolved
program and evidence bound to it are part of the language and build contract.
