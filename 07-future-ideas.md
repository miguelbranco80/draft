# Draft: Future ideas

Part of the [Draft language specification](README.md).

[← Compiler architecture](06-compiler.md)

<a id="section-16"></a>

## 16. Future ideas

This section is non-normative.

### Additional layout forms

Packed aggregates, bit sets and bit fields, transparent wrappers, niche-based
unions, and SIMD forms beyond a target profile may be added after their layout,
alignment, and ABI rules are explicit.

### GPU procedures

GPU support may require only a small native language boundary. Devices, queues,
buffers, transfers, dispatch, synchronization, and graphics-pipeline construction
can remain ordinary library facilities. Compiling a procedure for a GPU still
requires compiler knowledge of its target, entry ABI, address spaces, execution
model, and permitted operations.

The main surface-design question is whether that boundary deserves one direct
modifier:

```draft
blur :: gpu proc(input: gpu.Buffer[Pixel], output: gpu.Buffer[Pixel]) {
    index := gpu.global_id()
    ...
}
```

`gpu proc` would parallel `c proc`: it changes procedure lowering while keeping
most functionality in packages. Invocation IDs, barriers, and similar
operations could be library-shaped compiler intrinsics. Host code would manage
memory movement, dispatch, and synchronization explicitly.

The existing `asm` construct could become target-aware inside a GPU procedure,
although GPU instruction formats are backend- and vendor-specific. Existing
`...` synthesis, denials, target identities, tests, judgments, and benchmark
evidence could apply directly to generated kernels. Compute procedures are the
smallest useful starting point; graphics shader stages can build on the same
target model if needed.

### Raw-string assembly escape hatch

A future unsafe assembly form may accept backend- or target-assembler text for
instructions not yet supported by parsed `asm`. It would still require explicit
typed inputs, outputs, and clobbers. Draft 1 deliberately defines no raw-string
dialect or inline syntax; separate `.s`, `.S`, and `.asm` package files cover
that gap without coupling the language surface to LLVM or one assembler.
