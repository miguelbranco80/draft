# Native Interop and Targets

Use this guide for target-sensitive Draft, C boundaries, assembly, native
artifacts, or platform core work. Read the normative
[`native interop specification`](../../../../docs/specification/04-native-interop.md),
the selected [`target profile`](../../../../docs/targets/), and the current
[`command reference`](../../../../docs/operations/command-reference.md) before
editing.

## Contents

- [Current target boundary](#current-target-boundary)
- [Selecting target-specific source](#selecting-target-specific-source)
- [Target introspection](#target-introspection)
- [Ordinary and C procedures](#ordinary-and-c-procedures)
- [C-signature legality](#c-signature-legality)
- [Foreign imports and providers](#foreign-imports-and-providers)
- [Exports and C callbacks](#exports-and-c-callbacks)
- [Fixed-arity wrapper pattern](#fixed-arity-wrapper-pattern)
- [Parsed inline assembly](#parsed-inline-assembly)
- [Package assembly](#package-assembly)
- [Native artifact kinds](#native-artifact-kinds)
- [Portability checklist](#portability-checklist)

## Current target boundary

The compiler currently emits native code for exactly two target profiles:

| CLI target | Profile identity | ABI and object format | Page size | File tag |
| --- | --- | --- | --- | --- |
| `aarch64-macos` | `draft-aarch64-macos-v5` | Darwin arm64, Mach-O | 16 KiB | `aarch64-macos` |
| `aarch64-linux` | `draft-aarch64-linux-gnu-v1` | GNU AAPCS64, ELF/glibc 2.39 | 4 KiB | `aarch64-linux` |

Both are 64-bit little-endian AArch64 with baseline NEON, but they are not one
generic platform. C aggregate rules, narrow scalar extension, enum ABI,
pthread layouts, open flags, object/linker behavior, page size, and assembly
profile identity differ.

macOS is the CLI compatibility default. Portable code should be checked
explicitly against both profiles. x86-64 hosts can build and sanitize the C++
bootstrap, but the current backend does not emit x86-64 Draft artifacts.
Windows, x86 native output, other libcs, and other operating systems are not
implemented Draft targets.

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
native@aarch64-macos.s
native@aarch64-linux.s
```

Only the matching file is selected. Unqualified files participate on every
target. The tag is exact; there is no implicit “all Linux” or “all AArch64”
filename class.

Use compile-time `when` for a small declaration, member, or statement
difference inside otherwise shared source:

```draft
when target.os == .linux {
    Platform_Name :: "linux"
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

- machine scalars with defined C lowering and `core/c` scalar aliases;
- data pointers, `rawptr`, `cstring`, and C procedure pointers;
- recursively legal `@repr(C)` structs and raw unions;
- legal `@repr(C)` enums;
- fixed arrays as members of recursively legal C aggregates.

Pointers may point to opaque or non-C Draft types because only the pointer is
passed by value.

These are rejected by value:

- `string` and slices;
- tuples and tagged unions;
- SIMD values;
- default-layout aggregates;
- `runtime.Context` in a user C signature.

Represent a string as `cstring` or explicit pointer plus length. Represent a
slice as pointer plus length. Define a purpose-built `@repr(C)` record for a
stable external layout. Never add `@repr(C)` solely to silence a diagnostic
without auditing every member and both target ABIs.

Inside the compiler, C legality and lowering consume one published
target-specific ABI table. The semantic validator, generated C header, and LLVM
emitter are not independent authorities and must not rerun or duplicate ABI
classification. A new target or ABI rule therefore needs classifier oracle
tests, semantic-product dependency tests, and consumer tests on the same rows.

For a native input that reads but does not retain bytes, pass
`raw_data(text), len(text)`. This is zero-copy and not zero-termination. Draft's
`[^]u8` has no const qualifier, so the declaration, documentation, and foreign
implementation must agree not to mutate. If the native API retains the pointer,
the string's backing storage must outlive that retention; `raw_data` does not
extend it.

## Foreign imports and providers

A foreign block declares fixed-arity procedure symbols from one semantic link
provider:

```draft
import core/c as c

foreign zlib {
    compress_bytes :: c "compress" proc(
        destination: [^]u8,
        destination_length: ^c.unsigned_long,
        source: [^]u8,
        source_length: c.unsigned_long,
    ) -> c.int
}
```

`zlib` is provider identity, not a source namespace. Declarations enter the
containing package. The optional quoted string is the exact linker symbol; the
local Draft name remains `compress_bytes`.

The resolver or CLI maps non-system providers to an exact object, archive, or
shared library:

```sh
build/draftc build path/to/workspace --root package \
  --provider zlib=archive:/absolute/path/to/libz.a
```

Provider artifact bytes are resolved inputs. A separate audited
`--provider-summary` can describe denial effects. Do not invent provider
mappings for target-owned libc/runtime or package assembly.

`check` and `build` are provider-free in the agent sense: they never invoke
Codex, rerun judgments, or change generated-source pins. “Provider” in native
linking means an explicit artifact supplier, not an elaboration model.

Draft foreign blocks do not support variadic procedures or foreign data
symbols. Use an explicit fixed-signature wrapper.

## Exports and C callbacks

`export` creates a linker-visible C definition:

```draft
export add_for_c :: c "draft_add" proc(left, right: u32) -> u32 {
    return left + right
}
```

The quoted linker name is optional. `pub` is independent: it controls Draft
package visibility and is neither implied by nor sufficient for C export.

To enter ordinary Draft from an exported `c proc`, obtain and pass Context:

```draft
import core/runtime

add_impl :: proc(left, right: u32) -> u32 {
    return left + right
}

export add_for_c :: c "draft_add" proc(left, right: u32) -> u32 {
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

## Fixed-arity wrapper pattern

Many C APIs are variadic or macro-shaped. Draft cannot import those directly.
Supply a tiny C or package-assembly wrapper with one fixed signature:

```c
int draft_open_fixed(const char *path, int flags, unsigned mode) {
    return open(path, flags, mode);
}
```

```draft
foreign package_assembly {
    native_open :: c "draft_open_fixed" proc(
        path: cstring,
        flags: c.int,
        mode: c.unsigned_int,
    ) -> c.int
}
```

The wrapper should normalize only the ABI shape. Keep application policy and
error interpretation in Draft. Give every wrapper an explicit owner, build
input, target selection rule, and test.

## Parsed inline assembly

The current parsed assembly architecture is AArch64. The selected target owns
one versioned grammar; inspect
[`aarch64-macos-assembly.md`](../../../../docs/targets/aarch64-macos-assembly.md)
for the closed instruction, operand, register, and feature vocabulary. The
Linux profile has a distinct identity even where its initial grammar matches.

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

Keep target-specific assembly in exact `@aarch64-macos` and
`@aarch64-linux` files. Symbol prefixes, visibility, section directives,
unwind metadata, and object conventions differ between Mach-O and ELF even
when instructions look identical.

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
build/draftc build path/to/workspace --root package \
  --target aarch64-linux \
  --kind static-library \
  -o /tmp/libexample.a
```

The source-level `foreign` and `export` declarations do not change between
static and dynamic linking. Artifact kind selects how resolved symbols are
packaged.

The bootstrap emits package objects through its linked LLVM 22 C-API adapter.
macOS additionally requires the Apple linker/SDK, `libtool`, and the matching
LLVM `dsymutil`; applicable executable/shared outputs publish a verified
`.dSYM`. Linux uses the matching Clang/LLVM tools, `ld.lld`, `llvm-ar`, and the
selected glibc development contract; DWARF remains in ELF and no fake dSYM is
emitted. These are compiler build/host requirements, not Draft package
dependencies or resolution-manifest inputs.

After complete lowering, package modules and package-assembly inputs are
independent native work-graph tasks. Workers own isolated LLVM contexts or
private assembler paths and return task-indexed bytes. Diagnostics and artifact
publication occur only after the join, in stable task-ID order. Native changes
must preserve the one-worker/four-worker determinism gate and the real embedded
LLVM/external-Clang parity gate for all artifact kinds.

Assembly output is a directory bundle, not concatenated text. Native builds
also emit source-correlation metadata. Deterministic output must not contain
physical checkout paths, nondeterministic archive metadata, or filesystem
enumeration order.

## Portability checklist

- Check both `--target aarch64-macos` and `--target aarch64-linux`.
- Use exact target-tagged files for whole platform implementations.
- Use `target` facts, never host environment guesses, for compile-time choice.
- Audit every C-visible type recursively and generate/compile the C header.
- Keep ordinary `proc` and `c proc` pointer types distinct.
- Bridge Context explicitly in callbacks and exports.
- Replace variadic or macro C APIs with fixed-signature wrappers.
- Declare every parsed-assembly input, output, flags write, and memory effect.
- Keep complex or directive-heavy code in target package assembly.
- Test ABI classification, symbol spelling, artifact kind, and an independent C
  client where the boundary is externally consumed.
- Record new machine facts in a versioned target profile; do not scatter them
  through semantic analysis or ordinary application code.
