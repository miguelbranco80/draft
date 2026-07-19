# AArch64 macOS toolchain distribution

Status: selected bootstrap release layout and qualification recipe.

The compiler never searches for native tools in locked mode. Resolution hashes
one explicit toolchain directory and one explicit SDK directory; a later build
re-hashes both directories and invokes only verified absolute entries. The
selected layout is intentionally small and contains no headers or host package
manager paths.

## Required layout

```text
toolchain/
  bin/
    clang
    dsymutil
    ld
    ld-classic
    llvm-ar
    llvm-symbolizer
  lib/
    ...the exact dynamic dependency closure of those six programs...
    clang/22/lib/darwin/libclang_rt.asan_osx_dynamic.dylib

sdk/
  usr/lib/libSystem.tbd
```

All toolchain executables and dylibs are thin AArch64 Mach-O images. The first
distribution uses Clang, dsymutil, llvm-ar, llvm-symbolizer, libclang-cpp, and
LLVM 22.1.8. The address-validation capability additionally owns the thin arm64
ASan dynamic runtime at the fixed Clang resource path. Its install name is
`@rpath/libclang_rt.asan_osx_dynamic.dylib`.
Final Mach-O links use Apple ld project 1267. Relocatable `-r` links are
delegated by that program to its colocated Apple ld-classic project 957.1.
Keeping both linker programs is required: upstream LLD 22.1 does not implement
Mach-O `-r`, while Draft object output combines all package objects into one
relocatable object.

The minimal SDK is a link SDK, not a C development SDK. Draft lowers directly
to LLVM IR and its first target links only `libSystem`, so the selected SDK
contains the exact `libSystem.tbd` stub and its directory structure. A future
target feature that consumes headers, frameworks, or another library must
expand and re-version this contract instead of discovering a host SDK.

## Assembly procedure

Release engineering constructs a fresh directory from exact source
distributions; it does not edit an installed tree in place.

1. Copy the selected LLVM tools and their dylib closure while dereferencing
   package-manager symlinks.
2. Rewrite every non-system dylib ID and load path to `@rpath/<basename>` and
   retain only runpaths expressed through `@loader_path` or
   `@executable_path` inside the distribution.
3. Thin Apple `ld`, `ld-classic`, and their four supporting dylibs to AArch64.
   Remove the unused absolute `/usr/lib/swift` runpath from
   `libswiftDemangle.dylib`; all actual Swift-demangler loads are ordinary
   sealed-system dependencies.
4. Thin the Clang 22.1 ASan dynamic runtime to AArch64, rewrite its install name
   to the fixed `@rpath` spelling, remove its unused
   `@loader_path/../unwind` runpath, and copy `llvm-symbolizer` with its already
   selected LLVM dylib closure.
5. Ad-hoc sign every Mach-O image changed by thinning or install-name edits.
6. Copy the exact `libSystem.tbd` link stub into the minimal SDK layout.
7. Run the compiler's locked-input pin operation. Its built-in Mach-O parser
   follows every `LC_LOAD_*` edge recursively, verifies the system dynamic
   loader, rejects embedded `DYLD_*` environment commands, and rejects
   unresolved, ambiguous, non-relocatable, or out-of-tree dependencies before
   either tree is hashed.

The only allowed dependency paths outside the toolchain tree are explicit
`/usr/lib/...` and `/System/Library/...` paths supplied by the sealed operating
system. Homebrew, MacPorts, Xcode, Command Line Tools, user, and application
paths are rejected. The validator also checks `LC_ID_DYLIB` and `LC_RPATH`, so
an apparently local executable cannot retain an ambient fallback search path.

## Qualified selection

The first selected AArch64 distribution with the address profile has toolchain
content-tree identity
`6f3dc859b8aee177db86879b7e7503e8bfbf8b5013ee0b745ab9db3502e0ad1f`.
The minimal SDK has content-tree identity
`253fb9bad05f1a1abaacbf54cc642227a76def2c2dfd58839db0f8d5eafc5cb6`.
These identities include paths, file kinds, permissions, bytes, and safe
relative symlink spellings; the physical parent directory is not included.

Qualification runs both native integration programs with
`DRAFT_TEST_LOCKED_TOOLCHAIN_ROOT` and `DRAFT_TEST_LOCKED_SDK_ROOT` set. The
conformance program builds and executes the complete 17-program matrix,
including inline and package assembly. The determinism program builds every
artifact kind twice and compares complete output trees byte for byte. Ordinary
CTest runs keep their installed-toolchain host gate, so both paths remain
covered.

Address-profile qualification additionally resolves and executes the
`examples/validation` test and benchmark under the locked profile, verifies
that the executable loads the deployed runtime only through
`@rpath`/`@executable_path`, and requires a later locked build to select those
exact evidence-v2 attempts. A deliberate heap use-after-free separately aborts
under ASan and is symbolized to its logical Draft source line by the pinned
`llvm-symbolizer`; the failed attempt is stored as revoked evidence.

The selected binaries are release inputs, not source-repository contents. A
release publisher should distribute the already assembled trees or retrieve
them by the content identities above; users should not have to reproduce this
assembly procedure for an ordinary locked build.
