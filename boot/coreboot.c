/* SPDX-License-Identifier: BSD-2-Clause */
#include "coreboot.h"
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p)|(uint64_t)u32(p+4)<<32; }
uint16_t cb_checksum(const uint8_t *p,size_t n) {
    uint32_t sum=0;for(size_t i=0;i<n;i++){sum+=(uint32_t)p[i]<<((i&1)*8);sum=(sum&65535)+(sum>>16);}
    return (uint16_t)~sum;
}
static int name(const uint8_t *p,const char *s) {
    unsigned i=0;while(s[i]){if(i==16 || p[i]!=(uint8_t)s[i])return 0;i++;}
    return i<16 && !p[i];
}
int cb_display_parse(const uint8_t *p,size_t n,uint64_t pa,uint64_t first,size_t limit,struct cb_display *out) {
    if(!p || !out || !n || n>2*1024*1024 || pa>UINT64_MAX-n || first<pa || first-pa>=n || limit<24 || limit>65536 || limit>n-(first-pa))return 0;
    uint64_t seen[3]={0};uint64_t at=first;
    for(unsigned hop=0;hop<3;hop++) {
        for(unsigned i=0;i<hop;i++)if(seen[i]==at)return 0;
        seen[hop]=at;
        if(at<pa || at-pa>n || n-(at-pa)<24)return 0;
        const uint8_t *h=p+(at-pa);uint32_t bytes=u32(h+12),count=u32(h+20);
        if(h[0]!='L' || h[1]!='B' || h[2]!='I' || h[3]!='O' || u32(h+4)!=24 || u32(h+8)>65535 || u32(h+16)>65535 || cb_checksum(h,24) ||
           !bytes || (bytes&3) || bytes>65536-24 || !count || count>256 || bytes>n-(at-pa)-24 || (!hop && bytes+24>limit) || cb_checksum(h+24,bytes)!=u32(h+16))return 0;
        struct cb_display t={0};t.table_bytes=bytes;t.table_entries=count;
        unsigned memory=0,fb=0,gpio=0,backlight=0,forward=0;uint64_t next=0;uint32_t pos=24,end=24+bytes;
        for(unsigned i=0;i<count;i++) {
            if(pos>end || end-pos<8)return 0;
            const uint8_t *r=h+pos;uint32_t tag=u32(r),size=u32(r+4);
            if(size<8 || (size&3) || size>end-pos)return 0;
            if(tag==0x11) {
                if(count!=1 || size!=16 || forward++)return 0;next=u64(r+8);
            } else if(tag==1) {
                if(memory++ || size<=8 || (size-8)%20 || (size-8)/20>CB_MAX_RANGES)return 0;
                t.ranges=(size-8)/20;
                for(unsigned j=0;j<t.ranges;j++) {
                    const uint8_t *q=r+8+20*j;uint64_t a=u64(q),z=u64(q+8);
                    if(!z || a>UINT64_MAX-z)return 0;
                    for(unsigned k=0;k<j;k++)if(a<t.memory[k].base+t.memory[k].size && t.memory[k].base<a+z)return 0;
                    t.memory[j].base=a;t.memory[j].size=z;t.memory[j].type=u32(q+16);
                }
            } else if(tag==0x12) {
                if(fb++ || size!=40)return 0;
                t.physical=u64(r+8);t.width=u32(r+16);t.height=u32(r+20);t.stride=u32(r+24);t.bpp=r[28];t.orientation=r[37];
                if(!t.width || !t.height || t.width>8192 || t.height>8192 || t.stride<t.width*4 || t.stride>65536 || (t.stride&3) ||
                   (uint64_t)t.stride*t.height>DISPLAY_MAX_FRAME || t.bpp!=32 || t.orientation>3 ||
                   r[29]!=16 || r[30]!=8 || r[31]!=8 || r[32]!=8 || r[33]!=0 || r[34]!=8 || r[35]!=24 || r[36]!=8)return 0;
            } else if(tag==0x13) {
                if(gpio++ || size<12 || u32(r+8)>64 || size!=12+28*u32(r+8))return 0;
                for(unsigned j=0;j<u32(r+8);j++) {
                    const uint8_t *q=r+12+28*j;unsigned z=0;while(z<16 && q[12+z])z++;if(z==16)return 0;
                    if(name(q+12,"backlight")) {
                        if(backlight++ || u32(q)!=COACHZ_BACKLIGHT_PORT || u32(q+4)>1 || u32(q+8)>1)return 0;
                        t.bl_port=u32(q);t.bl_polarity=u32(q+4);t.bl_value=u32(q+8);
                    }
                }
            }
            pos+=size;
        }
        if(pos!=end)return 0;
        if(forward){at=next;continue;}
        if(memory!=1 || fb!=1 || gpio!=1 || backlight!=1)return 0;
        uint8_t *dest=(uint8_t *)out;const uint8_t *src=(const uint8_t *)&t;
        for(size_t i=0;i<sizeof(t);i++)dest[i]=src[i];
        return 1;
    }
    return 0;
}
int cb_display_ram(const struct cb_display *t,uint64_t a,size_t n) {
    if(!n || a>UINT64_MAX-n || t->ranges>CB_MAX_RANGES)return 0;
    for(unsigned i=0;i<t->ranges;i++) {
        const struct cb_range *r=&t->memory[i];
        if(r->type==1 && a>=r->base && a-r->base<=r->size && n<=r->size-(a-r->base))return 1;
    }
    return 0;
}
