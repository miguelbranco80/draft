# Validation, judgments, and evidence

This document records validation-only synthesis context, append-only evidence storage, judgment selection/publication, instrumentation requests, and resolution-time judgment scheduling.

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
evidence state is rejected instead of being silently reinterpreted by a newer
compiler. Evidence-object keys remain domain-separated by their typed formats, so
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

## Evidence and build separation

Status: provider-neutral multi-validator/artifact evidence and partial selection
are implemented; ordinary builds do not consume evidence as a prerequisite.

The provider-neutral judgment command accepts an ordered nonempty validator list
and an exact map of requested artifact bytes. It invokes every validator for
every selected site, then commits one evidence object whose aggregate passes
only when all rows pass. Validator identities are unique policy slots; provider,
model, and configuration remain separate nonempty audit identities. Artifact
kinds are unique and their content is rehashed before any invocation.

The public `judge` and resolution profile accept repeatable
`identity:model` validator slots and exact `kind:path` artifact inputs. The
legacy `--codex-model` spelling selects one `validator-0` slot. A later
failing attempt revokes the active evidence key immediately, preserving an
honest history for qualification and inspection.

`draftc build` does not require, re-run, or verify test, benchmark, or judgment
evidence. Its job is to compile the resolved program without a provider. Release
policy may inspect evidence or run validation as a separate operation, but that
policy does not alter the program accepted by the language compiler.

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

Status: the closed vocabulary, first host AddressSanitizer profile, and
fail-closed availability policy are implemented.

Validation profiles request instrumentation through typed kinds rather than
arbitrary Clang flags. Draft 1 names `address`, `lifetime`,
`undefined-operation`, `allocator-poisoning`, and `race`. Duplicate
requests are errors. Standalone validation and resolution precommit validation
use the same target-availability gate.

`draft-aarch64-macos-v5` supports exactly `address`. The native adapter adds
the standard `sanitize_address` attribute to every definition in its private
LLVM snapshot, compiles with `-fsanitize=address` and
`-fno-omit-frame-pointer`, and lets the selected host Clang driver link its
matching sanitizer runtime. The runtime and symbolizer are host tooling, not
resolution-manifest inputs.

Validation executes address-instrumented programs with
`ASAN_OPTIONS=abort_on_error=1:symbolize=0` in the same explicit process
environment as ordinary validation. Evidence records the host Clang version and
a separate instrumentation identity, so ordinary and address-instrumented
attempts cannot alias. The other four vocabulary items remain unavailable with
exact diagnostics; a requested but unavailable instrument is never silently
omitted.
