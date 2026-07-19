# Bootstrap compiler implementation decisions

This file records choices that are observable, potentially specification-worthy,
or easy to lose inside implementation details. It is not a second language
specification. A choice that affects Draft semantics should eventually be
confirmed in or removed by the normative specification.

## End-of-line `^`

Status: provisional; needs specification confirmation.

Draft uses `^` both as binary XOR and as postfix pointer dereference. The
semicolon rule inserts after postfix `^` but not after an operator that requires
a following operand. The token stream alone cannot distinguish these programs:

```draft
pointer^
next_statement()
```

```draft
left ^
right
```

The bootstrap lexer treats `^` at a semicolon-eligible newline or EOF as postfix,
following the specification's explicit statement-ending list. During parsing,
`^` is binary when the next token directly begins a primary operand, synthesis,
assembly, denial expression, or unary-only `!`/`~` operand. It is postfix before
a delimiter, postfix continuation, or another binary operator. Thus `left ^
right` works inside parentheses (where no semicolon is inserted), `pointer^ + 1`
dereferences before addition, and an ambiguous unary operand is made explicit as
`left ^ (-right)`. This rule is deterministic and whitespace-independent, but
the normative specification should state the disambiguation directly.

## Contextual `c`

Status: implementation representation; no intended semantic change.

The lexer records `c` distinctly because it introduces a C procedure calling
convention. The parser also accepts that token wherever an ordinary contextual
name is required, because the specification deliberately uses `c` as the local
alias in `import core/c as c` and in qualified names such as `c.int`. A line
ending in the alias therefore receives ordinary identifier semicolon behavior.

## Initial AArch64 macOS profile

Status: bootstrap target contract; versioned as `draft-aarch64-macos-v5`.

The first profile targets `arm64-apple-macosx14.0.0`, uses the generic AArch64
CPU with baseline NEON, 64-bit little-endian pointers, 16 KiB pages, Mach-O,
position-independent small-model code, general-dynamic TLS, and a macOS 14.0
deployment floor. Its pinned LLVM data-layout string is:

```text
e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32
```

The profile explicitly names its baseline NEON SIMD shapes: 64-bit and 128-bit
vectors with at least two lanes over `i8/u8`, `i16/u16`, `i32/u32`,
`i64/u64`, `f16`, `f32`, or `f64` where the total width matches. A different
lane count or element spelling is a semantic target error, not an LLVM fallback.

The parsed inline-assembly dialect identity is `draft-aarch64-apple-v2`; its
closed register, operand, addressing, condition, and instruction grammar is
enumerated in [the AArch64 assembly profile](../targets/aarch64-macos-assembly.md).
Package `.s`, `.S`, and `.asm` inputs all contain exact non-preprocessed bytes;
in particular, `.S` does not inherit the host C driver's preprocessing rule.
Changing any of these facts creates a new target-profile identity rather than
silently changing the meaning of the existing profile.

## Recursive implementation resource limits

Status: bootstrap crash-safety contract; not a Draft language limit.

The direct recursive implementation has separate budgets for graphs that are
independent in the language. Parsed declarations, members, types, expressions,
and statements share a 512-level syntax-nesting budget. After parsing, an
acyclic package-import chain is limited to 256 loaded levels, and the type
resolver independently limits forward declaration dependencies to 256 active
declarations. Compile-time constant bindings have their own 256-binding
dependency budget; compile-time procedure calls retain the separate 64-call
recursion budget plus the existing execution-step and value-size limits.

These guards report stable source diagnostics before the host C++ stack becomes
the accidental limit. Cycle diagnostics remain distinct: each graph checks its
visited or active state before applying the acyclic-depth bound. Structural
walks over HIR, MIR, interface types, effects, and constants either follow the
already bounded syntax/type shape or install a visited/cycle row before
following children.

## Parsed assembly staging

Status: implementation sequence; Draft 1 scope is unchanged.

The bootstrap front end preserves assembly statements, expressions, typed
operands, synthesis sites, source ranges, effects, and denial interactions from
the beginning. Target-independent MIR originally rejected an assembly region
until ordinary Draft MIR had a working AArch64 macOS emission path. That staging
boundary has now been crossed: language-owned directives are structural syntax,
the `draft-aarch64-apple-v2` analyzer validates fixed general, scalar FP, and
fixed-vector registers; scaled, unscaled, paired, narrow, and ordered memory
operations; the closed integer, selection, conversion, scalar FP, baseline NEON,
and barrier vocabulary; and register/flags/memory declarations. It also treats
condition flags as local dataflow so a select cannot consume ambient flags. MIR
lowers accepted regions as volatile assembly. Instructions outside that profile,
lane selection, address writeback, labels, branches, calls, stack changes, and
unwinding remain external-file features rather than implicit assembler syntax.

Package assembly follows the separate file contract in section 3. The compiler
copies every selected file's exact bytes into the compiled package snapshot;
the later native adapter never rereads the workspace. It writes those bytes to
the isolated build directory, passes `-x assembler` for all three extensions,
and appends each resulting object after its package LLVM object in canonical
package and filename order. The explicit language selection is essential for
`.S`: Draft's target profile specifies no preprocessing, independently of
Clang's conventional filename behavior. Symbols cross this boundary only
through `foreign` and `export` C-ABI declarations.

## Relocatable aggregate constants

Status: bootstrap backend representation; language layout is unchanged.

Strings and concrete procedure identities contain linker relocations and cannot
be flattened into the byte-array storage used for unions. An array, tuple,
struct, tagged union, or raw union constant may contain those values at any
nesting depth.
The LLVM backend walks the checked Draft layout, writes every non-relocatable
subtree as exact bytes, and retains each string or procedure leaf as a typed
field at its semantic byte offset. The allocation uses an initializer-specific
packed LLVM type; opaque pointers let all reads and external declarations keep
using the canonical Draft type. This representation changes neither size,
alignment, offsets, nor the public ABI and avoids imposing an LLVM aggregate
shape on the language type system. Global initializers use that storage
directly. A relocation-bearing MIR constant receives equivalent private module
storage and a canonical typed load at its source operation, because LLVM SSA
literals cannot carry the initializer-specific packed type.

## Initial locked native input contract

Status: bootstrap build contract; versioned by the resolution and content-tree
formats.

The first locked compiler-owned artifact seam accepts exactly two external
inputs: one native toolchain tree named `llvm-aarch64-macos` and one SDK tree
named `macos-sdk`. The toolchain exposes executable `bin/clang`, `bin/ld`,
`bin/ld-classic`, `bin/llvm-ar`, and `bin/dsymutil`; Clang is the manifest entry
point and the linker/helper/archiver/debug-linker locations are fixed by this
adapter version. Apple ld delegates relocatable object links to its colocated
classic helper, so both linker programs are required even though Clang invokes
only `bin/ld` directly.
Both complete trees are hashed as sorted relative path records including file
kind, permission bits, exact regular-file bytes, and exact internal symlink
spelling. Physical root paths are excluded. Absolute or escaping symlinks,
root symlinks, and special files are rejected.

Hashing alone cannot close a toolchain whose executable retains an ambient
Homebrew or developer-directory dylib path. Before the tree is hashed, a direct
load-command parser requires every tool and recursively loaded dylib to be a
thin AArch64 Mach-O image. Non-system loads must resolve exactly once through a
relocatable runpath to a regular file inside the selected root. Dylib IDs and
runpaths obey the same rule. Only explicit `/usr/lib/...` and
`/System/Library/...` dependencies may leave the tree.

Locked invocation uses the verified absolute Clang and linker paths, explicit
`-isysroot`, `--no-default-config`, the target deployment
floor, and the linker's content-derived Mach-O UUID. Current macOS requires the
UUID load command for executable launch; a real integration gate proves two
complete links to the same explicit output identity are byte-for-byte equal.
The child environment contains fixed `LANG`, `LC_ALL`, `HOME`, and `TMPDIR`
plus an empty `PATH`; it inherits no SDK, compiler, header, library, deployment,
or package search variables. Other manifest external-input roles fail closed
until their
artifact-to-command mapping exists. Ordinary development builds retain the
separate explicit host-toolchain escape hatch.

The adapter deliberately does not pass Apple's private `--no-xcselect` driver
option: upstream LLVM 22.1 rejects it. An absolute `-isysroot`, absolute
`--ld-path`, empty `PATH`, and scrubbed SDK/deployment environment close the
same discovery paths while remaining compatible with the pinned upstream
driver.

Runtime assets use a separate complete-set mapping from a nonempty logical name
to one absolute real file or directory root. Resolution records a
`runtime-asset` row with the existing portable content-tree digest; later native
build, test, and benchmark invocations must supply every row and may relocate
unchanged content. A runtime asset is not a link input, so the adapter never
passes it to Clang or guesses a location beside the produced artifact. Instead,
the reusable native API returns the verified canonical roots to its embedding
build/deployment layer. Root symlinks, escaping internal symlinks, special files,
duplicates, missing rows, extra mappings, and content changes all fail before a
compiler process starts.

Resolution manifests accept raw non-ASCII JSON string contents only as valid,
shortest-form UTF-8 scalar encodings. Continuation leads, overlong sequences,
surrogates, and values above U+10FFFF fail before any identity is installed.
The deterministic malformed corpus truncates at every byte boundary, replaces
every byte with NUL and `0xff`, and tries every possible trailing byte under the
ordinary and sanitizer suites.

The target profile maps the logical `darwin` and `libc` providers to the SDK's
explicit `System` library. `draft_runtime` is owned by the root LLVM module and
`package_assembly` is owned by captured package assembly; none can be remapped.
The v4 target also carries a closed, symbol-level denial-summary table for the
System calls used by the first core distribution. Most rows have no callback;
`pthread_create` identifies its start routine as flow-through parameter two.
Provider names alone never confer trust: an unlisted System symbol remains an
unknown edge, while every package-assembly call contributes the `asm` effect.
Every other foreign provider requires one command-line object, archive, or
shared-library mapping. Resolution stores the provider name, artifact role, and
content-tree digest without its physical path. Offline builds require the same
complete mapping, re-hash relocated bytes, reject unused mappings, and reject
unknown providers before invoking Clang. A locked link first copies each
verified artifact into the isolated build directory and re-hashes that snapshot,
so the linker never consumes a mutable workspace path after verification.

Procedure effect interfaces retain path-shaped procedure flow slots. Calls,
ordinary local and typed-field copies, hidden-Context provider fields, and
cross-package interface composition substitute finite named targets at the
exact call site. Transitive declaration calls are effects as well, so denying a
declaration reaches through helpers and procedure-valued fields. This is a
semantic may-target set: all source assignments are unioned, and optimization
is never allowed to narrow it. Returned procedure leaves carry either their
factory-input slots or a flattened imported call contract, keeping those two
parameter scopes distinct across package interfaces. Typed pointer-field writes
carry the same value contracts back through local or imported callees; a signed
address/dereference balance prevents writes to an addressed local copy from
being misreported as caller mutation. Body replay makes chained write-through
helpers independent of declaration order. Higher-order FlowCall rows recursively
retain the typed arguments supplied to the callback, including their own finite
procedure contracts, and the same shape crosses package interfaces. Callees and
arguments are snapshotted in source evaluation order before a call's write-back
becomes visible, so later argument effects cannot rewrite earlier values.

External artifact summaries use the strict
`draft-provider-denial-summary-v1` line format documented in section 12. The
summary declares the exact canonical artifact digest; the manifest separately
pins the summary bytes under the same logical provider name. Compilation keeps
the parsed audit and its digest in the compiled result, allowing the native
adapter to reject a manifest summary row that semantic checking did not consume.
An omitted symbol is unknown, and compiler-, package-assembly-, and target-owned
providers cannot be overridden by an external audit.

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

## Native artifact ownership and visibility

Status: first AArch64 macOS artifact contract.

The root LLVM module owns runtime support for every final native artifact, but
only executable compilation adds the hosted C `main`. This lets an exported C
wrapper use `runtime.default_context` from an object, archive, or dylib without
requiring a fake Draft entry procedure. Ordinary Draft procedures and globals
have hidden Mach-O visibility; only explicit C exports retain default visibility.

Object output performs a relocatable link over all package and package-assembly
objects. Static output always uses deterministic archive metadata: Apple
`libtool -static -D` for opted-in host builds and pinned `llvm-ar rcsD` for
locked builds. Dynamic output fixes an `@rpath/<filename>` install name.
Assembly output is a directory
bundle with one compiler-produced source per package and exact copied external
assembly inputs, avoiding local-label collisions that concatenation could create.
Generated C headers cover root-package exports and transitively required C
records, raw unions, enums, fixed-array fields, and callback types. Layout
assertions make size, alignment, and field-offset disagreement a C compile error.

An `@repr(C)` enum without an explicit backing follows Apple Clang's default
enum rule for this target instead of Draft's smallest-fitting rule. Its backing
is at least 32 bits: a wholly nonnegative member set uses `u32`, a set containing
a negative member uses `i32`, and either widens with the same signedness to 64
bits when required. Values that do not fit `u64` or `i64` are rejected. The
generated header exposes the selected fixed-width typedef, so C and Draft agree
on both the physical ABI and the backing value domain. An explicit backing
continues to use the separately validated fixed-backing C-enum contract. Enum
macros use the `<stdint.h>` exact-width constant forms through 64 bits; 128-bit
values are assembled from two `UINT64_C` halves, avoiding out-of-range decimal
tokens and the nonexistent `INT128_C` facility.

## Initial hosted runtime context layout

Status: bootstrap runtime ABI; synchronized with `core/runtime` by tests.

The AArch64 macOS root Context is 96 bytes with 8-byte alignment. Its fields
begin at offsets 0, 16, 32, 40, 56, 72, 80, and 88, in the source order declared
by `core/runtime.Context`. Allocator, logger, and random-generator provider
records each contain a procedure pointer and a provider-state pointer. The
assertion callback is an ordinary Draft procedure pointer, so its physical call
prepends the active Context pointer.

Only the executable root module defines runtime failure helpers and root process
state. Dependency modules reference those hidden link-unit symbols. This gives
all ordinary calls one coherent Context and prevents per-package runtime state
from emerging as a bootstrap artifact. Changing this layout or helper contract
requires a new runtime ABI and core distribution identity.

`context` is a predeclared, addressable value in every ordinary Draft procedure.
When `core/runtime` is imported, its type is exactly the public
`runtime.Context`; otherwise the compiler uses a private ABI-identical nominal
type. A lexical block that assigns a Context field or takes the address of one
starts with a complete copy of the surrounding Context. Calls in that block use
the copy, and leaving the block restores the surrounding pointer. A `c proc`
has no implicit Context and may not name `context`.

Two compiler-owned bridges cover that C boundary. `runtime.default_context`
lazily initializes Draft TLS from the process-default Context and returns the
calling thread's snapshot through the Darwin indirect aggregate-result
convention. `runtime.call_with_context` statically checks a non-nil `^Context`,
an ordinary Draft callback, and the callback's exact arguments, initializes
Draft TLS when entered from a foreign-created thread, then lowers directly to
that callback with the explicit hidden Context pointer. Named callbacks retain
their ordinary effect summaries; indirect callbacks remain unknown edges.
These are narrow versioned runtime exceptions, not permission to use Context in
arbitrary C signatures. The supplied pointer remains dynamic-call state and is
not installed as the thread default.

The hosted default allocator implements the three `core/runtime` operations
against the Darwin heap. Fresh storage is zeroed, alignments through 16 use the
ordinary allocator, larger alignments use `posix_memalign`, and aligned resize
allocates/copies/releases while preserving the old allocation on failure. The
root and each lazy thread Context use this provider for general allocation. The
temporary provider instead owns a pthread-key state containing a direct list of
separately aligned allocations. Individual free is a no-op, resize allocates and
preserves the live prefix, explicit reset releases the whole list, pthread key
destruction releases it on thread return, and hosted main releases its state
before process-view teardown. `core/memory` exposes temporary byte/typed helpers
and explicit reset without hiding a call-boundary reset. The runtime also
installs a stderr logger and `arc4random_buf` random provider rather than empty
records.

## Initial core memory facilities

Status: ordinary Draft library surface over the allocator and Darwin ABIs.

`core/memory.Arena` is a direct linked list of backing blocks with an absolute
address-aligned bump cursor. Its allocator performs allocate and preserving
resize, treats individual free as a logical no-op, and releases complete blocks
on explicit reset/destroy. Block metadata and bytes use the caller-selected
backing allocator, so no compiler ownership mechanism is hidden behind the
handle.

`memory.Buffer[T]` owns one fixed-length typed allocation. `Owned_String` owns a
zero-terminated byte copy and exposes a mutable bounded byte view plus `cstring`;
Draft's built-in `string` remains an immutable non-owning view and the library
does not fabricate one through an undocumented cast. Both handles store their
allocator and require explicit destruction.

The first virtual-memory seam is target-qualified Darwin source using fixed
signatures for `mmap`, `mprotect`, and `munmap`. Reserve creates inaccessible
private anonymous address space, commit/protect change whole-region permissions,
and release clears the move-by-convention handle. The constants are part of the
versioned AArch64 macOS core distribution rather than inferred from host headers.

## Nominal generic inference and transitive interfaces

Status: Draft 1 semantic rule and bootstrap representation.

Procedure inference unifies nominal template applications by their retained
template identity and ordered arguments. It never compares aggregate layouts or
member names: unrelated structs remain different even when their bytes match.
After an earlier argument fixes a type parameter, later arguments receive the
substituted expected type, allowing ordinary literals in calls such as
`array.append(&values, 42)` without weakening the rule that every parameter must
be inferred uniquely.

An imported nominal type may occur only inside another declaration's procedure
signature. Interface import therefore creates a private compiler-owned type
owner for its retained member packet even when the immediate dependency does
not publish a type declaration for it. This makes transitive values fully
usable—for example, the `runtime.Allocator` returned by `core/heap` still has
its `procedure` and `user` members—and lets a subsequent interface export carry
the same member information onward.

Generic aggregate construction applies that rule recursively. When a public
template contains a specialization from a transitive dependency, the canonical
interface packet is sufficient to create the concrete nested nominal even if
the consumer did not directly import the original template declaration. The
importer installs provenance before following members so recursive types
converge on the in-progress TypeId. Recursive instantiation snapshots aggregate
rows before appending new ones; sanitizer coverage enforces this append-only
table discipline.

## Unique dependent-value inference

Status: implemented for a closed, provably one-to-one Draft 1 subset.

Procedure-call inference first substitutes values learned from earlier runtime
arguments, then may invert one remaining dependent integer expression. The
solver requires exactly one occurrence of exactly one unresolved value
parameter. It follows only operations which are one-to-one over the node's exact
typed domain: unary positive, negation, bitwise complement, same-width or
widening integer casts, addition or subtraction by a constant expression, and
XOR by a constant expression. Fixed-width inversion uses the same
two's-complement wrapping as ordinary dependent-expression evaluation. The
candidate is substituted back into the complete canonical expression and
evaluated before it is accepted.

This supports calls inferred from shapes such as `[N + 1]T`, `[10 - N]T`, and
`Buffer[N + 1]`, including imported templates whose parameter leaves have been
remapped through package-interface ordinals. It also checks a later dependent
argument against a value inferred by an earlier argument.

The compiler does not guess through non-injective forms. Repeated occurrences
such as `N + N`, multiplication/division/remainder, shifts, AND, and OR require
an explicit generic argument unless a future solver can prove a unique solution
for the complete declared domain. Rejecting those forms is part of the
uniqueness rule, not a heuristic failure.

## Owner-evaluated generic layout constants

Status: implemented for array/SIMD recipes in local generic types, generic
procedure signatures, nested procedure-dependent nominal and structural alias
value arguments, and concrete cross-package public type applications.

The compact dependent-integer tree remains the canonical representation for
arithmetic and casts such as `[N + 1]T`. It must not pretend to represent a call
to an ordinary compile-time procedure: that procedure may contain conditionals,
loops, switches, recursion, aggregate values, and further calls, all of which
belong to the full interpreter rather than a second reduced evaluator.

When a parameter-dependent array or SIMD count needs that full interpreter, its
symbolic Type row carries an explicit owner-evaluated marker and the defining
SemanticPackage retains the source recipe in a package-local side table. A
canonical package interface exports only the marker. It never exports FileId,
NodeId, ScopeId, a private procedure SymbolId, or an ambient source pointer.

A consumer which later supplies concrete generic arguments forms a stable
instantiation request. Compiler orchestration sends that request to the package
which owns the template and procedure bodies. The owner evaluates the recipe
with the normal compile-time interpreter and returns a complete concrete
InterfaceTypeGraph; the consumer imports that graph exactly like any other
owner-produced generic result. A clean consumer rebuild removes the provisional
unknown-layout marker before body checking. Requests and results are ordered and
keyed by package identity, public template identity, and canonical argument
graphs. Private requester nominals returning through the owner graph resolve to
their original local TypeId. This mirrors cross-package generic procedure
instantiation and preserves private implementation details without making
consumer semantics read dependency source.

The same boundary applies when a nominal application itself supplies a full
procedure-dependent value such as `Buffer[increment(N)]`. Its ParametricArgument
carries an explicit owner-evaluated marker while the defining SemanticPackage
retains the recipe. A symbolic nominal instance has members for type checking but
an unknown layout. Substitution composes the captured environment through nested
templates; once concrete, it evaluates the argument and constructs the ordinary
canonical nominal instance. Interfaces again export only the marker.

A structural alias application such as `Bytes[increment(N)]` cannot use that
nominal-instance representation because its final identity is only the
canonical identity of the resulting array, pointer, tuple, procedure, or scalar.
The bootstrap therefore creates a deliberately non-interned placeholder which
copies the provisional structural shape, has unknown layout, and indexes a
package-local record containing the alias template plus its ordered arguments.
Structural interning explicitly ignores these placeholders, so an unresolved
application can never poison a later concrete lookup. Once all arguments are
exact, the placeholder disappears and the ordinary TypeStore constructor
returns the canonical structural TypeId; no alias-specific nominal identity is
invented.

Concrete cross-package structural results need a durable lookup key even though
the resulting Type row has no provenance. The owner annotates only the root of
the published InterfaceTypeGraph with the public template identity and canonical
arguments. A consumer records that application key beside the imported
canonical TypeId. Later rebuilds therefore reuse the exact array/tuple/etc.
without changing structural identity. Pending whole-application markers and
their package-local recipe indices never cross this boundary. Completed body
interfaces retain all self-contained application graphs published during the
layout fixed point instead of discarding them when effects are refreshed.

Owner requests close transitively across package imports. If package A requests
a concrete type from B and B's resulting layout requests a private recipe from
C, the workspace publishes C's graph first, cleanly rebuilds B against the
enriched C interface, and retries the original request. Canonical request
digests detect a repeated non-progressing set, while an explicit owner stack
guards the acyclic package invariant. Previously published type graphs are
self-contained and remain attached to B's interface across the rebuild.

## Procedure-dependent generic procedure arguments

Status: local and cross-package concrete specialization implemented.

An explicit generic procedure application may use a full compile-time call in
its value packet, for example `inner[increment(N)]()`. During the non-lowered
outer-template pass, the body checker validates the full expression's ordinary
type and carries an explicit deferred substitution marker; it does not invent a
compact integer expression or prematurely manufacture a callee instance. Every
concrete outer specialization checks the original syntax again, evaluates it
with the normal typed interpreter and its active type/value bindings, and then
instantiates the exact callee. Cross-package orchestration therefore transfers
only the final integer and never the caller's private helper or source recipe.

Symbolic signatures may retain the callee's unresolved value parameter or a
whole structural-alias placeholder during the template-only pass. They exist
only as type-checking evidence and are never lowered. A full-call substitution
is applied only when the saved recipe actually references that parameter; this
prevents an unrelated outer template environment from trying to evaluate a
callee-owned recipe. The concrete pass substitutes exact counts and is
authoritative for runtime-bearing HIR, including signatures whose array/SIMD or
structural-alias shape depends on the procedure-produced argument.

## Shift-count validation domain

Status: complete Draft 1 scalar runtime rule.

A shift count retains its independently checked source integer type through HIR;
there is no language-level coercion to the shifted operand's type. MIR validates
the count in unsigned `u128`, which exactly represents every nonnegative Draft 1
integer count and every scalar operand width. Sign-extending a negative signed
count into that domain produces a large unsigned value, so the single
`count < width` comparison rejects both negative and out-of-range counts. Only
the proven-safe count is then represented at the shifted operand's width for
LLVM's same-width shift instruction. This avoids truncating a width such as 128
into a narrow count type such as `i8` while retaining the source rule that every
invalid count traps before the shift executes.

## Conditional context discovery

Status: complete Draft 1 contextual-value inference rule.

An outer expected type continues to flow into both conditional value branches.
Without one, a direct `nil` or contextual enum/tagged-union alternative may take
its type from the opposite, independently typed branch regardless of whether it
appears on the left or right. This is a type-checking dependency only: runtime
evaluation still executes the condition first and then exactly one value branch.
If both branches require context, the compiler does not guess an owner or pointer
kind; an outer expected type remains mandatory. Grouping and denial wrappers do
not change this rule, and a nested conditional requests outer context exactly
when both of its own value branches require it.

## Hosted process views and core threads

Status: AArch64 macOS hosted runtime contract.

The C entry receives Darwin's third `envp` argument. Before Draft `main`, the
runtime materializes argv and envp as stable `{pointer,length}` string records;
`core/os` returns non-owning slices over those records. Normal return frees the
record arrays after all Draft defers finish. Environment entries preserve their
exact `name=value` bytes and ordering. The initial file API wraps already-open
fixed descriptors; pathname opening waits for a pinned fixed-signature wrapper
because Draft 1 deliberately rejects variadic C imports.

`core/thread` uses pthreads through fixed C signatures. Spawn state owns a copy
of the active Context. The C trampoline installs that copy as the child TLS
default before entering the ordinary Draft callback, replacing temp_allocator
with a provider whose state belongs to that OS thread, so ordinary calls,
defers, and `runtime.default_context` agree. Join clears the owning handle. Mutex
and condition storage uses the target-profile Darwin LP64 layouts (64 and 48
bytes, including their eight-byte signatures) and is accessed only through
pthread operations.

## Initial compiler-backed atomic interface

Status: first AArch64 macOS core surface; C11 memory semantics are normative.

`core/atomic.Value[T]` is an ordinary, naturally aligned nominal wrapper whose
storage may be initialized non-atomically only before publication. After
publication, all access uses the package operations. The first target accepts
one-, two-, four-, and eight-byte integer objects plus pointer objects; read,
write, exchange, integer read/modify/write, compare-exchange, and thread fences
lower to dedicated target-independent MIR instructions. LLVM emission uses
`load atomic`, `store atomic`, `atomicrmw`, `cmpxchg`, and `fence` rather than
foreign calls or pthread locks.

Every order argument is a compile-time `atomic.Order` value. Semantic checking
rejects release loads, acquire stores, releasing failure orders, and a
compare-exchange failure order stronger than its success order. A relaxed fence
is valid under the adopted C11 rules but has no synchronization effect; MIR
retains it and LLVM text emits an explicit no-op comment because LLVM has no
`fence monotonic` instruction.

The initial operations must be called directly through the imported package (an
alias is fine). Taking one as a procedure value or explicitly specializing it
is diagnosed because its order and storage facts belong in atomic HIR/MIR, not
in the ordinary calling convention. The Draft source bodies are interface
records and defensive traps only; no valid compiled call reaches them.

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

## Conditional judgment-manifest publication

Status: public default judgment command implemented.

`draftc judge` first compiles the complete provider-free resolved program, then
evaluates every authored judgment in canonical package/obligation order. Each
provider verdict reaches the crash-safe attempt history immediately, including
failures that revoke an exact prior key. Only a completed all-pass aggregate is
eligible for manifest selection. The default command replaces all judgment rows
because it selects all sites; synthesis pins, external inputs, and native
evidence rows are copied unchanged. Fine-grained selectors will instead replace
only their selected site keys.

All resolution-manifest writers now hold an interprocess lock on the workspace
directory. Judgment publication additionally compares the currently visible
canonical manifest with the exact optional snapshot retained by compilation
while holding that same lock. A missing snapshot requires a still-missing
manifest. This prevents a slow provider response for program A from overwriting
or attaching evidence to concurrently resolved program B, while retaining the
existing staged-object/fsync/manifest-last crash protocol.

## Offline locked judgment evidence gate

Status: provider-neutral multi-validator/artifact verification implemented;
public CLI uses the first all-sites profile.

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

## Native source correlation sidecar

Status: canonical operation map implemented; runtime instrumentation remains a
validation-profile concern.

Every LLVM debug marker now publishes the same row to an implementation-owned
`draft-source-correlation-v1` sidecar. A source-addressable row is identified by
package, canonical MIR procedure ordinal, and emission ordinal, describes the
procedure spelling plus MIR instruction or terminator, and carries both
generated and authored file/line/column coordinates. The ordinal is necessary
because multiple concrete specializations may retain one source procedure
spelling.
Generated rows additionally carry the persistent synthesis-site identity.
Filenames are logical basenames under the separately recorded package identity;
physical checkout paths never enter the native artifact or sidecar.

The native adapter writes the canonical JSON only after every package has a
valid LLVM module and returns its SHA-256 digest beside the native output. A
normal resolved build binds the map to the resolved-program digest. The lower
level backend API can deliberately compile a checked graph before resolution;
that form binds the map to a digest of the exact, package-framed LLVM module set
instead of inventing a resolved-program identity. The sidecar remains derived
output in both cases, avoiding a circular program identity.

The sidecar is the common join boundary for future counter-based coverage and
sampling profiles. Sanitizer, race, allocator-poisoning, and coverage runtimes
still require explicit versioned validation profiles; this map does not pretend
that an unrequested instrument ran.

## Native Mach-O debug companions

Status: implemented for executable and dynamic-library artifacts.

Mach-O final links retain a debug map rather than copying package-object DWARF
into the executable or dylib. A successful native build therefore runs
`dsymutil` before it reports success and publishes the conventional sibling
`<artifact>.dSYM`. Locked roots must contain executable `bin/dsymutil` beside
`clang`, Apple `ld`/`ld-classic`, and `llvm-ar`; the complete toolchain tree is
already one manifest input, so this adds no ambient tool lookup or separate
unbound pin.

The invocation ignores object and Swift-module timestamps, uses one worker, and
verifies its linked output. Locked children retain the empty search path and
fixed locale, with fixed `HOME=/` and `TMPDIR=/tmp` solely for LLVM scratch-state
discovery. The returned native result carries both the bundle path and its
canonical content-tree digest. Object, archive, and assembly artifacts keep
their debug data in their object members or `.loc` directives and therefore do
not receive a misleading final-link companion.

LLVM dsymutil also emits `Contents/Resources/Relocations`, a rewriting cache
whose YAML records the physical binary path. That cache is unnecessary for
symbolizing the already-linked binary and is removed before hashing or
publication. The standard `Info.plist` and `Contents/Resources/DWARF` payload
remain, and their logical Draft compilation directory/file coordinates agree
with `draft-source-correlation-v1`. Compiler content v121 makes the expanded
native artifact contract explicit without changing resolved source semantics.

The resolver's macOS native acceptance case now checks the complementary
generated-source path. After four declaration/member/expression/statement pins
are replayed without a provider, it links and runs the program and requires the
executable expression and statement site identities in both
`draft-source-correlation-v1` and the linked DWARF payload. Declaration-only
and layout-only pins are not required to fabricate machine operations. This is
a real host-toolchain/dSYM gate; the selected locked release distribution must
still repeat it.

## Canonical CLI workspace roots

Status: implemented for every package command.

The public driver canonicalizes the requested existing package directory before
deriving its workspace parent. This resolves symlinked parent spellings such as
macOS `/tmp` before they reach the resolution store. The store retains its
strict no-follow traversal after accepting that canonical root; weakening the
store would turn a presentation-path issue into a transaction security issue.

Only package/workspace ownership uses this helper. Requested output paths,
relocatable provider artifacts, runtime assets, and locked toolchain/SDK roots
keep their separate explicit policies. A public `resolve` regression reaches a
copied package through a symlinked parent, pins dummy native roots, and proves
that the manifest is published under the canonical real workspace.

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

## Runtime assertion build mode

Status: implemented and bound to resolved-program identity.

`--assertions=off` is a target-independent compile configuration, not an LLVM
optimization flag. Source still parses, resolves, type-checks, and participates
in effect and denial analysis normally. MIR lowering recognizes the typed
`assert` intrinsic before visiting its operands and emits nothing. This is the
only placement that directly proves the specified behavior: calls, stores,
traps, and message construction used solely by an assertion cannot survive,
and no condition is converted into an optimizer assumption.

The versioned `draft.resolved-program.v4` hash records the assertion mode beside
compiler content v129. `build`, `resolve`, and `judge` expose the same explicit
flag so an offline or locked manifest cannot be replayed under a different
mode. Test and benchmark compilations deliberately override the release choice
to assertions on and receive their own resolved validation digest; disabling
release assertions must never weaken the validation program that authorizes a
release.

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

## Selected self-contained AArch64 distribution

Status: qualified release input for compiler content v129.

The selected toolchain contains the five baseline programs, the address
profile's `llvm-symbolizer` and arm64 ASan dylib, and their recursive
dynamic-library closure. LLVM/Clang components are 22.1.8;
Mach-O links use Apple ld project 1267 and ld-classic project 957.1. Upstream
LLD remains unsuitable for the complete artifact contract because Mach-O `-r`
is unimplemented in LLVM 22.1. The colocated Apple helper preserves the
specified one-object aggregate output without host discovery.

The selected SDK is a 328 KiB link-only tree containing the exact
`usr/lib/libSystem.tbd` stub. Locked builds do not preprocess C or consume SDK
headers. Its use by executables, dylibs, and relocatable objects, together with
the complete native and determinism matrices, proves that no larger developer
SDK is currently an implicit input. The assembly recipe, exact content-tree
identities, dependency policy, and distribution boundary are recorded in
[the AArch64 macOS toolchain document](../releases/aarch64-macos-toolchain.md).
