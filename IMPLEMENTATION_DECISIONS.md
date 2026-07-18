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

Status: bootstrap target contract; versioned as `draft-aarch64-macos-v1`.

The first profile targets `arm64-apple-macosx14.0.0`, uses the generic AArch64
CPU with baseline NEON, 64-bit little-endian pointers, 16 KiB pages, Mach-O,
position-independent small-model code, general-dynamic TLS, and a macOS 14.0
deployment floor. Its pinned LLVM data-layout string is:

```text
e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32
```

The parsed inline-assembly dialect identity is `draft-aarch64-apple-v1`.
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
the `draft-aarch64-apple-v1` analyzer validates fixed general, scalar FP, and
fixed-vector registers, direct typed loads and stores, the initial integer/FP
and barrier vocabulary, and register/flags/memory declarations. MIR lowers
accepted regions as volatile assembly. More complex addressing, lane-qualified
SIMD operations, and the remainder of the closed Draft 1 instruction vocabulary
are still required before the agent-free implementation is declared complete.

Package assembly follows the separate file contract in section 3. The compiler
copies every selected file's exact bytes into the compiled package snapshot;
the later native adapter never rereads the workspace. It writes those bytes to
the isolated build directory, passes `-x assembler` for all three extensions,
and appends each resulting object after its package LLVM object in canonical
package and filename order. The explicit language selection is essential for
`.S`: Draft's target profile specifies no preprocessing, independently of
Clang's conventional filename behavior. Symbols cross this boundary only
through `foreign` and `export` C-ABI declarations.

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
returns a copy of the executable root's process-default Context through the
Darwin indirect aggregate-result convention. `runtime.call_with_context`
statically checks a non-nil `^Context`, an ordinary Draft callback, and the
callback's exact arguments, then lowers directly to that callback with the
explicit hidden Context pointer. Named callbacks retain their ordinary effect
summaries; indirect callbacks remain unknown edges. These are narrow versioned
runtime exceptions, not permission to use Context in arbitrary C signatures.
Per-thread attachment and a TLS-backed foreign-thread default are later runtime
work; the current bridge deliberately exposes the process-default snapshot.

The hosted default allocator implements the three `core/runtime` operations
against the Darwin heap. Fresh storage is zeroed, alignments through 16 use the
ordinary allocator, larger alignments use `posix_memalign`, and aligned resize
allocates/copies/releases while preserving the old allocation on failure. The
root Context currently uses this provider for both general and temporary
allocation. A thread-owned resettable temporary arena remains part of the TLS
runtime work; `core/memory` currently exposes the honest byte-level substrate
and explicit allocator forms without pretending that arena policy exists.
