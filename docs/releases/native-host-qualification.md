# Native host qualification

This report records the native-host qualification performed on 2026-07-19 for
the provider-free build model implemented by commit `42e6097`. The subsequent
documentation commit does not change compiler behavior.

The compiler uses the Clang, linker, archiver, system libraries, and SDK or
sysroot installed on the build host. Those tools are ordinary build
configuration: the compiler reports useful version information, but does not
copy them into the workspace or make their filesystem contents part of Draft
program identity.

## Qualified configurations

| Host | Compiler build | Tests | Native coverage |
| --- | --- | ---: | --- |
| macOS AArch64 | Apple Clang, warnings as errors | 52/52 | AArch64 native conformance, deterministic native output, C client integration, and a generated Draft executable run under AddressSanitizer |
| Ubuntu 24.04 AArch64 | GCC 13.3, warnings as errors | 51/51 | AArch64 Linux native conformance, deterministic native output, and C client integration using host Clang, LLD, and LLVM archiver tools |
| Ubuntu 24.04 x86-64 | GCC 13.3, warnings as errors, compiler ASan/UBSan enabled | 48/48 | The bootstrap compiler and applicable tests run under AddressSanitizer and UndefinedBehaviorSanitizer; AArch64 native execution tests are intentionally not registered on this host |

All three suites completed with no test failures. The two Linux suites ran in
fresh Ubuntu 24.04 containers with the source tree mounted read-only. The
macOS suite ran directly on the AArch64 development host.

## What the gates establish

The macOS and AArch64 Linux native gates exercise the full supported path from
Draft source through semantic analysis, MIR, LLVM IR, host-tool invocation, and
execution of the resulting program. They also verify that two builds of the
same resolved program produce identical native outputs where the artifact
format promises deterministic bytes.

The x86-64 Linux gate qualifies the bootstrap compiler itself on a second host
architecture. AddressSanitizer checks invalid memory accesses and lifetime
errors in the exercised C++ code. UndefinedBehaviorSanitizer checks operations
such as invalid shifts, misaligned accesses, and signed arithmetic overflow.
This gate does not claim that Draft currently emits x86-64 programs; the native
backend target remains AArch64.

The macOS generated-program AddressSanitizer test is a separate check. It asks
host Clang to instrument emitted Draft code, links with Clang's normal runtime
recipe, and runs that executable. This proves that sanitizer validation of a
Draft program does not require Draft to distribute or identify a sanitizer
runtime of its own.

## Reproduction shape

The native hosts configure CMake with warnings as errors, build all targets,
and run CTest. The Linux x86-64 job additionally configures
`DRAFT_ENABLE_SANITIZERS=ON` and runs with fatal ASan and UBSan options. The
exact maintained commands live in
[the continuous-integration workflow](../../.github/workflows/ci.yml) and are
explained in [Continuous integration](../operations/continuous-integration.md).

This report is a dated verification snapshot, not a semantic dependency of a
Draft build. Later builds use the tools installed on their own host, and CI is
responsible for detecting regressions across the maintained host matrix.
