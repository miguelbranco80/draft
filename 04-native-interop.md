# Draft: Native interop

Part of the [Draft language specification](README.md).

[← Design context and agent synthesis](03-agent-synthesis.md) ·
[Next: Denials and validation →](05-denials-validation.md)

<a id="section-11"></a>

## 11. Assembly

`asm` is a built-in parsed construct with expression and statement forms. The
parser, type checker, register allocator, source mapper, and backend understand
its operands and effects on machine state.

```draft
add :: proc(a, b: u64) -> u64 {
    return asm x86_64 -> u64 {
        in  rax = a
        in  rcx = b
        out rax
        clobber flags

        add rax, rcx
    }
}
```

The selected target profile names one versioned parsed-assembly architecture
and its instruction and operand grammar. `asm architecture` must match that
profile; unsupported architectures, instructions, operands, registers, or
features are compile errors.

The initial concrete grammar is recorded in
[the AArch64 parsed assembly profile](AARCH64_ASSEMBLY_PROFILE.md). That profile
is part of the compiler input rather than ambient host-assembler behavior.

Draft 1 assembly is straight-line and uses fixed registers, immediates, memory
operands, typed inputs, ordered outputs, and explicit register, flags, and memory
clobbers. Labels, branch instructions, calls, stack-pointer modification, and
unwinding are not supported. An assembly region is volatile: the compiler does
not remove or duplicate it, and volatile regions retain source order relative
to one another. Input/output dependencies constrain other motion; ordinary
memory may move across a region unless it declares `clobber memory`.

Every written register must appear as an `out` or register clobber, and written
flags require `clobber flags`. A typed memory operand is a parsed load or store
through a pointer supplied by an `in` operand; its pointee type supplies the
access size, alignment, and memory dependency. Any other memory access requires
`clobber memory`. An undeclared effect is a compile error.

An assembly instruction list may contain a synthesis site. Its expansion must
preserve the surrounding inputs, outputs, clobbers, and required target
features.

The result arrow distinguishes expression and statement forms. An
`asm architecture -> T { instructions }` expression has the declared result
type: one ordered `out` produces its value, while multiple ordered outputs
produce a matching tuple. An `asm architecture { instructions }` statement has
no result and may not declare value-producing `out` operands; externally visible
memory effects and machine-state clobbers remain explicit:

```draft
asm x86_64 {
    clobber memory

    mfence
}
```

More elaborate assembly, including labels and calls, belongs in package `.s`,
`.S`, or `.asm` files assembled and linked by the normal build.

<a id="section-12"></a>

## 12. C ABI and native linking

### Draft and C procedure ABIs

An ordinary `proc` is lowered by prepending a non-null `^runtime.Context` to its
logical parameter list and then lowering the complete signature through the
selected target's C ABI. Target-mandated hidden return parameters may therefore
physically precede the context argument. An ordinary procedure pointer is a
code pointer to that lowered signature; normal calls supply and propagate the
context automatically.

Compiler-generated calls always supply a non-null context. Passing `nil` through
`runtime.call_with_context` or the physical ordinary-procedure ABI is undefined
behavior; a constant `nil` is diagnosed.

The `c` modifier removes the hidden context argument and applies the target's C
ABI directly to `proc`:

```draft
callback: c proc(value: u8)
```

The built-in `context` value is unavailable inside a `c proc`. Ordinary and C
procedure-pointer types are distinct and do not convert. A `c proc` cannot call
an ordinary procedure directly; it must use `runtime.call_with_context`.
A foreign exception or `longjmp` that crosses a Draft frame has undefined
behavior.

A Draft 1 C signature may contain machine scalars with defined C lowering, C scalar
aliases, pointers, `cstring`, C procedure pointers, and `@repr(C)` structs, raw
unions, and enums. Slices, strings, tuples, tagged unions, SIMD values,
and default-layout aggregates are rejected by value; wrappers may pass them
through pointers or explicit C representations.

An `@repr(C)` aggregate is legal by value only when every member is recursively
C-ABI-legal; a fixed array is legal as a member when its element type is.
Pointers may point to opaque or non-C Draft types.

Compiler-defined `core/runtime` bridge intrinsics use the versioned runtime ABI
and are not user C imports or exports. This exception does not make
`runtime.Context` legal by value in a user `c proc` signature.

### Foreign imports

A `foreign` block declares C symbols supplied by a link provider:

```draft
import core/c as c

foreign zlib {
    zcompress :: c "compress" proc(dst: [^]u8, dst_len: ^usize,
                                    src: [^]u8, src_len: usize) -> c.int
}
```

The block name `zlib` is a link-provider identity used by resolution and
diagnostics; it is not a language namespace. Its declarations enter the
containing package normally. In the declaration above:

| Part | Meaning |
| --- | --- |
| `zcompress` | Local Draft declaration name used by source code. |
| `c` | C calling convention and ABI lowering. |
| `"compress"` | Exact symbol requested from the linker. |
| `proc(...) -> c.int` | Draft-visible signature of that symbol. |

The leading `c` is a Draft calling-convention modifier. The `c` in `c.int`
is merely the file-local alias introduced by `import core/c as c`; the shared
spelling is conventional and creates no semantic relationship.

If the quoted symbol is omitted, the linker symbol is the local declaration
name. The resolver or command line maps `zlib` to a system library, object file,
static archive, or dynamic library for the selected target. Choosing static or
dynamic linkage does not require a different source declaration or source-list
build file.

The bootstrap command-line spelling for a non-system mapping is
`--provider zlib=object|archive|shared-library:<absolute-path>`. Resolution pins
the provider identity, role, and exact artifact content; builds receive the
physical path separately and require it to match. Target-profile system
providers and compiler-owned runtime/package-assembly providers cannot be
overridden.

Draft 1 foreign blocks declare fixed-arity procedure symbols only. Variadic C calls
and foreign data symbols require a fixed-signature C wrapper.

### C exports

`export` defines an externally visible symbol implemented by the current
package. It uses the same local-name and optional-linker-name rule:

```draft
import core/c as c
import core/runtime

decode_for_c_impl :: proc(
    input: [^]u8,
    input_len: usize,
    output: [^]u8,
    output_len: usize,
) -> c.int {
    ... "validate the C buffers and call decode"
}

export decode_for_c :: c "jpeg_decode" proc(
    input: [^]u8,
    input_len: usize,
    output: [^]u8,
    output_len: usize,
) -> c.int {
    ctx := runtime.default_context()
    return runtime.call_with_context(
        &ctx,
        decode_for_c_impl,
        input, input_len, output, output_len,
    )
}
```

In `export decode_for_c :: c "jpeg_decode" proc(...)`, the parts have precise
roles:

| Part | Meaning |
| --- | --- |
| `export` | Emit an externally visible definition rather than a foreign import. |
| `decode_for_c` | Local Draft declaration name. |
| `c` | C calling convention and ABI lowering; no hidden `^runtime.Context`. |
| `"jpeg_decode"` | Exact exported linker symbol. |
| `proc(...) { ... }` | Draft signature and implementation body. |

Omitting `"jpeg_decode"` exports the local name `decode_for_c`. The quoted
string is only a linker-symbol alias: it is not an import alias, package name,
or second source-level declaration. `pub` controls visibility to importing
Draft packages independently and is not implied by `export`.

The build output kind chooses whether emitted exports live in an object file,
static archive, or dynamic library. The source `export` declaration is
unchanged; an object or archive contributes an external symbol when linked,
while a dynamic library exposes it according to the target's export-visibility
rules.

### Calling Draft code from C procedures

`runtime.call_with_context` is a context-free compiler intrinsic declared by
the runtime package and callable from a `c proc`. It accepts a
non-null `^runtime.Context`, an ordinary procedure, and its arguments; the
compiler specializes its argument and result types and invokes the procedure
with the supplied hidden context pointer. This is the explicit bridge used by
C callbacks and exported wrappers. `runtime.default_context` is a context-free
runtime intrinsic with signature `c proc() -> runtime.Context`. Foreign code
obtains compatible context state through versioned runtime bridge APIs; direct
host construction or field initialization is outside the portable Draft 1 ABI.

On a foreign-created thread, `runtime.default_context` and
`runtime.call_with_context` initialize Draft TLS if needed; that TLS remains
until thread exit. The host must keep the supplied `Context` live for the call,
and the bridge never retains its pointer afterward. Before either operation, a
`c proc` on an unattached foreign thread may not access Draft TLS.

### ABI artifacts

`@repr(C)` types, C integer aliases, symbol names, linkage strength, visibility,
and calling conventions have explicit lowering. The compiler emits object
files, static libraries, dynamic libraries, executables, C headers, assembly,
and LLVM IR.
