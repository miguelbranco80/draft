// GNU AAPCS64 passes the three fixed wrapper arguments in x0, w1, and w2. Its
// variadic rule leaves those same register assignments valid for open(2), so a
// tail branch preserves both the arguments and open's return value. ELF uses
// the source symbol spelling directly and the linker may resolve `open` through
// its normal PLT entry when producing a position-independent final artifact.
.text
.globl draft_os_open_fixed
.type draft_os_open_fixed, %function
.p2align 2
draft_os_open_fixed:
    .cfi_startproc
    b open
    .cfi_endproc
.size draft_os_open_fixed, .-draft_os_open_fixed
.section .note.GNU-stack,"",%progbits
