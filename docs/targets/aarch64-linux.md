# AArch64 Linux GNU target profile

This document records the versioned machine and ABI facts of Draft's second
bootstrap target. It deliberately selects one concrete GNU/Linux distribution
contract; it does not claim that every AArch64 Linux kernel, libc, page size,
or userspace ABI is interchangeable.

## Initial AArch64 Linux profile

Status: implemented and required in native AArch64 Linux CI; Draft-level Linux
sanitizer profiles remain future work.

The profile identity is `draft-aarch64-linux-gnu-v2`. It targets
`aarch64-unknown-linux-gnu`, the GNU AAPCS64 ABI, little-endian ELF, the generic
Armv8-A CPU with baseline NEON, 64-bit pointers, 4 KiB pages,
position-independent small-model code, and general-dynamic TLS. Its initial
hosted distribution contract is Linux 6.8 with glibc 2.39, corresponding to an
Ubuntu 24.04-class AArch64 sysroot. Changing the libc contract or base page size
requires a new profile identity.

The fixed LLVM data layout is:

```text
e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32
```

Fixed-arity C aggregates use the GNU AAPCS64 register classes: homogeneous
floating aggregates occupy one to four matching floating lanes, other records
of at most 16 bytes use one or two integer containers, and larger records are
indirect. Unlike Darwin arm64, GNU AAPCS64 does not attach LLVM `signext` or
`zeroext` contracts to sub-32-bit C scalar parameters and results. An unfixed
`c enum` retains the default 32-bit C width and widens to 64 bits only
when its values require it.

GNU AAPCS64 variadic calls retain their own unnamed-argument classification.
Draft emits a real LLVM variadic function type and delegates the concrete
register/stack assignment to the selected target machine; the promoted mode
argument of `core/os`'s `open(2)` call is therefore passed directly without a
package-assembly shim.

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
summary covers the libc, POSIX thread, mapping, file, process, and clock symbols
used by the compiler runtime and first core packages.

The current core source tree selects Linux file/open flags, anonymous-mapping
bits, glibc pthread handle/storage types, and `clock_gettime` with the profile's
`aarch64-linux` file tag. An AArch64 Linux build of `draftc` selects this profile
when `--target` and workspace policy are both absent. Every package command also
accepts explicit `--target aarch64-linux` and carries the profile through
compilation, C-header emission, validation, resolution, and judgment. The root
LLVM runtime now emits glibc's 32-bit `pthread_once_t` and
`pthread_key_t` layouts. The native adapter emits ELF relocatable objects,
deterministic archives, `.so` files with SONAMEs, and PIE executables using the
host glibc development files and `ld.lld`. `--debug-symbols` retains DWARF in
the final ELF artifact; every build carries a content-derived GNU build ID and
correctly omits a Mach-O dSYM companion.

The selected glibc 2.39 AArch64 terminal contract gives `struct termios` four
32-bit flag words, one line-discipline byte, thirty-two control bytes, padding,
and two 32-bit speed fields: 60 bytes total with four-byte alignment. `pollfd`
is eight bytes with four-byte alignment, and glibc `nfds_t` is `unsigned long`.
`core/terminal` isolates those facts in target-qualified source while sharing
its raw-session lifetime and timeout policy with macOS. Its size query uses the
common eight-byte `winsize` layout and Linux `TIOCGWINSZ = 0x5413` through
glibc's real variadic `ioctl` ABI.
Resize observation uses signal 28 (`SIGWINCH`) and glibc's 152-byte,
eight-aligned public `sigaction` record. Its `sigset_t` is sixteen
`unsigned long` words; flags follow the 128-byte mask and the restorer pointer
ends the record. The core watcher installs only over `SIG_DFL` with an empty
mask and zero flags, then restores the saved record exactly.

`core/process` uses glibc `fork`/`chdir`/`execv`/`execve`/`waitpid`. Argument
and any required replacement-environment pointer tables are complete before
`fork`; the child performs only async-signal-safe native calls. It retries wait
for `EINTR = 4`, interprets the Linux/POSIX low-seven-bit signal and
high-eight-bit exit fields, and terminates directory/exec failure through
`_exit(126)`/`_exit(127)`.

Filesystem canonicalization uses glibc `realpath(path, nil)` and releases the
returned allocation after copying its absolute UTF-8 bytes. Directory
enumeration uses `opendir`/`readdir`/`closedir`. The selected Linux `dirent`
prefix is two 64-bit words, a 16-bit record length, one-byte type, and a zero-
terminated name. Type values 4 and 8 identify directories and regular files;
native enumeration order is not semantic.

The initial cross-target qualification used LLVM/LLD 22.1.8 and an Ubuntu
24.04 arm64 sysroot containing glibc 2.39, Linux 6.8 UAPI headers, and the GCC
13 runtime. That run is preserved in the
[qualification archive](../history/releases/aarch64-linux-qualification.md).
Current native AArch64 Linux CI links LLVM 22 in-process for one complete object
per semantic package and uses matching Clang/LLD utilities for assembly and
final links.
It passes the language tour, console output,
argv/environment/process/file operations, target-qualified ELF package
assembly, pthread spawn/join/mutex/condition/TLS/Context attachment, and a
generated-header C client calling a Draft shared library across aggregate,
enum, HFA, callback, and foreign-thread boundaries. Repeated artifact builds
are byte-identical. AddressSanitizer and the other Draft validation
instrumentation profiles remain fail-closed for this target. The x86-64 CI job
instruments the C++ bootstrap implementation and also runs the complete native
x86-64 Draft target suite; generated Draft programs themselves still use the
ordinary target profile.

## Related contracts

The target-independent rules remain in the
[types/runtime specification](../specification/02-types-memory-runtime.md) and
[native interop specification](../specification/04-native-interop.md). The
[native backend document](../implementation/native-backend-and-artifacts.md)
records how concrete artifact emission realizes this target contract.
