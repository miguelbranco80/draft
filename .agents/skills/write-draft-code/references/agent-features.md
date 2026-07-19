# Agent Features, Denials, and Validation

Draft programs do not require an agent. `docs`, `judge`, and `...` are explicit
language facilities for durable design context, agent review, and pinned source
generation. Ordinary handwritten code compiles without any of them.

Read the normative
[`agent synthesis specification`](../../../../docs/specification/03-agent-synthesis.md)
and [`denials and validation specification`](../../../../docs/specification/05-denials-validation.md)
before changing these constructs or their tooling.

## Contents

- [Choose the right mechanism](#choose-the-right-mechanism)
- [Durable `docs`](#durable-docs)
- [Agent `judge`](#agent-judge)
- [Synthesis sites](#synthesis-sites)
- [Generated source and type obligations](#generated-source-and-type-obligations)
- [Resolution and provider-free build](#resolution-and-provider-free-build)
- [Compiler-enforced `deny`](#compiler-enforced-deny)
- [Tests](#tests)
- [Benchmarks and instrumentation](#benchmarks-and-instrumentation)
- [Review checklist](#review-checklist)

## Choose the right mechanism

Use the narrowest mechanism that expresses the property:

| Need | Mechanism |
| --- | --- |
| Local implementation reason | source comment |
| Durable architecture/invariant supplied to tools | `docs` |
| Compile-time objective fact | `static_assert` |
| Runtime programmer invariant | `assert` |
| Executable behavior | `*_test.draft` test |
| Measured performance budget | `*_bench.draft` benchmark |
| Transitive capability exclusion | `deny` |
| Hard-to-formalize agent review claim | `judge` |
| Agent-generated checked syntax | `...` |

A judgment is review evidence, not proof. It cannot make invalid syntax, types,
ABI, denials, tests, or budgets valid. Do not use `judge` when a type, check,
test, or denial can state the rule objectively.

Do not use `...` merely because writing the code is tedious. A complete
handwritten implementation is always a valid plan.

## Durable `docs`

Comments are lexical trivia. They remain valuable for local ownership,
algorithm, and implementation reasoning, but the default semantic context
builder omits them. A comment-only change does not stale a synthesis pin unless
the comment is included through an explicit raw-source attachment.

`docs` is durable compile-time design context with no runtime representation:

```draft
docs "Allocation-free decoder."

docs file "DESIGN.md"

docs folder "design-notes/"

docs `The decoder accepts progressive input.
The hot path must not allocate.`
    file "FORMAT.md"
    folder "test-vectors/"
```

At least one string, `file`, or `folder` is required. Paths are quoted and
relative to the containing package. Folder collection is recursive, includes
regular files only, rejects symlinks, and follows configured allowlists,
ignores, count limits, and byte limits. Never attach credentials or build
output.

Package documentation goes after `package` and before imports:

```draft
package codec

docs "Package architecture."
    file "DESIGN.md"

import core/io
```

An attachment group immediately before a declaration attaches to that
declaration:

```draft
docs "Public decoder contract."
pub decode :: proc(input: []u8) -> Error {
    return decode_impl(input)
}
```

An explicit semicolon ends an attachment group. Automatic semicolon insertion
is suppressed before immediately attached `file` or `folder` clauses.

Package docs are supplied to every synthesis site in the package. Declaration
docs are supplied while elaborating that declaration and whenever it appears
in semantic context. Package and public-declaration docs enter canonical public
interfaces. All supplied content enters affected synthesis input hashes.

Use `docs` for architecture, invariants, protocols, constraints, and intent an
agent must receive reliably. Keep source comments too: future humans still need
local reasoning that is not an elaboration input.

## Agent `judge`

`judge` is a non-value compiler construct evaluated only by a judging command:

```draft
judge "The public parser rejects every malformed length before dereference."
    folder "test-vectors/malformed/"
```

It may appear in a package declaration list, type member list, or procedure
statement list. Package-level judgments come after the package/import header.
Each judgment is independent; adjacent judgments do not form one attachment
group.

Within a procedure, `judge` is anchored to the exact control-flow program
point. It receives visible bindings, branch refinements, active denials,
Context availability, and other typed facts. It has zero runtime width:

- no value, binding, side effect, lifetime, or control-flow edge;
- no effect on initialization, reachability, or return analysis;
- evaluated once per selected validator invocation, not once per runtime visit;
- loop judgments use fixed-point facts, not one invocation per iteration;
- judgments in unselected `when` branches are absent;
- `defer judge ...` is invalid;
- a statically unreachable judgment is rejected.

A procedure-body claim can guide a later synthesis site only when it lexically
precedes and dominates that site. Claims do not schedule their own evaluation.
Generated source cannot introduce new `judge` constructs.

Use `draft judge`, a package/declaration selector, or a stable judgment ID to
evaluate selected claims. A failed selected judgment fails that command.
`draft build` never invokes a judge and does not require passing evidence.

## Synthesis sites

`...` asks the elaborator to produce syntax in its exact grammar position:

```draft
checksum: u64 = ... "compute the checksum"

decode :: proc(input: []u8) -> Error {
    ... "decode all blocks"
}

Packet :: struct {
    ... "declare fields from PROTOCOL.md"
        file "PROTOCOL.md"
}
```

Supported categories include an expression, a procedure statement list,
package declarations, type members, and assembly instructions. The parser
records the category before elaboration. The expansion cannot escape it.

An optional intent string and `file`/`folder` attachments use the same rules as
`docs`. Use explicit instructions that state invariants and required behavior,
but rely on the type obligation and checked program for enforcement.

Position matters:

```draft
decode :: proc(input: []u8) -> Decode_Result {
    ... "implement the body"        // statement-list obligation
}

decode_one :: proc(input: []u8) -> Decode_Result {
    return ... "produce one result" // expression obligation
}
```

In the first form the expansion may contain declarations, branches, calls, and
returns. In the second it must be exactly one `Decode_Result` expression.

## Generated source and type obligations

Name resolution and type checking derive each site's obligation from its
position: required grammar, expected type, visible bindings, active denials,
target/ABI/assembly facts, parametric constraints, documentation, and semantic
dependencies.

Expected type flows through annotations, arguments, operators, and returns:

```draft
value: Header = ...
return ...
consume(..., output)
```

If later uses cannot determine an unannotated expression's type uniquely, add
an explicit annotation. Do not expect the model to choose semantics for an
underconstrained value.

An expansion is ordinary Draft source. It is parsed and checked under the same
name, type, control-flow, bounds, denial, target, ABI, and assembly rules as
handwritten code. It cannot contain unresolved `...` or manufacture checked IR
directly. Generated declarations become visible only through semantic
dependency order, never through completion timing of unrelated sites.

Generated source is persisted and inspectable. Diagnostics retain the
generated location plus the surface synthesis origin. Treat it like code:
review it, test it, and keep its source maps meaningful.

## Resolution and provider-free build

A program with synthesis sites resolves to:

```text
surface source + pinned manifest + one generated expansion per required site
```

`draft resolve` is the only command allowed to request new generated source. It
loads valid pins, elaborates missing/stale sites in semantic dependency order,
checks each expansion and the coherent complete program, and commits successful
staged pins atomically. It does not run tests, benchmarks, or judgments.
The bootstrap adapter embeds this complete skill in `draftc`. A provider-using
resolve materializes its exact files once, exposes that immutable guide inside
each isolated Codex request directory, and removes it when the command ends.
Fresh provider-free builds and resolves never materialize it. The adapter
discovers `codex` through `PATH`; `--model` is optional and omission uses the
Codex-configured default. Skill, model, retry, and adapter identities are
generation provenance, not accepted-source freshness. Do not add executable
paths, credentials, or Codex installation hashes to Draft source identity.
Use `draftc resolve <package> --regenerate` to reconsider every fresh expansion,
or append one exact `site-...` identity to reconsider only that site. This is
the deliberate source-changing operation; changing `--model` alone is not.

`draft resolve --revalidate` never contacts an agent. It stages existing
generated source under current obligations, runs ordinary compiler checks, and
re-pins only on success. It cannot fill a missing pin and does not modify
validation or judgment evidence.

`draft build` consumes surface and valid generated-source pins. It never
contacts a model, updates a pin, runs tests, evaluates judgments, or reruns
benchmarks. Missing, stale, invalid, or ambiguously associated pins fail with a
request to resolve. A complete handwritten program needs no manifest at all.

Pins hash every synthesis input, including grammar/type obligation, prompt and
attachments, surrounding declaration, visible facts, target/ABI/assembly,
denials, supplied code/interfaces, and semantic compiler/resolver configuration.
Provider, model, retry, and adapter identities are recorded only as provenance;
changing them does not stale accepted source or change the resolved-program
hash. The
manifest also records exact source, dependency, foreign artifact, provider
summary, and runtime-asset content identity. Invocation-local physical paths
do not become semantic identity.

Keep the two uses of “provider” distinct:

- a synthesis provider generates source only during `resolve`;
- a native link provider supplies an exact object/archive/shared library.

Neither implies a package dependency manager or network access during build.

## Compiler-enforced `deny`

`deny` establishes a lexical, transitive capability boundary using resolved
program identities:

```draft
import core/heap as heap

checksum := deny heap, asm, context.allocator {
    compute_checksum(input)
}
```

Its position selects the region category:

- expression: exactly one expression and its result; no declaration scope;
- statement: an ordinary nested lexical block;
- declaration/member: contents contribute to the surrounding package/type.

Selectors can name an imported package alias, declaration, global, `context`,
a Context field, `assert`, `asm`, or `unchecked`. They resolve to stable
semantic identities despite aliases, parametric instances, or linker names.

The restriction applies to every reachable handwritten or generated helper,
compiled interface, assembly symbol, and finite procedure-pointer target.
Unknown dynamic or foreign edges fail an active denial. A matching exact-
artifact provider summary may account for foreign effects; absence is unknown,
not trusted.

Nested denials only add restrictions. `deny unchecked` permits checked or
statically proven bounded access but rejects multi-pointer indexing and
reachable unchecked operations. `deny assert` rejects runtime `assert`, not
`static_assert`. Because runtime assertion reporting uses Context, denying all
`context` also rejects it. Parsed and package assembly both count as `asm`.

Optimization may not weaken a denial, and a generated expansion cannot bypass
one. Use denials for architecture boundaries that must remain mechanically
enforced across future helper refactors.

## Tests

Draft tests are ordinary procedures in `*_test.draft`:

```draft
import core/testing

test_decode :: proc(test: ^testing.Test) {
    result := decode(test_input)
    testing.expect(test, result.valid)
}
```

Discovery requires a defined, package-level, non-parametric ordinary procedure
whose name begins `test_`, accepts exactly one authenticated
`^testing.Test`, and returns no value. Test files participate in `draft test`
and selected resolution validation, not ordinary package builds.

Use small tests that isolate behavior. Check boundaries, errors, ownership
cleanup, and target cases—not only a happy example. Tests run in canonical
package/declaration order; do not depend on ambient order or residue from a
previous test.

Compiler rejection behavior belongs in the C++ diagnostic suite when it must
assert an exact source range, parser recovery state, or language diagnostic.

## Benchmarks and instrumentation

Draft benchmarks are ordinary procedures in `*_bench.draft`:

```draft
import core/benchmark
import core/time as time

bench_decode :: proc(state: ^benchmark.Benchmark) {
    run :: proc() {
        decode(test_input)
    }

    benchmark.require_max_time(state, 200 * time.nanosecond)
    benchmark.measure(state, run)
}
```

Discovery parallels tests with the `bench_` prefix and exactly one
`^benchmark.Benchmark`. `draft bench --verify` reruns configured budgets.
Ordinary build neither requires nor reruns benchmark evidence.

A validation profile may require instrumentation. Unavailable required
instrumentation fails closed; it is never silently skipped. Current generated
Draft AddressSanitizer support is qualified only on macOS. The Linux x86-64
ASan/UBSan CI job instruments the C++ bootstrap, not generated Draft code.

## Review checklist

- Keep ordinary code fully valid without assuming an agent is present.
- Use comments and `docs` for their distinct local and durable purposes.
- Place package docs before imports and package judgments after imports.
- Give expression synthesis an explicit expected type when inference is not
  unique.
- Review generated source as normal checked source.
- Keep build provider-free: only resolve may generate or update source.
- Prefer objective checks over judgment claims.
- Confirm denial selectors resolve to the intended semantic identities and
  reject unknown edges.
- Put discovered tests/benchmarks in their exact suffix files and use exact
  signatures.
- Treat validation evidence as separate from whether a resolved program can
  build.
