/* SPDX-License-Identifier: BSD-2-Clause */
#include "xnu_checkpoint.h"
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int key(const uint8_t *p,const char *s,unsigned width) {
    unsigned i=0;while(s[i]){if(i>=width || p[i]!=(uint8_t)s[i])return 0;i++;}
    for(;i<width;i++)if(p[i])return 0;return 1;
}
struct scan { const uint8_t *p;uint64_t n,at,reg;unsigned nodes; };
static int node(struct scan *w,unsigned depth,unsigned parent) {
    if(depth>4 || ++w->nodes>64 || w->at>w->n || w->n-w->at<8)return 0;
    unsigned count=u32(w->p+w->at),children=u32(w->p+w->at+4);w->at+=8;
    if(!count || count>32 || children>64)return 0;
    uint64_t name=0,length=0,reg=0,regsize=0;
    for(unsigned i=0;i<count;i++) {
        if(w->at>w->n || w->n-w->at<36)return 0;
        uint64_t n=u32(w->p+w->at+32),value=w->at+36,padded=(n+3)&~3ull;
        if(padded>w->n-value)return 0;
        if(key(w->p+w->at,"name",32)){if(name)return 0;name=value;length=n;}
        if(key(w->p+w->at,"reg",32)){if(reg)return 0;reg=value;regsize=n;}
        w->at=value+padded;
    }
    if(!name || !length || length>32)return 0;
    unsigned role=depth==1 && key(w->p+name,"cpus",(unsigned)length)?1:0;
    if(depth==2 && parent==1) {
        if(children || !key(w->p+name,"cpu0",(unsigned)length) || !reg || regsize!=4 || w->reg)return 0;
        w->reg=reg;
    }
    for(unsigned i=0;i<children;i++)if(!node(w,depth+1,role))return 0;
    return 1;
}
uint64_t xnu_topology_reg_offset(const uint8_t *p,size_t n) {
    struct scan w={.p=p,.n=n};return n<=65536 && node(&w,0,0) && w.at==n?w.reg:0;
}
static uint8_t byte64(uint64_t value,unsigned index) { return (uint8_t)(value>>(8*index)); }
uint8_t xnu_topology_initial_byte(unsigned region,unsigned i,const uint64_t *v) {
    if(region==0) {
        if(i<4)return byte64(1,i);
        if(i>=24 && i<32)return byte64(v[1],i-24);
        if(i>=32 && i<40)return byte64(v[2],i-32);
    }
    if(region==4 && i==0)return 1;
    if(region==5 && i<4)return 0xff;
    return 0;
}
static const uint64_t sizes[9]={88,480,72,8,1,4,4,4,8};
int xnu_topology_valid(const struct xnu_expected *e) {
    if(e->topology!=1 || e->mode!=7 || e->end<e->phys || e->end-e->phys>XNU_ARENA_BYTES || e->topo_affinity>0xffffffffull)return 0;
    uint64_t v[9]={e->topo_info,e->topo_cpus,e->topo_clusters,e->topo_offsets,e->topo_correct,
        e->topo_eventi,e->topo_page,e->topo_userpage,e->topo_stall};
    for(unsigned r=0;r<9;r++) {
        if(v[r]&((r==4?1u:r>=5 && r<=7?4u:8u)-1u))return 0;
        if(v[r]<e->virt || v[r]-e->virt>e->end-e->phys || sizes[r]>e->end-e->phys-(v[r]-e->virt))return 0;
        const volatile uint8_t *p=(const volatile uint8_t *)(uintptr_t)(e->phys+v[r]-e->virt);
        for(unsigned i=0;i<sizes[r];i++) {
            uint8_t want=xnu_topology_initial_byte(r,i,v);
            if(r==0) {
                if((i>=4 && i<8) || (i>=12 && i<16) || (i>=60 && i<68) || (i>=76 && i<80))
                    want=byte64(1,i%4);
                if(i>=40 && i<48)want=byte64(v[1],i-40);
                if(i>=48 && i<56)want=byte64(v[2],i-48);
            }
            if(r==1 && i<120) {
                if(i>=4 && i<8)want=byte64(e->topo_affinity,i-4);
                if(i>=112 && i<116)want=byte64((e->topo_affinity>>8)&255,i-112);
                if(i>=116 && i<120)want=byte64(e->topo_affinity&255,i-116);
            }
            if(r==2 && ((i>=8 && i<12) || (i>=16 && i<24)))want=byte64(1,i-(i<12?8:16));
            if(r==5)want=byte64(4,i);
            if(r==6 || r==7)want=byte64(12,i);
            if(p[i]!=want)return 0;
        }
    }
    return 1;
}
int xnu_wfe_valid(const uint64_t *live,const uint64_t *saved) {
    return live[0]==(saved[17]|0x4full) && live[1]==0 && live[2]==0;
}
