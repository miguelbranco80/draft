# Draft: Design context and agent synthesis

Part of the [Draft language specification](../../README.md).

[← Types, memory, and runtime](02-types-memory-runtime.md) ·
[Next: Native interop →](04-native-interop.md)

<a id="section-8"></a>

## 8. Design context

### Documentation and comments

`docs` attaches durable compile-time design context to a package or declaration.
Its primary consumers are synthesis agents and compiler tooling. It has no
runtime representation and does not replace ordinary type or
[denial](05-denials-validation.md#section-13) checks.

Comments are secondary source notes:

```draft
// Explain a local decision.

/* Explain a larger implementation detail. */
```

Comments are lexical trivia rather than attached design context. The default
semantic context builder omits them, they are not exported in canonical package
interfaces, and changing only a comment does not stale a
[synthesis pin](#section-10). An agent may still see comments when tooling or an
explicit `file` attachment supplies a raw source view; that supplied content is
then hashed normally.

Use comments for local implementation explanation. Use `docs` for architecture,
constraints, invariants, protocols, performance intent, and other information
that agents should receive reliably. `docs` accepts an inline string, `file`
and `folder` attachments, or any combination of them:

```draft
docs "Short design intent."

docs file "DESIGN.md"

docs folder "design/"

docs "Decoder invariants and performance constraints."
    file "DESIGN.md"
    folder "design-notes/"
```

`docs` uses Draft's ordinary string literals. Escaped quoted strings are
suited to short text. Raw backtick strings preserve their contents and may span
lines:

```draft
docs `The decoder accepts progressive input.
The hot path must not allocate.`
    file "FORMATS.md"
```

At least one inline string, `file`, or `folder` source is required. Files and
folders are relative to the containing package. `folder` recursively collects
regular files and rejects symlinks. Resolver configuration fixes the extension
allowlist, ignore patterns, file-count and byte limits, and is itself an input
hash component. The manifest records the canonical sorted relative path and
content hash of every included file. Default ignores cover hidden, version-
control, and build-output paths; projects must explicitly ignore credentials.

Package documentation appears immediately after the `package` declaration and
before imports:

```draft
package jpeg

docs "JPEG package architecture."
    file "DESIGN.md"

import core/io
```

Documentation in an attachment group immediately preceding a declaration
attaches to that declaration:

```draft
docs "Allocation-free public decoder."
    file "DECODE.md"

pub decode :: proc(input: []u8, output: []Pixel) -> Decode_Result {
    return decode_pixels(input, output)
}
```

Package documentation is available to every synthesis site in the package.
Declaration documentation is available while elaborating that declaration and
whenever the semantic context builder includes the declaration. Documentation
on a package or `pub` declaration is part of its canonical public interface.
Documentation content participates in the input hash of every synthesis site
that receives it, so a change marks affected pins stale.

Quoted `file` and `folder` paths identify package-relative context content.
Imports use the separate unquoted package-path syntax and are resolved through
the package graph.

### Agent judgments

`judge` is an agent-evaluated validation assertion. Like `static_assert`, it is
a non-value compiler construct: it has no runtime representation, is never
executed by the program, and cannot be used as an expression. Unlike
`static_assert`, its claim is evaluated by a configured judging agent rather
than proved by compile-time evaluation.

A judgment may appear in a package declaration list, a type member list, or a
procedure statement list, including a nested block or branch. A package
judgment is anchored to its package. A type-member judgment is anchored to its
type and member position. A procedure-body judgment is anchored to its
procedure and exact control-flow-graph program point; a nested block or branch
refines that point's facts but is not a separate review scope.

As a statement, `judge` has zero width in the runtime program. It introduces no
control-flow edge, value, binding, scope, side effect, lifetime, or `defer`
action, and it does not affect runtime reachability, definite initialization,
or return analysis. `defer judge ...` is invalid. A judgment at a statically
unreachable program point is rejected.

The surface syntax is:

```draft
package jpeg

docs "Security and parser invariants."
    file "DESIGN.md"

import core/result

judge `The decoder follows the documented state machine
and handles every documented malformed-input case.`
    file "DESIGN.md"
    folder "test-vectors/malformed/"

pub decode :: proc(
    input: []u8,
    output: []Pixel,
) -> result.Result[usize, Error] {
    judge "This procedure validates all input offsets before dereference."

    header := parse_header(input)
    if !header.valid {
        return .err(Error.Invalid_Header)
    }

    judge "Every path reaching this point has validated the header."
    return ... "decode the validated image"
}
```

The string states the claim to evaluate. Optional `file` and `folder`
attachments follow the `docs` path, collection, and hashing rules and provide
additional evidence. Each `judge` is independent; adjacent judgments do not
form an attachment group.

A package-level judgment is an ordinary declaration-list item and therefore
appears after the package and import header. It reviews the package. A judgment
in a type member list reviews that type and its member or layout state at the
position. A judgment in a procedure reviews the enclosing procedure together
with visible bindings, active branch facts, denials, context availability, and
other typed facts at that program point. It may inspect relevant source and
generated code outside the immediate point through the enclosing region's
semantic dependency graph, but it cannot observe runtime values.

Every selected judgment is evaluated once per validator invocation, never when
runtime execution reaches its source position. A policy may schedule several
independent invocations and aggregate their verdicts. Judgments in runtime `if`
or `switch` branches are independently eligible and receive the branch's static
refinements. A judgment in a loop is evaluated once per invocation using the
compiler's fixed-point loop facts, not once per iteration. A judgment in an
unselected compile-time `when` branch is absent from the selected target
program.

Judgment claims can also guide synthesis under deterministic positional rules.
Package-level claims are available to every synthesis site in the package. A
type-member claim is available to later synthesis sites in the same member
list. A procedure-body claim is available to a later site only when it
lexically precedes and dominates that site: every control-flow path to the site
passes through the judgment's program point. Only the surface claim and its
attachments are synthesis input. Program-point facts involving generated code
are built after checked expansions enter the working
[resolved program](#section-10) and are
used when the judgment itself runs. Making a claim available for synthesis does
not schedule its evaluation.

`draft judge` evaluates selected judgments over the compiler's canonical source,
typed program facts, relevant generated source, attached context, and requested
target artifacts. `draft judge codec/jpeg:decode` selects all judgments in that
declaration; selecting a package evaluates its package and enclosed declaration
judgments. The compiler exposes each judgment's stable manifest identity, and
`draft judge <judgment-id>` can select one judgment. A failing selected judgment
fails the command. Resolution and builds do not invoke a judge.

A validation profile may require a judge model or provider distinct from the
synthesizer and may require multiple independent verdicts. Those identities,
counts, and aggregation rules are part of the evidence key; passing evidence
exists only when the configured policy is satisfied.

A judgment can reject an otherwise valid resolved program, but it can never
make invalid syntax, types, denials, ABI rules, tests, or budgets valid. It is
agent review evidence rather than a formal proof. Objective properties should
use types, `assert`, `static_assert`, `deny`, tests, or benchmarks whenever
those can express the requirement.

<a id="section-9"></a>

## 9. The `...` synthesis construct

`...` is valid surface-language syntax. It asks the compiler elaborator to
produce syntax satisfying the grammar and semantic constraints at that
position.

```draft
(err, marker): (Error, Marker) =
    ... "read a marker from input"
        file "MARKER_FORMAT.md"
        folder "test-vectors/markers/"
```

The package-declaration synthesis category begins after the package and import
header. Its expansion contains ordinary declarations and resolves names through
the already completed import scope.

The optional string supplies direct intent. `file` and `folder` use the design-
context attachment rules and add their content to the site's input hash.

The parser records the grammar category. Name resolution and type checking then
derive the semantic obligation:

```text
grammar:         expression
required type:   (Error, Marker)
visible values:  input: []u8
target:          x86_64-linux with AVX2
```

Expected types flow from annotations, argument positions, operators, return
positions, and downstream constraints:

```draft
marker: Marker = ...             // Marker
return ...                       // enclosing result type
write_marker(..., destination)   // parameter type
```

An unannotated synthesis result receives a type variable constrained by later
uses. An underconstrained result requires an explicit type annotation.

`...` can occupy several grammar categories:

```draft
x: u64 = ... "compute the checksum"

if ... "header is valid" {
}

decode :: proc(input: []u8) -> Error {
    ... "decode all blocks"
}

... "package declarations from API.md"
    file "API.md"

Packet :: struct {
    ... "wire fields from PROTOCOL.md"
        file "PROTOCOL.md"
}
```

A site in procedure-body position synthesizes a statement list. It may produce
local declarations, control flow, calls, and returns, and ordinary return
analysis checks the completed body:

```draft
decode :: proc(input: []u8) -> Decode_Result {
    read_marker :: proc(input: []u8, offset: ^usize) -> Marker_Result {
        ... "read the next marker"
    }

    ... "decode the image using read_marker"
}
```

Placing the site after `return` instead requires one expression of the
procedure's result type:

```draft
decode :: proc(input: []u8) -> Decode_Result {
    read_marker :: proc(input: []u8, offset: ^usize) -> Marker_Result {
        ... "read the next marker"
    }

    return ... "decode the image using read_marker"
}
```

Here the required expression type is `Decode_Result`; the expansion cannot
produce a surrounding statement list. The same distinction applies to the
nested `read_marker` body, which could instead contain `return ...` to request
one `Marker_Result` expression.

Compilation follows semantic dependency order, not file or source order. A
synthesis site is ready when its grammar and type obligation, visible names,
active [denials](05-denials-validation.md#section-13), target facts, and
prerequisite generated declarations are known.
Its expansion is parsed and checked before dependent compilation continues.
Independent ready sites may elaborate concurrently, but parallelism is only a
scheduling optimization: a site sees its semantic dependency closure, not
whichever unrelated work happened to finish first.

If a member expansion declares `id: u32`, dependent code and tools resolve
`packet.id` as an ordinary field. Every expansion passes ordinary name, type,
control-flow, bounds, denial, target, and
[ABI](04-native-interop.md#draft-and-c-procedure-abis) checks in its required grammar
category. Generated source contains no unresolved synthesis sites and cannot
introduce `judge`; judgment claims are fixed in surface source before synthesis.
Active denials apply exactly as they do to handwritten source.

<a id="section-10"></a>

## 10. Resolution and provider-free builds

A resolved program consists of surface source, a pinned resolution manifest,
and exactly one content-addressed generated expansion for every required
synthesis site in the selected build graph.

```text
surface source containing `...`
+ resolution manifest
+ generated source
```

The first successful resolution assigns a stable manifest identity to each
synthesis site. The compiler associates that identity using the package,
enclosing declaration, grammar category, and structural AST neighborhood.
Source edits retain the identity when the association is unique; ambiguous
movement requires the programmer to select the intended manifest identity in
tooling.

```text
draft resolve
draft resolve --regenerate
draft resolve --regenerate site-0123456789abcdef
draft resolve --revalidate
draft expand ./cmd/viewer --out /tmp/viewer-expanded
draft judge
draft judge codec/jpeg:decode
draft build ./cmd/viewer
```

`draft resolve` loads valid pins and synthesizes missing or stale sites in
semantic dependency order. Each expansion is checked before it becomes an input
to dependent sites. Once every required site has one expansion, the resolver
checks the complete coherent program and atomically commits staged generated
source and pins. It does not run tests, benchmarks, or judgments. Those are
separate commands that record evidence for the resolved program.

`draft build` consumes valid pins and never updates them or contacts a model.
Missing, stale, invalid, or ambiguously associated pins require `draft resolve`.
A complete handwritten program with no synthesis sites needs no resolution
manifest. This provider boundary is unconditional rather than an optional build
mode: only `draft resolve` may request generated source.

`draft expand <package> --out <directory>` performs the same provider-free
checks, then writes a complete source projection in which every selected `...`
site has been replaced by its checked generated Draft bytes. The requested
directory must not already exist. The projection includes deterministic root
metadata and a source-map sidecar for every selected Draft or assembly input;
it does not replace the manifest and content-addressed fragments as the durable
program representation, and it never changes them.

A provider-free build, ordinary resolution, or revalidation may consume a
content-fresh pin without installing the provider that originally produced it.
Provider, model, and adapter-configuration identities are provenance describing
how the accepted source was obtained. Changing them does not change the source,
the synthesis obligation, pin freshness, or resolved-program identity. A user
who wants the configured provider to reconsider otherwise-fresh source must
request regeneration explicitly.

`draft resolve --regenerate` requests a new checked proposal for every current
synthesis site. Supplying one exact persistent `site-...` identity regenerates
only that site; unrelated fresh sites retain their exact selected expansion.
Missing or stale unselected sites still follow ordinary resolution rules. Every
selector must match, and any rejected proposal or unmatched selector leaves the
previous manifest authoritative. Regeneration may accept the same source bytes;
the resolved program changes only when the selected expansion bytes change.
Regeneration and provider-free `--revalidate` are mutually exclusive.

An external program input is dependency source, a foreign-provider artifact or
summary, or a runtime asset supplied outside the workspace and selected compiler
distribution. When such content participates in resolution or semantic
checking, the manifest records its exact content identity while the physical
path remains invocation-local. The native compiler, linker, SDK, and sysroot are
host build configuration: they do not enter synthesis pins or resolved-program
identity.

`draft resolve --revalidate` stages existing generated source for stale sites,
recomputes current obligations and input hashes in semantic dependency order,
and runs ordinary parse, type, denial, target, and complete-program checks. If
they pass, it atomically re-pins the same expansion hashes under the new inputs.
It never contacts an agent, cannot fill a missing pin, does not run validation
or judgment commands, and leaves staged pin changes uncommitted on failure.

`draft judge` consumes surface source and valid generated-source pins. It never
synthesizes or updates generated code; a missing or stale synthesis pin requires
`draft resolve` first. A successful aggregate installs passing evidence for its
exact evidence key.

Validation-attempt history is independent of the generated-pin transaction.
Every selected test run, judgment invocation, and benchmark verification
records its result. A failed run or aggregate deactivates passing evidence for
that exact key while retaining history and leaving other keys unchanged.

`draft build` fails when a required synthesis pin is missing, stale, invalid, or
ambiguously associated with a synthesis site. Validation and judgment evidence
remain auditable records produced by their commands; compiling a resolved
program does not require or rerun them.

Each site has an input hash covering everything supplied to synthesis: its
prompt and attachments; grammar and type obligation; enclosing declaration;
visible bindings and parametric constraints; supplied interfaces and
definitions; target, ABI,
[assembly capabilities](04-native-interop.md#section-11), and denials; and compiler and
resolver configuration. Tests, judgments, and benchmarks supplied as context
are covered like any other input. A changed hash makes the site's pin stale.
Each pin also records the exact resolver or synthesizer provider, model, and
configuration identity as non-semantic provenance.

The manifest identifies the coherent resolved program from the selected target;
the exact identities of all selected workspace and dependency source,
compiler-distributed core and runtime inputs, explicit foreign artifacts,
provider summaries, and runtime assets; the build graph; and every
synthesis-site identity and selected expansion hash. Validation evidence binds
that identity to the selected validation definitions, requested artifacts,
tool version, and execution policy; it cannot be reused for a different
resolved program. Evidence objects and their active/revoked state are stored
separately from the resolution manifest.

Generated source is the inspectable persistent representation. The compiler
does not maintain a persistent AST, typed-IR, native-object, or incremental
cache; those forms are ordinary in-process state or per-build outputs derived
from source.

Diagnostics, debugger locations, profiles, coverage, and disassembly map
through generated source to the original synthesis site. Tools may show the
surface program, generated implementation, or both.
