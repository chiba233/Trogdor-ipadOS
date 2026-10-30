/* SPDX-License-Identifier: BSD-2-Clause */
#include "display.h"
int display_analyze(const struct cb_display *b,const uint32_t *r,uint64_t image,uint64_t size,struct display_state *o) {
    if(!b || !r || !o || !size || image>UINT64_MAX-size || !b->width || !b->height || b->width>8192 || b->height>8192 || b->stride<b->width*4 || b->stride>65536 || (b->stride&3) || b->bpp!=32 || b->orientation>3)return 0;
    uint64_t n=(uint64_t)b->stride*b->height;if(!n || n>DISPLAY_MAX_FRAME)return 0;
    uint32_t geometry=b->width|(b->height<<16);
    o->span=n;o->allocation=(n+4095)&~4095ull;o->consistent=(r[2]==geometry && r[3]==geometry && !r[4] && !r[5] && r[6] &&
        r[7]==b->stride && r[8]==0x236ff && r[9]==0x03020001 && (!b->physical || b->physical==r[6]));
    uint64_t end=(uint64_t)r[6]+o->allocation;
    /* libpayload aligns the end of a dynamic allocation, not its base. */
    o->allocated=!b->physical && !(end&0xfffff);
    o->window=r[6]>=COACHZ_DEPTHCHARGE_BASE && end<=0x100000000ull;
    o->conflict=(uint64_t)r[6]<image+size && image<(uint64_t)r[6]+o->allocation;
    o->ram=cb_display_ram(b,r[6],(size_t)o->allocation);
    return 1;
}
