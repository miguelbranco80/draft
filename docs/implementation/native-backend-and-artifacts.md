# Native backend and artifacts

This document records the bootstrap compiler's target-independent MIR to LLVM representation choices and the concrete artifact/debug contracts layered above target ABI semantics.

## In-process LLVM boundary

Status: LLVM 22 C-API object and assembly emission implemented and qualified.

The bootstrap links the shared LLVM 22 distribution selected at CMake time.
Each package module is parsed and verified in a fresh `LLVMContext`, checked
against the selected Draft target triple and fixed data-layout string, and
emitted through a task-owned target machine into an in-memory object or assembly
buffer. A disagreement between LLVM's computed layout and the versioned Draft
profile is a compiler/toolchain error; LLVM never supplies missing language or
ABI facts.

The adapter exposes no LLVM reference outside its module. Its process-global
AArch64 registry is initialized once, while contexts, modules, target machines,
pass options, messages, and output buffers have one explicit call lifetime. The
linked distribution must report thread support so isolated package calls can be
scheduled concurrently without sharing an LLVM context.

The former external Clang IR compilation path remains only as a low-level
qualification oracle. Ordinary commands never select it and never run
`clang --version`. Native evidence records the LLVM version compiled into
`draftc`; the linker driver, package assembler, sanitizer runtime, `llvm-ar`, and
`dsymutil` default to tools from that same selected LLVM installation. Platform
SDKs, startup objects, and system libraries remain operational host inputs.

## Relocatable aggregate constants

Status: bootstrap backend representation; language layout is unchanged.

Strings and concrete procedure identities contain linker relocations and cannot
be flattened into the byte-array storage used for unions. An array, tuple,
struct, tagged union, or raw union constant may contain those values at any
nesting depth.
The LLVM backend walks the checked Draft layout, writes every non-relocatable
subtree as exact bytes, and retains each string or procedure leaf as a typed
field at its semantic byte offset. The allocation uses an initializer-specific
packed LLVM type; opaque pointers let all reads and external declarations keep
using the canonical Draft type. This representation changes neither size,
alignment, offsets, nor the public ABI and avoids imposing an LLVM aggregate
shape on the language type system. Global initializers use that storage
directly. A relocation-bearing MIR constant receives equivalent private module
storage and a canonical typed load at its source operation, because LLVM SSA
literals cannot carry the initializer-specific packed type.

## Native artifact ownership and visibility

Status: AArch64 Mach-O and ELF artifact contracts implemented.

The root LLVM module owns runtime support for every final native artifact, but
only executable compilation adds the hosted C `main`. This lets an exported C
wrapper use `runtime.default_context` from an object, archive, or dylib without
requiring a fake Draft entry procedure. Ordinary Draft procedures and globals
have hidden native visibility; only explicit C exports retain default visibility.

Object output performs a relocatable link over all package and package-assembly
objects. Static output always uses deterministic archive metadata: Apple
`libtool -static -D` for Mach-O and `llvm-ar rcsD` for ELF. Mach-O dynamic
output fixes an `@rpath/<filename>`
install name; ELF dynamic output fixes a filename SONAME. ELF executables use
the host system's startup objects, loader, glibc, and GCC runtime through the
selected LLVM Clang driver and lld. Relocatable ELF output disables Clang's PIE
default before `-r`; final links retain a deterministic SHA-1 GNU build ID.
Assembly output is a directory
bundle with one compiler-produced source per package and exact copied external
assembly inputs, avoiding local-label collisions that concatenation could create.
Generated C headers cover root-package exports and transitively required C
records, raw unions, enums, fixed-array fields, and callback types. Layout
assertions make size, alignment, and field-offset disagreement a C compile error.

An `@repr(C)` enum without an explicit backing follows the selected AArch64 C
compiler's default enum rule instead of Draft's smallest-fitting rule. Its backing
is at least 32 bits: a wholly nonnegative member set uses `u32`, a set containing
a negative member uses `i32`, and either widens with the same signedness to 64
bits when required. Values that do not fit `u64` or `i64` are rejected. The
generated header exposes the selected fixed-width typedef, so C and Draft agree
on both the physical ABI and the backing value domain. An explicit backing
continues to use the separately validated fixed-backing C-enum contract. Enum
macros use the `<stdint.h>` exact-width constant forms through 64 bits; 128-bit
values are assembled from two `UINT64_C` halves, avoiding out-of-range decimal
tokens and the nonexistent `INT128_C` facility.

## Native source correlation sidecar

Status: canonical operation map implemented; runtime instrumentation remains a
validation-profile concern.

Every LLVM debug marker now publishes the same row to an implementation-owned
`draft-source-correlation-v1` sidecar. A source-addressable row is identified by
package, canonical MIR procedure ordinal, and emission ordinal, describes the
procedure spelling plus MIR instruction or terminator, and carries both
generated and authored file/line/column coordinates. The ordinal is necessary
because multiple concrete specializations may retain one source procedure
spelling.
Generated rows additionally carry the persistent synthesis-site identity.
Filenames are logical basenames under the separately recorded package identity;
physical checkout paths never enter the native artifact or sidecar.

The native adapter writes the canonical JSON only after every package has a
valid LLVM module and returns its SHA-256 digest beside the native output. A
normal resolved build binds the map to the resolved-program digest. The lower
level backend API can deliberately compile a checked graph before resolution;
that form binds the map to a digest of the exact, package-framed LLVM module set
instead of inventing a resolved-program identity. The sidecar remains derived
output in both cases, avoiding a circular program identity.

The sidecar is the common join boundary for future counter-based coverage and
sampling profiles. Sanitizer, race, allocator-poisoning, and coverage runtimes
still require explicit versioned validation profiles; this map does not pretend
that an unrequested instrument ran.

## Native Mach-O debug companions

Status: implemented for executable and dynamic-library artifacts.

Mach-O final links retain a debug map rather than copying package-object DWARF
into the executable or dylib. A successful native build therefore runs the
host `dsymutil` before it reports success and publishes the conventional
sibling `<artifact>.dSYM`.

The invocation ignores object and Swift-module timestamps, uses one worker, and
verifies its linked output. The returned native result carries both the bundle path and its
canonical content-tree digest. Object, archive, and assembly artifacts keep
their debug data in their object members or `.loc` directives and therefore do
not receive a misleading final-link companion.

LLVM dsymutil also emits `Contents/Resources/Relocations`, a rewriting cache
whose YAML records the physical binary path. That cache is unnecessary for
symbolizing the already-linked binary and is removed before hashing or
publication. The standard `Info.plist` and `Contents/Resources/DWARF` payload
remain, and their logical Draft compilation directory/file coordinates agree
with `draft-source-correlation-v1`. Compiler content v121 first made the
expanded native artifact contract explicit without changing resolved source
semantics; the current compiler-content version retains that contract.

The resolver's macOS native acceptance case now checks the complementary
generated-source path. After four declaration/member/expression/statement pins
are replayed without a provider, it links and runs the program and requires the
executable expression and statement site identities in both
`draft-source-correlation-v1` and the linked DWARF payload. Declaration-only
and layout-only pins are not required to fabricate machine operations. This is
a real host-toolchain/dSYM gate and remains part of release qualification.

## Native ELF debug information

Status: implemented for AArch64 Linux executable and shared-library artifacts.

ELF final links retain package-object DWARF directly in the executable or
shared library. They therefore do not run `dsymutil` or publish an empty
platform-shaped companion. The native result leaves its separate debug-symbol
fields empty, while the primary artifact remains unstripped and carries
`.debug_info`, `.debug_line`, logical Draft source coordinates, the source
correlation sidecar, and its deterministic GNU build ID. A future split-debug
profile must be a separate explicit artifact contract rather than an ambient
`objcopy` convention.

## Native integration gates

Status: required on AArch64 macOS and AArch64 Linux CI hosts.

The native integration executables select the target matching the host pair:
Apple Silicon exercises `draft-aarch64-macos-v5`, while AArch64 Linux exercises
`draft-aarch64-linux-gnu-v1`. Both hosts compile complete example packages,
build and run executables, provoke the target `BRK` trap path, repeat every
artifact build to compare its complete output tree, and compile a C client
against the generated header and shared library. Mach-O cases additionally
require the `.dSYM` companion; ELF cases require its absence because their DWARF
stays in the primary artifact.

These gates use the declared host toolchain and prove that current source
composes into working native artifacts on each host. Reproducibility is tested
at the produced artifact boundary: repeated builds use the same explicit target
profile and compare complete output trees. Compiler and SDK installations are
not Draft program inputs, so pull-request and release testing need no vendored
toolchain archive or fabricated host-file hashes.
