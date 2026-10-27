/* SPDX-License-Identifier: BSD-2-Clause */
#include "xnu_checkpoint.h"
int xnu_cpu_valid(const struct xnu_expected *e) {
    if ((e->mode<5 || e->mode>7) || e->end<e->phys || e->end-e->phys>XNU_ARENA_BYTES || e->cpu_data<e->virt ||
        e->cpu_data-e->virt<16 || (e->cpu_data&15) || e->cpu_data-e->virt>e->end-e->phys ||
        0x510>e->end-e->phys-(e->cpu_data-e->virt) || e->rtclock<e->virt ||
        e->rtclock-e->virt>e->end-e->phys || 8>e->end-e->phys-(e->rtclock-e->virt)) return 0;
    const volatile uint8_t *data=(const volatile uint8_t *)(uintptr_t)(e->phys+e->cpu_data-e->virt);
    for (unsigned i=0;i<16;i++) if (data[(int)i-16] || data[0x500+i]) return 0;
    /* Independent byte model of every store in the pinned 0xc4-byte leaf.
       Unwritten bytes retain mode5 diagnostic 0xa5 or mode6 normal zero. */
    const unsigned zero[][2]={{2,2},{0x44,0x14},{0x68,0x54},{0xc0,8},{0x158,8},
        {0x170,0x50},{0x1d0,0x20},{0x260,8},{0x290,0xc},{0x2a0,0x107}};
    const struct { unsigned at,size;uint64_t value; } nonzero[]={
        {0x40,4,0x80000000},{0x100,8,UINT64_MAX},{0x150,8,e->rtclock},
        {0x1cc,4,0x50535553},{0x280,4,0xffffffff}};
    for (unsigned i=0;i<0x500;i++) {
        uint8_t want=e->mode==5?0xa5:0;
        for (unsigned j=0;j<sizeof(zero)/sizeof(zero[0]);j++)
            if (i>=zero[j][0] && i-zero[j][0]<zero[j][1]) want=0;
        for (unsigned j=0;j<sizeof(nonzero)/sizeof(nonzero[0]);j++)
            if (i>=nonzero[j].at && i-nonzero[j].at<nonzero[j].size)
                want=(uint8_t)(nonzero[j].value>>(8*(i-nonzero[j].at)));
        if (data[i]!=want) return 0;
    }
    return 1;
}
