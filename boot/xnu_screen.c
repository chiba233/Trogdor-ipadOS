/* SPDX-License-Identifier: BSD-2-Clause */
/* Load unmodified-flow XNU for a screen-output boot: no checkpoint HVC and no
   return to Linux. Once XNU runs, the screen is the only output. */
#include "xnu_checkpoint.h"
#include "display.h"
#define HEADER 1152u
#define SCREEN_VERSION 12u
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p+4) << 32; }
static void wr64(volatile uint8_t *p,uint64_t v) { for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i)); }
static uint64_t up(uint64_t v) { return (v+0x3fff)&~0x3fffull; }
static int bounded(uint64_t at,uint64_t n,uint64_t limit) { return at<=limit && n<=limit-at; }
static void copy(volatile uint8_t *d,const uint8_t *s,size_t n) {
    if(!(((uintptr_t)d|(uintptr_t)s)&7))
        while(n>=8){*(volatile uint64_t *)(uintptr_t)d=*(const volatile uint64_t *)(const void *)s;d+=8;s+=8;n-=8;}
    while(n--)*d++=*s++;
}
int xnu_screen_load(const uint8_t *p,size_t len,const void *dt,uint64_t affinity,uint64_t frequency,
                    const struct display_video *video,struct xnu_handoff *out) {
    const char magic[]="TRXNUC01";
    out->entry=out->args=0;
    if(!video->ready || len<HEADER || le32(p+8)!=SCREEN_VERSION)return 0;
    for(unsigned i=0;i<8;i++)if(p[i]!=(uint8_t)magic[i])return 0;
    uint32_t count=le32(p+12);
    if(!count || count>64 || !bounded(HEADER,(uint64_t)count*40,len))return 0;
    uint64_t v[14];for(unsigned i=0;i<14;i++)v[i]=le64(p+16+8*i);
    /* The checkpoint-address field carries the physical window in this version. */
    uint64_t base=v[0],entry=v[1],phys=v[2],ramdisk=v[3],raw=v[4],rawsize=v[5],adt=v[6],adtsize=v[7],span=v[8],memory=v[9];
    uint64_t bias=base&0x1fffff,input=(uintptr_t)p;
    if((phys&0x1fffff) || (memory&0x1fffff) || !span || span>memory || phys>UINT64_MAX-memory || (raw&15) ||
       !bounded(raw,rawsize,len) || !bounded(adt,adtsize,len) || adt+adtsize!=ramdisk || ramdisk>=len || ((len-ramdisk)&4095) || !adtsize || adtsize>1048576 ||
       !bounded(v[10],8,adtsize) || !bounded(v[11],8,adtsize) || affinity!=v[12] || frequency!=v[13] || !frequency ||
       entry<base || (entry&3) || entry-base>=span || input>UINT64_MAX-len ||
       (input<phys+memory && phys<input+len) || !xnu_range_allowed(dt,phys,memory))return 0;
    if(video->pa<phys+memory && phys<video->pa+video->bytes)return 0;
    uint64_t args=up(span+bias),tree=args+4096,disk=up(tree+adtsize),top=up(disk+(len-ramdisk));
    if(top>memory)return 0;
    for(unsigned i=0;i<count;i++) {
        const uint8_t *s=p+HEADER+40*i;
        uint64_t off=le64(s),n=le64(s+8),file=le64(s+16),bytes=le64(s+24);
        if(off>span || n>span-off || bytes>n || !bounded(file,bytes,rawsize))return 0;
    }
    volatile uint64_t *zero=(volatile uint64_t *)(uintptr_t)phys;
    for(uint64_t i=0;i<top/8;i++)zero[i]=0;
    volatile uint8_t *window=(volatile uint8_t *)(uintptr_t)phys;
    for(unsigned i=0;i<count;i++) {
        const uint8_t *s=p+HEADER+40*i;
        copy(window+bias+le64(s),p+raw+le64(s+16),(size_t)le64(s+24));
    }
    volatile uint8_t *a=window+args,*t=window+tree;
    a[0]=2;a[2]=2;
    wr64(a+8,base-bias);wr64(a+16,phys);wr64(a+24,memory);wr64(a+32,phys+top);
    wr64(a+40,video->pa);wr64(a+48,1);wr64(a+56,video->stride);wr64(a+64,video->width);wr64(a+72,video->height);wr64(a+80,32);
    wr64(a+0x60,base-bias+tree);
    for(unsigned i=0;i<4;i++)a[0x68+i]=(uint8_t)(adtsize>>(8*i));
    copy(a+0x6c,p+128,1024);wr64(a+0x478,memory);
    copy(t,p+adt,(size_t)adtsize);wr64(t+v[10],phys);wr64(t+v[11],memory);
    copy(window+disk,p+ramdisk,(size_t)(len-ramdisk));
    __asm__ volatile("dsb sy; ic iallu; dsb sy; isb":::"memory");
    out->entry=phys+bias+entry-base;out->args=phys+args;
    return 1;
}
