// Exact Darwin AArch64 package assembly. Draft invokes the assembler without
// a C preprocessor; the Mach-O object symbol carries the platform underscore.
.section __TEXT,__text,regular,pure_instructions
.globl _draft_external_add
.p2align 2
_draft_external_add:
    add x0, x0, x1
    ret
