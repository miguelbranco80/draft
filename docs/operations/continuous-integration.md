# Continuous integration

Status: native AArch64 and Linux x86-64 sanitizer gates implemented.

The ordinary GitHub Actions workflow builds and tests the bootstrap compiler on
the two implemented native host/target pairs:

- `macos-15` runs the Apple Silicon Mach-O path with AppleClang and Homebrew
  LLVM 22;
- `ubuntu-24.04-arm` builds the C++ bootstrap with GCC and runs Draft's ELF path
  with the pinned LLVM 22 development library, distribution Clang/lld, and LLVM
  utilities.

Both jobs treat warnings as errors, build the complete test suite, and run it on
the host. On these AArch64 pairs CMake includes the native conformance,
byte-for-byte artifact determinism, generated-C-header/client, and validation
harness tests. Linux native execution is therefore a required job, not an
optional cross-compilation probe.

A separate `ubuntu-24.04` x86-64 job builds the C++ bootstrap with GCC's Address
Sanitizer and UndefinedBehaviorSanitizer enabled. It runs every
target-independent test under both runtimes with leak detection and immediate
failure. Because Draft does not yet implement an x86-64 target, this job does
not cross-compile and emulate the native AArch64 integration executables. The
two ARM jobs own artifact execution; the x86-64 job adds host-implementation
memory and undefined-behavior coverage without confusing those responsibilities.

## Host toolchains and resolved inputs

Pull-request and release CI pins LLVM 22 as a bootstrap compiler component and
links its C API through the narrow in-process backend adapter. The selected
`LLVMConfig.cmake` path is explicit in every job. Clang, lld, Apple ld, SDKs,
LLVM utilities, and sanitizer runtimes remain host build configuration. None of
these are synthesized source or Draft program dependencies, and they do not
appear in resolution manifests.

Foreign objects, archives, shared libraries, provider summaries, and runtime
assets remain exact resolved-program inputs when a program selects them. Their
content-tree verification is covered by target-independent tests. Native
artifact reproducibility is checked directly: each AArch64 host repeats all
artifact kinds under the same target profile and compares the complete output
trees.

## Local equivalent

Run the same warning-clean debug configuration on a native AArch64 host with:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLLVM_DIR=/opt/homebrew/opt/llvm@22/lib/cmake/llvm \
  -DDRAFT_WARNINGS_AS_ERRORS=ON \
  -DDRAFT_ENABLE_SANITIZERS=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On Linux, use `-DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm`. Clang, `ld.lld`,
and the LLVM utilities must also be available in `PATH`, even when GCC is
selected as `CMAKE_CXX_COMPILER`: GCC compiles the bootstrap, LLVM's linked C
API emits package objects, and the remaining host tools provide qualification,
assembly, linking, archiving, and debug operations.

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
