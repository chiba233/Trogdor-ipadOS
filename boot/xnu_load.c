/* SPDX-License-Identifier: BSD-2-Clause */
#include "xnu_checkpoint.h"
#define HEADER 1152u
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}
static void wr64(uint8_t *p, uint64_t v) { for (unsigned i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint64_t up(uint64_t v) { return (v + 0x3fff) & ~0x3fffull; }
static int bounded(uint64_t at, uint64_t n, uint64_t limit) { return at<=limit && n<=limit-at; }
static void copy(uint8_t *d, const uint8_t *s, size_t n) {
    if (!(((uintptr_t)d | (uintptr_t)s) & 7)) {
        while (n>=8) { *(volatile uint64_t *)d=*(const volatile uint64_t *)s;d+=8;s+=8;n-=8; }
    }
    while (n--) *(volatile uint8_t *)d++=*s++;
}
int xnu_load(const uint8_t *p, size_t len, void *window, size_t capacity,
             uint64_t affinity, uint64_t frequency, struct xnu_handoff *out,
             struct xnu_expected *expected) {
    const char magic[]="TRXNUC01";
    out->entry=out->args=0;
    if (len<HEADER || le32(p+8)!=1) return 0;
    for (unsigned i=0;i<8;i++) if (p[i]!=(uint8_t)magic[i]) return 0;
    uint32_t count=le32(p+12);
    if (!count || count>64 || !bounded(HEADER,(uint64_t)count*40,len)) return 0;
    uint64_t v[14];for (unsigned i=0;i<14;i++) v[i]=le64(p+16+8*i);
    uint64_t base=v[0],entry=v[1],checkpoint=v[2],pages=v[3];
    uint64_t raw=v[4],rawsize=v[5],adt=v[6],adtsize=v[7],span=v[8],memory=v[9];
    uint64_t phys=(uintptr_t)window;
    if (capacity<XNU_ARENA_BYTES || memory!=XNU_ARENA_BYTES || (phys&0x1fffff) ||
        phys>UINT64_MAX-memory || base>UINT64_MAX-memory || (base&4095) ||
        !span || span>memory || raw<HEADER+(uint64_t)count*40 || (raw&15) ||
        !bounded(raw,rawsize,len) || !bounded(adt,adtsize,len) ||
        adt<raw+rawsize || adt+adtsize!=len || !adtsize || adtsize>1048576 ||
        !bounded(v[10],8,adtsize) || !bounded(v[11],8,adtsize) ||
        (v[10]<v[11]+8 && v[11]<v[10]+8) || affinity!=v[12] || frequency!=v[13] || !frequency)
        return 0;
    uint64_t input=(uintptr_t)p;
    if (input>UINT64_MAX-len || (input<phys+memory && phys<input+len)) return 0;
    int terminated=0;
    for (unsigned i=0;i<1024;i++) { if (!p[128+i]) terminated=1;else if (terminated || p[128+i]>127) return 0; }
    if (!terminated) return 0;
    uint64_t args=up(span),tree=args+4096,top=up(tree+adtsize);
    if (top>memory || entry<base || checkpoint<base || pages<base ||
        !bounded(pages-base,4096,span) || (pages&4095)) return 0;
    int executable_entry=0,executable_checkpoint=0;
    uint64_t last=0;
    for (unsigned i=0;i<count;i++) {
        const uint8_t *s=p+HEADER+40*i;
        uint64_t off=le64(s),n=le64(s+8),file=le64(s+16),bytes=le64(s+24),prot=le64(s+32);
        if (!n || off<last || !bounded(off,n,span) || bytes>n || !bounded(file,bytes,rawsize) || prot>7) return 0;
        last=off+n;
        if ((prot&4) && entry>=base+off && bounded(entry-base-off,4,bytes)) executable_entry=1;
        if ((prot&4) && checkpoint>=base+off && bounded(checkpoint-base-off,4,bytes)) {
            if (le32(p+raw+file+checkpoint-base-off)!=(0xd4000002u|(0x584eu<<5))) return 0;
            executable_checkpoint=1;
        }
    }
    if (last!=span || !executable_entry || !executable_checkpoint || (entry&3) || (checkpoint&3)) return 0;
    /* All writes stay in the Image-owned arena, after every input is checked. */
    volatile uint64_t *zero=window;for (uint64_t i=0;i<top/8;i++) zero[i]=0;
    for (unsigned i=0;i<count;i++) {
        const uint8_t *s=p+HEADER+40*i;
        copy((uint8_t *)window+le64(s),p+raw+le64(s+16),(size_t)le64(s+24));
    }
    uint8_t *a=(uint8_t *)window+args,*t=(uint8_t *)window+tree;
    a[0]=2;a[2]=2;
    wr64(a+8,base);wr64(a+16,phys);wr64(a+24,memory);wr64(a+32,phys+top);
    wr64(a+0x60,base+tree);
    for (unsigned i=0;i<4;i++) a[0x68+i]=(uint8_t)(adtsize>>(8*i));
    copy(a+0x6c,p+128,1024);wr64(a+0x478,memory);
    copy(t,p+adt,(size_t)adtsize);wr64(t+v[10],phys);wr64(t+v[11],memory);
    out->entry=phys+entry-base;out->args=phys+args;
    expected->pc=phys+checkpoint-base;expected->args=out->args;
    expected->virt=base;expected->phys=phys;expected->memory=memory;
    expected->pages=phys+pages-base;expected->flags=0;
    return 1;
}

int xnu_capture_valid(const uint64_t *c,const struct xnu_expected *e) {
    return e->pc && e->pc<=UINT64_MAX-4 && c[0]==e->pc+4 &&
        (c[1]&15)==5 && !(c[2]&0x1005) && c[3]==e->args &&
        c[4]==e->virt && c[5]==e->phys && c[6]==e->memory &&
        c[7]==e->pages && c[8]==e->flags;
}
