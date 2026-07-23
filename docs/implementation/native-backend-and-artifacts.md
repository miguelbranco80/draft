# Native backend and artifacts

This document records the bootstrap compiler's target-independent MIR to LLVM representation choices and the concrete artifact/debug contracts layered above target ABI semantics.

## In-process LLVM boundary

Status: LLVM 22 C-API object and assembly emission implemented and qualified.

The bootstrap links the shared LLVM 22 distribution selected at CMake time.
Each semantic package produces one complete LLVM module containing its globals
and all concrete runtime procedures. That module is parsed and verified in a
fresh `LLVMContext`, checked against the selected Draft target triple and fixed
data-layout string, and emitted through a task-owned target machine into an
in-memory object or assembly buffer. A disagreement between LLVM's computed
layout and the versioned Draft profile is a compiler/toolchain error; LLVM never
supplies missing language or ABI facts.

The adapter exposes no LLVM reference outside its module. Its process-global
AArch64 and X86 registries are initialized once, while contexts, modules, target machines,
pass options, messages, and output buffers have one explicit call lifetime. The
linked distribution must report thread support so isolated package-module calls
can be scheduled concurrently without sharing an LLVM context.

The former external Clang IR compilation path remains only as a low-level
qualification oracle. Ordinary commands never select it and never run
`clang --version`. Native evidence records the LLVM version compiled into
`draftc`; the linker driver, package assembler, sanitizer runtime, `llvm-ar`, and
`dsymutil` default to tools from that same selected LLVM installation. Platform
SDKs, startup objects, and system libraries remain operational host inputs.

## Native optimization boundary

Status: explicit O0 and O2 package-module pipelines implemented.

Optimization begins only after the compiler has emitted and verified one
complete LLVM module per semantic package. O0 performs no LLVM middle-end
transformation and creates the target machine with `LLVMCodeGenLevelNone`. O2
runs LLVM's version-selected `default<O2>` pipeline over that complete module,
then creates code using `LLVMCodeGenLevelDefault`. AddressSanitizer, when
selected by validation, runs after optimization so it instruments the memory
operations which survive into code generation.

Each native worker owns the module, target machine, pass options, and output
buffer for the complete operation. Different packages therefore remain safe to
optimize concurrently while procedures inside one package are visible to the
same LLVM pipeline. There is no cross-package LTO and no user-supplied pass
string. The external Clang qualification route receives the corresponding
`-O0` or `-O2` spelling so parity tests compare equivalent optimization modes.

Optimization is artifact configuration rather than Draft program meaning. It
does not enter the semantic product graph, source/resolved-program identity,
synthesis context, target profile, or package granularity. The canonical LLVM
module retained by the compiler and printed by `emit-llvm` is pre-optimization;
object and assembly bytes are derived from a task-private parsed copy. This
keeps the inspectable lowering stable while allowing repeated O0 and O2 builds
from the same checked graph.

Test and benchmark harnesses use the same native option. Because their evidence
asserts facts about an executed binary, the validation policy identity records
O0 or O2 and prevents runs at different levels from sharing one evidence key.
This derived-execution distinction does not enter the resolved-program digest.

## SysV AMD64 C ABI lowering

Status: fixed scalar, aggregate, callback, import, and export lowering implemented.

The target-independent C ABI table retains one classification row for every
semantic type. SysV aggregate rows contain at most two INTEGER/SSE eightbytes.
A second ordered procedure plan consumes those rows with the ABI's six-GPR and
eight-XMM budgets, including the hidden result pointer. Keeping placement in a
signature product is necessary because a two-component argument which cannot
fit completely must revert to memory without consuming a partial assignment.
The classifier remains the single source for native interop validation, header
emission, LLVM declarations, definitions, calls, and returns.

The LLVM adapter expands register aggregates into physical component
parameters and reconstructs the logical Draft value in bounded stack storage.
An exhausted or intrinsically indirect parameter uses `byval` with the exact
logical type and alignment; a large result uses `sret`. Scratch storage rounds
up to complete physical eightbytes because the last XMM carrier can be wider
than the meaningful source bytes. Component accesses retain the alignment
actually guaranteed by the aggregate base. Unit tests compare Clang-shaped IR
contracts and compile them through the embedded X86 backend; native x86-64 CI
closes the boundary with the independent generated-header C client.

## Win64 C ABI lowering

Status: fixed scalar, aggregate, callback, import, and export lowering implemented.

Microsoft x64 uses one integer carrier only for C records of exactly 1, 2, 4,
or 8 bytes. Other record parameters are pointers and other record results use
`sret`; record members do not create SysV-style INTEGER/SSE classes. The
Clang-compatible 128-bit integer extension is a separate ABI class because its
contract is asymmetric: the parameter is a pointer to 16-byte storage, while
the result is LLVM `<2 x i64>`. The adapter uses bounded scratch storage to
translate both directions without changing ordinary Draft `i128`/`u128`
procedure representation.

The same address carrier applies when a 128-bit C integer appears in an
unnamed variadic tail. MIR retains the promoted logical scalar; the emitter
reserves one entry-block 16-byte slot, stores the value, and passes that slot's
address in the variadic call. Other aggregate variadic arguments remain a
semantic error.

Classifier tests cover record and wide-integer decisions, emitted-IR tests
compile the exact public signatures through LLVM's X86 backend, and native
Windows CI closes the boundary with the independently compiled generated-header
C client.

## C variadic LLVM lowering

Status: promoted scalar/pointer tails implemented for every current native ABI.

Foreign declarations retain LLVM's real `(fixed, ...)` function type. At each
call, MIR contains the fixed operands followed by already-promoted unnamed
operands with their exact scalar or pointer types. The textual adapter prints
the complete variadic function type at the call site, as LLVM requires, and
does not attach fixed-parameter extension attributes to the unnamed tail.
LLVM's selected target lowering then owns the materially different Darwin,
GNU AArch64, and SysV AMD64 register/stack placement. This removes the former per-target
`open(2)` assembly shim without moving ABI policy into `core/os`.

Aggregate tails fail during semantic checking. The fixed-parameter aggregate
classifier is intentionally not reused for them because C variadic aggregate
rules are a separate target ABI contract.

## Raw string-data lowering

Status: explicit zero-copy MIR and LLVM lowering implemented.

HIR `raw_data` lowers to one `MirInstructionKind::RawData` instruction with one
string operand and a `[^]u8` result. MIR verification checks that exact shape,
so a malformed producer cannot silently turn an unrelated aggregate into a
pointer. The instruction is retained as a visible semantic operation instead
of being disguised as a cast or delegated to a runtime helper.

Draft strings use the physical `{data, length}` view fixed by the target
profile. LLVM lowering extracts field zero from that already-materialized value
and returns the pointer unchanged. It emits no helper call, allocation, copy,
terminator, encoding conversion, or length adjustment; a sliced string has
already adjusted the extracted address. The ordinary borrow lifetime and
non-writability contract therefore remain exactly those established by semantic
checking.

## Deterministic native work graph

Status: bounded parallel emission and ordered publication implemented.

Once semantic closure and MIR lowering have completed, each package owns one
`PackageLlvmModule` product and an explicit `ArtifactLayout` product. Its
canonical order is the complete package module followed by selected package
assembly. The backend freezes one task for every row in those package layouts.
The native graph is intentionally edgeless: each package module defines its own
storage and procedures, declares dependency imports, and can be emitted without
another package module. Final linking combines package and assembly objects.
The layout row retains the exact semantic producer for every native input.
LLVM owns section and relocation placement inside one isolated object; its
verified object bytes are the task result. The coordinator owns only the
canonical cross-object publication and linker order, so it never reconstructs
or guesses LLVM's internal section graph.

The shared work scheduler uses at most the requested worker bound, or the
host-reported hardware concurrency capped to the task count. Each LLVM task
owns a fresh context, module, target machine, and in-memory output buffer. Each
assembler or Clang-oracle task uses task-private input/output paths and launches
through `posix_spawnp`, avoiding unsafe post-`fork` C++ work in a multithreaded
process. Workers mutate only their matching result slots and never touch the
diagnostic sink or timing recorder.

After every started task joins, the command thread replays timing records,
selects the lowest-ID failure, and publishes successful products in task-ID
order. A failed ready set publishes no canonical native object. Successful
publication fixes assembly filenames, object/link argument order, and archive
member order independently of worker completion order. One-worker and
four-worker qualification builds compare every artifact tree byte for byte.

The external Clang oracle is also exercised against the same real compiled
graphs for executable, relocatable object, static library, dynamic library, and
assembly output. Both routes must produce the expected native container, and
both executable results are launched. LLVM and Clang output bytes need not be
identical when the target contract permits incidental encoding differences.

## Relocatable aggregate constants

Status: bootstrap backend representation; language layout is unchanged.

Named Draft structs use packed LLVM bodies plus explicit byte padding so LLVM's
field indices reproduce the already-computed Draft offsets exactly. This LLVM
representation does not imply Draft `packed` semantics: semantic HIR/MIR carry
the actual occurrence alignment, and every emitted load/store uses that value.
An under-aligned logical `u32`, for example, is still represented as `i32` but
is accessed with `align 1` when its containing field guarantees only one byte.

A struct containing `bits(N)` fields instead uses exact opaque byte-array
storage. LLVM has no addressable aggregate field beginning at an arbitrary bit.
The backend therefore emits bytewise extraction and read-modify-write from the
explicit MIR bit range, with byte zero contributing the low bits regardless of
target scalar endianness. Signed fields extend from their declared width;
writes truncate to that width and preserve unowned bits in the first and last
bytes. A partial write freezes the loaded containing byte before masking, so
writing one field into deliberate `---` storage defines the selected bits
without letting LLVM poison contaminate them. Runtime composite construction
and constant-byte encoding follow the same checked member bit offsets.

Strings and concrete procedure identities contain linker relocations and cannot
be flattened into the byte-array storage used for variants and unions. An
array, tuple, struct, variant, or union constant may contain those values at any
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

Status: AArch64 Mach-O plus AArch64/x86-64 ELF artifact contracts implemented.

The root package module owns runtime support for every final native artifact,
but only executable compilation adds the hosted C `main`. This lets
an exported C wrapper use `runtime.default_context` from an object, archive, or
dylib without requiring a fake Draft entry procedure. Ordinary Draft procedures
and globals have hidden native visibility; only explicit C exports retain
default visibility.

Object output performs a relocatable link over all package and package-assembly
objects. Static output always uses deterministic archive metadata: Apple
`libtool -static -D` for Mach-O and `llvm-ar rcsD` for ELF. Mach-O dynamic
output fixes an `@rpath/<filename>`
install name; ELF dynamic output fixes a filename SONAME. ELF executables use
the host system's startup objects, loader, glibc, and GCC runtime through the
selected LLVM Clang driver and lld. Relocatable ELF output disables Clang's PIE
default before `-r`; final links retain a deterministic SHA-1 GNU build ID.
An ELF executable or dynamic library consuming an explicit shared foreign
provider records `$ORIGIN` as a runtime search directory. This supports the
repository's vendored sibling-library layout without encoding the provider's
physical build path or requiring ambient `LD_LIBRARY_PATH`.
Assembly output is a directory bundle with one compiler-produced source for
each semantic package module plus exact copied external assembly inputs,
avoiding local-label collisions that concatenation could create.
Generated C headers cover root-package exports and transitively required C
records, unions, enums, fixed-array fields, and callback types. Layout
assertions make size, alignment, and field-offset disagreement a C compile error.

A `c enum` without an explicit backing follows the selected LP64 C compiler's
default enum rule instead of Draft's smallest-fitting rule. Its backing
is at least 32 bits: a wholly nonnegative member set uses `u32`, a set containing
a negative member uses `i32`, and either widens with the same signedness to 64
bits when required. Values that do not fit `u64` or `i64` are rejected. The
generated header exposes the selected fixed-width typedef, so C and Draft agree
on both the physical ABI and the backing value domain. An explicit backing
continues to use the separately validated fixed-backing C-enum contract. Enum
macros use the `<stdint.h>` exact-width constant forms through 64 bits; 128-bit
values are assembled from two `UINT64_C` halves, avoiding out-of-range decimal
tokens and the nonexistent `INT128_C` facility.

## Native Mach-O debug companions

Status: implemented for executable and dynamic-library artifacts.

Mach-O final links retain a debug map rather than copying input-object DWARF
into the executable or dylib. A successful native build therefore runs the
matching LLVM `dsymutil` before it reports success and publishes the conventional
sibling `<artifact>.dSYM`.

The invocation ignores object and Swift-module timestamps, uses one worker, and
verifies its linked output. The returned native result carries the bundle path.
Object, archive, and assembly artifacts keep their debug data in their object
members or `.loc` directives and therefore do not receive a misleading
final-link companion. The compiler does not walk or hash the bundle: it is a
derived native product, not a Draft program input.

LLVM dsymutil also emits `Contents/Resources/Relocations`, a rewriting cache
whose YAML records the physical binary path. That cache is unnecessary for
symbolizing the already-linked binary and is removed before publication. The
standard `Info.plist` and `Contents/Resources/DWARF` payload remain with logical
Draft compilation directory/file coordinates.

The resolver's macOS native acceptance case checks the complementary
generated-source path. After four declaration/member/expression/statement pins
are replayed without a provider, it links and runs the program and requires a
nonempty linked DWARF payload. Persistent synthesis-site identities remain in
resolution metadata; the backend does not fabricate native labels solely to
duplicate them. This is a real host-toolchain/dSYM gate and remains part of
release qualification.

## Native ELF debug information

Status: implemented for AArch64 and x86-64 Linux executable and shared-library artifacts.

ELF final links retain input-object DWARF directly in the executable or
shared library. They therefore do not run `dsymutil` or publish an empty
platform-shaped companion. The native result leaves its separate debug-symbol
fields empty, while the primary artifact remains unstripped and carries
`.debug_info`, `.debug_line`, logical Draft source coordinates, and its
deterministic GNU build ID. A future split-debug profile must be a separate
explicit artifact contract rather than an ambient `objcopy` convention.

## Native PE/COFF artifacts and debug information

Status: artifact construction implemented with a native Windows build/launch
gate; full validation/determinism harness parity remains pending.

The x86-64 Windows adapter emits AMD64 COFF package objects in process, invokes
matching Clang/LLD for `.exe` and `.dll` links, and uses `llvm-lib` for
deterministic `.lib` archives. C exports use LLVM `dllexport`, so a DLL link
also publishes an explicit import library. Linked outputs request lld-link
`/Brepro` and full CodeView debug information in a sibling PDB. The result
returns the companion paths without reading or hashing their bytes.

Tools are launched with `CreateProcessW` and a restricted inherited-handle
list. Argument conversion/quoting preserves the exact UTF-8 operational paths
without a shell, and process timing uses Windows user/kernel time. This keeps
parallel package-assembly tasks from leaking unrelated compiler handles.

COFF provides no relocatable partial link. A one-input object graph can publish
its exact `.obj`; a provider-free multi-package or package-assembly object set
must be represented by `--kind static-library`. Mapped providers remain
separate by the existing artifact contract and must instead participate in a
final executable/DLL link or be supplied by the consumer. The adapter rejects
impossible shapes rather than labeling archive bytes as an object.

## Native integration gates

Status: complete harness required on AArch64 macOS, AArch64 Linux, and x86-64
Linux; native artifact/application smoke required on x86-64 Windows.

The native integration executables select the target matching the host pair:
Apple Silicon exercises `draft-aarch64-macos-v5`, AArch64 Linux exercises
`draft-aarch64-linux-gnu-v1`, and x86-64 Linux exercises
`draft-x86_64-linux-gnu-v1`. Every host compiles complete example packages,
builds and runs executables, provokes the selected trap path, repeats every
artifact build to compare its complete output tree, and compile a C client
against the generated header and shared library. Mach-O cases additionally
require the `.dSYM` companion; ELF cases require its absence because their DWARF
stays in the primary artifact.

The native matrix also runs the embedded-LLVM/external-Clang parity gate for all
five artifact kinds and the one-worker/four-worker determinism gate. Those tests
make the retained oracle executable, prove actual multi-package overlap, and
keep stable publication a required behavior rather than a comment-only design.

Windows CI uses a separate driver-level gate while the validation runner and
POSIX-oriented C++ launch harnesses are ported. It builds the MSVC bootstrap
against LLVM-C, builds and launches every ordinary example except the
signal-classification trap fixture, exercises exact COFF package assembly and a
mapped foreign object, publishes object/archive/EXE/DLL/PDB/import-library
forms, and compiles and runs the independent C client. This proves the hosted
runtime/core path without overstating validation evidence or byte-for-byte
artifact determinism on Windows.

These gates use the declared host toolchain and prove that current source
composes into working native artifacts on each host. Reproducibility is tested
at the produced artifact boundary: repeated builds use the same explicit target
profile and compare complete output trees. Compiler and SDK installations are
not Draft program inputs, so pull-request and release testing need no vendored
toolchain archive or fabricated host-file hashes.
