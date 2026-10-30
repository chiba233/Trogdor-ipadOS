/* SPDX-License-Identifier: BSD-2-Clause */
.text
.global display_read32_abi
display_read32_abi:
    stp x19, x20, [sp, #-96]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    mov x19, #19
    mov x20, #20
    mov x21, #21
    mov x22, #22
    mov x23, #23
    mov x24, #24
    mov x25, #25
    mov x26, #26
    mov x27, #27
    mov x28, #28
    mov x29, #29
    bl display_read32
    cmp x19, #19
    b.ne abi_bad
    cmp x20, #20
    b.ne abi_bad
    cmp x21, #21
    b.ne abi_bad
    cmp x22, #22
    b.ne abi_bad
    cmp x23, #23
    b.ne abi_bad
    cmp x24, #24
    b.ne abi_bad
    cmp x25, #25
    b.ne abi_bad
    cmp x26, #26
    b.ne abi_bad
    cmp x27, #27
    b.ne abi_bad
    cmp x28, #28
    b.ne abi_bad
    cmp x29, #29
    b.eq abi_restore
abi_bad:
    mov w0, #2
abi_restore:
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp x19, x20, [sp], #96
    ret
.global display_inject
display_inject:
    msr esr_el1, x0
    msr elr_el1, x1
    msr far_el1, x2
    msr spsr_el1, x3
    mov x3, x4
    mov x0, x5
    isb
    b display_fault_dispatch
