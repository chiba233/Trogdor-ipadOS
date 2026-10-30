/* SPDX-License-Identifier: BSD-2-Clause */
#include "display.h"
#include "fdt.h"
void probe_value(const char *,uint64_t);
int xnu_range_allowed(const void *,uint64_t,uint64_t);
static uint64_t be64(const uint8_t *p) { return (uint64_t)fdt_be32(p)<<32|fdt_be32(p+4); }
static int eq(const char *a,const char *b) { while(*a && *a==*b){a++;b++;}return *a==*b; }
static int overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t z) { return a<b+z && b<a+n; }
static int cells(const void *dt,const char *name) {
    uint32_t n;const uint8_t *p=fdt_property(dt,FDT_MAX_SIZE,"/",name,&n);return p && n==4 && fdt_be32(p)==2;
}
static int identity(const void *dt) {
    uint32_t n;const uint8_t *p=fdt_property(dt,FDT_MAX_SIZE,"/","compatible",&n);unsigned coachz=0,soc=0;
    if(!p || !n || n>256 || p[n-1])return 0;
    for(uint32_t i=0;i<n;) {
        const char *s=(const char *)p+i;
        if(eq(s,"google,coachz-rev1-sku0") || eq(s,"google,coachz-rev2-sku0"))coachz=1;
        if(eq(s,"qcom,sc7180"))soc=1;
        while(i<n && p[i])i++;i++;
    }
    return coachz && soc;
}
/* The pinned Depthcharge builder inherits the root cell widths when absent.
   Explicit overrides must be unique and equal to two; ranges must be empty. */
static int firmware_shape(const void *dt) {
    const uint8_t *b=dt;uint32_t at=fdt_be32(b+8),end=at+fdt_be32(b+36),names=fdt_be32(b+12);
    unsigned depth=0,fw=0,nodes=0,coreboot=0,addr=0,size=0,ranges=0;
    while(at+4<=end) {
        uint32_t token=fdt_be32(b+at);at+=4;
        if(token==1) {
            depth++;const char *s=(const char *)b+at;
            if(depth==2 && eq(s,"firmware")){fw=1;if(++nodes!=1)return 0;}
            if(fw && depth==3 && eq(s,"coreboot") && ++coreboot!=1)return 0;
            while(b[at])at++;at=(at+4)&~3u;
        } else if(token==2) {if(depth==2)fw=0;depth--;}
        else if(token==3) {
            uint32_t n=fdt_be32(b+at),key=fdt_be32(b+at+4);at+=8;
            if(fw && depth==2) {
                const char *s=(const char *)b+names+key;
                if(eq(s,"#address-cells")){if(++addr!=1 || n!=4 || fdt_be32(b+at)!=2)return 0;}
                if(eq(s,"#size-cells")){if(++size!=1 || n!=4 || fdt_be32(b+at)!=2)return 0;}
                if(eq(s,"ranges")){if(++ranges!=1 || n)return 0;}
            }
            at+=(n+3)&~3u;
        }
    }
    return nodes==1 && coreboot==1 && ranges==1;
}
static const struct { char name[40];uint32_t address; } registers[DISPLAY_READS]={
    {"Display TE=",0xae6b800},{"Display DSI=",0xae94004},
    {"Display source size=",0xae05000},{"Display output size=",0xae0500c},
    {"Display source XY=",0xae05008},{"Display output XY=",0xae05010},
    {"Display scanout PA=",0xae05014},{"Display scanout stride=",0xae05024},
    {"Display format=",0xae05030},{"Display unpack=",0xae05034},
    {"Display GPIO config=",COACHZ_BACKLIGHT_PORT},{"Display GPIO IO=",COACHZ_BACKLIGHT_PORT+4},
    {"Display EC protocol=",0xa80068}
};
void display_discover(const void *dt,uint64_t image,uint64_t imagesize) {
    struct cb_display b={0};struct display_state state={0};uint32_t r[DISPLAY_READS]={0},n=0;
    unsigned good=0,reads=0,fault=0,fdt_ram=0;
    if(!imagesize || image>UINT64_MAX-imagesize || !fdt_valid(dt,FDT_MAX_SIZE) || !identity(dt) ||
       !cells(dt,"#address-cells") || !cells(dt,"#size-cells") || !firmware_shape(dt))goto done;
    const uint8_t *p=fdt_property(dt,FDT_MAX_SIZE,"/firmware/coreboot","reg",&n);
    if(!p || n!=32)goto done;
    uint64_t first=be64(p),limit=be64(p+8),base=be64(p+16),cap=be64(p+24);
    if(!cap || cap>2*1024*1024 || base<0x80000000ull || base>=0x100000000ull || cap>0x100000000ull-base ||
       limit<24 || limit>65536 || first<base || first-base>cap || limit>cap-(first-base) ||
       overlap(base,cap,image,imagesize) || overlap(base,cap,(uintptr_t)dt,fdt_be32((const uint8_t *)dt+4)))goto done;
    good=cb_display_parse((const uint8_t *)(uintptr_t)base,(size_t)cap,base,first,(size_t)limit,&b);
    if(!good)goto done;
    for(unsigned i=0;i<DISPLAY_READS;i++) {
        if(!display_read32(registers[i].address,&r[i])){fault=1;break;}reads++;
    }
    if(reads==DISPLAY_READS && display_analyze(&b,r,image,imagesize,&state))
        fdt_ram=(unsigned)xnu_range_allowed(dt,r[6],state.allocation);
done:
    probe_value("Display schema=",1);probe_value("Display coreboot table verified=",good);
    probe_value("Display framebuffer metadata verified=",good);probe_value("Display GPIO metadata verified=",good);
    probe_value("Display MMIO reads=",reads);probe_value("Display MMIO fault=",fault);
    probe_value("Display discovery complete=",good && reads==DISPLAY_READS && state.span!=0);
    probe_value("Display hardware writes=",0);
    probe_value("Display table bytes=",b.table_bytes);probe_value("Display table entries=",b.table_entries);
    probe_value("Display table framebuffer PA=",b.physical);probe_value("Display width=",b.width);probe_value("Display height=",b.height);
    probe_value("Display stride=",b.stride);probe_value("Display bpp=",b.bpp);probe_value("Display orientation=",b.orientation);
    probe_value("Display Image PA=",image);probe_value("Display Image bytes=",imagesize);probe_value("Display frame span=",state.span);probe_value("Display allocation bytes=",state.allocation);
    for(unsigned i=0;i<DISPLAY_READS;i++)probe_value(registers[i].name,r[i]);
    probe_value("Display scanout consistent=",state.consistent);probe_value("Display Coreboot RAM=",state.ram);
    probe_value("Display FDT range allowed=",fdt_ram);probe_value("Display Image conflict=",state.conflict);
    probe_value("Display candidate writable=",state.consistent && state.ram && fdt_ram && !state.conflict);
}
