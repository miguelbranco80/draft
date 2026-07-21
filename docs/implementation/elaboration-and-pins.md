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

The resolver freezes each semantic ready set before provider work. Independent
requests in that set run through the base work graph with a provider-declared
worker bound; the Codex adapter currently permits four concurrent calls. Each
worker reads one immutable request and writes one private response/diagnostic
slot. After the entire wave joins, the resolver checks proposals sequentially in
canonical package/obligation order. Rejected sites form the next correction
wave, while accepted sites leave that wave. This overlaps provider latency
without sharing SourceManager, semantic state, or timing state with workers, and
without making sibling expansions visible inside an opaque completeness set.
Provider failures are also published in that stable canonical order, independent
of completion timing.

Usable visible names remain compact name/type/value rows. Separately, the
compiler starts from exact prompt identifier mentions and the enclosing checked
HIR, then follows resolved procedure references to a fixed point capped at 256
source declarations. A package-level judgment is the deliberate exception to
that narrow root selection: because it reviews the package rather than its
lexical source position, every source-backed declaration in the package scope
is a root. The same denial filtering, de-duplication, closure, canonical sort,
and size bound then apply. An incomplete declaration visible only during an
opaque interface round is omitted until a later round gives it a complete type.
This prevents the usual placement immediately after imports from hiding all
later declarations from the judging provider without manufacturing premature
typed context during resolution.

Each selected row carries its canonical comment-free declaration, source-
relative file, readable type, canonical constant value when applicable, and
digest. Keeping this closure separate from visible bindings is semantic: a
nested helper reached through another definition may explain that definition
without becoming a legal unqualified name at the synthesis site. Aggregate
fields remain in canonical type graphs, imports remain compact package
interfaces, and the current anchor remains in its dedicated enclosing-
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
the compiler-known `bool` expectation. A statement condition may contribute a
provisional discovery selection, but executable HIR selects it again during
source-ordered body checking, after lexical locals exist. These sites run in
interface rounds; a later round reevaluates the constant and may reveal another
declaration/member completeness set. Complete semantic checking retains the
rejecting behavior, so only the resolver/offline pin scheduler can cross this
temporary blocked state.
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
each compilation and final-commit boundary, and immediately before every queued
provider task invokes its callback. Fresh-pin work and calls still waiting in a
bounded wave therefore cannot ignore cancellation; an adapter remains
responsible for interrupting a call already in progress. `draftc resolve` maps
SIGINT to both callbacks; the reusable adapter installs no process-global signal
handler itself.

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

A provider-free compile, ordinary resolution, or revalidation may consume a
content-fresh pin without installing its original provider. Provider, model,
and adapter-configuration identities remain recorded as provenance, but they do
not participate in freshness or resolved-program identity. Regeneration is an
explicit source-changing request rather than a side effect of selecting a
different model.

Explicit regeneration is a resolver scheduling override, not a freshness input.
An empty regeneration selection forces every encountered synthesis obligation;
an exact persistent site identity forces only that obligation. Matches are
recorded across interface and body stages and checked after all dependent body
sites become visible. An unmatched identity fails before commit. The selected
proposal still crosses the generated-source grammar boundary and ordinary
semantic checks, and every unselected site follows normal fresh/stale behavior.

The resolver commits its source transaction and returns the final checked
`CompileWorkspaceResult` at semantic closure. `resolve --build` then continues
that same result through target lowering and hands it directly to the native
adapter. Plain resolve stops after the commit. This ordering makes generated
source durable even when MIR, LLVM, assembly, linking, or debug-symbol work
fails, while the graph ownership boundary ensures the combined command does not
reload the manifest or reconstruct declarations, types, HIR, or dependency
edges merely to emit the artifact.

Without a source transition, interface discovery, semantic closure, and target
lowering advance monotonically on one result rather than forming separate
workspace compilations. A checked complete-file expansion deliberately returns
the aggregate result to interface discovery while retaining exact per-package
progress. Package/import IDs remain stable. Declaration/member replacement
rebuilds the changed package and transitive consumers; body-category replacement
rebuilds declarations only for its containing package, retains consumer HIR,
and invalidates effect/obligation closure through those consumers. It also
retains already typed validation context because it cannot change declarations.

The semantic ownership boundary is explicit. A package's declaration generation
is immutable. Each checked procedure owns its HIR arena beside a body-owned
semantic copy and constant table under a work key consisting of the declaration
generation and canonical external generic demand set. No package-wide HIR copy
is retained; a package-wide consumer builds and discards an ID-rewritten view.
Proposal checking can therefore copy the command graph without replaying
BodyChecker over a dependency's already-enriched tables. Equal keys reuse the
dependency result; added demands extend only new specializations; removal starts
from declarations. Handwritten commands still load and analyze declarations
once before later MIR/LLVM continuation.

Provider proposals need isolation because sibling sites in one opaque set may
not observe each other. The resolver copies the current command-local semantic
state for each private proposal check, applies only that proposal through the
same in-memory transition, and discards the candidate afterward. Accepted
siblings are then installed together in the authoritative graph. This copy is
not a persistent cache and performs no repeated filesystem discovery. Private
compile-time body discovery also checks copied declaration tables, so a rejected
proposal or speculative member round cannot append lexical rows to the retained
surface graph.

The compiler treats the saved fragment as source inserted at its exact `...`
site while retaining the original surface buffer and a composed source map.
`draftc expand <workspace> --root <package> --out <directory>` exposes that final checked view as
ordinary files plus deterministic `.draft-map` sidecars. Indexed output-root
directories avoid treating semantic root identities as trusted host paths; a
top-level map retains each root's actual kind, import prefix, and identity.
The command refuses an existing destination and publishes a complete staging
tree with one rename, so stale or partial files cannot be mistaken for current
source. It does not change resolution or build semantics, and the durable
manifest plus content-addressed fragments remain authoritative. An IDE may
construct the same virtual view without writing it.

The first Codex adapter permits two process attempts per call, each with a
five-minute deadline, and at most four independent calls per ready wave. That
policy is part of adapter configuration identity. A timed-out child is killed
and reaped before another process attempt starts, and only a successful complete
response can reach the compiler-owned resolution transaction. Processes start
through `posix_spawnp`, which is safe while other provider workers are active.
The adapter finds `codex` through the user's `PATH`; installation bytes,
credentials, and physical paths are ambient user tooling and are neither hashed
nor serialized. An embedding may supply another executable command for
deterministic tests without exposing that plumbing in the language CLI.

`draftc` embeds the repository's complete `write-draft-code` skill as immutable
bytes. The first stale or explicitly regenerated site lazily materializes those
bytes once for the resolve command. Each Codex process still receives its own
private request directory; a compiler-created read-only view named
`draft-skill` points at the shared command-owned guide. The child reads the
skill's explicit synthesis-provider mode, which treats the typed obligation as
the complete environment and forbids repository edits, command execution, and
nested synthesis. The materialization is removed with provider state at command
exit. A provider-free resolve or build creates no skill directory.

The exact skill bundle digest is generation-policy provenance. Changing the
guide can change a future proposal, so it changes Codex configuration identity,
but it does not stale already accepted source. Physical temporary paths never
enter either provenance or semantic identity.

## Shared Codex runtime for synthesis and judgment

Status: first synthesis and judging adapters implemented.

Both adapters use one complete `AgentObligation` renderer and one hardened
Codex child-process runtime. The shared boundary owns compiler-generated private
filenames, output schema identity, fixed argv, read-only isolated directory,
retry deadline, cancellation, child reaping, and bounded output.
Synthesis and judgment retain separate prompt contracts, output schemas,
response parsers, configured state, and provider-neutral function tables.

The judgment adapter adds the resolved-program, compiler, aggregation-policy,
validator, and requested-artifact identities after rendering the common typed
context. Artifact bytes are rehashed immediately before entering the private
request directory. Its exact two-field JSON parser accepts member order as JSON
semantics require, but rejects duplicates, unknown fields, malformed Unicode,
unknown verdicts, empty rationales, and trailing bytes. A verdict remains only
a provider response: the compiler-owned command constructs and durably records
evidence in the independent evidence store. Resolution manifests never select
judgment evidence.

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

`draft-agent-obligation-v19`, synthesis request v21 / prompt v22, judgment
request/prompt v4, and compiler content v140 identities make these facts and
the compiler-checked correction policy stale-pin and evidence inputs. The
synthesis adapter uses provider identity `openai-codex-cli-v28`; the judgment
adapter uses `openai-codex-cli-v23`. Both recheck the canonical
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
content identity includes this trust boundary, so changing it invalidates prior
resolved-program and evidence identities rather than silently accepting them
under different authentication rules.
