// Darwin's open(2) declaration is variadic, while Draft 1 foreign declarations
// are intentionally fixed-arity. This adapter gives Draft a stable three-
// argument C ABI: x0 is the pathname, w1 the flags, and w2 the creation mode.
// Passing the third argument unconditionally is valid; open ignores it unless
// O_CREAT is set.
//
// Apple AArch64 passes variadic arguments on the stack even when another fixed
// argument would still fit in a register. The wrapper therefore cannot simply
// tail-branch to _open: it must place the promoted mode value in the first
// eight-byte variadic slot. This is exactly the sequence emitted by Clang for
// an equivalent fixed-signature C wrapper.
//
// The public C/linker name is `draft_os_open_fixed`. Mach-O object symbols add
// one leading underscore, hence the spelling below. Error reporting remains
// exactly as open(2) defines it: a negative descriptor denotes failure.
.section __TEXT,__text,regular,pure_instructions
.globl _draft_os_open_fixed
.p2align 2
_draft_os_open_fixed:
    .cfi_startproc
    sub sp, sp, #32
    stp x29, x30, [sp, #16]
    add x29, sp, #16
    .cfi_def_cfa w29, 16
    .cfi_offset w30, -8
    .cfi_offset w29, -16
    str x2, [sp]
    bl _open
    ldp x29, x30, [sp, #16]
    add sp, sp, #32
    ret
    .cfi_endproc
