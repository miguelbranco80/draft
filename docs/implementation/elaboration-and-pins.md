# Elaboration, semantic context, and pins

This document records provider-neutral obligation construction, opaque scheduling, proposal checking, Codex execution, source identity, freshness, and authenticated overlays. The language-level behavior of `...`, `docs`, and `judge` remains in the specification.

## Synthesis request and semantic definition context

Provider requests never substitute hashes for information the synthesizer must
understand. `draft-synthesis-request-v21` carries canonical Draft spellings for
the expected type and every visible binding, together with complete canonical
values for visible compile-time constants and explicit target, SIMD, and
parsed-assembly facts. Procedure-valued constants lose their process-local
symbol index and gain the same package-qualified identity used by interfaces.
It also carries the checked runtime `Context` fields with exact offsets and
types. Active denials resolve through the ordinary
semantic symbol table: denied bindings and packages are removed, a denied
import member is redacted from its compact package interface, and `context` or
`context.field` removes the matching usable field rows without erasing the
diagnostic policy selector. Package and enclosing-declaration documentation
retain their anchor, text, and exact isolated attachment bytes. The same rows
remain content-hashed, so their human-readable representation is both useful to
Codex and a stale-pin input.

Compiler rejection is provider correction context, not a change to the stable
site obligation. A resolver gives each new proposal a private lexical-boundary
check and an ordinary parser/semantic compile over exactly that one site and
the already accepted earlier-stage overlays. Other proposals in the same opaque
completeness set remain invisible. A rejected response is retried at most once
by default; the stateless second request carries the exact rejected bytes and
generated-source-aware diagnostics. Intermediate diagnostics never reach the
user or store, while exhaustion publishes only the final rejection and leaves
the prior manifest untouched. This compiler-level budget is independent of the
Codex adapter's process retry budget for timeouts and process failures.

Usable visible names remain compact name/type/value rows. Separately, the
compiler starts from exact prompt identifier mentions and the enclosing checked
HIR, then follows resolved procedure references to a fixed point capped at 256
source declarations. Each selected row carries its canonical comment-free
declaration, source-relative file, readable type, canonical constant value when
applicable, and digest. Keeping this closure separate from visible bindings is
semantic: a nested helper reached through another definition may explain that
definition without becoming a legal unqualified name at the synthesis site.
Aggregate fields remain in canonical type graphs, imports remain compact
package interfaces, and the current anchor remains in its dedicated enclosing-
declaration fields. No whole source file or process-local symbol identity is
exposed. Exceeding the bound is a compile error rather than a silently partial
request.

## Public context, opaque rounds, and dependency scheduling

Public dependency documentation is published in the early package interface,
alongside types and constants, rather than after consumers have already bound
it. Its exact explicit attachment bytes remain process-local and are never
written into the resolution manifest. Imported package and declaration anchors
survive into the provider request; denying one imported declaration redacts its
documentation and attachments together with its interface row.

The request also carries persistent package and source coordinates plus a
canonical token rendering of the enclosing procedure or type declaration.
Lexical comments are omitted by construction, while names, literals, grammar,
and the synthesis site's exact structural surroundings remain readable. The
rendering and its digest are obligation inputs; package-level declaration
synthesis explicitly has no enclosing declaration instead of receiving an
unbounded source-file dump.

An enclosing declaration also has a separate typed semantic skeleton. It is a
small length-framed contract containing declaration flags, checked type and
layout, named runtime parameters and result type, and aggregate members with
their checked offsets. It deliberately contains no body statements, comments,
or semantic arena IDs. This keeps exact source structure available without
forcing a provider to reverse-engineer basic checked facts from that source.

Interface proposals are opaque within one completeness set in the literal
request data, not only by convention. The compiler constructs all obligations
from one immutable surface compilation before invoking any provider and installs
the complete set of returned source only afterward. A resolver acceptance test
captures both same-package request binding sets, proves neither contains either
generated name, then requires their merged pins to compile provider-free.

Compile-time constant execution has one interface-discovery-only blocked state
for unresolved synthesis. A reached package procedure is body-checked through
the ordinary typed HIR path, so an expression or statement site used by a
constant or declaration-level `when` receives the same expected type, lexical
bindings, denials, enclosing declaration, and structured branch facts as a
runtime body site. A direct `when ...` condition is evaluator-owned and receives
the compiler-known `bool` expectation. These sites run in interface rounds; a
later round reevaluates the constant and may reveal another declaration/member
completeness set. Complete semantic checking retains the rejecting behavior, so
only the resolver/offline pin scheduler can cross this temporary blocked state.
Package-level conditional rounds share one persistent site namespace. When an
earlier site has already been replaced, its generated-source map reserves that
structural identity while obligations for the next round are built. The next
same-kind site under the same file and anchor receives the lowest unused
occurrence. Prompt and type content remain outside the identity and inside the
staleness digest, while a later selected site cannot alias an earlier pin.

Required type/layout integers retain their contextual TypeId alongside the
source recipe. Interface discovery runs the full constant interpreter without
diagnosing an ordinary fixed-point failure; only a recipe that actually reaches
synthesis defers the generic type-resolution diagnostic. Direct array/SIMD
counts, generic value arguments, alignment attributes, and explicitly backed
enum values therefore publish typed expression obligations. Procedure-produced
values publish the reached body site instead. While any such obligation is
pending, the package interface is withheld rather than exporting an invalid
public type, and dependency consumers resume only after a clean rebuilt round.

Structural synthesis is split at the semantic boundary it can affect. A package
declaration site remains an opaque prerequisite for every reached compile-time
body because it may introduce any package name. A member-only set is narrower:
the compiler checks reached bodies on copied semantic and constant tables. If
that ordinary check succeeds, its typed sites join the same opaque provider
round without observing any member proposal. If it fails, the copy and its
diagnostics are discarded; only member sites run, and the next clean round
either discovers the body sites or reports the authoritative body error.

Codex execution polls an embedding-owned cancellation callback alongside its
fixed deadline. Cancellation never retries: the adapter kills and reaps the
active child, emits one compiler diagnostic, and returns before its private
request directory is destroyed. The resolver also polls the same source before
each compilation, validation, provider, and final-commit boundary, so fresh-pin
or pre-provider work cannot ignore cancellation. `draftc resolve` maps SIGINT to
both callbacks; the reusable adapter installs no process-global signal handler
itself.

Every lexically enclosing `deny` region contributes its exact canonical selector
spellings in outer-to-inner order. These facts are readable policy instructions
and obligation hash inputs. Ordinary semantic denial checking still resolves
the selectors and rejects generated operations; the provider cannot waive that
compiler-owned check.

Parametric sites additionally receive the enclosing declaration's parameters in
source order, with their type/value kind, source constraint vocabulary, and
canonical type spelling and digest. Concrete body instances route back to their
template parameter contract rather than silently losing generic constraints.

Surface judgment claims guide synthesis only under the specification's
positional rule. Package claims are universal. A type/procedure claim must be
an earlier direct item of a member/statement list that structurally encloses a
later item containing the site; claims inside a sibling branch therefore do not
leak. Claim text and exact attachment bytes are context and stale-pin inputs,
but no verdict is implied and no judgment is executed during synthesis.

Expected and visible-binding types also carry deduplicated complete interface
type graphs. The labeled length-prefixed rendering includes nominal identity,
layout, members, offsets, structural children, calling convention, and generic
type/value arguments. Its definition digest is a separate obligation input from
the compact reference digest, so changing fields of a same-named nominal type
correctly stales synthesis and still gives Codex the facts it needs.

Every visible file-local import contributes a compact interface under its actual
alias and canonical package identity. The interface includes public declaration
names/kinds/flags, signatures, generic constraints, constants, native bindings,
and composed effect/return/write contracts; its referenced types reuse the
complete type graphs above. A shadowed import is absent. Interface bytes and
their digest are obligation inputs, so a dependency API or constant change
stales the site even when its expected type and enclosing source do not change.

A provider-free compile or resolution revalidation may consume a content-fresh
pin without installing its original provider. When `draft resolve` explicitly
selects a provider, however, provider, model, and complete adapter-configuration
identities must match the pin; any changed selection regenerates the expansion.

The first Codex adapter permits two attempts, each with a five-minute deadline.
That policy is part of adapter configuration identity. A timed-out child is
killed and reaped before another attempt starts, and only a successful complete
response can reach the compiler-owned resolution transaction. Configuration
also binds the exact non-followed content tree of an explicit Codex distribution
root and the launcher's root-relative canonical target. The tree is rehashed
before and after every child invocation; mutation rejects the response even
when the selected launcher file itself did not change.

## Shared Codex runtime for synthesis and judgment

Status: first synthesis and judging adapters implemented.

Both adapters use one complete `AgentObligation` renderer and one hardened
Codex child-process runtime. The shared boundary owns the explicit immutable
distribution identity, compiler-generated private filenames, output schema
identity, fixed argv, read-only isolated directory, retry deadline,
cancellation, child reaping, bounded output, and pre/post distribution checks.
Synthesis and judgment retain separate prompt contracts, output schemas,
response parsers, configured state, and provider-neutral function tables.

The judgment adapter adds the resolved-program, compiler, aggregation-policy,
validator, and requested-artifact identities after rendering the common typed
context. Artifact bytes are rehashed immediately before entering the private
request directory. Its exact two-field JSON parser accepts member order as JSON
semantics require, but rejects duplicates, unknown fields, malformed Unicode,
unknown verdicts, empty rationales, and trailing bytes. A verdict remains only
a provider response: the compiler-owned command constructs and durably records
evidence, and only the later all-pass manifest operation may select it.

## Typed branch and loop-range facts in agent obligations

Status: structured entry decisions and conservative mutable-loop ranges
implemented.

The body checker snapshots static control-flow facts at every body judgment and
synthesis site while it is already traversing structured HIR. Runtime `if`
branches contribute a typed condition with true or false polarity in
outer-to-inner order. A `switch` case contributes its typed subject and authored
matching labels. A default contributes the complete explicit label set from the
whole switch, including labels written after the default, because reaching that
body proves that none matched.

Conditional and three-clause loop bodies carry the typed condition decision
that admitted the current iteration. Array/slice iteration bodies carry the
typed iterable expression. All rows describe historical structured-control
decisions: if code mutates an operand afterward, the row does not assert that
re-evaluating the displayed source at the agent site would produce the entry
value. The provider spellings make this explicit with `*-entered-*` kinds.

The semantic rows retain only process-local syntax references and TypeIds.
Obligation construction converts them to comment-free canonical Draft source,
the readable subject type, and the same portable interface type graph used by
other provider context. Source and duplicated digests are rechecked by the
shared Codex renderer before either synthesis or judgment starts a child. The
structured entry rows remain historical rather than current-value assertions.

After HIR is complete, the separate `sema/agent_flow` pass computes current
loop-binding facts. Its state is a small ordered vector, branches merge by
intersection, and every loop repeats analysis until its backedge reaches a
monotone fixed point. Direct stores remove the affected binding. Taking its
address removes it and, for a source iteration index, suppresses the static fact
for the whole body because an escaped pointer could survive the backedge.
Consequently a site before a direct index store may retain the fact while a site
afterward does not; a mutation in one branch or nested loop removes the fact at
the join/fixed point.

Array/slice iteration proves `0 <= index < captured_len(iterable)`: MIR captures
the iterable once and resets the source index from its hidden induction slot on
every body entry. A three-clause loop proves
`0 <= index < header_value(upper)` only for the canonical
`index = 0; index < upper; index += 1` shape, only when `upper` does not depend
on `index`, and only when the body neither mutates nor exposes `index`. The two
explicit snapshot terms are provider vocabulary; neither promises that mutable
upper source re-evaluates to the displayed value at the site. Other loop shapes
produce no inferred range.

`draft-agent-obligation-v19`, synthesis request v21 / prompt v20, judgment
request/prompt v4, and compiler content v129 identities make these facts and
the compiler-checked correction policy stale-pin and evidence inputs. The
synthesis adapter uses provider identity `openai-codex-cli-v24`; the unchanged
judgment adapter remains `openai-codex-cli-v22`. Both recheck the canonical
upper-source digest before a child starts. Obligation construction also drops a
range when the binding or any resolved upper-expression input is hidden by an
active denial.

Nested procedures are static and cannot capture runtime locals. Checking a
nested body therefore clears the declaration point's active branch stack and
restores it afterward; only branches inside that procedure refine its sites.
Package and type-member sites have an empty stack and no loop ranges.

## Authenticated synthesis overlays in validation graphs

Status: implemented with an ordinary-graph authentication prerequisite.

An ordinary resolved compilation authenticates every synthesis pin against the
current typed obligation, including the canonical test and benchmark context.
A test or benchmark command then derives a separate graph that adds command-only
files, imports, entry procedures, and harness lowering. Recomputing an ordinary
site's synthesis digest inside that derived graph is not stable: validation mode
deliberately uses different context enrichment even though the handwritten site
and selected generated object are unchanged.

`compile_workspace_with_resolution` therefore authenticates the complete
ordinary graph first. Only after that succeeds may its validation-only child
compilation omit the redundant input-digest comparison while applying the same
manifest. The overlay still requires exact target, structural site identity,
grammar category, source coordinates, generated-object digest, and complete pin
coverage; the final validation graph then parses and type-checks the installed
source normally. Ordinary builds, offline replay, and the resolver continue to
require the current input digest directly.

Focused overlay coverage proves that a stale pin remains rejected in the normal
mode. A compiler integration regression constructs a declaration pin whose
generated constant is consumed by a test-only file, authenticates the ordinary
graph, and proves the derived validation graph compiles the test. Compiler
content v129 invalidates earlier resolved-program and evidence identities rather
than silently changing this trust boundary.
