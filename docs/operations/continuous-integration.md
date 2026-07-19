# Continuous integration

Status: native AArch64 and Linux x86-64 sanitizer gates implemented.

The ordinary GitHub Actions workflow builds and tests the bootstrap compiler on
the two implemented native host/target pairs:

- `macos-15` runs the Apple Silicon Mach-O path with AppleClang;
- `ubuntu-24.04-arm` builds the C++ bootstrap with GCC and runs Draft's ELF path
  through the distribution Clang, lld, and LLVM utilities.

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

## Development CI and locked qualification

Pull-request CI deliberately uses the toolchains already shipped by the GitHub
runner images. It passes the backend's explicit development opt-in for those
ambient tools; it does not invent a resolution manifest, vendor a second copy of
LLVM, or maintain hashes for runner-owned files. This gate answers whether the
current source compiles and works with the declared host environment.

Locked release qualification answers a different question: whether exact
content-pinned compiler, SDK/sysroot, runtime, and external-input trees reproduce
the qualified artifact contract. Those roots and their content-tree SHA-256
identities remain documented in the release qualification records and never
become prerequisites for an ordinary pull request. A CI failure cannot be
silenced by changing locked identities, and a green ambient build is not
presented as locked release evidence.

## Local equivalent

Run the same warning-clean debug configuration on a native AArch64 host with:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DDRAFT_WARNINGS_AS_ERRORS=ON \
  -DDRAFT_ENABLE_SANITIZERS=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On Linux, `clang`, `ld.lld`, and the LLVM utilities must be available in
`PATH`, even when GCC is selected as `CMAKE_CXX_COMPILER`: GCC compiles the
bootstrap implementation, while Draft's backend invokes the LLVM tools to
produce the program under test.

The sanitizer job's local equivalent on an x86-64 Linux host is:

```sh
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
cmake -S . -B build-sanitized \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=g++ \
  -DDRAFT_WARNINGS_AS_ERRORS=ON \
  -DDRAFT_ENABLE_SANITIZERS=ON
cmake --build build-sanitized --parallel
ctest --test-dir build-sanitized --output-on-failure
```
