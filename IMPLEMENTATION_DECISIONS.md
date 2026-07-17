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
the beginning. Target-independent MIR lowering rejects an assembly region with
a precise diagnostic until ordinary Draft MIR has a working AArch64 macOS
emission path. Instruction parsing, register/operand validation, clobber
checking, and final lowering are then implemented against that same target
profile. Parsed assembly remains required before the first complete agent-free
Draft 1 implementation is declared finished; this staging only keeps it from
blocking bring-up of ordinary calls, control flow, runtime checks, and linking.
