# AArch64 Linux GNU target profile

This document records the versioned machine and ABI facts of Draft's second
bootstrap target. It deliberately selects one concrete GNU/Linux distribution
contract; it does not claim that every AArch64 Linux kernel, libc, page size,
or userspace ABI is interchangeable.

## Initial AArch64 Linux profile

Status: implemented target-profile boundary; native artifact and core-runtime
qualification proceeds in the subsequent Linux implementation slices.

The profile identity is `draft-aarch64-linux-gnu-v1`. It targets
`aarch64-unknown-linux-gnu`, the GNU AAPCS64 ABI, little-endian ELF, the generic
Armv8-A CPU with baseline NEON, 64-bit pointers, 4 KiB pages,
position-independent small-model code, and general-dynamic TLS. Its initial
hosted distribution contract is Linux 6.8 with glibc 2.39, corresponding to an
Ubuntu 24.04-class AArch64 sysroot. Changing the libc contract or base page size
requires a new profile identity.

The pinned LLVM data layout is:

```text
e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32
```

Fixed-arity C aggregates use the GNU AAPCS64 register classes: homogeneous
floating aggregates occupy one to four matching floating lanes, other records
of at most 16 bytes use one or two integer containers, and larger records are
indirect. Unlike Darwin arm64, GNU AAPCS64 does not attach LLVM `signext` or
`zeroext` contracts to sub-32-bit C scalar parameters and results. An unfixed
`@repr(C)` enum retains the default 32-bit C width and widens to 64 bits only
when its values require it.

The enabled and known CPU feature vocabulary and legal baseline SIMD shapes
match the current AArch64 macOS profile because they describe the same baseline
machine architecture. The parsed-assembly contract has its own
`draft-aarch64-linux-v1` identity even where its first instruction and operand
grammar is intentionally equal to the existing AArch64 grammar. This prevents
a later platform-specific directive or object spelling from silently changing
the older target.

## Hosted system boundary

The logical `libc` and `linux` foreign providers are both supplied by glibc's
`libc.so.6` through the compiler-owned `-lc` link. The separate names remain
semantic provider identities for denial summaries. The initial target-owned
summary covers only the fixed libc, POSIX thread, mapping, file, process, and
clock symbols used by the compiler runtime and first core packages.

ELF artifact spelling, the dynamic loader, deterministic link flags, debug
information, locked toolchain/sysroot shape, pthread storage, and the Linux core
source selection are implementation work layered on this profile. Release
documentation must not call the target qualified until those gates have run.

## Related contracts

The target-independent rules remain in the
[types/runtime specification](../specification/02-types-memory-runtime.md) and
[native interop specification](../specification/04-native-interop.md). The
[native backend document](../implementation/native-backend-and-artifacts.md)
records how concrete artifact emission realizes this target contract.
