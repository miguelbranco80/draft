# x86-64 Windows MSVC target profile

This document records the versioned machine, Windows SDK, object, data-model,
and C ABI facts of Draft's first Windows target. It selects the Microsoft x64
ABI and Windows 10 system boundary rather than treating all x86-64 machines as
the already implemented SysV GNU/Linux target.

## Initial profile

Status: initial hosted target implemented. Target selection, compile-time
introspection, LLP64/Win64 C semantics, COFF emission, PE artifact publication,
the hosted runtime/core packages, and native Windows build/launch CI are one
coherent target path. Cross-platform validation execution and provider-backed
Codex commands remain bootstrap-host limitations rather than target semantics.

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

The Clang-compatible `__int128` extension is asymmetric on Microsoft x64:
Draft passes `i128`, `u128`, endian storage equivalents, and fixed-backing C
enums by address, while results use LLVM's `<2 x i64>` carrier. This rule is
part of C interoperation only; ordinary Draft procedures keep Draft's direct
128-bit value representation. An unnamed C variadic `i128`/`u128` argument uses
the same caller-owned address carrier; other aggregate variadic tails remain
outside Draft 1.

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

The hosted implementation preserves Draft's target-independent public core
APIs. `core/os` converts UTF-8 C paths synchronously for UCRT wide pathname
operations while retaining ordinary integer file descriptors. `core/memory`
uses `VirtualAlloc`, `VirtualProtect`, and `VirtualFree`; `core/time` uses the
performance counter; `core/thread` uses owned thread handles, SRW locks, and
condition variables. `core/terminal` translates CRT descriptors to console
handles, saves/restores exact input and output modes, enables VT processing,
waits for input, and reports the visible console window in cells. All Windows
handles, flags, and physical records remain private to target-qualified source.
The portable resize watcher compares that visible-window query on each poll;
Windows does not fabricate a POSIX-style process signal or mix console window
records into the byte-oriented raw input stream.

The runtime exposes Draft byte strings on Windows without an ANSI-code-page
dependency. Its `wmain` adapter converts UTF-16 arguments and environment rows
once to owned, zero-terminated UTF-8, then feeds the same stable string-record
shape used by other targets. UCRT `_read`/`_write` provide byte I/O, `rand_s`
provides random words, and fiber-local storage owns each thread's temporary
allocator state and destructor. General allocations use a small aligned-header
scheme over `malloc` so every supported alignment has one matching release
operation.

`core/process` uses the LLP64 104-byte/eight-aligned `STARTUPINFOW` and
24-byte/eight-aligned `PROCESS_INFORMATION` records, converts an exact UTF-8
application path with `MultiByteToWideChar`, starts it through `CreateProcessW`,
waits indefinitely, reads the DWORD exit code, and closes both process and
thread handles. It does not construct a command-line string for the first
zero-extra-argument operation.

## Native artifacts and assembly

The embedded LLVM 22 adapter registers the X86 target and emits deterministic
AMD64 COFF package objects at O0 and O2. Matching Clang/LLD produces `.exe` and
`.dll`; `llvm-lib` produces deterministic `.lib` archives. C exports carry the
COFF `dllexport` contract, and a DLL publishes its `.lib` import library as an
explicit hashed companion. Linked executables and DLLs request reproducible
CodeView/PDB output and publish the sibling `.pdb` path and digest.

COFF has no relocatable partial-link operation equivalent to ELF/Mach-O `-r`.
`--kind object` therefore publishes an exact `.obj` only when the complete
selected graph has one native input. A graph with several semantic packages,
or package-assembly inputs must use `--kind static-library`; the compiler does
not disguise archive bytes as an object file. A mapped foreign provider remains
a separate link input and therefore requires a final executable/DLL link or
must be supplied separately when a Draft object/archive is consumed.

The Windows process adapter invokes tools with `CreateProcessW`, exact UTF-8 to
UTF-16 argument conversion, CRT-correct quoting, combined output capture, and
an explicit inherited-handle list. Native Windows CI builds the bootstrap
against the official LLVM-C 22 distribution, builds and launches the complete
ordinary example inventory, exercises exact COFF package assembly and foreign
objects, and compiles a C client against a generated header and Draft DLL.
Off-host command/COFF tests alone do not qualify the hosted runtime.

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
