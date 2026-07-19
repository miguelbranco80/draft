// Exact GNU/ELF AArch64 package assembly. The C linker symbol is spelled
// directly, and the size/type directives keep ELF symbol tooling precise.
.text
.globl draft_external_add
.type draft_external_add, %function
.p2align 2
draft_external_add:
    add x0, x0, x1
    ret
.size draft_external_add, .-draft_external_add
.section .note.GNU-stack,"",@progbits
