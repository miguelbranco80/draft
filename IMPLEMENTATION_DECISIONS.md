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
following the specification's explicit statement-ending list. Consequently a
binary XOR may not continue after `^` across an outermost newline; it can remain
on one line or be continued inside parentheses, where semicolon insertion is
suppressed. This rule is deterministic and makes the common dereference form
work, but the normative specification should state the disambiguation directly.

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
enumerated in [AARCH64_ASSEMBLY_PROFILE.md](AARCH64_ASSEMBLY_PROFILE.md).
Package `.s`, `.S`, and `.asm` inputs all contain exact non-preprocessed bytes;
in particular, `.S` does not inherit the host C driver's preprocessing rule.
Changing any of these facts creates a new target-profile identity rather than
silently changing the meaning of the existing profile.

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

## Initial locked native input contract

Status: bootstrap build contract; versioned by the resolution and content-tree
formats.

The first locked compiler-owned artifact seam accepts exactly two external inputs: one LLVM
toolchain tree named `llvm-aarch64-macos` and one SDK tree named `macos-sdk`.
The LLVM tree exposes executable `bin/clang`, `bin/ld64.lld`, and `bin/llvm-ar`;
Clang is the manifest entry point and the linker/archiver locations are fixed by
this adapter version.
Both complete trees are hashed as sorted relative path records including file
kind, permission bits, exact regular-file bytes, and exact internal symlink
spelling. Physical root paths are excluded. Absolute or escaping symlinks,
root symlinks, and special files are rejected.

Locked invocation uses the verified absolute Clang and linker paths, explicit
`-isysroot`, `--no-default-config`, `--no-xcselect`, the target deployment
floor, and the linker's content-derived Mach-O UUID. Current macOS requires the
UUID load command for executable launch; a real integration gate proves two
complete links to the same explicit output identity are byte-for-byte equal.
The child environment contains fixed `LANG` and `LC_ALL` plus an empty `PATH`;
it inherits no SDK, compiler, header, library, deployment, or package search
variables. Other manifest external-input roles fail closed until their
artifact-to-command mapping exists. Ordinary development builds retain the
separate explicit host-toolchain escape hatch.

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
understand. `draft-synthesis-request-v7` carries canonical Draft spellings for
the expected type and every visible binding, together with explicit target,
SIMD, and parsed-assembly facts. Package and enclosing-declaration documentation
retain their anchor, text, and exact isolated attachment bytes. The same rows
remain content-hashed, so their human-readable representation is both useful to
Codex and a stale-pin input.

The request also carries persistent package and source coordinates plus a
canonical token rendering of the enclosing procedure or type declaration.
Lexical comments are omitted by construction, while names, literals, grammar,
and the synthesis site's exact structural surroundings remain readable. The
rendering and its digest are obligation inputs; package-level declaration
synthesis explicitly has no enclosing declaration instead of receiving an
unbounded source-file dump.

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

A provider-free compile or resolution revalidation may consume a content-fresh
pin without installing its original provider. When `draft resolve` explicitly
selects a provider, however, provider, model, and complete adapter-configuration
identities must match the pin; any changed selection regenerates the expansion.

The first Codex adapter permits two attempts, each with a five-minute deadline.
That policy is part of adapter configuration identity. A timed-out child is
killed and reaped before another attempt starts, and only a successful complete
response can reach the compiler-owned resolution transaction.

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
