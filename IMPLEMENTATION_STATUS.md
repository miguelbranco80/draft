# Draft bootstrap compiler implementation status

Status: living completion audit for the first implementation plan.

This file records what evidence currently proves and, more importantly, what it
does not prove. Passing unit tests are evidence for the behavior they exercise;
they are not treated as proof that an entire specification section is complete.
The authoritative requirements remain the language specification and
`IMPLEMENTATION_PLAN.md`.

## Release-level assessment

The bootstrap is a substantial end-to-end compiler, but it is not yet the full
first solid release described by the plan. It can parse and semantically check a
large Draft subset, lower representative programs through MIR and LLVM, build
AArch64 macOS executables and library artifacts, link exact package assembly,
resolve every synthesis grammar category through transactional pins, and
consume those pins offline.

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

The release acceptance test is still unproved. Complete runtime/initial-core
conformance remains broader than the current representative surface. Public
judgment execution, resolution-profile selection, and provider-free locked
verification now exist, but have not been qualified against the selected
release Codex distribution. Generated-source maps now reach hermetic LLVM
operation locations and real native line tables, but pinned-toolchain
DWARF/disassembly qualification and explicit coverage/profile correlation
remain.

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
| Small, direct C++20 bootstrap with warnings, sanitizers, and tests | `AGENTS.md`, root `CMakeLists.txt`, the compiler/test targets, and a passing 48-test AddressSanitizer/UndefinedBehaviorSanitizer run | Implemented foundation | Repeat the release matrix with both supported host compilers available to release engineering. |
| One explicit AArch64 macOS target profile | `src/target/profile.*` and `draft_target_profile_tests` | Implemented for the first target | Final native tests must run on the pinned SDK/toolchain rather than only checking profile data. |
| Complete lexer, parser, semicolon insertion, and folder packages | `src/source`, `src/syntax`, `src/workspace`, parser/package/workspace tests, including deterministic binary-XOR/postfix-dereference disambiguation, one clean fixture that reaches every concrete syntax-node production, and 44 isolated malformed-production recovery fixtures spanning declarations, types, members, expressions, control flow, and assembly | Implemented Draft 1 surface | Keep the exhaustive node walk and malformed matrix synchronized with any grammar revision; semantic operator/type combinations remain a separate conformance gate. |
| Symbols, scopes, types, constants, layouts, parametrics, and `when` | `src/sema` and semantic/type/constant/interface tests; declaration-modifier boundary tests; aliases inherit closed generic-constraint membership while `distinct` wrappers retain numeric, logical, and comparison operators without joining closed generic constraints; target-owned SIMD shape validation; enum zero-value and explicit tagged-union discriminator-capacity validation; canonical typed dependent integer expressions retain arithmetic over multiple value parameters through local specialization, explicit procedure applications, unique checked inversion of one-to-one value patterns, nested nominal composition, structural alias composition, package-interface ordinals, full procedure-dependent nominal/structural value arguments, and transitive owner-evaluated layout requests across three packages | Broadly implemented | Keep dependent-layout, operator, and ABI conformance matrices synchronized with language changes; expand malformed deep-recursion coverage. |
| Canonical interfaces and transitive denials | `src/sema/interface.*`, `effect.*`, `denial.*`, artifact-summary parser/verifier, and tests; target v4 system-symbol summaries | Implemented first-release closure | Direct, returned, typed-field, interprocedural write-back, hidden-Context, and recursively higher-order flows compose at exact call sites and across package interfaces. Transitive declarations, compiler/runtime bridges, target System symbols, package assembly, and artifact-bound external audits are included. Expand malformed-contract and deep-recursion conformance coverage. |
| Complete handwritten language through HIR, MIR, LLVM, and native execution | `src/sema/body_checker.*`, `src/mir`, `src/backend`, phase tests, byte-identical LLVM proof that `docs`/`judge` have no runtime footprint, native aggregate/union/string globals, procedure pointers, static nested procedures with lexical constants/types/local generics, qualified/applied/grouped/tuple type-value aliases, exact positional-array/named-struct/single-field-raw-union literal validation with zero-filled omissions, constant/unique switch labels with enum/union exhaustiveness, IEEE-aware signed-zero duplicate detection and dead-NaN rejection, and scalar equality-chain lowering, immutable value parameters with mutation through only explicit locals or pointer/view parameters, outer generic parameters, specialization-specific local layouts, procedure-dependent explicit generic calls, and enforced no-capture semantics, discard and tuple-pattern assignment with staged stores, aligned runtime/compile-time scalar operator validation including exact mixed-untyped numeric promotion and comparisons plus non-executing type validation of dead constant operands, local/imported contextual IEEE payload rounding, recursive tuple-constant contextualization through scalar selection, destructuring, grouping-invariant bidirectional conditional/comparison inference for contextual `nil`/alternatives, and grouped/selected/aggregate-field procedure callees, exact concrete numeric matching, every integer identity and floating width exercised through one native operator matrix, independently typed integer shift counts with full-width runtime validation, and poison-free signed-minimum remainder, endian/boolean-storage/distinct conversions plus inherited logical, pointer, call, aggregate postfix, iteration, and switch operations, pointer arithmetic, parametrics, short-circuit/conditional evaluation, every loop shape, real LIFO/saved-argument `defer` execution across compile-time and runtime fallthrough/continue/break/switch/return, a 17-program real native execution matrix, and exact Darwin trap tests for invalid arithmetic, shifts, conversions, indexing, and slicing | Broadly implemented | The parser-production and scalar operator/type matrices are complete; repeat the native matrix with pinned release inputs. |
| Runtime context, entry, TLS, failures, allocator, and OS support | entry/runtime lowering, lazily attached foreign C pthreads executing ordinary Draft through the explicit Context bridge, explicit child-thread Context installation, independent initialized package `thread_local` storage across both core/thread and foreign pthreads, pthread-key-owned temporary allocation/reset/destruction, stable process argument/environment views with teardown, zero-preserving default allocate/resize, default allocator/logger/random providers, virtual-memory mappings, typed pathname open/read/write/close/remove through a package-assembly ABI shim, `core/runtime`, `core/memory`, and the real native matrix | Implemented foundation | Richer runtime/internal-state facilities and pinned-release execution coverage remain to be completed. |
| Initial core package set from specification section 7 | Every named package exists as inspectable Draft source; allocator-explicit arenas/buffers/owned strings, virtual memory, containers, OS, pthread mutex/condition synchronization, compiler-backed atomic, concurrent atomic, and validation examples check, lower, and execute in a 17-program native matrix | Implemented foundation | Expand package APIs from the representative first surface as conformance programs require them. |
| Parsed inline assembly plus package assembly | `src/assembly`, `AARCH64_ASSEMBLY_PROFILE.md`, `examples/assembly`, `examples/external-assembly`, assembly/toolchain tests | Implemented for the first profile | `draft-aarch64-apple-v2` fixes and validates the closed straight-line scalar, memory, selection, conversion, baseline NEON, and barrier grammar. Labels, branches, calls, stack changes, and unwinding intentionally remain external-file features. |
| C imports/exports and native artifacts | `src/interop`, `src/backend/foreign_inputs.*`, `foreign_summaries.*`, scalar/aggregate Darwin ABI and generated-header tests, complete concrete `c proc` signature validation (including nested and recursively typed callbacks), opaque header lowering for pointers to ordinary Draft procedures, signed `rune` header spelling, target-default and explicit narrow/wide signed/unsigned C-enum backing contracts, body-checked no-context rules for runtime assertions and ordinary Draft calls from `c proc`, the exact compiler/runtime context-bridge exception, a real generated-header/dylib/C11-client gate covering direct scalars, odd-sized and aligned integer aggregates, half-precision and recursive homogeneous floats (including unequal-lane raw-union alternatives), indirect records, raw unions, enums, and callbacks, `examples/c-interop`, `examples/c-library`, `examples/foreign-provider`, native artifact tests | Implemented for first target | All output kinds work. Built-in providers are profile-owned and have closed symbol summaries; every other logical provider maps to one exact pinned object/archive/dylib and optional exact summary, both reverified before use. |
| Provider-independent docs, judgments, and synthesis obligations | agent metadata/obligation modules, canonical expected/binding type spellings and visible constant values, a denial-filtered 256-row transitive declaration closure seeded by exact prompt mentions and resolved enclosing HIR references, independently labeled comment-free definitions for semantically relevant helpers that are not directly visible, focused relevance/staleness tests shared by synthesis and judgments, complete referenced type graphs/layouts/members, compact visible imported-package interfaces with constants and effect contracts, ordered parametric constraints, explicit target/assembly context, persistent source coordinates, comment-free canonical enclosing declaration source plus a reduced typed parameter/result/member/layout skeleton, typed outer-to-inner conditional, switch case/default, and loop-entry decisions, lexically active denial selectors, semantic-identity filtering of denied locals/declarations/import members/packages and their documentation, denial-filtered typed runtime `Context` fields even for early interface synthesis, package/enclosing/imported-public documentation and positionally guiding judgment claims with exact isolated attachments, canonical target-selected test/benchmark source plus separately checked procedure signatures, validation-state layouts, and resolved typed HIR references that cannot leak into ordinary visibility, and tests | Implemented foundation | Context still lacks mutable-state loop fixed points/range inference. |
| Dependency-ordered synthesis and opaque interface completeness sets | staged resolver/compiler passes and resolver tests, including symmetric captured-request proof that same-package sites cannot observe either generated name; early expression/statement sites reached by constants, declaration-level `when`, and type/layout integer recipes; direct synthesized `when` conditions; public layout-interface withholding; independent-versus-dependent member/body round proofs; and a real provider-free AArch64 executable that combines declaration, member, expression, and statement expansions, rebuilds byte-identically, and exits successfully | Implemented for first-profile package and same-package dependency rounds | The final locked replay still requires the pinned release toolchain/SDK. |
| Codex adapters behind provider-neutral boundaries | shared `src/elaborator/codex_cli_runtime.*`, synthesis and judgment adapters, complete common typed-context rendering, judgment-specific resolved-program/artifact inputs and strict verdict parsing, exact explicit distribution-tree plus root-relative launcher identity reverified before/after execution, fixed hashed timeout/retry policy, user/SIGINT cancellation with forced child kill-and-reap, and real process-boundary fixture tests | Implemented first adapters | Exercise both contracts against the selected release Codex distribution during release qualification. |
| Content-addressed generated source and atomic manifest commit | v4 resolution manifests, resolution/store/overlay modules, workspace-serialized commits, stale-snapshot compare-and-replace for long-running judgments, typed-history mapping for partial selected-site replacement, interrupted-publish recovery, typed native precommit Test/Benchmark execution, publicly configurable selected precommit judgment profiles, resolved-program-based judgment-row preservation/invalidation, ordered multi-validator all-pass aggregation, exact requested-artifact maps, policy-shaped locked verification, and exact passing native/judgment evidence keys/content hashes | Implemented foundation | Add alternative aggregation policies only with equally explicit transaction and verification rules. |
| Ordinary offline builds consume pins without a provider | staged offline compiler path, resolver tests, locked executable/archive adapter tests, and byte-identical repeated real builds of all five output kinds | Implemented for the first host gate | Repeat the same proof with the pinned release toolchain/SDK. |
| `draft build --locked` with no ambient external search | Versioned external-input rows, resolved-program binding, content-tree verification, explicit toolchain/SDK/provider/summary/runtime-asset CLI roots, clean process environment, absolute Clang/linker/archiver paths, provider snapshots, complete relocated file-or-directory runtime-asset verification, consumed summary verification, SDK/link flags, optional exact-key test/benchmark evidence gates, and exact per-site active judgment-evidence verification with no provider call | Implemented for every external role consumed by the first native adapter | Runtime assets are returned to embedding deployment tooling rather than assigned an unspecified CLI output layout; unsupported future external roles still fail closed. Repeat with the pinned release distribution. |
| Tests, benchmarks, judgments, and validation evidence | Typed core-nominal discovery, target-qualified file selection, canonical package/declaration order, compiler-owned isolated native harnesses, resolution precommit Test/Benchmark and selected judgment execution, private result pipe, direct process runner, process-isolated benchmark warmup/sampling, provider-neutral deterministic ordered multi-validator/all-pass judgment execution with exact artifact requests plus Codex adapter and public policy flags, stable site discovery, package/declaration/exact-site union selection, strict native and judgment evidence schemas, canonical content-addressed shared attempt storage, exact environment/tool/policy keys, append-only history, failure revocation, partial/all-pass atomic manifest selection, policy-shaped locked native and per-site judgment evidence gates, validation tests, and `examples/validation` | Implemented for tests, first benchmark profile, and extensible all-pass judgment policies | Add richer instrumentation profiles and statistical aggregation/tolerances. |
| Generated-source diagnostics/source maps | Per-pin persistent surface/expansion byte maps, composed in-memory maps, diagnostic origin notes, and hermetic LLVM subprogram/operation locations that retain both the authored site and generated coordinate | Implemented through native debug line inputs | Qualify the mapping in the pinned toolchain's DWARF/disassembly and add explicit instrumentation coverage/profile correlation. |
| Crash-safe and deterministic release verification | Atomic pin-store tests, injected stops at every staging/object-sync/manifest-visibility boundary with recovery, a real multi-process writer race over exclusive immutable-object installation and manifest replacement, serialized manifest writers and checked conditional updates, interrupted object-before-manifest recovery, rejection of redirected/non-regular/oversized store entries, strict UTF-8 manifest parsing, exhaustive truncation/NUL/invalid-byte/trailing-byte plus structural-delimiter/integer-shape mutation corpora, deterministic serializers, byte-identical repeated real builds of all five artifact kinds, and passing ordinary/sanitized 48-test suites | Implemented first-host foundation | Run the same matrix through the pinned LLVM 22.1/SDK distribution and repeat under release-scale filesystem fault infrastructure. |

## Current executable commands

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
contacting a provider.

## Next release-critical slice

The next implementation slice continues the production-by-production language
audit, then returns to the final release gates: pinned-native execution,
coverage/profile source correlation, and the judgment-provider boundary.
