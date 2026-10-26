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
#if PROBE_LINUX_CHAIN
    b halt                  /* This chain must return Linux to factory EL2. */
#endif
    msr vbar_el1, x9
    mrs x21, sctlr_el1
    mov x22, #0
    b el1_entry

from_el2:
#if PROBE_LINUX_CHAIN
    mrs x23, cptr_el2
    mrs x24, mdcr_el2
    mrs x25, cnthctl_el2
    mrs x26, cntvoff_el2
    mrs x27, vbar_el2
#endif
    msr vbar_el2, x9
    msr vbar_el1, x9
    mrs x21, sctlr_el2
    mrs x22, hcr_el2
#if PROBE_XNU_CHECKPOINT
    adrp x10, xnu_saved
    add x10, x10, :lo12:xnu_saved
    stp x19, x20, [x10]
    stp x21, x22, [x10, #16]
    stp x23, x24, [x10, #32]
    stp x25, x26, [x10, #48]
    str x27, [x10, #64]
#endif
#if PROBE_RESET_EL == 2
    b probe_call
#endif
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
probe_call:
    mov x0, x19
    mov x1, x20
    mov x2, x21
    mov x3, x22
    bl probe_main
#if PROBE_XNU_CHECKPOINT
    bl xnu_prepare
    cbz x0, xnu_load_failed
    /* Snapshot the EL1 state; XNU will switch both stacks and translations. */
    adrp x10, xnu_el1_saved
    add x10, x10, :lo12:xnu_el1_saved
    mrs x11, sctlr_el1
    str x11, [x10]
    mrs x11, tcr_el1
    str x11, [x10, #8]
    mrs x11, ttbr0_el1
    str x11, [x10, #16]
    mrs x11, ttbr1_el1
    str x11, [x10, #24]
    mrs x11, mair_el1
    str x11, [x10, #32]
    mrs x11, vbar_el1
    str x11, [x10, #40]
    mrs x11, sp_el0
    str x11, [x10, #48]
    mov x11, sp
    str x11, [x10, #56]
    mrs x11, tpidr_el0
    str x11, [x10, #64]
    mrs x11, tpidrro_el0
    str x11, [x10, #72]
    mrs x11, par_el1
    str x11, [x10, #80]
    mrs x11, cpacr_el1
    str x11, [x10, #88]
    mrs x11, tpidr_el1
    str x11, [x10, #96]
    mov x9, x0
    mov x0, x1
    mov x1, xzr
    mov x2, xzr
    mov x3, xzr
    /* Mode4 makes every original C-prologue save independently checkable.
       The pinned cold path preserves x27/x28/x29; EL2 originals are saved. */
    adrp x10, xnu_expected
    add x10, x10, :lo12:xnu_expected
    ldr x10, [x10, #56]
    cmp x10, #4
    b.ne xnu_branch
    mov x27, xzr
    mov x28, xzr
    mov x29, xzr
xnu_branch:
    dsb sy
    br x9
xnu_load_failed:
#endif
#if PROBE_LINUX_CHAIN
    hvc #0x5452             /* Only our lower-EL sync vector accepts this. */
xnu_fallback_return:
#endif
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
#if PROBE_XNU_CHECKPOINT
    mov x18, x0             /* Preserve the C entry argument before ESR overwrites x0. */
#endif
    mrs x4, CurrentEL
    cmp x4, #8
    b.eq 3f
    mrs x0, esr_el1
    mrs x1, elr_el1
    mrs x2, far_el1
    b 4f
3:  mrs x0, esr_el2
#if PROBE_LINUX_CHAIN
    cmp x3, #8
    b.ne 5f
    ubfx x9, x0, #26, #6
    cmp x9, #0x16           /* HVC64, not SMC, abort, or an IRQ. */
    b.ne 5f
    and x9, x0, #0xffff
    mov x10, #0x5452
    cmp x9, x10
#if PROBE_XNU_CHECKPOINT
    b.ne 6f
    mrs x9, elr_el2
    adr x10, xnu_fallback_return
    cmp x9, x10
    b.eq linux_handoff
    b 5f
6:
#else
    b.eq linux_handoff
#endif
#if PROBE_XNU_CHECKPOINT
    mov x10, #0x584e
    cmp x9, x10
    b.eq xnu_checkpoint_return
#endif
5:
#endif
    mrs x1, elr_el2
    mrs x2, far_el2
4:  bl probe_exception
    b halt

#if PROBE_LINUX_CHAIN
linux_handoff:
    mov x0, x19
    bl probe_export_linux_log
    cbz w0, halt
    /* Restore the EL2 state changed solely for the EL1 probe. */
    msr hcr_el2, x22
    msr cptr_el2, x23
    msr mdcr_el2, x24
    msr cnthctl_el2, x25
    msr cntvoff_el2, x26
    msr vbar_el2, x27
    dsb sy
    isb
    adrp x9, _linux_entry
    add x9, x9, :lo12:_linux_entry
    mov x0, x19
    mov x1, xzr
    mov x2, xzr
    mov x3, xzr
    br x9
#endif

#if PROBE_XNU_CHECKPOINT
xnu_checkpoint_return:
    adrp x10, xnu_capture
    add x10, x10, :lo12:xnu_capture
    mrs x9, elr_el2
    str x9, [x10]
    mrs x9, spsr_el2
    str x9, [x10, #8]
    mrs x9, sctlr_el1
    str x9, [x10, #16]
    stp x20, x22, [x10, #24]
    stp x23, x24, [x10, #40]
    stp x25, x26, [x10, #56]
    str x18, [x10, #144]
    str x19, [x10, #152]
    mrs x9, cpacr_el1
    str x9, [x10, #160]
    mrs x9, tpidr_el1
    str x9, [x10, #168]
    str x30, [x10, #176]
    str x29, [x10, #192]
    mrs x9, tcr_el1
    str x9, [x10, #72]
    mrs x9, ttbr0_el1
    str x9, [x10, #80]
    mrs x9, ttbr1_el1
    str x9, [x10, #88]
    mrs x9, mair_el1
    str x9, [x10, #96]
    mrs x9, sp_el0
    str x9, [x10, #104]
    mrs x9, sp_el1
    str x9, [x10, #112]
    mrs x9, vbar_el1
    str x9, [x10, #120]
    ldr x9, [x10, #16]
    tbz x9, #0, 7f
    /* AT uses the still-active EL1 regime, while EL2 stays MMU-off. */
    adrp x11, xnu_expected
    add x11, x11, :lo12:xnu_expected
    ldp x9, x12, [x11, #8]   /* original args PA and linked kernel VA */
    ldr x13, [x11, #24]      /* owned physical kernel base */
    add x9, x9, x12
    sub x9, x9, x13
    at s1e1r, x9
    isb
    mrs x9, par_el1
    str x9, [x10, #128]
    mrs x9, elr_el2
    tbnz x9, #63, 8f
    add x9, x9, x22
    sub x9, x9, x23
8:  at s1e1r, x9
    isb
    mrs x9, par_el1
    str x9, [x10, #136]
    mrs x9, elr_el2
    tbz x9, #63, 7f
    mrs x9, sp_el0
    add x9, x9, #0x70
    at s1e1r, x9
    isb
    mrs x9, par_el1
    str x9, [x10, #184]
    adrp x11, xnu_expected
    add x11, x11, :lo12:xnu_expected
    ldr x9, [x11, #56]      /* mode, stored before XNU executes */
    cmp x9, #4
    b.eq xnu_clean_arena
    /* Invalidate the clean stack-read line before Linux can reuse this RAM.
       EL2 translation is off, so this cache-maintenance VA is physical. */
    mrs x9, sp_el0
    add x9, x9, #0x70
    add x9, x9, x23
    sub x9, x9, x22
    adrp x11, _xnu_window
    add x11, x11, :lo12:_xnu_window
    cmp x9, x11
    b.lo halt
    adrp x11, _image_end
    add x11, x11, :lo12:_image_end
    cmp x9, x11
    b.hs halt
    dsb sy
    dc civac, x9
    dsb sy
    b 7f
xnu_clean_arena:
    /* All C stores in the audited prefix stay inside this owned arena.
       EL2 M=0: these maintenance VAs equal physical addresses. Never use
       potentially clobbered EL1 registers as the range bounds. */
    adrp x9, _xnu_window
    add x9, x9, :lo12:_xnu_window
    adrp x11, _image_end
    add x11, x11, :lo12:_image_end
    mrs x12, ctr_el0
    ubfx x12, x12, #16, #4
    mov x13, #4
    lsl x12, x13, x12
    mov x13, #0
    dsb sy
xnu_clean_line:
    dc civac, x9
    add x9, x9, x12
    add x13, x13, #1
    cmp x9, x11
    b.lo xnu_clean_line
    dsb sy
    str x12, [x10, #200]
    str x13, [x10, #208]
7:
    /* Mode4 has drained all audited cached stores before disabling M/C/I. */
    mrs x9, sctlr_el1
    mov x11, #0x1005
    bic x9, x9, x11
    dsb sy
    msr sctlr_el1, x9
    isb
    tlbi vmalle1
    dsb sy
    isb
    bl xnu_checkpoint_complete
    cbz w0, halt
    adrp x10, xnu_el1_saved
    add x10, x10, :lo12:xnu_el1_saved
    ldr x9, [x10, #8]
    msr tcr_el1, x9
    ldr x9, [x10, #16]
    msr ttbr0_el1, x9
    ldr x9, [x10, #24]
    msr ttbr1_el1, x9
    ldr x9, [x10, #32]
    msr mair_el1, x9
    ldr x9, [x10, #40]
    msr vbar_el1, x9
    ldr x9, [x10, #48]
    msr sp_el0, x9
    ldr x9, [x10, #56]
    msr sp_el1, x9
    ldr x9, [x10, #64]
    msr tpidr_el0, x9
    ldr x9, [x10, #72]
    msr tpidrro_el0, x9
    ldr x9, [x10, #80]
    msr par_el1, x9
    ldr x9, [x10, #88]
    msr cpacr_el1, x9
    ldr x9, [x10, #96]
    msr tpidr_el1, x9
    ldr x9, [x10]
    msr sctlr_el1, x9
    isb
    adrp x10, xnu_saved
    add x10, x10, :lo12:xnu_saved
    ldp x19, x20, [x10]
    ldp x21, x22, [x10, #16]
    ldp x23, x24, [x10, #32]
    ldp x25, x26, [x10, #48]
    ldr x27, [x10, #64]
    b linux_handoff
.bss
.balign 16
.global xnu_saved
xnu_saved:
    .zero 80
.balign 16
xnu_el1_saved:
    .zero 112
#endif
