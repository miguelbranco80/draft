# x86-64 Windows MSVC target profile

This document records the versioned machine, Windows SDK, object, data-model,
and C ABI facts of Draft's first Windows target. It selects the Microsoft x64
ABI and Windows 10 system boundary rather than treating all x86-64 machines as
the already implemented SysV GNU/Linux target.

## Initial profile

Status: target selection, compile-time introspection, LLP64 C scalar aliases,
Win64 aggregate classification, and in-process COFF object emission are
implemented. PE linking, the hosted Windows runtime/core implementations, and
native Windows CI are the remaining stages of this target's implementation.

The command selector is `x86_64-windows`, the profile identity is
`draft-x86_64-windows-msvc-v1`, and the target-qualified source tag is
`x86_64-windows`. It targets `x86_64-pc-windows-msvc`, little-endian COFF,
64-bit pointers, 4 KiB pages, position-independent small-model code, and
general-dynamic TLS. Its hosted boundary is Windows 10 with the Universal CRT
and Kernel32 supplied by a matching Windows SDK.

The fixed LLVM data layout is:

```text
e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128
```

The CPU is baseline `x86-64` with SSE2 enabled. `avx` and `avx2` are known but
disabled target features. Parsed inline assembly is unavailable; exact
target-qualified package assembly remains part of the native target contract.

## LLP64 C data model and Win64 ABI

Windows uses LLP64: pointers, `size_t`, and pointer-sized Draft integers are 64
bits, but C `long` and `unsigned long` are 32 bits. `core/c_abi` selects those
aliases through `target.abi`; all other fixed C scalar aliases retain their
ordinary widths. An unfixed `c enum` uses a 32-bit signed or unsigned backing
according to its values. An enumerator outside that range must use an explicit
fixed backing rather than silently widening to the LP64 `long` rule.

Microsoft x64 passes a naturally laid-out `c struct` or `c union` in one
integer slot only when its complete size is exactly 1, 2, 4, or 8 bytes. The
record's member kinds do not create an SSE aggregate class: an eight-byte pair
of floats still uses the integer carrier. Every other nonempty record parameter
is passed through a pointer, and every other record result uses the hidden
result pointer. Scalar floating-point values retain the ordinary Win64 XMM
placement. C variadic tails use Draft's checked default promotions and LLVM's
target calling-convention lowering, including Win64's required register
duplication for unnamed floating-point arguments.

The implementation applies one classification product to imports, exports,
definitions, and call sites. Unit tests compare the exact public LLVM types
with Clang's `x86_64-pc-windows-msvc` lowering and verify that LLVM emits AMD64
COFF rather than a host object.

## Hosted system boundary

The `libc` provider names Universal CRT functions; the `windows` provider names
Kernel32 functions. The target profile publishes individual closed symbol
summaries, including callback positions for `CreateThread` and `FlsAlloc`, so a
denial never treats an entire DLL as semantically trusted. The linker selects
the UCRT through the MSVC driver contract and adds `kernel32` explicitly.

The hosted implementation will preserve Draft's target-independent public
core APIs. Windows handles, virtual-memory flags, synchronization objects,
performance counters, and console modes remain private to target-qualified
core files or the hosted runtime adapter; they do not become new language
types.

## Native artifacts and assembly

The embedded LLVM 22 adapter registers the X86 target and already emits
deterministic AMD64 COFF package objects at O0 and O2. The completed artifact
stage will use matching Clang/LLD and `llvm-lib` to produce `.exe`, `.dll`,
`.lib`, and `.obj` outputs with PE/COFF debug information. Native Windows CI is
the independent execution and C-client oracle; off-host COFF generation alone
does not qualify the runtime or linker contract.

Files such as `native@x86_64-windows.s` are exact assembler inputs. The profile
does not claim a parsed `asm x86_64` dialect; selected parsed assembly receives
a source diagnostic.

## Related contracts

Target-independent meaning remains in the
[types/runtime specification](../specification/02-types-memory-runtime.md) and
[native interop specification](../specification/04-native-interop.md). The
[native backend](../implementation/native-backend-and-artifacts.md) and
[runtime/core](../implementation/runtime-and-core.md) documents describe the
compiler mechanisms which realize this profile.
