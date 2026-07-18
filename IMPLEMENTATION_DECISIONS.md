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

Status: bootstrap target contract; versioned as `draft-aarch64-macos-v2`.

The first profile targets `arm64-apple-macosx14.0.0`, uses the generic AArch64
CPU with baseline NEON, 64-bit little-endian pointers, 16 KiB pages, Mach-O,
position-independent small-model code, general-dynamic TLS, and a macOS 14.0
deployment floor. Its pinned LLVM data-layout string is:

```text
e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32
```

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
floor, and no Mach-O UUID. The child environment contains fixed `LANG` and
`LC_ALL` plus an empty `PATH`; it inherits no SDK, compiler, header, library,
deployment, or package search variables. Other manifest external-input roles
fail closed until their artifact-to-command mapping exists. Ordinary
development builds retain the separate explicit host-toolchain escape hatch.

## Native artifact ownership and visibility

Status: first AArch64 macOS artifact contract.

The root LLVM module owns runtime support for every final native artifact, but
only executable compilation adds the hosted C `main`. This lets an exported C
wrapper use `runtime.default_context` from an object, archive, or dylib without
requiring a fake Draft entry procedure. Ordinary Draft procedures and globals
have hidden Mach-O visibility; only explicit C exports retain default visibility.

Object output performs a relocatable link over all package and package-assembly
objects. Static output uses deterministic LLVM-ar mode in locked builds. Dynamic
output fixes an `@rpath/<filename>` install name. Assembly output is a directory
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
