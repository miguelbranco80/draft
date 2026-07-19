# Validation, judgments, and evidence

This document records validation-only synthesis context, append-only evidence
storage, judgment selection, instrumentation requests, and the strict separation
between source resolution and later validation evidence.

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

The selected validation role is a genuinely different source graph, not a
repeat pass over the ordinary graph: it adds target-qualified files and may add
imports such as `core/testing`. The compiler builds each required role once,
stores its enriched typed rows on the owning ordinary package, and retains that
state across later body-source replacement. Consequently a body expansion does
not reload or recompile validation context. This is command-local semantic
state; it is neither serialized nor a persistent cache.

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
not fabricate a qualitative verdict. The returned evidence references exist for
reporting and release tooling; they are not copied into the resolution manifest.

## Evidence and build separation

Status: provider-neutral multi-validator/artifact evidence and partial selection
are implemented; ordinary builds do not consume evidence as a prerequisite.

The provider-neutral judgment command accepts an ordered nonempty validator list
and an exact map of requested artifact bytes. It invokes every validator for
every selected site, then commits one evidence object whose aggregate passes
only when all rows pass. Validator identities are unique policy slots; provider,
model, and configuration remain separate nonempty audit identities. Artifact
kinds are unique and their content is rehashed before any invocation.

The public `judge` command accepts repeatable `identity:model` validator slots
and exact `kind:path` artifact inputs. The optional `--model` spelling selects
one ordinary `validator-0` slot; omitting both forms uses the Codex-configured
default model. A later
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

The judgment store retains the latest typed attempt even when it is a failure
that revoked active state, together with the complete immutable attempt history.
The default empty selector remains all-sites behavior. Because evidence keys
contain the resolved-program digest and exact policy inputs, evidence for one
program or policy cannot authorize another without coupling it to source
selection.

## Validation instrumentation request boundary

Status: the closed vocabulary, first host AddressSanitizer profile, and
fail-closed availability policy are implemented.

Validation profiles request instrumentation through typed kinds rather than
arbitrary Clang flags. Draft 1 names `address`, `lifetime`,
`undefined-operation`, `allocator-poisoning`, and `race`. Duplicate
requests are errors. Standalone validation uses the target-availability gate;
resolution does not accept instrumentation options or run validation.

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
