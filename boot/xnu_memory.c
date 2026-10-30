/* SPDX-License-Identifier: BSD-2-Clause */
#include "xnu_checkpoint.h"
#include "fdt.h"
static uint64_t be64(const uint8_t *p) { return (uint64_t)fdt_be32(p)<<32 | fdt_be32(p+4); }
static int equal(const char *a,const char *b) { while (*a && *a==*b) {a++;b++;}return *a==*b; }
static int conflict(uint64_t lo,uint64_t n,uint64_t a,uint64_t z) {
    return !z || a>UINT64_MAX-z || (lo<a+z && a<lo+n);
}
static int tuples(const uint8_t *p,uint32_t n,uint64_t lo,uint64_t size) {
    if (!n || n%16) return 0;
    for (uint32_t i=0;i<n;i+=16) if (conflict(lo,size,be64(p+i),be64(p+i+8))) return 0;
    return 1;
}
static int two_cells(const void *dt,const char *path,const char *name) {
    uint32_t n;const void *p=fdt_property(dt,FDT_MAX_SIZE,path,name,&n);
    return p && n==4 && fdt_be32(p)==2;
}
int fdt_range_clear(const void *dt,uint64_t lo,uint64_t size) {
    const uint8_t *b=dt;uint32_t n;
    if (!size || lo>UINT64_MAX-size || !fdt_valid(dt,FDT_MAX_SIZE) ||
        !two_cells(dt,"/","#address-cells") || !two_cells(dt,"/","#size-cells")) return 0;
    const uint8_t *mem=fdt_property(dt,FDT_MAX_SIZE,"/memory","reg",&n);
    if (!mem || !n || n%16) return 0;
    int inside=0;
    for (uint32_t i=0;i<n;i+=16) {
        uint64_t a=be64(mem+i),z=be64(mem+i+8);
        if (!z || z>UINT64_MAX-a) return 0;
        if (a<=lo && lo-a<=z && size<=z-(lo-a)) inside=1;
    }
    if (!inside || conflict(lo,size,(uintptr_t)dt,fdt_be32(b+4))) return 0;
    const void *fw=fdt_property(dt,FDT_MAX_SIZE,"/firmware","ranges",&n);
    if (!fw || n) return 0;
    const uint8_t *cb=fdt_property(dt,FDT_MAX_SIZE,"/firmware/coreboot","reg",&n);
    if (!cb || !tuples(cb,n,lo,size)) return 0;
    uint32_t at=fdt_be32(b+16);
    while (at+16<=fdt_be32(b+4)) {
        uint64_t a=be64(b+at),z=be64(b+at+8);at+=16;
        if (!(a|z)) break;
        if (conflict(lo,size,a,z)) return 0;
    }
    const uint8_t *s=fdt_property(dt,FDT_MAX_SIZE,"/chosen","linux,initrd-start",&n);
    uint32_t en;const uint8_t *e=fdt_property(dt,FDT_MAX_SIZE,"/chosen","linux,initrd-end",&en);
    if (!!s!=!!e) return 0;
    if (s) {
        if ((n!=4 && n!=8) || en!=n) return 0;
        uint64_t a=n==4?fdt_be32(s):be64(s),z=n==4?fdt_be32(e):be64(e);
        if (z<=a || conflict(lo,size,a,z-a)) return 0;
    }
    const void *r=fdt_property(dt,FDT_MAX_SIZE,"/reserved-memory","ranges",&n);
    if (!r || n || !two_cells(dt,"/reserved-memory","#address-cells") ||
        !two_cells(dt,"/reserved-memory","#size-cells")) return 0;
    at=fdt_be32(b+8);uint32_t end=at+fdt_be32(b+36),names=fdt_be32(b+12);
    unsigned depth=0,reserved=0,have_reg=0,have_alloc=0,have_size=0;
    unsigned firmware=0,fwaddr=0,fwsize=0,chosen=0,initrd_start=0,initrd_end=0;
    while (at+4<=end) {
        uint32_t token=fdt_be32(b+at);at+=4;
        if (token==1) {
            const char *name=(const char *)b+at;depth++;
            if (depth==2 && equal(name,"firmware")) firmware=1;
            if (depth==2 && equal(name,"chosen")) chosen=1;
            if (depth==2 && equal(name,"reserved-memory")) reserved=1;
            else if (reserved && depth!=3) return 0;
            if (reserved && depth==3) have_reg=have_alloc=have_size=0;
            while (b[at]) at++;
            at=(at+4)&~3u;
        } else if (token==2) {
            if (reserved && depth==3 && ((!have_reg && !have_size) || (!have_reg && have_size && !have_alloc))) return 0;
            if (reserved && depth==2) reserved=0;
            if (firmware && depth==2) firmware=0;
            if (chosen && depth==2) chosen=0;
            depth--;
        } else if (token==3) {
            uint32_t len=fdt_be32(b+at),key=fdt_be32(b+at+4);at+=8;
            if (chosen && depth==2) {
                const char *name=(const char *)b+names+key;
                if (equal(name,"linux,initrd-start") && ++initrd_start!=1) return 0;
                if (equal(name,"linux,initrd-end") && ++initrd_end!=1) return 0;
            }
            if (firmware && depth==2) {
                const char *name=(const char *)b+names+key;
                if (equal(name,"#address-cells")) {
                    if (++fwaddr!=1 || len!=4 || fdt_be32(b+at)!=2) return 0;
                } else if (equal(name,"#size-cells")) {
                    if (++fwsize!=1 || len!=4 || fdt_be32(b+at)!=2) return 0;
                }
            }
            if (reserved && depth==3) {
                const char *name=(const char *)b+names+key;
                if (equal(name,"reg") || equal(name,"alloc-ranges")) {
                    if (!tuples(b+at,len,lo,size)) return 0;
                    if (equal(name,"reg")) have_reg=1;else have_alloc=1;
                } else if (equal(name,"size")) {
                    if (len!=8 || !be64(b+at)) return 0;
                    have_size=1;
                }
            }
            at+=(len+3)&~3u;
        }
    }
    return 1;
}
/* A new XNU arena also stays clear of the whole firmware load and run area. */
int xnu_range_allowed(const void *dt,uint64_t lo,uint64_t size) {
    if (!size || lo>UINT64_MAX-size || conflict(lo,size,0xd0000000ull,0x30000000ull)) return 0;
    return fdt_range_clear(dt,lo,size);
}
