// This is exact target assembly. Draft invokes the assembler without a C
// preprocessor for .s, .S, and .asm alike. Darwin's object symbol has a leading
// underscore even though the C-ABI linker name in Draft does not.
.section __TEXT,__text,regular,pure_instructions
.globl _draft_external_add
.p2align 2
_draft_external_add:
    add x0, x0, x1
    ret
