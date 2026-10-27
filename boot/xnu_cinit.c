/* SPDX-License-Identifier: BSD-2-Clause */
#include "xnu_checkpoint.h"
/* Access only the prevalidated Image-owned arena, after CIVAC and M/C/I off. */
static int range(const struct xnu_expected *e,uint64_t va,uint64_t size) {
    return va>=e->virt && e->end>=e->phys && va-e->virt<=e->end-e->phys &&
        size<=e->end-e->phys-(va-e->virt);
}
static uint64_t word(const struct xnu_expected *e,uint64_t va) {
    return *(const volatile uint64_t *)(uintptr_t)(e->phys+va-e->virt);
}
int xnu_cinit_valid(const struct xnu_expected *e) {
    if ((e->mode<4 || e->mode>5) || (e->phys&7) || (e->virt&7) || e->end<e->phys ||
        e->end-e->phys>XNU_ARENA_BYTES || (e->sp0&15) || e->sp0<16 ||
        !range(e,e->sp0-16,0xe0) || !range(e,e->const_args,0x480) ||
        !range(e,e->boot_args,8) || !range(e,e->slide,8) || !range(e,e->static_cache,1) ||
        !range(e,e->guard,8) || (e->const_args&7) || (e->boot_args&7) || (e->slide&7) || (e->guard&7) ||
        e->args<e->phys || e->args>e->end || 0x480>e->end-e->args) return 0;
    const volatile uint8_t *original=(const volatile uint8_t *)(uintptr_t)e->args;
    const volatile uint8_t *copy=(const volatile uint8_t *)(uintptr_t)(e->phys+e->const_args-e->virt);
    for (unsigned i=0;i<0x480;i++) if (copy[i]!=original[i]) return 0;
    if (word(e,e->boot_args)!=e->const_args || word(e,e->slide)!=0 || word(e,e->guard)!=0 ||
        *(const volatile uint8_t *)(uintptr_t)(e->phys+e->static_cache-e->virt)!=0) return 0;
    /* All original prologue saves, locals, guard and the popped memmove
       child frame. x27/x28/x29 were normalized before the cold path;
       x19 was set to the C-entry LR by its audited TTBR helper sequence. */
    uint64_t frame[28]={0};
    frame[0]=e->sp0+0xc0;frame[1]=e->c_entry+0x58c;
    frame[(0x80+16)/8]=e->flags;frame[(0x88+16)/8]=e->pages;
    frame[(0x90+16)/8]=e->memory;frame[(0x98+16)/8]=e->phys;
    frame[(0xa0+16)/8]=e->virt;frame[(0xb0+16)/8]=e->args;
    frame[(0xb8+16)/8]=e->c_entry;frame[(0xc8+16)/8]=e->c_entry;
    for (unsigned i=0;i<28;i++) if (word(e,e->sp0-16+i*8)!=frame[i]) return 0;
    return 1;
}
