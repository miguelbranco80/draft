# Draft bootstrap compiler implementation status

Status: living completion audit for the first implementation plan.

This file records what evidence currently proves and, more importantly, what it
does not prove. Passing unit tests are evidence for the behavior they exercise;
they are not treated as proof that an entire specification section is complete.
The authoritative requirements remain the language specification and the
[bootstrap architecture](../implementation/architecture.md); the completed
sequencing is preserved in the
[historical first implementation plan](../history/first-implementation-plan.md).

## Release-level assessment

The bootstrap now implements the first solid compiler described by the plan.
It parses and semantically checks the complete Draft 1 surface, lowers complete
handwritten programs through HIR, MIR, and LLVM, builds every specified AArch64
macOS artifact kind, links exact package assembly, resolves every synthesis
grammar category through transactional pins, executes judgments and validation,
and consumes the resulting pins and evidence offline. The qualification below
is the release-candidate evidence for that implementation claim; it is not a
claim that every future language combination or core-library API already has a
dedicated regression.

Early interface scheduling now includes synthesis reached by a package constant
or declaration-level `when`. The compile-time interpreter records its blocked
procedure dependency, ordinary body checking constructs the typed site, and the
next interface round reevaluates the constant before exposing selected
declarations. Direct synthesized `when` conditions follow the same provider and
offline-replay path with a `bool` obligation. Generated-source maps reserve site
identities across rounds, preventing a later conditional site from aliasing a
previously replaced package-level site.

Type/layout integer recipes participate in those rounds as well. Arrays, SIMD
vectors, generic value arguments, alignments, and explicitly backed enum values
carry exact integer context; procedure-produced recipes selectively check their
reached bodies. A public synthesis-dependent layout withholds its package
interface until a clean round can rebuild it completely.

Member-only completeness sets now admit independently type-checkable
compile-time procedure sites in the same opaque round. A procedure that needs a
pending member waits on a discarded semantic copy and is retried after the
member overlay; package declaration sites remain prior dependencies.

The end-to-end release acceptance path is now qualified. Bundled Codex produced
declaration, member, expression, and statement fragments plus an
artifact-backed judgment. The generated member rebuilt its aggregate and
was consumed by both executable and test graphs; provider-free replay reused
all four selected objects; and the resulting program passed its generated test
and locked native build. Future runtime and initial-core API expansion remains
broader than the representative first-release surface. Generated-source maps
now reach hermetic LLVM
operation locations, real native line tables, and a canonical native
operation-correlation sidecar. Validation commands also expose a closed
diagnostic-instrument vocabulary; the first locked address profile now owns its
IR attribute, Clang options, dynamic runtime, symbolizer, clean process
environment, and evidence-v2 identity. Pinned-toolchain
DWARF/dSYM/disassembly qualification now passes for authored source. The real
provider-free synthesized native acceptance program also requires its generated
expression and statement site identities in both the canonical correlation map
and linked dSYM payload. Counter-based coverage/profile result ingestion
remains.

The exact fixture inputs and reproduction commands are documented in the
[agent acceptance fixture](agent-acceptance.md).

The selected self-contained native distribution combines LLVM/Clang 22.1.8
with Apple ld project 1267 and its colocated ld-classic project 957.1. A direct
Mach-O closure validator rejects ambient dependency paths before hashing. The
minimal content-pinned SDK contains only `libSystem.tbd`. On 2026-07-19, the
exact selected roots independently reproduced their recorded content-tree
identities, and compiler content `draft-bootstrap-cpp-v129` qualified
`draft-core-bootstrap-v2` against them. The complete 21-program native
matrix—including inline and package assembly plus the language-tour, console,
file-I/O, and denial additions—passed. Repeated builds of all five artifact
kinds were byte-for-byte identical. Exact layout, identities, assembly
procedure, and the preserved 17-program v1 baseline are recorded in
[the AArch64 macOS toolchain document](aarch64-macos-toolchain.md).

The bootstrap host-language matrix also passes independently of the native
input distribution. Apple Clang 21 and upstream Homebrew Clang 22.1.8 both
compile the complete C++20 tree with the repository warning set promoted to
errors. Under each host compiler, all 52 tests pass in an ordinary build and in
a separate AddressSanitizer/UndefinedBehaviorSanitizer build. The two complete
suites run sequentially because their black-box fixtures intentionally use
stable temporary directory names.

An isolated locked-build smoke now uses upstream Homebrew LLVM and LLD 22.1.8
with the macOS 26.5 SDK. It pins both complete selected input trees, resolves the
handwritten `examples/hello` source in an isolated workspace, builds and runs it
offline twice at the same output identity, and obtains byte-identical complete
Mach-O files (SHA-256
`d4d3153dbcc8393018468ae693234ff431617b3dbc710e8bb5b884bfd67df38b`).
This gate found and removed the Apple-driver-only `--no-xcselect` argument,
which upstream Clang 22.1 rejects. The smoke uses a temporary qualification tree
assembled from separately installed Homebrew LLVM/LLD packages; it is evidence
for the locked adapter and upstream driver, not a substitute for the selected
self-contained release distribution, the full native matrix, or live Codex
qualification. A follow-up CLI regression also closes the symlinked-parent path
found while staging that smoke: package commands canonicalize an existing
package before deriving the no-follow workspace/store root.

A second locked qualification with compiler content v121 adds pinned LLVM
`dsymutil` to that same upstream 22.1.8 tool tree. Two identical offline hello
builds both run successfully and produce byte-identical executable, canonical
source-correlation sidecar, linked DWARF payload, and dSYM plist. Their SHA-256
digests are respectively
`fb8d926d0308a6013f0049cf3f6138d52a398139e52e2785394e817dd15099eb`,
`54ae20f39819b724b6168b2c31bee4675a1899f4742f1e61658b614d7321c26a`,
`b45993b1cc5ac7661455d159facce9b97bb3018c6279bdaec9832ce987303eed`,
and `9eaaa3e3e1df1877800c431358e95fc8d842f6d3f676274b4c7aa37325babcea`.
Upstream `llvm-dwarfdump` sees logical `draft/workspace/hello/package.draft`,
`sum`/`main`, exact line/column rows, and every stable `draft.operation` label;
`llvm-objdump --dsym` interleaves those logical lines with the final AArch64
instructions. The dSYM contains only its standard plist and linked DWARF, and
neither it nor the sidecar contains the temporary qualification or user path.
This closes the upstream authored-source smoke, not qualification of the final
self-contained distribution or synthesized-source DWARF.

The production audit now also proves byte-exact compile-time string indexing
and every half-open string slice form. These operations preserve distinct
string identity, enforce the same contextual-`usize` boundary as runtime
views, and diagnose invalid constant ranges before native lowering.

Aggregate constant emission now preserves string and concrete-procedure
relocations through arbitrary array, tuple, struct, tagged-union, and raw-union
nesting in both globals and procedure-local uses. A native fixture reads and
calls those values after the backend places each relocation at its checked Draft
layout offset. Explicit tuple member types also contextualize symbolic enum and
tagged-union alternatives during constant evaluation before aggregate
conversion.

The native scalar conformance program now exercises every boolean-storage width
and both byte-order variants of every fixed integer and floating storage scalar.
Each matrix is also executed by `static_assert`, proving the same conversions
and equality rules in the compile-time evaluator before native execution.

Numeric operator conformance now instantiates every unary and binary operation
and all ten integer compound assignments at every Draft integer identity; the
four numeric compound assignments likewise run at every floating width. One
matrix executes through `static_assert` and again through launched AArch64 code.
The native pointer matrix covers equality and contextual `nil` for `^T`,
`[^]T`, `rawptr`, `cstring`, and procedure values, while the body-checker matrix
rejects operators outside the closed vocabulary for scalar, aggregate, view,
pointer, and procedure type families.

C header generation now materializes a nominal aggregate definition only when
the Darwin classifier proves that every field is C-declarable. Pointer-only
records containing Draft views remain valid opaque ABI addresses and are
rendered as `void *` without leaking an invalid `void` field into the header.
The generated-header/dylib/C11-client gate calls that exact boundary. The ABI
classifier also defensively rejects zero-length embedded arrays even though the
ordinary Draft resolver already rejects them before native validation.

Judgments now have a strict provider-neutral evidence object covering one stable
site, its complete typed-context identity, resolved program, requested artifact
hashes, validator identities, verdicts, rationales, and aggregate policy result.
The same audited crash-safe attempt store used by tests and benchmarks persists
judgment history, including exact-key failure revocation. A provider-neutral
command now executes every selected site in canonical order over its real
resolved typed obligation, runs an ordered set of independent validators,
records every returned verdict and exact requested-artifact digest, and exposes
rows only after an all-pass aggregate. Provider-free verification checks the
same explicit validator order and artifact map. A Codex judging adapter now
renders the complete shared typed obligation, adds exact resolved-program and
artifact identities, and interprets only a strict pass/fail response through
the same isolated, content-identified CLI runtime as synthesis. Public judge
and resolution commands accept repeatable validator/model slots and exact
artifact files; locked verification accepts only the matching identities and
digests. The public all-sites `judge` command selects only all-pass rows through
an atomic, stale-snapshot-safe manifest replacement. Locked builds can require
exact active judgment evidence without configuring or invoking a provider. The
public command lists stable site identities and accepts package,
declaration-anchor, exact-site, and union
selectors; partial success replaces only the selected manifest rows. Resolution
can run the same all-site or selected judgment profile after synthesis and
native validation, then publish synthesis pins and all passing evidence through
one final manifest transaction. Provider-free resolution retains judgment rows
only across an identical resolved-program digest.

Body-level agent obligations now retain typed control-flow context. Nested
`if`/`else if` paths are recorded outer-to-inner with true/false entry polarity;
`switch` cases carry their matching labels, while defaults carry the complete
explicit label exclusion set. Conditional/clause loops record the condition
decision that entered the current iteration, and array/slice loops record their
typed iterable. The rows are explicitly historical, so later mutation does not
turn them into unsound re-evaluation claims. Canonical comment-free source,
subject type graph, and readable type spelling are hashed into the obligation
and rendered through both Codex adapters. Nested static procedure bodies
correctly start a fresh path instead of inheriting the runtime branch where
their declaration appeared.

Dependent integer type/value expressions now use a canonical typed post-order
tree rather than a one-parameter special case. Arithmetic expressions survive
nested nominal substitution, explicit parametric procedure applications, and
package-interface export/import; specialization evaluates them with the same
fixed-width wrapping and trap rules as typed compile-time arithmetic. Native
coverage exercises `[N + 1]T` and `Buffer[N + 1]`, while focused tests cover
multi-parameter composition, contextual shifts, malformed trees, resource
limits, and declaration-ordinal remapping. Inference now recovers a single value
parameter through a closed set of provably one-to-one typed patterns, including
`[N + 1]T`, `[10 - N]T`, and nominal `Buffer[N + 1]`. Every candidate is
substituted and re-evaluated under the canonical wrapping rules.
Repeated/non-injective patterns such as `N + N` or `N * 2` remain explicit-only
rather than being guessed.

Explicit integer casts are preserved as typed expression nodes, including
casts whose source parameter has a different integer type. Cast operands keep
their independent context, so explicit modulo conversion remains distinct from
an out-of-range implicit contextual constant. Node types also retain exact
integer identity, preventing same-width types such as `u64` and `usize` from
becoming implicitly interchangeable.

Procedure-dependent generic layouts now retain full defining-package recipes
through nested nominal value arguments and structural type aliases, and close
transitively across package imports. Structural aliases use non-interned,
unknown-layout application placeholders but collapse to the ordinary canonical
structural TypeId after evaluation; published graph roots carry only the public
template/argument cache key. A concrete request may cross A -> B -> C without
importing either private layout procedure: C publishes its exact type graph, B
is cleanly rebuilt against the enriched interface, and the original request is
retried. The packages-generic semantic and native conformance program exercises
both nominal and structural forms of this three-package path.

Generic procedure applications also accept full procedure-dependent value
arguments. Template HIR records a typed deferred expression, while concrete
specializations evaluate the original syntax with active type/value bindings.
Focused coverage includes dependent result shapes and type-parametric layout
queries; the native cross-package program proves that only the concrete value,
not a private caller helper, reaches the callee's owner.

## Requirement audit

| Plan requirement | Current evidence | Assessment | Remaining proof or work |
| --- | --- | --- | --- |
| Small, direct C++20 bootstrap with warnings, sanitizers, and tests | `AGENTS.md`, root `CMakeLists.txt`, the compiler/test targets, and passing ordinary plus AddressSanitizer/UndefinedBehaviorSanitizer 52-test runs under both Apple Clang 21 and upstream Clang 22.1.8 with warnings as errors | Implemented first-release host matrix | Repeat the matrix for each release candidate and newly supported host compiler. |
| One explicit AArch64 macOS target profile | `src/target/profile.*`, `draft_target_profile_tests`, the locked `examples/agent-acceptance` executable, and the complete native matrix built with the selected LLVM 22.1.8, Apple ld/ld-classic, and minimal SDK content trees | Implemented and qualification-tested for the first target | Publish the selected trees by content identity and repeat the matrix for release candidates. |
| Complete lexer, parser, semicolon insertion, and folder packages | `src/source`, `src/syntax`, `src/workspace`, parser/package/workspace tests, including deterministic binary-XOR/postfix-dereference disambiguation, one clean fixture that reaches every concrete syntax-node production, 44 isolated malformed-production recovery fixtures spanning declarations, types, members, expressions, control flow, and assembly, a shared parser nesting budget exercised by 4096-deep expression, type, statement, and declaration inputs, and an independent 256-level acyclic package-import bound | Implemented Draft 1 surface | Keep the exhaustive node walk and malformed matrix synchronized with any grammar revision; semantic operator/type combinations remain separate conformance gates. |
| Symbols, scopes, types, constants, layouts, parametrics, and `when` | `src/sema` and semantic/type/constant/interface tests; declaration-modifier boundary tests; 256-declaration forward alias/ambiguous-constant and 256-binding compile-time constant dependency limits; aliases inherit closed generic-constraint membership while `distinct` wrappers retain numeric, logical, and comparison operators without joining closed generic constraints; target-owned SIMD shape validation; enum zero-value and explicit tagged-union discriminator-capacity validation; canonical typed dependent integer expressions retain arithmetic over multiple value parameters through local specialization, explicit procedure applications, unique checked inversion of one-to-one value patterns, nested nominal composition, structural alias composition, package-interface ordinals, full procedure-dependent nominal/structural value arguments, and transitive owner-evaluated layout requests across three packages | Implemented Draft 1 semantic core | Keep dependent-layout, operator, ABI, and semantic-graph conformance matrices synchronized with language changes. |
| Canonical interfaces and transitive denials | `src/sema/interface.*`, `effect.*`, `denial.*`, artifact-summary parser/verifier, and tests; target v4 system-symbol summaries | Implemented first-release closure | Direct, returned, typed-field, interprocedural write-back, hidden-Context, and recursively higher-order flows compose at exact call sites and across package interfaces. Transitive declarations, compiler/runtime bridges, target System symbols, package assembly, and artifact-bound external audits are included. Expand malformed-contract and deep-recursion conformance coverage. |
| Complete handwritten language through HIR, MIR, LLVM, and native execution | `src/sema/body_checker.*`, `src/mir`, `src/backend`, phase tests, byte-identical LLVM proof that `docs`/`judge` have no runtime footprint, one-declaration LLVM folding for repeated file-local proxies of the same imported symbol, native aggregate/union/string globals, procedure pointers, static nested procedures with lexical constants/types/local generics, qualified/applied/grouped/tuple type-value aliases, exact positional-array/named-struct/single-field-raw-union literal validation with zero-filled omissions, constant/unique switch labels with enum/union exhaustiveness, IEEE-aware signed-zero duplicate detection and dead-NaN rejection, and scalar equality-chain lowering, immutable value parameters with mutation through only explicit locals or pointer/view parameters, outer generic parameters, specialization-specific local layouts, procedure-dependent explicit generic calls, and enforced no-capture semantics, discard and tuple-pattern assignment with staged stores, aligned runtime/compile-time scalar operator validation including exact mixed-untyped numeric promotion and comparisons plus non-executing type validation of dead constant operands, local/imported contextual IEEE payload rounding, recursive tuple-constant contextualization through scalar selection, destructuring, grouping-invariant bidirectional conditional/comparison inference for contextual `nil`/alternatives, and grouped/selected/aggregate-field procedure callees, exact concrete numeric matching, every integer identity and floating width exercised through one native operator matrix, independently typed integer shift counts with full-width runtime validation, and poison-free signed-minimum remainder, endian/boolean-storage/distinct conversions plus inherited logical, pointer, call, aggregate postfix, iteration, and switch operations, pointer arithmetic, parametrics, short-circuit/conditional evaluation, every loop shape, real LIFO/saved-argument `defer` execution across compile-time and runtime fallthrough/continue/break/switch/return, the current 21-program selected-root locked and ordinary/sanitizer-hosted native matrices, and exact Darwin trap tests for invalid arithmetic, shifts, conversions, indexing, and slicing | Implemented first-release language pipeline | Keep the production and native conformance matrices synchronized with language changes; expand malformed deep-recursion coverage. |
| Runtime context, entry, TLS, failures, allocator, and OS support | entry/runtime lowering, lazily attached foreign C pthreads executing ordinary Draft through the explicit Context bridge, explicit child-thread Context installation, independent initialized package `thread_local` storage across both core/thread and foreign pthreads, pthread-key-owned temporary allocation/reset/destruction, stable process argument/environment views with teardown, zero-preserving default allocate/resize, default allocator/logger/random providers, virtual-memory mappings, typed pathname open/read/write/close/remove through a package-assembly ABI shim, `core/runtime`, `core/memory`, and the real locked native matrix | Implemented foundation | Expand richer runtime/internal-state facilities as the initial-core surface grows. |
| Initial core package set from specification section 7 | Every named package exists as inspectable Draft source; allocator-explicit arenas/buffers/owned strings, virtual memory, containers, allocation-free integer formatting, checked console output, OS, pthread mutex/condition synchronization, compiler-backed atomic, concurrent atomic, and validation examples check, lower, and execute in the current 21-program native matrix | Implemented foundation; v2 selected-root qualified | Expand package APIs from the representative first surface as conformance programs require them. |
| Parsed inline assembly plus package assembly | `src/assembly`, [the AArch64 assembly profile](../targets/aarch64-macos-assembly.md), `examples/assembly`, `examples/external-assembly`, assembly/toolchain tests | Implemented for the first profile | `draft-aarch64-apple-v2` fixes and validates the closed straight-line scalar, memory, selection, conversion, baseline NEON, and barrier grammar. Labels, branches, calls, stack changes, and unwinding intentionally remain external-file features. |
| C imports/exports and native artifacts | `src/interop`, `src/backend/foreign_inputs.*`, `foreign_summaries.*`, scalar/aggregate Darwin ABI and generated-header tests, complete concrete `c proc` signature validation (including nested and recursively typed callbacks), opaque header lowering for pointers to ordinary Draft procedures, signed `rune` header spelling, target-default and explicit narrow/wide signed/unsigned C-enum backing contracts, body-checked no-context rules for runtime assertions and ordinary Draft calls from `c proc`, the exact compiler/runtime context-bridge exception, a real generated-header/dylib/C11-client gate covering direct scalars, odd-sized and aligned integer aggregates, half-precision and recursive homogeneous floats (including unequal-lane raw-union alternatives), indirect records, raw unions, enums, and callbacks, `examples/c-interop`, `examples/c-library`, `examples/foreign-provider`, native artifact tests, and deterministic path-stable dSYM companions for final links | Implemented for first target | All output kinds work. Built-in providers are profile-owned and have closed symbol summaries; every other logical provider maps to one exact pinned object/archive/dylib and optional exact summary, both reverified before use. |
| Provider-independent docs, judgments, and synthesis obligations | agent metadata/obligation modules, canonical expected/binding type spellings and visible constant values, a denial-filtered 256-row transitive declaration closure seeded by exact prompt mentions and resolved enclosing HIR references, independently labeled comment-free definitions for semantically relevant helpers that are not directly visible, focused relevance/staleness tests shared by synthesis and judgments, complete referenced type graphs/layouts/members, compact visible imported-package interfaces with constants and effect contracts, ordered parametric constraints, explicit target/assembly context, persistent source coordinates, comment-free canonical enclosing declaration source plus a reduced typed parameter/result/member/layout skeleton, typed outer-to-inner conditional, switch case/default, and loop-entry decisions, monotone post-HIR mutable-state loop fixed points with intersection at branches/backedges, reset-aware array/slice iteration-index ranges, conservative canonical clause induction ranges, store/address-escape invalidation, lexically active denial selectors, semantic-identity filtering of denied locals/declarations/import members/packages and their documentation, denial-filtered typed runtime `Context` fields even for early interface synthesis, package/enclosing/imported-public documentation and positionally guiding judgment claims with exact isolated attachments, canonical target-selected test/benchmark source plus separately checked procedure signatures, validation-state layouts, and resolved typed HIR references that cannot leak into ordinary visibility, focused tests, and the real `examples/agent-acceptance` Codex transcript/evidence boundary | Implemented foundation and real-provider qualified for the acceptance graph | Repeat the complete context contract against the selected release Codex distribution and broaden adversarial context fixtures. |
| Dependency-ordered synthesis and opaque interface completeness sets | staged resolver/compiler passes and resolver tests, including symmetric captured-request proof that same-package sites cannot observe either generated name; early expression/statement sites reached by constants, declaration-level `when`, and type/layout integer recipes; direct synthesized `when` conditions; public layout-interface withholding; independent-versus-dependent member/body round proofs; and `examples/agent-acceptance`, where bundled Codex produced declaration/member/expression/statement fragments, the generated member rebuilt an aggregate consumed by executable and test graphs, provider-free replay reused all four pins, and the selected locked executable exited successfully | Implemented for first-profile package and same-package dependency rounds; every synthesis grammar category has real-provider acceptance | Repeat this proof against the selected release Codex distribution. |
| Codex adapters behind provider-neutral boundaries | shared `src/elaborator/codex_cli_runtime.*`, synthesis and judgment adapters, complete common typed-context rendering, judgment-specific resolved-program/artifact inputs and strict verdict parsing, exact explicit distribution-tree plus root-relative launcher identity reverified before/after execution, fixed hashed process timeout/retry policy, compiler-owned bounded proposal retries with exact rejected-source/diagnostic correction context, user/SIGINT cancellation with forced child kill-and-reap, real process-boundary fixture tests, and successful synthesis plus artifact-backed judgment through the Codex executable bundled with ChatGPT using `gpt-5.6-sol` | Implemented first adapters and exercised across the real process boundary | Repeat both contracts against the selected release Codex distribution; the bundled application distribution is a qualification candidate, not the compiler release package. |
| Content-addressed generated source and atomic manifest commit | v4 resolution manifests, resolution/store/overlay modules, workspace-serialized commits, stale-snapshot compare-and-replace for long-running judgments, typed-history mapping for partial selected-site replacement, interrupted-publish recovery, typed native precommit Test/Benchmark execution, publicly configurable selected precommit judgment profiles, resolved-program-based judgment-row preservation/invalidation, ordered multi-validator all-pass aggregation, exact requested-artifact maps, policy-shaped locked verification, and exact passing native/judgment evidence keys/content hashes | Implemented foundation | Add alternative aggregation policies only with equally explicit transaction and verification rules. |
| Ordinary offline builds consume pins without a provider | staged offline compiler path, resolver tests, locked executable/archive adapter tests, byte-identical repeated selected-root builds of all five output kinds, and provider-free reuse of all four real Codex pins in `examples/agent-acceptance` | Implemented for the selected first-target distribution and locked acceptance executable | Preserve this proof for every published release candidate. |
| `draft build --locked` with no ambient external search | Versioned external-input rows, resolved-program binding, content-tree verification, recursive thin-AArch64 Mach-O load-closure validation, canonical existing-package/workspace roots before no-follow store access, explicit toolchain/SDK/provider/summary/runtime-asset CLI roots, clean process environment, absolute Clang/linker/helper/archiver/dsymutil paths, provider snapshots, complete relocated file-or-directory runtime-asset verification, consumed summary verification, SDK/link flags, optional exact-key test/benchmark evidence gates, exact per-site active judgment-evidence verification with no provider call, and a real acceptance build requiring both selected evidence kinds under the selected LLVM 22.1.8, Apple linker, and minimal SDK trees | Implemented for every external role consumed by the first native adapter; locked acceptance and complete native matrices passed | Runtime assets are returned to embedding deployment tooling rather than assigned an unspecified CLI output layout; unsupported future external roles still fail closed. Add a trustworthy content-addressed snapshot/cache so cold tree re-verification is not needlessly expensive. |
| Tests, benchmarks, judgments, and validation evidence | Typed core-nominal discovery, target-qualified file selection, canonical package/declaration order, compiler-owned isolated native harnesses, resolution precommit Test/Benchmark and selected judgment execution, private result pipe, direct process runner, process-isolated benchmark warmup/sampling, closed typed address/lifetime/undefined-operation/allocator-poisoning/race instrumentation requests with duplicate and target-availability gates before compilation, a real locked address profile with private-IR function attributes, fixed Clang options, pinned relocatable runtime/symbolizer, clean execution environment, evidence-v2 identity, passing test/benchmark evidence, and a symbolized heap-use-after-free revocation, provider-neutral deterministic ordered multi-validator/all-pass judgment execution with exact artifact requests plus Codex adapter and public policy flags, stable site discovery, package/declaration/exact-site union selection, strict native and judgment evidence schemas, canonical content-addressed shared attempt storage, exact environment/tool/policy keys, append-only history, failure revocation, partial/all-pass atomic manifest selection, policy-shaped locked native and per-site judgment evidence gates, validation tests, `examples/validation`, and acceptance evidence where the default judgment failed closed before the exact imported source artifact produced a passing selected row | Implemented for uninstrumented tests, first benchmark profile, first locked address profile, explicit unavailable-instrument diagnostics, and extensible all-pass judgment policies | Add the remaining requested instrumentation kinds and statistical aggregation/tolerances only with equally explicit identities. |
| Generated-source diagnostics/source maps | Per-pin persistent surface/expansion byte maps, composed in-memory maps, diagnostic origin notes, hermetic LLVM subprogram/operation locations, canonical `draft-source-correlation-v1` native sidecars that bind every source-addressable emitted MIR operation to generated/authored coordinates and stable synthesis identity without physical checkout paths, standard dSYM publication, an upstream LLVM 22.1 authored-source DWARF/disassembly smoke, the selected-root locked `examples/agent-acceptance` v129 sidecar carrying its real expression and statement synthesis identities, and symbolized address-profile diagnostics using logical Draft filenames | Implemented through native debug line inputs, correlation output, and the first runtime instrumentation consumer; real synthesized locked output qualified | Add counter-based coverage/profile ingestion through versioned validation profiles. |
| Crash-safe and deterministic release verification | Atomic pin-store tests, injected stops at every staging/object-sync/manifest-visibility boundary with recovery, a real multi-process writer race over exclusive immutable-object installation and manifest replacement, serialized manifest writers and checked conditional updates, interrupted object-before-manifest recovery, rejection of redirected/non-regular/oversized store entries, strict UTF-8 manifest parsing, exhaustive truncation/NUL/invalid-byte/trailing-byte plus structural-delimiter/integer-shape mutation corpora, bounded 4096-deep recursive-syntax corpora, 320-node acyclic package-import, declaration, and compile-time constant dependency corpora, deterministic serializers, byte-identical repeated selected-root builds of all five artifact kinds, and passing ordinary/sanitized suites | Implemented first-release verification | Repeat under release-scale filesystem fault infrastructure and with each supported host compiler. |

## Current executable commands

The production audit now implements the specified runtime-assertion build
mode. `--assertions=off` removes the condition and message before either operand
is lowered, is bound into `draft.resolved-program.v4` under compiler content
v129, and is accepted consistently by `build`, `resolve`, and `judge`. Test and
benchmark validation always overrides this release choice to assertions on and
uses its own exact validation-program digest.

The driver currently exposes `lex`, `syntax`, `target`, `check`, `emit-llvm`,
`emit-c-header`, ordinary and locked `build` for executable, object, static
library, dynamic library, and assembly-bundle kinds, `test`, `bench --verify`,
`resolve`, and the public provider-backed `judge` command. Resolution profiles
can schedule selected judgments before publication. Test and benchmark commands
compile their otherwise excluded files into a compiler-owned AArch64 macOS
harness, execute selected procedures in canonical order, and append immutable
evidence attempts under `.draft/evidence`. Locked builds may require matching
active evidence with `--require-test-evidence` and
`--require-benchmark-evidence`, plus all current judgments with
`--require-judgment-evidence`; they verify it without executing validation or
contacting a provider. Release builds and the manifest-producing/consuming
agent commands also accept `--assertions=off`.

## Post-release work

The implementation plan is closed. Release engineering still needs to publish
the already selected native and Codex distributions by their recorded content
identities and repeat the qualification for each release candidate. Additional
validation profiles, counter-based coverage ingestion, broader core APIs, and
more target profiles are extensions described by the plan as work after the
first release.

Cold locked builds deliberately re-read every selected external-input byte. A
persistent same-user metadata cache cannot prove that mutable macOS files are
unchanged; replacing this verification requires a genuinely immutable or
authenticated distribution store, not an mtime shortcut inside the compiler.
