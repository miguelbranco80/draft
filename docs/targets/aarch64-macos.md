# AArch64 macOS target profile

This document records the versioned machine and ABI facts of the first Draft target. The closed parsed-assembly grammar is specified separately in [the AArch64 assembly profile](aarch64-macos-assembly.md). Runtime implementation details and concrete release binaries remain in the implementation and release documentation.

## Initial AArch64 macOS profile

Status: bootstrap target contract; versioned as `draft-aarch64-macos-v6`.

The first profile targets `arm64-apple-macosx14.0.0`, uses the generic AArch64
CPU with baseline NEON, 64-bit little-endian pointers, 16 KiB pages, Mach-O,
position-independent small-model code, general-dynamic TLS, and a macOS 14.0
deployment floor. Its fixed LLVM data-layout string is:

```text
e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32
```

The profile explicitly names its baseline NEON SIMD shapes: 64-bit and 128-bit
vectors with at least two lanes over `i8/u8`, `i16/u16`, `i32/u32`,
`i64/u64`, `f16`, `f32`, or `f64` where the total width matches. A different
lane count or element spelling is a semantic target error, not an LLVM fallback.

The parsed inline-assembly dialect identity is `draft-aarch64-apple-v2`; its
closed register, operand, addressing, condition, and instruction grammar is
enumerated in [the AArch64 parsed assembly profile](aarch64-macos-assembly.md).
Package `.s`, `.S`, and `.asm` inputs all contain exact non-preprocessed bytes;
in particular, `.S` does not inherit the host C driver's preprocessing rule.
Changing any of these facts creates a new target-profile identity rather than
silently changing the meaning of the existing profile.

## Hosted runtime ABI

The AArch64 macOS root `runtime.Context` is 96 bytes with 8-byte alignment.
Its fields begin at offsets 0, 16, 32, 40, 56, 72, 80, and 88 in source order.
Allocator, logger, and random-generator provider records each contain one
procedure pointer and one provider-state pointer. The assertion callback is an
ordinary Draft procedure pointer, so its physical call prepends the active
Context pointer. Changing this layout requires a new runtime ABI and core
distribution identity.

`runtime.default_context` returns the calling thread's Context snapshot through
Darwin's indirect aggregate-result convention. The first `core/thread`
implementation uses Darwin LP64 pthread layouts: mutex storage is 64 bytes and
condition storage is 48 bytes, including their eight-byte signatures.

The first target accepts compiler-backed atomic objects of one, two, four, or
eight bytes for integer values, plus pointer-sized pointer objects. C11 memory
semantics remain defined by the runtime specification; the supported object
widths are facts of this target and core distribution.

Apple arm64 C variadic calls place unnamed arguments according to Darwin's
variadic stack rules rather than the fixed-parameter register assignment.
Draft emits a real LLVM variadic function type and delegates that placement to
the selected target machine; `core/os` therefore calls variadic `open(2)`
directly with its promoted mode value.

Darwin interactive terminal state uses the macOS 14 arm64 `termios` contract:
four 64-bit flag words, twenty one-byte control characters, padding to two
64-bit speed fields, 72-byte total size, and eight-byte alignment. `pollfd` is
eight bytes with four-byte alignment, while Darwin `nfds_t` is a 32-bit
unsigned integer. `core/terminal` keeps these layouts target-selected and uses
the fixed `tcgetattr`, `tcsetattr`, `cfmakeraw`, and `poll` libc signatures.
Its size query uses the eight-byte `winsize` layout and Darwin
`TIOCGWINSZ = 0x40087468` through the real variadic `ioctl` ABI.
Resize observation uses signal 28 (`SIGWINCH`) and Darwin's public 16-byte,
eight-aligned `sigaction` record: one handler pointer, 32-bit signal mask, and
32-bit flags. The core watcher installs only over `SIG_DFL`, uses an empty mask
and zero flags, and restores the complete prior record.

`core/process` uses Darwin's LP64 `fork`/`chdir`/`execv`/`execve`/`waitpid`
boundary. The argument and any required replacement-environment pointer tables
are complete before `fork`; the child performs only async-signal-safe native
calls. It retries wait only for Darwin `EINTR = 4`, interprets the standard
low-seven-bit signal and high-eight-bit exit fields, and terminates
directory/exec failure through `_exit(126)`/`_exit(127)`.

Filesystem canonicalization uses Darwin `realpath(path, nil)` and releases the
returned libc allocation after copying its absolute UTF-8 bytes. Directory
enumeration uses `opendir`/`readdir`/`closedir`. The selected `dirent` begins
with two 64-bit fields, 16-bit record and name lengths, one-byte type, and a
1024-byte name array. Type values 4 and 8 identify directories and regular
files; every other value remains an unknown portable entry kind.

## C enum ABI

A `c enum` without an explicit backing follows Apple Clang's default
enum rule for this target instead of Draft's smallest-fitting rule. Its backing
is at least 32 bits: a wholly nonnegative member set uses `u32`, a set
containing a negative member uses `i32`, and either widens with the same
signedness to 64 bits when required. Values that do not fit `u64` or `i64`
are rejected. An explicit backing continues to use the separately validated
fixed-backing C-enum contract.

## Related target contracts

The general Draft rules remain in the
[types/runtime specification](../specification/02-types-memory-runtime.md) and
[native interop specification](../specification/04-native-interop.md). The
bootstrap implementation records how it realizes the hosted
[Context, process, thread, and atomic surface](../implementation/runtime-and-core.md)
and [native artifact behavior](../implementation/native-backend-and-artifacts.md).
The linked LLVM 22 library and matching Clang tools, Apple linker, SDK, and
`libtool` are operational prerequisites; `--debug-symbols` additionally uses
matching `dsymutil`. These are documented in the
[command reference](../operations/command-reference.md), not language inputs.
