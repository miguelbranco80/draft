# Selected self-contained AArch64 distribution decision

This release decision complements the exact layout, assembly recipe, content identities, and qualification evidence in [the AArch64 macOS toolchain document](aarch64-macos-toolchain.md).

## Selected self-contained AArch64 distribution

Status: qualified release input for compiler content v129.

The selected toolchain contains the five baseline programs, the address
profile's `llvm-symbolizer` and arm64 ASan dylib, and their recursive
dynamic-library closure. LLVM/Clang components are 22.1.8;
Mach-O links use Apple ld project 1267 and ld-classic project 957.1. Upstream
LLD remains unsuitable for the complete artifact contract because Mach-O `-r`
is unimplemented in LLVM 22.1. The colocated Apple helper preserves the
specified one-object aggregate output without host discovery.

The selected SDK is a 328 KiB link-only tree containing the exact
`usr/lib/libSystem.tbd` stub. Locked builds do not preprocess C or consume SDK
headers. Its use by executables, dylibs, and relocatable objects, together with
the complete native and determinism matrices, proves that no larger developer
SDK is currently an implicit input. The assembly recipe, exact content-tree
identities, dependency policy, and distribution boundary are recorded in
[the AArch64 macOS toolchain document](aarch64-macos-toolchain.md).
