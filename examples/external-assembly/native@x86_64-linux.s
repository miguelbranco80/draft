# Exact GNU/ELF x86-64 package assembly. SysV AMD64 passes the two unsigned
# words in rdi/rsi and returns the sum in rax. The type/size directives and
# non-executable-stack note keep ELF symbol and linker behavior explicit.
.text
.globl draft_external_add
.type draft_external_add, @function
.p2align 4
draft_external_add:
    leaq (%rdi,%rsi), %rax
    ret
.size draft_external_add, .-draft_external_add
.section .note.GNU-stack,"",@progbits
