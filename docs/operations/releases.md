# Building and releasing Draft

Status: `0.1.0-alpha.1` distribution contract.

Draft has one CMake install manifest for local installation, distribution
smoke tests, and release archives. A package is not a bootstrap binary which
quietly reaches back into its build machine. It contains:

```text
bin/
    draftc
    draftide
    draft_compiler_service shared library
libexec/draft/
    bin/        private Clang, linker/debug, and archive tools
    lib/        matching LLVM/Clang runtime libraries
share/draft/
    core/       readable source matching the compiler-embedded core
    docs/
    examples/
    .agents/skills/write-draft-code/
```

`draftc` finds private tools from its loaded executable path. `draftide` finds
the compiler service beside itself. Neither operation depends on a checkout,
current working directory, `DYLD_LIBRARY_PATH`, `LD_LIBRARY_PATH`, or ambient
LLVM installation. `draftc --version` and `draftide --version` report
`toolchain: bundled` in an installed distribution and include the public
version, exact source commit, LLVM version, embedded core identity, and target
set.

The platform SDK is deliberately not redistributed. The macOS archive requires
Xcode Command Line Tools and discovers the active SDK once per native build
through `SDKROOT` or `xcrun`. Linux archives use the selected Ubuntu 24.04-class
glibc contract and need ordinary libc development/startup files. Windows needs
an x64 Visual Studio 2022 developer environment for the Windows SDK and UCRT.

## Build and install locally

Configure against an exact LLVM 22 CMake package, build, and install to any
prefix:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR=/opt/homebrew/opt/llvm@22/lib/cmake/llvm \
  -DDRAFT_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
cmake --install build --prefix /tmp/draft
/tmp/draft/bin/draftc --version
```

Use `/usr/lib/llvm-22/lib/cmake/llvm` on the maintained Ubuntu hosts. On
Windows, supply `--config Release` to the build and install commands.

Create the platform archive from that same manifest with:

```sh
cpack --config build/CPackConfig.cmake -C Release -B build/packages
```

The supported filenames are:

- `draft-0.1.0-alpha.1-aarch64-macos.tar.xz`
- `draft-0.1.0-alpha.1-aarch64-linux.tar.xz`
- `draft-0.1.0-alpha.1-x86_64-linux.tar.xz`
- `draft-0.1.0-alpha.1-x86_64-windows.zip`

CPack also creates an individual SHA-256 file for a local archive.

## Qualification

The ordinary CI workflow builds all four native hosts. macOS and Linux run the
complete CTest suite; its distribution test installs to an isolated prefix,
checks both version reports, builds and launches a real Draft program, and
starts DraftIDE in noninteractive smoke mode. The Windows job performs the
equivalent install smoke after its focused native compiler/C interoperation
gate.

The macOS package receives an ad-hoc signature after its loader paths are made
relative. That preserves local execution after archive extraction; it is not a
Developer ID signature or Apple notarization. The release page and checksum,
not Gatekeeper identity, are the current alpha distribution trust boundary.
