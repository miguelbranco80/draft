# Native host qualification

This report records the native-host qualification performed on 2026-07-19 for
the compiler implementation through commit `c0638e1`, the completed semantic-
work-graph qualification run on 2026-07-21 through commit `10ab54a`, and the
package-module qualification through commit `80768cc` later that day. It also
records the direct-LLVM and deterministic O0-unit qualification performed on
2026-07-23 through commit `1dd14d9`. This report commit changes documentation
only.

The current bootstrap links LLVM 22.1.8 and constructs LLVM IR directly in
process. O2 retains one complete module/object per semantic package; a large
native-only O0 package may use deterministic internal units. Matching Clang,
linker, archiver, debug, system-library, and SDK or sysroot tools remain
ordinary build configuration. Their locations do not enter Draft program
identity, and ordinary compilation does not launch a version probe.

## Qualified configurations

| Host | Compiler build | Tests | Native coverage |
| --- | --- | ---: | --- |
| macOS AArch64 | Apple Clang, warnings as errors, Debug | 63/63 | Complete AArch64 native suite, including one-worker/four-worker determinism, embedded-LLVM/Clang parity for every artifact kind, C-client integration, and generated Draft AddressSanitizer execution |
| macOS AArch64 | Apple Clang, warnings as errors, Release | 63/63 | The same complete native suite with assertions removed and bootstrap optimization enabled |
| macOS AArch64 | Apple Clang, bootstrap ASan/UBSan | 63/63 | The complete bootstrap and native suite under AddressSanitizer and UndefinedBehaviorSanitizer |
| Ubuntu 24.04 AArch64 | GCC 13.3, warnings as errors, Release | 62/62 | Complete AArch64 Linux native suite using linked LLVM 22.1.8 and matching Clang, LLD, and LLVM archiver tools |
| Ubuntu 24.04 x86-64 | GCC 13.3, warnings as errors, bootstrap ASan/UBSan | 56/56 | Every target-independent bootstrap test under AddressSanitizer and UndefinedBehaviorSanitizer; AArch64 execution tests are intentionally not registered |

All five suites completed with no test failures. Linux tool installation happened
in source-free containers. Qualification then ran in separate containers with
networking disabled and the source tree mounted read-only. The macOS suites ran
directly on the AArch64 development host.

## Semantic work graph addendum

On 2026-07-21, commit `10ab54a` passed 72/72 tests in the normal macOS AArch64
LLVM 22 build and 72/72 tests in the LLVM 22 ASan/UBSan build. This run includes
the macOS native determinism, embedded-LLVM/Clang parity, native conformance,
C-client, provider-free resolution, fake-provider resolution, and complete
example gates. It also runs both AArch64 target front-end checks; it is not a
new native Linux-host qualification and does not replace the Linux rows above.

The semantic scheduler portion compares one and four workers for complete
product graphs, ordered diagnostics, declaration failure selection, synthesis
request and manifest identity, resolved-program identity, HIR/MIR/LLVM payloads,
and native artifacts. A generated 256-procedure package additionally proves a
wide declaration/body ready set and exact deterministic timing counters without
asserting host-dependent wall time. Every `SemanticProductKind` has an explicit
transition assertion and real compiler consumer.

## Package-module addendum

On 2026-07-21, commit `80768cc` passed 72/72 tests in the normal macOS AArch64
LLVM 22 build. The run included native determinism, embedded-LLVM/external-Clang
parity, native conformance, C-client integration, provider-free and fake-
provider resolution, both target front-end checks, and every example gate.

The refactor retains one immutable `MirProcedure` product per concrete runtime
procedure, then publishes exactly one `PackageLlvmModule` per semantic package
in a workspace-wide ready wave. Artifact layouts contain that module followed
by package assembly inputs; native planning therefore creates one internal
module-object task per package rather than one LLVM task per procedure. A direct
`emit-llvm` qualification of `examples/language-tour` processed eight semantic
packages and 43 MIR procedures, emitted exactly eight package modules, and
published eight artifact layouts. The structurally validated Draft coding skill
was also forward-tested by fresh agents against the CLI and current compiler
product graph; no granularity flag or obsolete per-function LLVM route was
reported.

## Direct-LLVM and O0-unit addendum

On 2026-07-23, commit `1dd14d9` passed 91/91 tests in each of the macOS AArch64
Debug, Release/NDEBUG, and bootstrap ASan/UBSan configurations. All three builds
used warnings as errors. These suites include native determinism, direct-
LLVM/external-Clang parity, native conformance, C-client integration, compiler-
service and Draft IDE execution, provider-free resolution, every maintained
target front-end check, and the complete example matrix.

The qualified compiler checks every authored procedure, publishes one compact
direct-reference product for every concrete runtime body, and computes the
artifact-live procedure/global closure before MIR. The Draft IDE measurement
checked 463 runtime procedures and selected 398 as native-live, leaving 65
valid checked procedures out of machine emission. Target lowering scheduled
398 independent MIR tasks, 26 package LLVM units, and 22 package layouts. A
generated 131-live-procedure package additionally proved three canonical
48-procedure O0 units with byte-identical output at one and four workers, while
the equivalent O2 build retained one complete package unit.

Three uncontended fresh O0 Draft IDE builds with the optimized bootstrap and
`--timings` completed in 208.645, 209.688, and 212.521 ms. Their target-lowering
times were 34.928, 33.928, and 34.554 ms; final native-link times were 54.436,
57.182, and 56.628 ms. The same source measured about 67 ms in target lowering
before O0 unit splitting. Each run performed one compiler pass, one workspace
load, 26 in-process direct-LLVM unit emissions, and one external process—the
final linker. Debug metadata and `.dSYM` generation were not requested, and no
persistent compiler cache participated.

## What the gates establish

The macOS and AArch64 Linux native gates exercise the full supported path from
Draft source through semantic analysis, MIR, LLVM IR, embedded object emission,
the remaining platform-tool invocations, and execution of the resulting
program. The parity gate runs the same real package graphs through embedded LLVM
and the retained external-Clang oracle for executable, object, static-library,
dynamic-library, and assembly outputs. Both executable routes are launched.

The determinism gate builds every artifact kind with one worker and four workers
and compares the complete output trees byte for byte. Its failure case also
proves that concurrent completion cannot change the selected diagnostic or
publish a partial canonical artifact. Together with scheduler unit tests, this
qualifies actual concurrent work rather than only the shape of a task graph.

At the dated commits recorded above, the x86-64 Linux gate qualified only the
bootstrap compiler on a second host
architecture. AddressSanitizer checks invalid memory accesses and lifetime
errors in exercised C++ code. UndefinedBehaviorSanitizer checks operations such
as invalid shifts, misaligned accesses, and signed arithmetic overflow. This
did not claim that Draft emitted x86-64 programs. The current implementation
has since added `draft-x86_64-linux-gnu-v1`; its first native pass belongs in a
new dated qualification row after the updated workflow completes, rather than
being retroactively attributed to these earlier counts.

The generated-program AddressSanitizer test is distinct from sanitizing the C++
bootstrap. Draft asks embedded LLVM to apply its AddressSanitizer pass, then uses
the matching Clang driver to supply the platform runtime and launches the
instrumented program. The evidence records linked LLVM 22.1.8 rather than an
ambient runtime probe.

## Performance observation

An optimized macOS AArch64 hello-world build completed in 84.340 ms of measured
compiler wall time and 0.09 seconds of shell wall time. Front-end resolution,
checking, identity construction, and target lowering took 3.066 ms. Seven
independent native object tasks used seven workers and occupied 19.289 ms of
wall time; their individual durations overlap by design. Three external
processes remain: one package-assembly invocation, the final link, and dSYM
generation. This is a measurement from the qualification host, not a semantic
or performance guarantee.

## Reproduction shape

Native hosts configure CMake with the explicit LLVM 22 package directory,
warnings as errors, build all targets, and run CTest. Release qualification sets
`CMAKE_BUILD_TYPE=Release`. Sanitizer qualification sets
`DRAFT_ENABLE_SANITIZERS=ON` and runs with fatal ASan and UBSan options. The exact
maintained CI commands live in
[the continuous-integration workflow](../../.github/workflows/ci.yml) and are
explained in [Continuous integration](../operations/continuous-integration.md).

This report is a dated verification snapshot, not a semantic dependency of a
Draft build. Later builds use the tools installed on their own host, and CI is
responsible for detecting regressions across the maintained matrix.
