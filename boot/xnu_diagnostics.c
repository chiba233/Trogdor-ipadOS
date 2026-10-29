/* SPDX-License-Identifier: BSD-2-Clause */
#include "xnu_checkpoint.h"
/* Exact linked ranges of the audited reachable prefix, not whole text/data. */
static const struct { uint64_t start,size; } code[] = {
    {0xfffffff0073f205cull,0x330},{0xfffffff0073f6224ull,0x10},
    {0xfffffff0079b3704ull,0x5b0},{0xfffffff0073e95c4ull,4},
    {0xfffffff007a14990ull,0x1ac},{0xfffffff0073e562cull,0xc4},
    {0xfffffff007948ba0ull,0xf4},{0xfffffff007948080ull,0x24},
    {0xfffffff0079483dcull,0xb8},{0xfffffff007945e00ull,0x38},
    {0xfffffff007945e78ull,0x28},{0xfffffff007945ea0ull,0x194},
    {0xfffffff0073b7114ull,0x74},{0xfffffff0073b693cull,0x2c},
    {0xfffffff0073f6cc0ull,0x64},{0xfffffff0073f57a0ull,8},
    {0xfffffff0079463d4ull,0x18},{0xfffffff0079463ecull,0x16c}
};
uint64_t xnu_diagnostic_code_mask(uint64_t start,uint64_t bytes) {
    uint64_t mask=0;
    if (bytes>UINT64_MAX-start) return 0;
    for (unsigned i=0;i<sizeof(code)/sizeof(code[0]);i++)
        if (code[i].start>=start && code[i].start-start<=bytes &&
            code[i].size<=bytes-(code[i].start-start)) mask|=1ull<<i;
    return mask;
}
static int fault_pc(uint64_t pc) {
    if (pc&3) return 0;
    for (unsigned i=0;i<sizeof(code)/sizeof(code[0]);i++)
        if (pc>=code[i].start && pc-code[i].start<code[i].size) return 1;
    return 0;
}
static int owned(const struct xnu_expected *e,uint64_t pa,uint64_t size) {
    return pa>=e->phys && pa-e->phys<=e->end-e->phys && size<=e->end-pa;
}
int xnu_diagnostic_return_valid(const uint64_t *c,const uint64_t *pool,
        const struct xnu_expected *e,const uint64_t *f) {
    if (e->mode!=8 || (e->diagnostics!=9 && e->diagnostics!=10) ||
        e->end<e->phys || e->end-e->phys>XNU_ARENA_BYTES ||
        !owned(e,e->pages,65536) || !owned(e,e->args,4096) || !owned(e,e->pc,4) ||
        e->kernel_end<=e->virt || e->kernel_end-e->virt>e->end-e->phys ||
        e->vbar<e->virt || e->kernel_end-e->virt<0x204 ||
        e->vbar-e->virt>e->kernel_end-e->virt-0x204 || (e->vbar&2047) || f[4]>1) return 0;
    uint64_t pc=e->virt+(e->pc-e->phys),hvc=c[0];
    if (f[4]) {
        if ((hvc!=e->vbar+4 && hvc!=e->vbar+0x204) || (c[1]&31)!=5 ||
            ((f[3]&31)!=4 && (f[3]&31)!=5) ||
            hvc!=e->vbar+((f[3]&31)==4?4u:0x204u) || f[1]<e->virt || f[1]>=e->kernel_end || !fault_pc(f[1])) return 0;
        if (e->diagnostics==10 && (f[1]!=e->soc+0x34 || f[0]!=(0xf2000000ull|0x5846))) return 0;
    } else {
        if (e->diagnostics==10 || hvc!=pc+4 || (c[1]&31)!=4) return 0;
        for (unsigned i=0;i<4;i++) if (f[i]) return 0;
    }
    if ((c[2]&0x1005)!=0x1005 || c[9]!=e->tcr || c[10]!=e->pages ||
        c[11]!=e->pages+4096 || c[12]!=e->mair || c[15]!=e->vbar ||
        (c[16]&1) || (c[17]&1) ||
        (c[16]&0x0000fffffffff000ull)!=(e->args&0x0000fffffffff000ull) ||
        (c[17]&0x0000fffffffff000ull)!=((e->phys+hvc-e->virt)&0x0000fffffffff000ull) ||
        c[25]<4 || c[25]>131072 || (c[25]&(c[25]-1)) || c[26]!=XNU_ARENA_BYTES/c[25]) return 0;
    return xnu_tables_valid(pool,e);
}
