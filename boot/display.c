/* SPDX-License-Identifier: BSD-2-Clause */
#include "display.h"
#include "fdt.h"
#ifndef PROBE_DISPLAY_PATTERN
#define PROBE_DISPLAY_PATTERN 0
#endif
#ifndef PROBE_DISPLAY_BACKLIGHT
#define PROBE_DISPLAY_BACKLIGHT 0
#endif
void probe_value(const char *,uint64_t);
int fdt_range_clear(const void *,uint64_t,uint64_t);
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
#if PROBE_DISPLAY_PATTERN
#define DISPLAY_TE 0xae6b800u
#define DISPLAY_COUNT_EN (DISPLAY_TE+0xa8) /* DPU INTF counters; offsets from Linux, not the pinned firmware. */
#define DISPLAY_FRAME (DISPLAY_TE+0xac)
#define DISPLAY_LINE (DISPLAY_TE+0xb0)
#define DISPLAY_DSI 0xae94004u
#define DISPLAY_SCANOUT 0xae05014u
#define DISPLAY_DSI_VIDEO 0x1f3u /* Coreboot mdss_dsi_video_mode_config, four lanes. */
#define DISPLAY_HOLD_SECONDS 20u
#define DISPLAY_BACKLIGHT_DUTY 52428u /* 80%, the Depthcharge default. */
struct display_pattern_result {
    uint32_t attempted,mismatches,writes,fault,saved,te,dsi,frame0,frame1,line0,line1,te_after,dsi_after;
#if PROBE_DISPLAY_BACKLIGHT
    uint32_t hello,ec_on,ec_off,gpio_on,gpio_off,ec_error;
#endif
};
static uint32_t pattern_read(uint32_t address,uint32_t *fault) {
    uint32_t value=0;if(!display_read32(address,&value))*fault=1;return value;
}
static void pattern_write(uint32_t address,uint32_t value,uint32_t *writes) {
    *(volatile uint32_t *)(uintptr_t)address=value;__asm__ volatile("dsb sy":::"memory");(*writes)++;
}
static uint64_t pattern_ticks(void) {
    uint64_t ticks;__asm__ volatile("isb; mrs %0, cntpct_el0":"=r"(ticks)::"memory");return ticks;
}
static void pattern_wait(uint64_t ticks) {
    uint64_t start=pattern_ticks();while(pattern_ticks()-start<ticks)__asm__ volatile("yield");
}
static uint32_t pattern_pixel(uint32_t x,uint32_t y,const struct cb_display *b) {
    return (x*8/b->width+y*6/b->height)&1?0xffffffffu:0xff000000u;
}
/* Restart the scanout Depthcharge stopped, hold it, then stop it the same way. No EC or backlight. */
static void display_pattern(const struct cb_display *b,uint32_t pa,struct display_pattern_result *o) {
    uint64_t freq;__asm__ volatile("mrs %0, cntfrq_el0":"=r"(freq));
    if(freq<1000 || freq>1000000000)return;
    o->attempted=1;
    for(uint32_t y=0;y<b->height;y++) {
        volatile uint32_t *line=(volatile uint32_t *)(uintptr_t)(pa+(uint64_t)y*b->stride);
        for(uint32_t x=0;x<b->width;x++)line[x]=pattern_pixel(x,y,b);
    }
    __asm__ volatile("dsb sy":::"memory");
    for(uint32_t y=0;y<b->height;y++) {
        volatile uint32_t *line=(volatile uint32_t *)(uintptr_t)(pa+(uint64_t)y*b->stride);
        for(uint32_t x=0;x<b->width;x++)if(line[x]!=pattern_pixel(x,y,b))o->mismatches++;
    }
    o->saved=pattern_read(DISPLAY_COUNT_EN,&o->fault);
    if(o->fault)return;
    pattern_write(DISPLAY_COUNT_EN,3,&o->writes);pattern_write(DISPLAY_DSI,DISPLAY_DSI_VIDEO,&o->writes);
    pattern_write(DISPLAY_SCANOUT,pa,&o->writes);pattern_write(DISPLAY_TE,1,&o->writes);
    o->te=pattern_read(DISPLAY_TE,&o->fault);o->dsi=pattern_read(DISPLAY_DSI,&o->fault);
    pattern_wait(freq);
    o->frame0=pattern_read(DISPLAY_FRAME,&o->fault);o->line0=pattern_read(DISPLAY_LINE,&o->fault);
#if PROBE_DISPLAY_BACKLIGHT
    /* A failed EC exchange only loses the backlight; the scanout hold still completes. */
    o->hello=(uint32_t)display_ec_hello(freq,&o->ec_error);
    if(o->hello)o->ec_on=(uint32_t)display_ec_backlight(freq,DISPLAY_BACKLIGHT_DUTY,&o->ec_error);
    if(o->ec_on) {
        pattern_write(COACHZ_BACKLIGHT_PORT+4,b->bl_polarity?2u:0u,&o->writes);
        o->gpio_on=pattern_read(COACHZ_BACKLIGHT_PORT+4,&o->fault);
    }
#endif
    pattern_wait(freq*(DISPLAY_HOLD_SECONDS-1));
    o->frame1=pattern_read(DISPLAY_FRAME,&o->fault);o->line1=pattern_read(DISPLAY_LINE,&o->fault);
#if PROBE_DISPLAY_BACKLIGHT
    if(o->ec_on) {
        pattern_write(COACHZ_BACKLIGHT_PORT+4,b->bl_polarity?0u:2u,&o->writes);
        o->gpio_off=pattern_read(COACHZ_BACKLIGHT_PORT+4,&o->fault);
        o->ec_off=(uint32_t)display_ec_backlight(freq,0,&o->ec_error);
    }
#endif
    pattern_write(DISPLAY_TE,0,&o->writes);pattern_wait(freq/50);pattern_write(DISPLAY_DSI,0,&o->writes);
    pattern_write(DISPLAY_COUNT_EN,o->saved,&o->writes);
    o->te_after=pattern_read(DISPLAY_TE,&o->fault);o->dsi_after=pattern_read(DISPLAY_DSI,&o->fault);
}
#define DISPLAY_SCHEMA (PROBE_DISPLAY_BACKLIGHT?4:3)
#define DISPLAY_WRITES pattern.writes
#else
#define DISPLAY_SCHEMA 2
#define DISPLAY_WRITES 0
#endif
void display_discover(const void *dt,uint64_t image,uint64_t imagesize) {
#if PROBE_DISPLAY_PATTERN
    struct display_pattern_result pattern={0};
#endif
    struct cb_display b={0};struct display_state state={0};uint32_t r[DISPLAY_READS]={0},n=0;
    unsigned good=0,reads=0,fault=0,owners=0;
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
        owners=(unsigned)fdt_range_clear(dt,r[6],state.allocation);
#if PROBE_DISPLAY_PATTERN
    if(state.consistent && state.allocated && state.window && state.ram && owners && !state.conflict && !r[0] && !r[1])
        display_pattern(&b,r[6],&pattern);
#endif
done:
    probe_value("Display schema=",DISPLAY_SCHEMA);probe_value("Display coreboot table verified=",good);
    probe_value("Display framebuffer metadata verified=",good);probe_value("Display GPIO metadata verified=",good);
    probe_value("Display MMIO reads=",reads);probe_value("Display MMIO fault=",fault);
    probe_value("Display discovery complete=",good && reads==DISPLAY_READS && state.span!=0);
    probe_value("Display hardware writes=",DISPLAY_WRITES);
    probe_value("Display table bytes=",b.table_bytes);probe_value("Display table entries=",b.table_entries);
    probe_value("Display table framebuffer PA=",b.physical);probe_value("Display width=",b.width);probe_value("Display height=",b.height);
    probe_value("Display stride=",b.stride);probe_value("Display bpp=",b.bpp);probe_value("Display orientation=",b.orientation);
    probe_value("Display Image PA=",image);probe_value("Display Image bytes=",imagesize);probe_value("Display frame span=",state.span);probe_value("Display allocation bytes=",state.allocation);
    for(unsigned i=0;i<DISPLAY_READS;i++)probe_value(registers[i].name,r[i]);
    unsigned owned=state.window && state.ram && owners && !state.conflict;
    probe_value("Display metadata consistent=",state.consistent);probe_value("Display allocation relation=",state.allocated);
    probe_value("Display allocator window=",state.window);probe_value("Display Coreboot RAM=",state.ram);
    probe_value("Display FDT owners clear=",owners);probe_value("Display Image conflict=",state.conflict);
    probe_value("Display ownership verified=",owned);
    probe_value("Display candidate writable=",state.consistent && state.allocated && owned);
#if PROBE_DISPLAY_PATTERN
    probe_value("Display pattern attempted=",pattern.attempted);probe_value("Display pattern mismatches=",pattern.mismatches);
    probe_value("Display pattern hold seconds=",pattern.writes?DISPLAY_HOLD_SECONDS:0);probe_value("Display pattern MMIO fault=",pattern.fault);
    probe_value("Display pattern count enable saved=",pattern.saved);
    probe_value("Display pattern TE running=",pattern.te);probe_value("Display pattern DSI running=",pattern.dsi);
    probe_value("Display pattern frame start=",pattern.frame0);probe_value("Display pattern frame end=",pattern.frame1);
    probe_value("Display pattern line start=",pattern.line0);probe_value("Display pattern line end=",pattern.line1);
    probe_value("Display pattern TE stopped=",pattern.te_after);probe_value("Display pattern DSI stopped=",pattern.dsi_after);
#endif
#if PROBE_DISPLAY_BACKLIGHT
    probe_value("Display backlight hello=",pattern.hello);probe_value("Display backlight EC on=",pattern.ec_on);
    probe_value("Display backlight EC off=",pattern.ec_off);probe_value("Display backlight error=",pattern.ec_error);
    probe_value("Display backlight GPIO on=",pattern.gpio_on);probe_value("Display backlight GPIO off=",pattern.gpio_off);
#endif
}
