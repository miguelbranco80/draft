# Validation, judgments, and evidence

This document records validation-only synthesis context, append-only evidence storage, judgment selection/publication, locked verification, instrumentation requests, and resolution-time judgment scheduling.

## Validation-only synthesis context

Target-selected `_test.draft` and `_bench.draft` files remain absent from the
ordinary package graph, but a package containing synthesis or judgment sites
receives a parallel syntax load with both validation roles enabled. Each such
file is rendered in canonical filename order without comments, labeled as test
or benchmark, and hashed into every obligation in that package. Once interface
synthesis is complete, the compiler also builds each selected validation role
in a separate checked workspace graph. Body synthesis and judgments then carry
the discovered procedure signatures, exact target validation-state layouts,
and portable type/value facts for every resolved HIR reference used by those
procedures. These facts remain under the validation row and never enter the
ordinary visible-name set. Declaration/member sites retain the syntax-only form
at their final boundary, keeping their earlier pins stable. Invalid validation
syntax or signatures fail resolution before body-provider execution; test-only
imports and names still cannot leak into ordinary checking.

## Shared evidence attempt storage

Status: implemented persistence foundation for native validation and judgments.

Test, benchmark, and judgment evidence use distinct canonical typed objects but
share one content-addressed attempt store. A typed codec owns serialization and
semantic validation; the store alone owns the interprocess lock, immutable
object installation, ordered attempt state, pass activation, failure
revocation, and the fsync-before-reference publication sequence. This avoids a
second durability protocol for qualitative judgments without pretending their
validator verdicts and rationales are native counter reports.

The state header is `draft-evidence-state-v1`. Changing the prior
validation-specific header also advances the compiler content identity: an old
evidence state is rejected instead of being silently reinterpreted by a locked
build. Evidence-object keys remain domain-separated by their typed formats, so
both kinds safely occupy the same `.draft/evidence` object namespace.

The provider-neutral judgment command evaluates sites in deterministic compiled
package/obligation order. It revalidates every attachment identity before the
provider sees bytes, strips process-local syntax handles, and supplies only the
resolved program plus canonical typed obligation. Each returned verdict is
persisted immediately. A semantic failure therefore revokes prior evidence even
when the aggregate command fails, while an invocation or protocol failure does
not fabricate a qualitative verdict. Only an all-pass completed aggregate may
publish its returned rows into a resolution manifest.

## Conditional judgment-manifest publication

Status: public all-sites default and fine-grained selection implemented.

`draftc judge` first compiles the complete provider-free resolved program, then
evaluates every authored judgment in canonical package/obligation order. Each
provider verdict reaches the crash-safe attempt history immediately, including
failures that revoke an exact prior key. Only a completed all-pass aggregate is
eligible for manifest selection. The default command replaces all judgment rows
because it selects all sites; synthesis pins, external inputs, and native
evidence rows are copied unchanged. Fine-grained selectors instead replace
only their selected site keys.

All resolution-manifest writers now hold an interprocess lock on the workspace
directory. Judgment publication additionally compares the currently visible
canonical manifest with the exact optional snapshot retained by compilation
while holding that same lock. A missing snapshot requires a still-missing
manifest. This prevents a slow provider response for program A from overwriting
or attaching evidence to concurrently resolved program B, while retaining the
existing staged-object/fsync/manifest-last crash protocol.

## Offline locked judgment evidence gate

Status: provider-neutral multi-validator/artifact verification, public
validator/artifact configuration, and partial selection implemented.

`draftc build --locked --require-judgment-evidence` never configures a judging
provider. It starts from the already compiled resolved graph and requires one
manifest judgment row for every current judgment obligation. Each row's store
key must be active, passing, and still point at the exact immutable attempt
selected by the manifest. The typed object must match the full stable claim and
input digests, resolved program, target, compiler, package, and active policy.

The provider-neutral command accepts an ordered nonempty validator list and an
exact map of requested artifact bytes. It invokes every validator for every
selected site even after an ordinary failing verdict, then commits one evidence
object whose aggregate passes only when all validator rows pass. Validator
identities are unique policy slots; provider, model, and configuration remain
separate exact nonempty audit identities. Artifact kinds are unique and their
content is rehashed before any invocation.

Offline verification receives the expected policy identity, validator order,
and artifact content identities. It needs no provider installation or
credentials, and rejects a selected object when any row, order, kind, or digest
differs. The public `judge` and resolution profile accept repeatable
`identity:model` validator slots and exact `kind:path` artifact inputs. The
legacy `--codex-model` spelling selects the explicit first profile: one
`validator-0`, aggregate all-pass, with no requested artifacts. Locked build
verification receives the matching ordered identities and `kind:sha256` rows;
it never reopens provider artifact files. A later failing attempt revokes the
key immediately, so an unchanged manifest that names the older pass fails
offline verification until judgment succeeds and republishes selection.

## Judgment discovery and partial selection

Status: package, declaration, and exact-site selection implemented.

The compiler exposes judgment sites in canonical compiled obligation order.
Each listing includes the unambiguous persistent `site-...` identity plus its
package, semantic anchor, source file, and occurrence. A command selector may be
that exact identity, a package path/full identity, or a package plus declaration
anchor; several selectors form a de-duplicated union. Every supplied selector
must match at least one current site before provider configuration.

Resolution v4 evidence rows intentionally avoid duplicating the site identity,
so partial replacement maps an old row through its typed evidence history. The
judgment store now retains the latest typed attempt even when it is a failure
that revoked active state. This is enough to identify and remove only selected
old rows. Missing or corrupt history fails closed. Each proposed replacement is
then reloaded and required to be an active attempt for exactly one selected
site before the stale-snapshot-safe manifest transaction begins. The default
empty selector remains all-sites behavior.

## Resolution-profile judgment scheduling

Status: selected precommit judgment profiles implemented.

`draftc resolve --judge` evaluates all current judgments only after interface
and body synthesis have produced a complete checked program and after selected
native Test and Benchmark procedures pass. Repeated `--judge-select` flags use
the same stable exact-site, package, and declaration-anchor selection rules as
the standalone command. Wholly handwritten programs follow this path too; the
absence of synthesis pins is not an early return when a judgment runner was
requested.

The resolver owns ordering and publication but not provider execution. A narrow
driver callback receives the immutable resolved compilation and returns the
complete judgment-row set it wants selected. Every attempt is already durable
at that boundary, including failed verdicts used for audit and revocation. The
callback returns success only for a completed all-pass selection, and the
resolver rejects any non-judgment row before merging the result beside freshly
produced native validation evidence. Only the existing final
object-before-manifest transaction makes those rows visible.

An ordinary resolution run carries old judgment rows forward only when the old
and new resolved-program digests are byte-identical. A selected profile may then
replace its sites while preserving those unchanged unselected rows. If source,
generated bytes, target, compiler, external inputs, or synthesis pins alter the
program digest, no old judgment row enters the candidate; partial execution can
publish only newly judged sites. This keeps qualitative evidence attached to
the exact program it evaluated without requiring a provider on routine
unchanged resolution.

## Validation instrumentation request boundary

Status: the closed vocabulary, first locked address profile, and fail-closed
availability policy are implemented.

Validation profiles request instrumentation through typed kinds rather than
ambient Clang flags. Draft 1 names `address`, `lifetime`,
`undefined-operation`, `allocator-poisoning`, and `race`. Duplicate requests are
errors. Standalone validation, resolution precommit validation, and locked
evidence verification all use the same target-availability gate.

`draft-aarch64-macos-v5` supports exactly `address`. It is locked-only: the
selected toolchain tree contains the arm64 Clang 22.1 ASan dylib with a
relocatable install name plus `llvm-symbolizer`. Both entries and their Mach-O
dependency closures are checked before the complete tree is hashed. The native
adapter adds the standard `sanitize_address` attribute to every definition in
its private LLVM snapshot, compiles with `-fsanitize=address` and
`-fno-omit-frame-pointer`, links the runtime snapshot, adds only
`@executable_path` as the runtime search path, and deploys the exact dylib beside
the harness.

Validation processes receive a complete clean environment. The address profile
adds exact `ASAN_OPTIONS=abort_on_error=1:symbolize=1` and the absolute path of
the verified symbolizer; that physical path is relocatable presentation state,
while the evidence identity names `bin/llvm-symbolizer` and the toolchain digest
pins its bytes. Evidence/key v2 has a separate instrumentation identity. Thus
ordinary, differently instrumented, and address-instrumented attempts cannot
alias. Resolution and locked build evidence requirements expose the same
`--instrument` selection.

A locked passing test/benchmark pair and a deliberate Draft heap
use-after-free qualify both sides of the profile. The latter aborts with a
symbolized logical Draft location and commits a revoked failed attempt. The
other four vocabulary items remain unavailable with exact diagnostics; a
required but unavailable instrument is never silently omitted.
