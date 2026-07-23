# x86-64 Linux GNU target profile

This document records the versioned machine, libc, object, and C ABI facts of
Draft's first x86-64 target. It selects one concrete GNU/Linux distribution
contract rather than claiming that every kernel, libc, or x86 ISA extension is
interchangeable.

## Initial profile

Status: implemented. The native Linux x86-64 CI row is the required execution,
C-client, determinism, and sanitizer gate; dated pass counts belong in release
evidence only after that workflow completes.

The command selector is `x86_64-linux`, the profile identity is
`draft-x86_64-linux-gnu-v1`, and the target-qualified source tag is
`x86_64-linux`. It targets `x86_64-unknown-linux-gnu`, little-endian ELF,
64-bit pointers, 4 KiB pages, position-independent small-model code, and
general-dynamic TLS. Its hosted contract is Linux 6.8 with glibc 2.39,
corresponding to Ubuntu 24.04 x86-64. Changing the libc contract or base page
size requires a new identity.

The fixed LLVM data layout is:

```text
e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128
```

The CPU is baseline `x86-64` with SSE2 enabled. `avx` and `avx2` are known but
disabled target features, so compilation does not inherit the build host's ISA.
The legal source SIMD storage shapes remain the current 64-bit and 128-bit
Draft set; feature-gated machine instructions are a separate contract.

## SysV AMD64 C ABI

The profile uses the System V AMD64 ABI. Draft's legal C surface reaches the
ABI's INTEGER and SSE aggregate classes but not x87, complex, or native C
vector classes. A naturally aligned `c struct` or `c union` of at most 16 bytes
is classified as one or two source-order eightbytes. Any INTEGER member makes
its containing eightbyte INTEGER; otherwise floating members make it SSE.
Larger aggregates and aggregates containing an unaligned member are MEMORY.

Placement is derived from the complete ordered signature, not merely from one
type. Six integer and eight SSE argument registers are available; a hidden
indirect-result pointer consumes the first integer register. A two-eightbyte
argument uses registers only when every required class still fits. Otherwise
the complete argument reverts to stack memory and the tentative assignments
are not consumed. LLVM represents that case with an exact `byval` aggregate
pointer. INTEGER results use `rax`/`rdx`, SSE results use `xmm0`/`xmm1`, and a
MEMORY result uses the hidden `rdi` pointer. Narrow C integer parameters and
results carry the SysV `signext` or `zeroext` contract.

Unfixed `c enum` uses the same initial LP64 C rule as the current AArch64
profiles: at least 32 bits, signed when a member is negative, unsigned for a
wholly nonnegative set, and widened to 64 bits when required. Fixed backings
retain their explicitly checked scalar ABI.

The implementation's classifier and signature planner are shared by imports,
exports, calls, definitions, and generated C headers. LLVM object tests verify
the physical signatures off-host; the native Linux C-client gate independently
compiles and runs aggregate, enum, union, callback, large-result, and register-
exhaustion boundaries with Clang. The governing external ABI is the
[System V x86-64 psABI](https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/master/x86-64-ABI/low-level-sys-info.tex).

Real C variadic calls use LLVM's ordinary variadic function type after Draft
performs the default scalar promotions. The target machine owns final unnamed-
argument register and stack placement. Aggregate variadic tails remain outside
the current Draft C surface.

## Hosted system and core boundary

The `libc` and `linux` providers are supplied by glibc through `-lc`. The
target-owned summaries cover the libc, pthread, virtual-memory, file, process,
clock, polling, and terminal calls used by the distributed runtime and core.
The hosted runtime uses glibc's 32-bit `pthread_once_t` and `pthread_key_t`.

Target-selected core files fix the following x86-64 glibc 2.39 facts:

- `pthread_t` is `unsigned long`;
- `pthread_mutex_t` is 40 bytes with alignment eight;
- `pthread_cond_t` is 48 bytes with alignment eight;
- `termios` is 60 bytes with alignment four;
- `pollfd` is eight bytes with alignment four and `nfds_t` is
  `unsigned long`;
- `winsize` is eight bytes and `TIOCGWINSZ` is `0x5413`;
- `SIGWINCH` is signal 28 and public `sigaction` is 152 bytes aligned to
  eight, including a 128-byte `sigset_t` before flags and the restorer;
- `timespec` is two signed 64-bit words; and
- Linux anonymous private mapping uses `MAP_PRIVATE | MAP_ANONYMOUS = 0x22`.

`core/process` uses glibc `fork`/`execv`/`waitpid`, retries wait for
`EINTR = 4`, interprets the Linux/POSIX low-seven-bit signal and high-eight-bit
exit fields, and terminates a failed child exec through `_exit(127)`.

The common runtime, allocator, files, threads, terminal, TUI, and validation
policy is otherwise the same target-independent Draft source used on AArch64.

## Native artifacts and assembly

The embedded LLVM 22 adapter registers the X86 target and emits one ELF object
per semantic package at O0 or O2. Matching Clang/LLD and `llvm-ar` assemble
target-qualified sources, link executables and `.so` files, and create
deterministic archives. `--debug-symbols` retains DWARF in ELF outputs. Final
artifacts use a deterministic GNU build ID and no Mach-O dSYM companion.

Files such as `native@x86_64-linux.s` are exact package-assembly inputs and may
contain the full external assembler language. The first x86-64 profile has no
parsed inline `asm x86_64` grammar. This is an explicit capability boundary:
selected parsed assembly receives a source diagnostic, while compile-time
unselected AArch64 assembly and ordinary Draft code remain valid.

## Related contracts

Target-independent meaning remains in the
[types/runtime specification](../specification/02-types-memory-runtime.md) and
[native interop specification](../specification/04-native-interop.md). The
[native backend](../implementation/native-backend-and-artifacts.md) and
[runtime/core](../implementation/runtime-and-core.md) documents describe the
compiler mechanisms which realize this profile.
