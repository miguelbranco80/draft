# Selected self-contained AArch64 distribution decision

This release decision complements the exact layout, assembly recipe, content identities, and qualification evidence in [the AArch64 macOS toolchain document](aarch64-macos-toolchain.md).

## Selected self-contained AArch64 distribution

Status: qualified release input for compiler content v129 and
`draft-core-bootstrap-v2`.

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

On 2026-07-19, the selected roots were independently pinned again to toolchain
identity `6f3dc859b8aee177db86879b7e7503e8bfbf8b5013ee0b745ab9db3502e0ad1f`
and SDK identity
`253fb9bad05f1a1abaacbf54cc642227a76def2c2dfd58839db0f8d5eafc5cb6`.
The locked 21-program native conformance gate and the repeated five-artifact
determinism gate both passed with core identity `draft-core-bootstrap-v2`.
