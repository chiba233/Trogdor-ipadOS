/* SPDX-License-Identifier: BSD-2-Clause */
#include "xnu_checkpoint.h"
static int range(const struct xnu_expected *e,uint64_t va,uint64_t n) {
    return va>=e->virt && va-e->virt<=e->end-e->phys && n<=e->end-e->phys-(va-e->virt);
}
static const volatile uint8_t *address(const struct xnu_expected *e,uint64_t va) {
    return (const volatile uint8_t *)(uintptr_t)(e->phys+va-e->virt);
}
static void put(uint8_t *p,uint64_t value) { for (unsigned i=0;i<8;i++) p[i]=(uint8_t)(value>>(8*i)); }
static int same(const volatile uint8_t *a,const uint8_t *b,unsigned n) {
    for (unsigned i=0;i<n;i++) if (a[i]!=b[i]) return 0;
    return 1;
}
static int frame(const struct xnu_expected *e,uint64_t va,const uint64_t *want,unsigned n) {
    const volatile uint64_t *p=(const volatile uint64_t *)address(e,va);
    for (unsigned i=0;i<n;i++) if (p[i]!=want[i]) return 0;
    return 1;
}
int xnu_platform_valid(const struct xnu_expected *e) {
    uint64_t below=e->mode==7?0xf30u:0xe00u;
    if ((e->mode!=6 && e->mode!=7) || e->end<e->phys || e->end-e->phys>XNU_ARENA_BYTES ||
        e->sp0<below || (e->sp0&15) || !range(e,e->sp0-below,4096) ||
        !range(e,e->pe_state,0xb0) || (e->pe_state&7) || !range(e,e->protection,4) ||
        !range(e,e->dt_root,28) || !range(e,e->dt_cursor,8) || (e->dt_root&7) || (e->dt_cursor&7) ||
        !range(e,e->tree,e->tree_size) || e->tree_size>65536 || e->node>e->tree_size ||
        52>e->tree_size-e->node || e->cursor!=e->node+52) return 0;
    uint8_t state[0xb0]={0},dt[28]={0},cursor[8],zero[64]={0};
    state[0]=1;state[0x89]=1;
    const char pixel[]="BBBBBBBBGGGGGGGGRRRRRRRR";
    for (unsigned i=0;i<sizeof(pixel);i++) state[0x38+i]=(uint8_t)pixel[i];
    put(state+0x98,e->tree);put(state+0xa0,e->const_args);put(state+0xa8,e->tree_size);
    put(dt,e->tree);put(dt+8,e->tree_size);put(dt+16,e->tree+e->tree_size);dt[24]=1;
    put(cursor,e->tree+(e->mode==7?56u:e->cursor));
    if (!same(address(e,e->pe_state),state,sizeof(state)) || !same(address(e,e->dt_root),dt,sizeof(dt)) ||
        !same(address(e,e->dt_cursor),cursor,8) || !same(address(e,e->protection),zero,4) ||
        !same(address(e,e->sp0-below),zero,64)) return 0;
    if (e->mode==7) return 1; /* Only arm_init remains active; checked by xnu_cinit_valid. */
    /* Complete active SoC, identify and PE frames. Retired recursive frames
       below SP are allowed; their maximum extent was bounded by the input. */
    uint64_t soc[8]={0},identify[22]={0},pe[8]={0};
    soc[3]=e->tree+e->node;soc[5]=e->const_args;soc[6]=e->sp0+0xe0;soc[7]=e->identify+0x24;
    identify[0x68/8]=e->pages;identify[0x70/8]=e->memory;identify[0x78/8]=e->phys;
    identify[0x80/8]=e->virt;identify[0x98/8]=e->const_args;
    identify[0xa0/8]=e->sp0+0x120;identify[0xa8/8]=e->pe_init+0xf4;
    pe[4]=e->const_args;pe[5]=e->cpu_data;pe[6]=e->sp0+0x1f0;pe[7]=e->c_entry+0x5b0;
    return frame(e,e->sp0,soc,8) && frame(e,e->sp0+0x40,identify,22) && frame(e,e->sp0+0xf0,pe,8);
}
