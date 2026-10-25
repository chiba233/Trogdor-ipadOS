/* SPDX-License-Identifier: BSD-2-Clause */
.section .text.head, "ax"
.global _start
_start:
    b entry
    .long 0
    .quad 0                 /* text_offset: 2 MiB aligned load base */
    .quad _image_end         /* includes BSS and stack, not just file bytes */
    .quad 0x0a              /* little endian, 4K, relocatable placement */
    .quad 0, 0, 0
    .ascii "ARM\x64"
    .long 0

.text
entry:
    msr daifset, #15
    mov x19, x0             /* keep the physical FDT pointer */
    mrs x20, CurrentEL
    msr spsel, #1
    adrp x9, _stack_top
    add x9, x9, :lo12:_stack_top
    mov sp, x9
    adrp x9, _bss_start
    add x9, x9, :lo12:_bss_start
    adrp x10, _bss_end
    add x10, x10, :lo12:_bss_end
1:  cmp x9, x10
    b.hs 2f
    stp xzr, xzr, [x9], #16
    b 1b
2:  adrp x9, vectors
    add x9, x9, :lo12:vectors
    cmp x20, #8
    b.eq from_el2
    cmp x20, #4
    b.ne halt
    msr vbar_el1, x9
    mrs x21, sctlr_el1
    mov x22, #0
    b el1_entry

from_el2:
    msr vbar_el2, x9
    msr vbar_el1, x9
    mrs x21, sctlr_el2
    mrs x22, hcr_el2
    /* AArch64 EL1, no stage-2 translation. EL3/BL31 stays installed. */
    mov x9, #0x80000000
    msr hcr_el2, x9
    mov x9, #0x33ff         /* Armv8 CPTR_EL2 RES1, no FP trapping */
    msr cptr_el2, x9
    msr mdcr_el2, xzr
    mrs x9, cnthctl_el2
    orr x9, x9, #3         /* EL1 physical timer and counter access */
    msr cnthctl_el2, x9
    msr cntvoff_el2, xzr
    mov x9, #0x0800
    movk x9, #0x30d0, lsl #16  /* SCTLR_EL1 RES1, M/C/I disabled */
    msr sctlr_el1, x9
    msr ttbr0_el1, xzr
    msr ttbr1_el1, xzr
    msr tcr_el1, xzr
    adrp x9, _stack_top
    add x9, x9, :lo12:_stack_top
    msr sp_el1, x9
    adr x9, el1_entry
    msr elr_el2, x9
    mov x9, #0x3c5         /* DAIF masked, EL1h */
    msr spsr_el2, x9
    isb
    eret

el1_entry:
    isb
    mov x0, x19
    mov x1, x20
    mov x2, x21
    mov x3, x22
    bl probe_main
halt:
    wfe
    b halt

/* An exception is terminal. Preserve ESR/ELR/FAR without UART recursion. */
.macro vector slot
    .balign 128
    mov x3, #\slot
    b exception_entry
.endm
.balign 2048
vectors:
    vector 0
    vector 1
    vector 2
    vector 3
    vector 4
    vector 5
    vector 6
    vector 7
    vector 8
    vector 9
    vector 10
    vector 11
    vector 12
    vector 13
    vector 14
    vector 15
    .balign 128
exception_entry:
    mrs x4, CurrentEL
    cmp x4, #8
    b.eq 3f
    mrs x0, esr_el1
    mrs x1, elr_el1
    mrs x2, far_el1
    b 4f
3:  mrs x0, esr_el2
    mrs x1, elr_el2
    mrs x2, far_el2
4:  bl probe_exception
    b halt
