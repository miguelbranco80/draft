# Exact COFF AMD64 package assembly. Win64 passes the two words in rcx/rdx and
# returns the sum in rax. `.def` publishes external-function storage class and
# type metadata expected by COFF tools; no platform underscore is added.
.text
.globl draft_external_add
.def draft_external_add; .scl 2; .type 32; .endef
.p2align 4
draft_external_add:
    leaq (%rcx,%rdx), %rax
    retq
