# Draft AArch64 parsed assembly profile

This document fixes the complete grammar of `draft-aarch64-apple-v2`, the
parsed inline-assembly dialect selected by `draft-aarch64-macos-v6`. It is a
small straight-line language, not an alias for every instruction accepted by
Apple's assembler. Anything not listed here is rejected before LLVM lowering.
External package `.s`, `.S`, and `.asm` files remain the unrestricted escape
hatch for labels, branches, calls, stack work, uncommon instructions, and
unwinding directives.

## Registers and effects

`in`, `out`, and register `clobber` directives use fixed `x0`-`x30`,
`w0`-`w30`, `b0`-`b31`, `h0`-`h31`, `s0`-`s31`, `d0`-`d31`, or `q0`-`q31`
spellings. `xzr` and `wzr` are legal instruction operands but not directives.
The stack pointer is never legal. A NEON instruction uses a whole-vector
`vN.8b`, `vN.16b`, `vN.4h`, `vN.8h`, `vN.2s`, `vN.4s`, or `vN.2d` operand;
the matching boundary directive uses `dN` for 64 bits or `qN` for 128 bits.
Element selection is not supported.

Every read register must have been initialized by an input or earlier write.
Every written register must be an output or clobber. Flag-reading instructions
must follow a flag-writing instruction in the same region, and any region that
writes flags declares `clobber flags`. The accepted conditions are `eq`, `ne`,
`cs`, `cc`, `mi`, `pl`, `vs`, `vc`, `hi`, `ls`, `ge`, `lt`, `gt`, and `le`.

## Integer and selection instructions

The scalar integer forms operate on consistently sized `w` or `x` registers:

- Three-operand arithmetic: `add`, `sub`, `adds`, and `subs`; the last operand
  is a register or an unsigned 12-bit immediate.
- Three-register logic and arithmetic: `and`, `ands`, `orr`, `eor`, `bic`,
  `orn`, `eon`, `mul`, `udiv`, and `sdiv`.
- Shifts and rotate: `lsl`, `lsr`, `asr`, and `ror`; the last operand is a
  same-width register or an immediate smaller than the register width.
- Carry arithmetic: `adc`, `adcs`, `sbc`, and `sbcs`.
- Multiply-add: `madd` and `msub` with four same-width registers.
- Unary and move: `mov`, `mvn`, `neg`, `negs`, `clz`, `cls`, `rbit`, `rev`,
  `rev16`, and `rev32`. `mov` also accepts an unsigned 16-bit immediate;
  `rev32` uses `x` registers.
- Flag setting: `cmp`, `cmn`, and `tst`. `cmp` and `cmn` accept a same-width
  register or unsigned 12-bit immediate; `tst` accepts a register.
- Selection: `csel`, `csinc`, `csinv`, `csneg`, `cset`, and `csetm`.

Whole-vector integer `add`, `sub`, `and`, `orr`, `eor`, `bic`, and `mul` are
also accepted. Bitwise operations use `.8b` or `.16b`. Multiply accepts every
listed arrangement except `.2d`; add and subtract accept every arrangement.
`dup` broadcasts a `w` register into byte, half, or word lanes, or an `x`
register into doubleword lanes.

## Floating-point instructions

Scalar `s` and `d` forms include `fadd`, `fsub`, `fmul`, `fdiv`, `fmin`,
`fmax`, `fminnm`, `fmaxnm`, `fmov`, `fneg`, `fabs`, and `fsqrt`. `fmov` also
permits the exact-width `w`/`s` and `x`/`d` bit transfers. `fcvt` converts
between scalar `s` and `d`; `scvtf`, `ucvtf`, `fcvtzs`, and `fcvtzu` convert
between general and scalar floating-point registers.

`fcmp` accepts two same-width scalar registers or a register and `#0.0`.
`fcsel` selects between two scalar values using flags initialized earlier in
the region. The binary floating-point operations also accept `.2s`, `.4s`, and
`.2d` whole-vector forms.

## Memory and barriers

The memory grammar has three non-writing address forms:

```text
[xN]
[xN, #unsigned-byte-offset]
[xN, #-signed-byte-offset]
```

`ldr` and `str` accept a naturally scaled nonnegative offset of at most 4095
elements. `ldur` and `stur` accept an unscaled offset from -256 through 255.
`ldrb`/`strb` and `ldrh`/`strh` use `w` values; `ldrsb` and `ldrsh` sign-extend
into `w` or `x`; `ldrsw` sign-extends into `x`. `ldar` and `stlr` accept a
base-only address and a `w` or `x` value.

`ldp` and `stp` accept two equally sized `w`/`x`, `s`/`d`, or `q` registers
and a signed scaled seven-bit offset. Pre-index, post-index, register offset,
and writeback addressing are deliberately absent. Every access is checked
against a typed pointer input of the matching element width unless the region
declares `clobber memory`.

The non-data instructions are `dmb`, `dsb`, `isb`, and `nop`. `dmb` and `dsb`
accept `sy`, `ish`, `ishld`, or `ishst`; `isb` accepts no operand or `sy`.
