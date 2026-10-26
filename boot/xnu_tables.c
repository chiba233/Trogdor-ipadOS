/* SPDX-License-Identifier: BSD-2-Clause */
#include "xnu_checkpoint.h"
/* Compare the full pool without following untrusted table pointers. */
int xnu_tables_valid(const uint64_t *pool,const struct xnu_expected *e) {
    struct record { uint64_t word,value; } records[32];
    unsigned used=0,page=2;
    uint64_t count=e->memory>>21,physical=e->phys&~0x1fffffull;
    if (e->mode!=2 || !count || count>12 || e->phys>=(1ull<<38) ||
        e->virt>>38!=0x3ffffff || (e->phys&0x1fffff)!=(e->virt&0x1fffff) ||
        e->end>physical+(count<<21)) return 0;
    for (unsigned root=0;root<2;root++) {
        uint64_t va=root?e->virt:e->phys,last=UINT64_MAX,l2=0;
        for (uint64_t i=0;i<count;i++) {
            uint64_t cursor=va+(i<<21),index=(cursor>>30)&255;
            if (index!=last) {
                if (page>=16 || used>=32) return 0;
                l2=page++;
                records[used++]=(struct record){root*512+index,(e->pages+l2*4096)|3};
                last=index;
            }
            if (used>=32) return 0;
            records[used++]=(struct record){l2*512+((cursor>>21)&511),physical+(i<<21)+0x601};
        }
    }
    for (unsigned word=0;word<8192;word++) {
        uint64_t want=0;
        for (unsigned i=0;i<used;i++) if (records[i].word==word) want=records[i].value;
        if (((const volatile uint64_t *)pool)[word]!=want) return 0;
    }
    return 1;
}
