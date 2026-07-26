# Native Interop and Targets

Use this guide for target-sensitive Draft, C boundaries, assembly, native
artifacts, or platform core work. Read the normative
[`native interop specification`](../../../../docs/specification/04-native-interop.md),
the selected [`target profile`](../../../../docs/targets/), and the current
[`command reference`](../../../../docs/operations/command-reference.md) before
editing.
Those repository documents are available during repository editing. In an
isolated compiler Codex request their links are provenance only; use this guide
and the exact target, ABI, assembly, provider, and artifact facts supplied in
the request instead.

## Contents

- [Current target boundary](#current-target-boundary)
- [Selecting target-specific source](#selecting-target-specific-source)
- [Target introspection](#target-introspection)
- [Ordinary and C procedures](#ordinary-and-c-procedures)
- [C-signature legality](#c-signature-legality)
- [Foreign imports and providers](#foreign-imports-and-providers)
- [C variadic imports](#c-variadic-imports)
- [Exports and C callbacks](#exports-and-c-callbacks)
- [Fixed-signature wrapper pattern](#fixed-signature-wrapper-pattern)
- [Parsed inline assembly](#parsed-inline-assembly)
- [Package assembly](#package-assembly)
- [Native artifact kinds](#native-artifact-kinds)
- [Portability checklist](#portability-checklist)

## Current target boundary

The compiler currently selects four hosted native target profiles:

| CLI target | Profile identity | ABI and object format | Page size | File tag |
| --- | --- | --- | --- | --- |
| `aarch64-macos` | `draft-aarch64-macos-v6` | Darwin arm64, Mach-O | 16 KiB | `aarch64-macos` |
| `aarch64-linux` | `draft-aarch64-linux-gnu-v2` | GNU AAPCS64, ELF/glibc 2.39 | 4 KiB | `aarch64-linux` |
| `x86_64-linux` | `draft-x86_64-linux-gnu-v2` | SysV AMD64, ELF/glibc 2.39 | 4 KiB | `x86_64-linux` |
| `x86_64-windows` | `draft-x86_64-windows-msvc-v2` | Microsoft x64, COFF/UCRT | 4 KiB | `x86_64-windows` |

All are 64-bit little-endian native profiles, but they are not one generic
platform. C aggregate rules, narrow scalar extension, enum ABI, thread and
terminal records, readiness count types, open flags, object/linker behavior,
page size, and assembly profile identity differ.

macOS is the CLI compatibility default. Portable code should be checked
explicitly against all four profiles; native execution requires the matching
host toolchain and runtime. Other libcs and other operating systems are not
implemented Draft targets. Parsed inline assembly is
available only on AArch64; x86-64 still supports ordinary native code and exact
target-qualified package assembly.

Do not turn a current target absence into a language restriction. Put machine
facts in versioned target profiles, platform implementations in exact tagged
files, and temporary backend limitations in implementation-limit docs.

## Selecting target-specific source

Every direct package child with `.draft`, `.s`, `.S`, or `.asm` participates
unless excluded by its final target qualifier. Use exact file tags when a whole
implementation differs:

```text
platform@aarch64-macos.draft
platform@aarch64-linux.draft
platform@x86_64-linux.draft
platform@x86_64-windows.draft
native@aarch64-macos.s
native@aarch64-linux.s
native@x86_64-linux.s
native@x86_64-windows.s
```

Only the matching file is selected. Unqualified files participate on every
target. The tag is exact; there is no implicit “all Linux” or “all AArch64”
filename class.

Use compile-time `when` for a small declaration, member, or statement
difference inside otherwise shared source:

```draft
when target.os == .linux {
    Platform_Name :: "linux"
} else when target.os == .windows {
    Platform_Name :: "windows"
} else {
    Platform_Name :: "macos"
}
```

`when` parses every branch but only the selected branch contributes names and
receives semantic checking. It creates no runtime branch and no lexical scope.
Do not duplicate a complete subsystem behind one giant `when` when exact
target files make ownership clearer.

## Target introspection

The compile-time `target` value exposes:

```draft
target.identity
target.arch
target.os
target.abi
target.byte_order
target.object_format
target.file_tag
target.pointer_bits
target.page_size
target.has_feature("feature-name")
```

The categorical fields have distinct compiler-defined enum types:
`Target_Architecture`, `Target_Operating_System`, `Target_ABI`,
`Target_Byte_Order`, and `Target_Object_Format`. Compare each field only with a
matching contextual alternative or a value of that exact type; for example,
`target.os == .linux` is valid while `target.os == target.arch` is not.
`target.has_feature` requires a compile-time string, and an unknown
architecture feature name is a compile error. Prefer a profile fact to an
ambient host check. The host running `draftc` does not define program meaning.
Direct fields and `has_feature` results are compile-time scalar values, so code
may bind or return one like any other constant. The compiler materializes the
selected scalar; it never creates a runtime value for the whole `target` object.

Target-sensitive constants are evaluated and cached per selected profile.
Physical SDK paths, linker paths, cwd, and environment ordering must not enter
semantic identity or generated source.

## Ordinary and C procedures

An ordinary Draft `proc` has an implicit, non-null `^runtime.Context` argument.
The complete physical signature is lowered through the target C ABI; an
ABI-mandated indirect result slot may physically precede the context.
Ordinary calls and procedure pointers propagate Context automatically.

A `c proc` uses the target C ABI directly and has no hidden Context:

```draft
ordinary: proc(value: u32)
callback: c proc(value: u32)
```

The two procedure-pointer types are distinct and do not convert. Inside a
`c proc`:

- the built-in `context` value is unavailable;
- runtime `assert` is unavailable because it uses Context;
- an ordinary Draft procedure cannot be called directly;
- `runtime.default_context` and `runtime.call_with_context` form the explicit
  bridge when ordinary Draft code must be entered.

A foreign exception or `longjmp` crossing a Draft frame is undefined behavior.
Keep foreign callbacks small and make the Context bridge visible.

## C-signature legality

Draft 1 permits these types by value at a user C boundary:

- machine scalars with defined C lowering and `core/c_abi` scalar aliases;
- data pointers, `rawptr`, `cstring`, and C procedure pointers;
- recursively legal `c struct` and `c union` types;
- legal `c enum` types;
- fixed arrays as members of recursively legal C aggregates.

Pointers may point to opaque or non-C Draft types because only the pointer is
passed by value.

These are rejected by value:

- `string` and slices;
- tuples and variants;
- SIMD values;
- default-layout aggregates;
- `runtime.Context` in a user C signature.

Represent a string as `cstring` or explicit pointer plus length. Represent a
slice as pointer plus length. Define a purpose-built `c struct` for a stable
external layout. Never add C layout solely to silence a diagnostic
without auditing every member and all selected target ABIs.

Inside the compiler, C legality and lowering consume one published
target-specific ABI table. The semantic validator, generated C header, and LLVM
emitter are not independent authorities and must not rerun or duplicate ABI
classification. A new target or ABI rule therefore needs classifier oracle
tests, semantic-product dependency tests, and consumer tests on the same rows.

SysV AMD64 small aggregates use one or two INTEGER/SSE eightbytes. Their final
placement depends on the complete ordered signature: if every component cannot
fit the remaining six-GPR/eight-XMM budget, the whole argument is passed in
memory. Do not derive x86 aggregate lowering from size alone or omit the hidden
result pointer's GPR consumption.

Win64 uses a different rule on the same x86-64 architecture. A legal C record
uses one exact-width integer carrier only when its complete size is 1, 2, 4, or
8 bytes; member kinds never create an SSE record class. Every other nonempty
record parameter is indirect and every such result uses the hidden result
pointer. Thus 3- and 16-byte records are indirect, while an exact 8-byte record
uses one i64 carrier. Never reuse the SysV eightbyte or register-budget rule for
`--target x86_64-windows`.

On that target, Clang-compatible 128-bit C integers are also not ordinary
direct scalars: parameters pass by address and results use a `<2 x i64>`
carrier. This applies to `i128`, `u128`, endian storage equivalents, and C enums
with those fixed backings. It does not change ordinary Draft procedure ABI.
The same address carrier is used in a C variadic tail.

For a native input that reads but does not retain bytes, pass
`raw_data(text), len(text)`. This is zero-copy and not zero-termination. Draft's
`[^]u8` has no const qualifier, so the declaration, documentation, and foreign
implementation must agree not to mutate. If the native API retains the pointer,
the string's backing storage must outlive that retention; `raw_data` does not
extend it.

## Generated C headers

Generate a header for the selected library root and exact artifact target:

```sh
build/draftc emit-c-header project/library \
  --target x86_64-linux -o /tmp/library.h
```

Without `-o`, the header is written below the workspace's derived
`.draft/build/<target>/` tree. The header contains explicit root exports and the
C records, unions, enums, fixed arrays, and callback types reachable from their
signatures. It emits layout assertions; opaque pointer pointees that cannot be
rendered as C are exposed as `void *`. Types such as `_Float16`, `__int128`, or
explicitly aligned records may require the target's Clang-compatible C
extensions.

Always generate the header and native library with the same `--target`. Compile
the header with the intended C compiler, then link and run a real C client.
Header assertions check layout, but only the executable boundary checks symbol
spelling and target register classification.

## Foreign imports and providers

A foreign block declares procedure symbols from one semantic link provider:

```draft
import core/c_abi

foreign zlib {
    compress_bytes as "compress" :: c proc(
        destination: [^]u8,
        destination_length: ^c_abi.unsigned_long,
        source: [^]u8,
        source_length: c_abi.unsigned_long,
    ) -> c_abi.int
}
```

`zlib` is provider identity, not a source namespace. Declarations enter the
containing package. The optional `as "compress"` clause is the exact linker
symbol; the local Draft name remains `compress_bytes`.

The resolver or CLI maps non-system providers to an object, archive, or shared
library for the current invocation:

```sh
build/draftc build path/to/workspace/package \
  --provider zlib=archive:/absolute/path/to/libz.a
```

The CLI resolves the path against its current directory, validates a real
non-symlink regular file, and passes it to the linker. It neither hashes the
artifact nor records it in the source-resolution manifest. A separate explicit
`--provider-summary` can describe denial effects for the invocation. Do not
invent provider mappings for target-owned libc/runtime or package assembly.

[`examples/raylib-asteroids`](../../../../examples/raylib-asteroids/) is the
complete downloaded-and-included shared-provider pattern. Its focused binding
uses C records and `c proc` imports behind small ordinary wrappers, the vendored
raylib source builds as a dylib/so/DLL, and a headless memory-backend test proves
the provider can link and render on every supported host. Pass the canonical
real library file to `--provider`; provider inputs may not be
symlinks. On Windows the `.lib` import library is an `archive` provider input
and the matching DLL remains a runtime file beside the executable. ELF
executables and dynamic libraries linked with a shared provider record
`$ORIGIN`, so a copied `.so` can be discovered beside the final artifact.
Mach-O follows the provider's install name and normal loader/rpath rules.

`check` and `build` are provider-free in the agent sense: they never invoke
Codex, rerun judgments, or change generated-source pins. “Provider” in native
linking means an explicit artifact supplier, not an elaboration model.

Foreign data symbols are not supported. Use an explicit procedure or a
fixed-signature wrapper supplied by the provider.

## C variadic imports

Bare final `..` declares a real C variadic tail:

```draft
foreign libc {
    ioctl :: c proc(
        descriptor: c_abi.int,
        request: c_abi.unsigned_long,
        ..,
    ) -> c_abi.int
}
```

The marker is valid only in `c proc`, must follow at least one fixed parameter,
and has no binding name. It is structural procedure-type identity and can be
queried with `type_is_variadic`. Draft cannot define or export a C variadic
body.

Supply the fixed prefix positionally before the tail. Untyped integers become
C `int`/i32, narrow integers and bool promote to i32, and f16/f32 promote to
f64. Wider C scalars, data pointers, `cstring`, and C procedure pointers retain
their type. The current backend rejects strings, slices, ordinary Draft
procedure pointers, distinct values, and every aggregate tail. Use a fixed
wrapper for those cases. A bare `nil` has no tail type; cast it to the required
pointer type first. `values: ..type` is instead a compile-time Draft pack; `...`
is always synthesis.

## Exports and C callbacks

`export` creates a linker-visible C definition:

```draft
export add_for_c as "draft_add" :: c proc(left, right: u32) -> u32 {
    return left + right
}
```

The `as "draft_add"` linker-name clause is optional. `pub` is independent: it controls Draft
package visibility and is neither implied by nor sufficient for C export.

To enter ordinary Draft from an exported `c proc`, obtain and pass Context:

```draft
import core/runtime

add_impl :: proc(left, right: u32) -> u32 {
    return left + right
}

export add_for_c as "draft_add" :: c proc(left, right: u32) -> u32 {
    ctx := runtime.default_context()
    return runtime.call_with_context(&ctx, add_impl, left, right)
}
```

`runtime.call_with_context` is compiler-specialized for the callback's exact
arguments and result. It is not a variadic native function despite the narrow
source declaration in `core/runtime`. The Context pointer must be non-null and
live for the call. The bridge does not retain it and does not permanently
replace the thread's default Context.

On a foreign-created thread, the bridge establishes Draft TLS as needed.
Foreign hosts must still obey the generated header, provider, artifact, and
lifetime contracts.

## Fixed-signature wrapper pattern

Macro-shaped APIs, foreign data, and aggregate variadic arguments cannot use
the direct procedure-import surface. Supply a tiny C or package-assembly
wrapper with one fixed signature:

```c
int draft_wifexited(int status) {
    return WIFEXITED(status);
}
```

```draft
import core/c_abi

foreign process_helpers {
    process_exited as "draft_wifexited" :: c proc(
        status: c_abi.int,
    ) -> c_abi.int
}
```

The wrapper should normalize only the ABI shape. Keep application policy and
error interpretation in Draft. Give every wrapper an explicit owner, build
input, target selection rule, and test.

## Parsed inline assembly

The current parsed assembly architecture is AArch64. An AArch64 target owns
one versioned grammar; inspect
[`aarch64-macos-assembly.md`](../../../../docs/targets/aarch64-macos-assembly.md)
for the closed instruction, operand, register, and feature vocabulary. The
AArch64 Linux profile has a distinct identity even where its initial grammar
matches.
The x86-64 profile has no parsed dialect and rejects every selected `asm`
construct. Use compile-time `when target.arch == .aarch64` only when a complete
program deliberately provides a non-assembly x86 alternative.

Expression form declares a result and ordered outputs:

```draft
increment :: proc(value: u64) -> u64 {
    return asm aarch64 -> u64 {
        in x0 = value
        out x0

        add x0, x0, #1
    }
}
```

Statement form has no result and cannot declare value-producing outputs.

Draft 1 parsed assembly is straight-line. It has:

- typed inputs and ordered outputs;
- fixed registers, immediates, and typed memory operands;
- explicit register, flags, and memory clobbers;
- volatile regions ordered relative to other volatile regions.

It does not have labels, branches, calls, stack-pointer changes, unwinding, or
arbitrary raw assembly text. Every written register must be an output or
clobber. Written flags require `clobber flags`. Undeclared memory effects
require `clobber memory`; that clobber is a compiler dependency, not atomic
synchronization.

More complex routines belong in package assembly.

## Package assembly

Direct package children ending in `.s`, `.S`, or `.asm` are exact,
non-preprocessed bytes selected like Draft source. In particular, `.S` does
not gain the host C driver's preprocessing semantics.

Use package assembly for labels, branches, native calls, platform directives,
or wrappers outside parsed inline assembly. Its boundary to Draft remains an
explicit `foreign`/`export` C ABI symbol. Assembly cannot name private Draft
symbols or bypass package/provider accounting.

Keep target-specific assembly in exact `@aarch64-macos`, `@aarch64-linux`,
`@x86_64-linux`, and `@x86_64-windows` files. Symbol prefixes, visibility,
section directives, unwind metadata, and object conventions differ between
Mach-O, ELF, and COFF even when instructions look identical.

## Native artifact kinds

`draftc build` supports:

```text
executable
object
static-library
dynamic-library
assembly
```

Example:

```sh
build/draftc build path/to/workspace/package \
  --target aarch64-linux \
  --kind static-library \
  -o /tmp/libexample.a
```

The source-level `foreign` and `export` declarations do not change between
static and dynamic linking. Artifact kind selects how resolved symbols are
packaged.

For non-assembly native builds, O0 emits internal linker-input objects from
package units through linked LLVM 22. O2 prepares one summary module per
semantic package and obtains package-associated native outputs from one
whole-artifact ThinLTO operation. Final
executables and libraries also include the exact target's hosted-runtime object
embedded in `draftc`. `--kind object` deliberately leaves runtime and foreign
references unresolved; it publishes one relocatably linked package-graph object
on Mach-O and ELF. COFF can publish one exact single-package/no-assembly `.obj`;
a multi-package or package-assembly object set must use
`--kind static-library` because COFF has no relocatable partial link. Mapped
providers remain separate and require a final executable/DLL link or
consumer-side linking. `--kind assembly` publishes package and hosted-runtime
`.s` files instead. The output directory is one replaceable artifact: a hidden
Draft marker owns its exact leaf filenames, the next build removes that set,
and a nonempty unmarked directory or marked directory with unrelated entries
is rejected rather than deleted.
macOS additionally requires the Apple linker/SDK and `libtool`; a
`--debug-symbols` build also uses matching LLVM `dsymutil` and publishes a
verified `.dSYM`. Linux uses the matching Clang/LLVM tools, `ld.lld`, `llvm-ar`,
and the selected glibc development contract; requested DWARF remains in ELF and
no fake dSYM is emitted. These are compiler build/host requirements, not Draft
package dependencies or resolution-manifest inputs.
Windows uses matching Clang/lld-link and `llvm-lib`, with `.exe`, `.obj`,
`.lib`, and `.dll` defaults. Linked PE outputs built with `--debug-symbols`
publish a sibling PDB; a DLL also publishes its import `.lib` companion. Its
bootstrap links the static LLVM LTO and AArch64/X86 component closure because
LLVM's monolithic libLLVM dylib is unavailable on Windows and the C-only DLL
does not expose ThinLTO's summary and resolution APIs.

After complete lowering, one explicit artifact layout orders each package's
LLVM units followed, for the root, by the exact compiler-embedded hosted
runtime and then its package-assembly inputs. Retained LLVM text and any
compiler-assembly request use one complete input unit per package; native-only
O0 object builds may use fixed internal units. O2
instead adds one workspace ThinLTO task after all package summary inputs are
ready. Its LLVM backend threads own disjoint output buffers; ordinary O0 and
assembler workers own isolated LLVM contexts or private assembler paths. The
hosted-runtime row borrows immutable embedded bytes.
Diagnostics and artifact publication occur only after the join, in stable
task-ID order. Native changes must preserve the one-worker/four-worker
determinism gate and the real embedded LLVM/external-Clang parity gate for all
artifact kinds.

Assembly output is a directory bundle, not concatenated text. It contains
`package-<package-index>-unit-0.s` for each semantic package and
`hosted-runtime.s` for the selected target, plus
`package-<package-index>-assembly-<input-index><source-extension>` for each
selected package-assembly input. Ordinary source locations remain in native
debug information; no separate source-correlation artifact is emitted.
Deterministic output must not contain physical checkout paths, nondeterministic
archive metadata, or filesystem enumeration order.

## Portability checklist

- Check `--target aarch64-macos`, `--target aarch64-linux`,
  `--target x86_64-linux`, and `--target x86_64-windows`.
- Use exact target-tagged files for whole platform implementations.
- Use `target` facts, never host environment guesses, for compile-time choice.
- Audit every C-visible type recursively and generate/compile the C header.
- Keep ordinary `proc` and `c proc` pointer types distinct.
- Bridge Context explicitly in callbacks and exports.
- Use bare final `..` only for supported C scalar/pointer variadic tails.
- Replace C macros, foreign data, and aggregate variadic tails with
  fixed-signature wrappers.
- Declare every parsed-assembly input, output, flags write, and memory effect.
- Keep complex or directive-heavy code in target package assembly.
- Test ABI classification, symbol spelling, artifact kind, and an independent C
  client where the boundary is externally consumed.
- For a shared native provider, test both the exact linker input and runtime
  library discovery; a successful frontend check proves neither.
- Record new machine facts in a versioned target profile; do not scatter them
  through semantic analysis or ordinary application code.
