# AArch64 Linux initial qualification

Status: initial locked cross-target qualification passed on 2026-07-19.

This report records the first concrete evidence for programs targeting
`draft-aarch64-linux-gnu-v1`. It qualifies the macOS-hosted bootstrap compiler
as an AArch64 Linux cross compiler; it does not yet qualify running the compiler
itself on a Linux host.

## Selected inputs

The host was AArch64 macOS. Native target execution used an AArch64 Docker
daemon and the official `ubuntu:24.04` arm64 image with local image identity
`sha256:4fbb8e6a8395de5a7550b33509421a2bafbc0aab6c06ba2cef9ebffbc7092d90`.
The container reported `aarch64` and glibc 2.39.

The locked resolution manifest selected:

- `llvm-aarch64-linux`, content-tree SHA-256
  `4922b0c45be9106cb066aa6954d8e75b8ec9ba7a4347b04c1c260b0b9e35ed39`,
  with `bin/clang` as entry point and LLVM/LLD 22.1.8;
- `aarch64-linux-sysroot`, content-tree SHA-256
  `4e3d0c34a11fe882404add7416474d742aae82665f987021f924147724b3e521`,
  containing Ubuntu arm64 `libc6`/`libc6-dev` 2.39-0ubuntu8.7,
  `linux-libc-dev` 6.8.0-136.136, and `libgcc-13-dev`
  13.3.0-6ubuntu2~24.04.1.

The compiler semantic identity was `draft-bootstrap-cpp-v130`. The final
language-tour determinism pair and C shared-library/client rerun were both
produced after selecting that identity and the exact toolchain tree above.

The physical temporary roots are deliberately absent from semantic identity.
Both trees were re-hashed before every locked build, and every compiler/linker
child ran with an empty `PATH`, fixed locale/scratch variables, explicit
`--sysroot`, `--no-default-config`, and an absolute `ld.lld` path.

## Passed gates

| Gate | Evidence |
| --- | --- |
| ELF emission | Real LLVM produced AArch64 ELF relocatable objects with DWARF; the adapter produced deterministic archives, PIE executables, and `.so` files. |
| Hosted entry/runtime | The locked `hello` executable launched through `/lib/ld-linux-aarch64.so.1` and returned success under glibc 2.39. |
| Broad ordinary language | `examples/language-tour` ran natively and printed the expected total, range, nickname, and 64-bit target result. |
| OS and memory seam | `examples/core-os` passed argv, environment, descriptors, process ID, 4 KiB page size, allocator, Linux open flags, file create/write/read/close/unlink, and cleanup checks. |
| Threads and Context | `examples/core-thread` passed pthread spawn/join, mutex, condition, yield, package TLS, foreign Context attachment, and per-thread temporary allocation checks. |
| Package assembly | The target-qualified ELF `examples/external-assembly` object linked with the exact unprefixed C symbol and returned success. |
| C shared-library ABI | A generated Linux C header compiled with the selected Clang/sysroot. Its independent C client loaded the Draft `.so` and passed enum, aggregate, HFA, callback, opaque-pointer, TLS, and foreign-created pthread checks. |
| Debug contract | Executables and the shared library retained `.debug_info`/`.debug_line`, a GNU build ID, and no synthetic `.dSYM` path. |
| Determinism | Two complete locked `language-tour` links were byte-identical at SHA-256 `160ab896e90f05259be20053cfd9f7ab333d087b9587327a882bc0b906031ef3`. |

Unit coverage also records the exact ordinary and locked Clang/LLD/archiver
argument vectors, Linux manifest names and entry paths, omission of Darwin
deployment/link/dSYM options, target-selected core source files, GNU AAPCS64
classification, C-header symbol spelling, and glibc pthread runtime IR types.

## Limits retained after this gate

- Locked Linux output is currently produced by the qualified macOS-hosted
  cross toolchain. A Linux-hosted compiler needs an ELF host-tool dependency
  closure verifier equivalent to the existing Mach-O closure policy.
- Validation instrumentation remains fail-closed. No Linux ASan, race,
  undefined-operation, lifetime, or allocator-poisoning runtime profile has
  been pinned or qualified.
- ELF debug information remains in the primary artifact. Split debug files and
  debug-package publication are not implemented.
- This gate uses the Linux 6.8 userspace/UAPI contract in the sysroot. Docker's
  actual host kernel is execution infrastructure, not part of the semantic
  target identity.

These are implementation limits, not changes to Draft language semantics.
