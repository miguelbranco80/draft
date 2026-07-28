# Continuous integration

Status: four native target and installed-distribution gates, with bootstrap
sanitizers on Linux x86-64.

The ordinary GitHub Actions workflow builds and tests the bootstrap compiler on
the four implemented native host/target pairs:

- `macos-15` runs the Apple Silicon Mach-O path with AppleClang and Homebrew
  LLVM 22;
- `ubuntu-24.04-arm` builds the C++ bootstrap with GCC and runs Draft's ELF path
  with the pinned LLVM 22 development library, distribution Clang/lld, and LLVM
  utilities;
- `ubuntu-24.04` builds the bootstrap with GCC ASan/UBSan and runs the complete
  x86-64 SysV/ELF Draft path with the same LLVM 22 tool family.
- `windows-2022` builds the MSVC bootstrap against the official LLVM 22
  development archive and runs the x86-64 Win64/PE path with matching Clang,
  lld-link, llvm-lib, UCRT, and Windows SDK components.

All jobs treat warnings as errors. The macOS/Linux jobs build the complete test
suite and run it on the host. On each of those matching pairs CMake includes
native conformance,
one-worker/four-worker byte-for-byte artifact determinism, embedded-LLVM versus
external-Clang parity, generated-C-header/client, explicit foreign-provider
linking, and validation harness tests. The exhaustive
`examples/qualification.tsv` gate checks every tracked example package for all
four targets. Windows uses a focused driver-level gate while its validation
process runner is still POSIX-only: it builds and launches every ordinary
executable row except the signal-classification trap fixture, publishes all
initial COFF artifact families including an assembly bundle, links a foreign
provider, and compiles/launches the independent C client against a Draft DLL.
Draft tests and benchmarks remain in the macOS/Linux gates. Linux and Windows
native execution are therefore required jobs, not optional cross-compilation
probes.

Every native host also exercises the relocatable install contract. The
macOS/Linux CTest suite installs to a clean prefix; Windows performs the same
step after its focused native gate. The common smoke checks that both version
reports select `toolchain: bundled`, builds and runs an installed example with
no checkout-relative core or LLVM path, and launches installed DraftIDE against
its sibling compiler service. Windows removes the development LLVM `bin` entry
from `PATH` before that smoke, so a missing bundled DLL or private executable
cannot be supplied accidentally by the toolchain used to build the archive.

The x86-64 row additionally enables GCC AddressSanitizer and
UndefinedBehaviorSanitizer with leak detection and immediate failure. Those
runtimes instrument the C++ bootstrap and test executables. Generated Draft
programs use the ordinary x86-64 target unless a separately selected Draft
instrumentation profile is under test; bootstrap sanitizer flags do not become
Draft program configuration. DraftIDE is deliberately a mixed process: its
ordinary Draft executable loads the instrumented C++ compiler service. CTest
preloads the exact compiler-selected ASan runtime for only those service-backed
tests so ELF load order cannot disable the sanitizer or leak into unrelated
generated-program tests.

## Host toolchains and resolved inputs

Pull-request and release CI pins LLVM 22 as a bootstrap compiler component and
links its C and C++ LTO APIs behind narrow in-process backend adapters. The
selected
`LLVMConfig.cmake` path is explicit in every job. Clang, lld, Apple ld, SDKs,
LLVM utilities, and sanitizer runtimes remain host build configuration. CMake's
selected LLVM directory supplies the default absolute paths for matching Clang,
`llvm-ar`, and `dsymutil`; ordinary builds do not probe or silently select an
unrelated ambient Clang. None of these tools are synthesized source or Draft
program dependencies, and they do not appear in resolution manifests.

Windows uses the official `clang+llvm` development archive rather than the
smaller tool-only installer because the bootstrap needs LLVM headers, CMake
exports, static LTO/target libraries, and native tools. CI verifies the upstream
SHA-256 before extraction. Its setup script expands the xz and tar layers with
the runner's 7-Zip rather than the Windows bsdtar implementation, then caches
that immutable tree by version and digest. The Visual Studio
developer environment supplies the matching Windows SDK include/library paths
to the Clang processes launched by `draftc`.

## Tagged releases

The separate release workflow runs only for `v*` tags. It repeats Release-mode
native qualification, creates one CPack archive on each native host, extracts
that archive, and reruns the distribution smoke against the extracted bytes.
The final job cannot run unless all four packages pass. It verifies the tag
against the binary version, generates one `SHA256SUMS`, and publishes a GitHub
prerelease. See [Building and releasing Draft](releases.md) for the archive
layout and local equivalent.

Foreign objects, archives, shared libraries, provider summaries, and runtime
assets remain exact resolved-program inputs when a program selects them. Their
content-tree verification is covered by target-independent tests. Native
artifact reproducibility is checked directly by the macOS/Linux harnesses,
which repeat all artifact kinds under the same target profile and compare the
complete output trees. The Windows gate checks successful reproducible-mode
publication and required companions; repeated-byte comparison remains pending.

## Local equivalent

Run the same warning-clean debug configuration on any matching native host with:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLLVM_DIR=/opt/homebrew/opt/llvm@22/lib/cmake/llvm \
  -DDRAFT_WARNINGS_AS_ERRORS=ON \
  -DDRAFT_ENABLE_SANITIZERS=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The example integration subset is independently selectable without changing
what the unfiltered CI command runs:

```sh
ctest --test-dir build -L examples --output-on-failure
```

On Linux, use `-DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm`. That LLVM
installation must include Clang, lld, and the utilities even when GCC is
selected as `CMAKE_CXX_COMPILER`: GCC compiles the bootstrap, LLVM's linked C
API emits one complete object per semantic package, and matching tools
provide qualification, assembly, linking, archiving, and debug operations.

On Windows, configure a 64-bit Visual Studio build with `LLVM_DIR` naming
`lib/cmake/llvm` inside the official LLVM 22 development archive. Build
`Release/draftc.exe`, enter the Visual Studio x64 developer environment, and
reproduce the maintained native slice with:

```powershell
cmake -DDRAFTC="$PWD/build/Release/draftc.exe" `
  -DCLANG="$env:LLVM_ROOT/bin/clang.exe" `
  -DSOURCE_ROOT="$PWD" `
  -DTEST_ROOT="$PWD/build/windows-native-smoke" `
  -P "$PWD/tests/driver_windows_native_smoke_test.cmake"
```

The sanitizer job's local equivalent on an x86-64 Linux host is:

```sh
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
cmake -S . -B build-sanitized \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++ \
  -DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm \
  -DDRAFT_WARNINGS_AS_ERRORS=ON \
  -DDRAFT_ENABLE_SANITIZERS=ON
cmake --build build-sanitized --parallel
ctest --test-dir build-sanitized --output-on-failure
```
