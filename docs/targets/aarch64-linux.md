# AArch64 Linux GNU target profile

This document records the versioned machine and ABI facts of Draft's second
bootstrap target. It deliberately selects one concrete GNU/Linux distribution
contract; it does not claim that every AArch64 Linux kernel, libc, page size,
or userspace ABI is interchangeable.

## Initial AArch64 Linux profile

Status: implemented and initial locked cross-target qualification passed;
Linux-hosted bootstrap tools and Linux sanitizer profiles remain future work.

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

The v3 core source tree selects Linux file/open flags, anonymous-mapping bits,
glibc pthread handle/storage types, `clock_gettime`, and ELF assembly symbol
spelling with the profile's `aarch64-linux` file tag. Every package command now
accepts `--target aarch64-linux` and carries this profile through compilation,
C-header emission, validation, resolution, and judgment; macOS remains the
compatibility default. The root LLVM runtime now emits glibc's 32-bit
`pthread_once_t` and `pthread_key_t` layouts. The native adapter emits ELF
relocatable objects, deterministic archives, `.so` files with SONAMEs, and PIE
executables using the pinned glibc sysroot and `ld.lld`. Final ELF artifacts
retain DWARF and a content-derived GNU build ID; they correctly omit a Mach-O
dSYM companion.

The first locked qualification cross-compiled with LLVM/LLD 22.1.8 and an
Ubuntu 24.04 arm64 sysroot containing glibc 2.39, Linux 6.8 UAPI headers, and
the GCC 13 runtime. Native AArch64 execution passed the ordinary language tour,
console output, argv/environment/process/file operations, target-qualified ELF
package assembly, pthread spawn/join/mutex/condition/TLS/Context attachment,
and a generated-header C client calling a Draft shared library across aggregate,
enum, HFA, callback, and foreign-thread boundaries. Two locked language-tour
links were byte-identical. Exact evidence and commands are recorded in the
[Linux qualification report](../releases/aarch64-linux-qualification.md).

This qualifies programs targeting AArch64 Linux from the current macOS-hosted
bootstrap. Running the compiler itself on Linux remains a separate host-porting
task: locked mode needs an ELF dependency-closure verifier for the host Clang,
lld, and archiver. AddressSanitizer and the other validation instrumentation
profiles also remain fail-closed for this target.

## Related contracts

The target-independent rules remain in the
[types/runtime specification](../specification/02-types-memory-runtime.md) and
[native interop specification](../specification/04-native-interop.md). The
[native backend document](../implementation/native-backend-and-artifacts.md)
records how concrete artifact emission realizes this target contract.
