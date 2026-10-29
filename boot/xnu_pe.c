/* SPDX-License-Identifier: BSD-2-Clause */
#include "xnu_checkpoint.h"
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p)|(uint64_t)u32(p+4)<<32; }
static int equal(const uint8_t *p,const char *s) {
    unsigned i=0;while (s[i]) { if (p[i]!=(uint8_t)s[i]) return 0;i++; }return !p[i];
}
static int string(const uint8_t *p,uint64_t n) {
    if (!n || n>32 || p[n-1]) return 0;
    for (uint64_t i=0;i+1<n;i++) if (!p[i] || p[i]>=128) return 0;
    return 1;
}
struct property { uint64_t at,n; };
struct walk { const uint8_t *p;uint64_t n,at,nodes,seen,out[8],parents[64],names[64]; };
static const struct property *get(const struct walk *w,const struct property *p,unsigned n,const char *key) {
    for (unsigned i=0;i<n;i++) if (equal(w->p+p[i].at-36,key)) return p+i;
    return 0;
}
static int allowed(const uint8_t *key,unsigned role) {
    const char *const lists[][10]={
        {"name","compatible","#address-cells","#size-cells","model",0},
        {"name","random-seed","dram-base","dram-size","nvram-bank-size","nvram-bank-count","nvram-current-bank","nvram-proxy-data","osenvironment",0},
        {"name",0},
        {"name","device_type","#address-cells","#size-cells","ranges",0},
        {"name","reg","state","device_type","compatible","timebase-frequency",0}};
    if (role>=5) return 1;
    for (unsigned i=0;lists[role][i];i++) if (equal(key,lists[role][i])) return 1;
    return 0;
}
static int visit(struct walk *w,unsigned depth,unsigned parent,unsigned parent_node) {
    if (depth>4 || w->nodes>=64 || w->at>w->n || w->n-w->at<8) return 0;
    unsigned node=(unsigned)w->nodes++;
    unsigned count=u32(w->p+w->at),children=u32(w->p+w->at+4);w->at+=8;
    if (!count || count>32 || children>64) return 0;
    struct property props[32];
    for (unsigned i=0;i<count;i++) {
        if (w->at>w->n || w->n-w->at<36) return 0;
        const uint8_t *key=w->p+w->at;unsigned end=0;
        while (end<32 && key[end] && key[end]<128) end++;
        if (!end || end==32) return 0;
        for (unsigned j=end;j<32;j++) if (key[j]) return 0;
        for (unsigned j=0;j<i;j++) if (equal(w->p+props[j].at-36,(const char *)key)) return 0;
        if (!i && !equal(key,"name")) return 0;
        uint64_t size=u32(key+32),value=w->at+36,padded=(size+3)&~3ull;
        if (padded>w->n-value) return 0;
        for (uint64_t j=size;j<padded;j++) if (w->p[value+j]) return 0;
        props[i]=(struct property){value,size};w->at=value+padded;
    }
    const uint8_t *name=w->p+props[0].at;
    if (!string(name,props[0].n)) return 0;
    for (unsigned i=0;i<node;i++)
        if (w->parents[i]==parent_node && equal(w->p+w->names[i],(const char *)name)) return 0;
    w->parents[node]=parent_node;w->names[node]=props[0].at;
    unsigned role=5;
    if (!depth) { if (!equal(name,"device-tree")) return 0;role=0; }
    else if (depth==1) {
        const char *const names[]={"chosen","cpus","arm-io","defaults","options","product"};
        unsigned i=0;while (i<6 && !equal(name,names[i])) i++;
        if (i==6 || (w->seen&(1u<<i))) return 0;
        w->seen|=1u<<i;role=i<3?i+1:5;
    } else if (parent==2) { if (!equal(name,"cpu0") || children) return 0;role=4; }
    else if (equal(name,"device-tree") || equal(name,"chosen") || equal(name,"cpus") || equal(name,"cpu0") || equal(name,"arm-io")) return 0;
    for (unsigned i=0;i<count;i++) if (!allowed(w->p+props[i].at-36,role)) return 0;
    const struct property *q;
    if (!role) {
        q=get(w,props,count,"model");if (!q || !string(w->p+q->at,q->n)) return 0;
        w->out[0]=q->at;w->out[1]=q->n;
    } else if (role==1) {
        const char *const keys[]={"dram-base","dram-size"};
        for (unsigned i=0;i<2;i++) { q=get(w,props,count,keys[i]);if (!q || q->n!=8) return 0;w->out[6+i]=q->at; }
    } else if (role==2) { if (children!=1) return 0; }
    else if (role==3) {
        q=get(w,props,count,"device_type");if (!q || !string(w->p+q->at,q->n)) return 0;
        w->out[2]=q->at;w->out[3]=q->n;
        q=get(w,props,count,"ranges");if (!q || q->n!=24 || !u64(w->p+q->at+8)) return 0;
        w->out[4]=u64(w->p+q->at+8);
    } else if (role==4) {
        q=get(w,props,count,"state");if (!q || q->n!=8 || !equal(w->p+q->at,"running")) return 0;
        q=get(w,props,count,"timebase-frequency");if (!q || q->n!=4 || !u32(w->p+q->at)) return 0;
        w->out[5]=u32(w->p+q->at);
        q=get(w,props,count,"reg");if (!q || q->n!=4) return 0;
    }
    for (unsigned i=0;i<children;i++) if (!visit(w,depth+1,role,node)) return 0;
    return 1;
}
int xnu_pe_contract(const uint8_t *p,size_t n,uint64_t out[8]) {
    if (n<8 || n>65536) return 0;
    struct walk w={.p=p,.n=n};
    if (!visit(&w,0,5,64) || w.at!=n || (w.seen&7)!=7) return 0;
    for (unsigned i=0;i<8;i++) { if (!w.out[i]) return 0;out[i]=w.out[i]; }
    return 1;
}
/* Independently rescan the prevalidated node; never trust an offset supplied
   by the packer without checking its real property header and bounds. */
uint64_t xnu_soc_ranges_offset(const uint8_t *p,size_t n,uint64_t node) {
    if (node>n || n-node<8) return 0;
    unsigned count=u32(p+node);uint64_t at=node+8;
    if (!count || count>32) return 0;
    for (unsigned i=0;i<count;i++) {
        if (at>n || n-at<36) return 0;
        uint64_t bytes=u32(p+at+32),value=at+36,padded=(bytes+3)&~3ull;
        if (padded>n-value) return 0;
        const char key[]="ranges";int match=1;
        for (unsigned j=0;j<32;j++) if (p[at+j]!=(j<sizeof(key)?(uint8_t)key[j]:0)) match=0;
        if (match) return bytes==24?value:0;
        at=value+padded;
    }
    return 0;
}
uint64_t xnu_pe_checksum(const volatile uint8_t *p,size_t n) {
    uint64_t h=14695981039346656037ull;
    for (size_t i=0;i<n;i++) h=(h^p[i])*1099511628211ull;
    return h;
}
static int range(const struct xnu_expected *e,uint64_t va,uint64_t n) {
    return e->end>=e->phys && e->end-e->phys<=XNU_ARENA_BYTES && va>=e->virt &&
        va-e->virt<=e->end-e->phys && n<=e->end-e->phys-(va-e->virt);
}
static const uint8_t *address(const struct xnu_expected *e,uint64_t va) {
    return (const uint8_t *)(uintptr_t)(e->phys+va-e->virt);
}
static void put(uint8_t *p,uint64_t v) { for (unsigned i=0;i<8;i++) p[i]=(uint8_t)(v>>(i*8)); }
static int same(const volatile uint8_t *p,const uint8_t *q,unsigned n) {
    for (unsigned i=0;i<n;i++) if (p[i]!=q[i]) return 0;
    return 1;
}
int xnu_pe_valid(const struct xnu_expected *e) {
    if ((e->mode!=7 && e->mode!=8) || !range(e,e->tree,e->tree_size) || !range(e,e->soc_data,48) ||
        !range(e,e->pe_info,0x208) || !range(e,e->config_bits,4) ||
        !range(e,e->debug_enabled,4) || !range(e,e->degradation,4) ||
        (e->soc_data&7) || (e->pe_info&7) || (e->config_bits&3) || (e->debug_enabled&3) || (e->degradation&3)) return 0;
    const uint8_t *tree=address(e,e->tree);uint64_t v[8];
    if (!xnu_pe_contract(tree,e->tree_size,v) || xnu_pe_checksum(tree,e->tree_size)!=e->adt_checksum) return 0;
    uint8_t soc[48]={0},info[0x208]={0},bits[4]={0},zero[4]={0},factor[4]={1};
    put(soc,e->soc_data+8);put(soc+40,v[4]);
    for (uint64_t i=0;i<v[3];i++) soc[8+i]=tree[v[2]+i];
    if (e->mode==7) {
    bits[0]=3;
    for (uint64_t i=0;i<v[1];i++) info[0x110+i]=tree[v[0]+i];
    /* Independent byte model of the original missing-frequency branch.
       UDIV 0/0 yields zero on AArch64; no hardware clock programming occurs. */
    put(info+0x140+0x10,v[5]);put(info+0x140+0x20,1);put(info+0x140+0x30,2);
    put(info+0x140+0x38,1);put(info+0x140+0x48,v[5]);
    }
    return same(address(e,e->soc_data),soc,sizeof(soc)) && same(address(e,e->pe_info),info,sizeof(info)) &&
        same(address(e,e->config_bits),bits,4) && same(address(e,e->debug_enabled),zero,4) &&
        same(address(e,e->degradation),factor,4);
}

/* Diagnostic return never relaxes the original capture, AT, cache or table
   gates. A failed data check stays failed; only the bounded Linux receiver
   becomes available after restoring the saved EL1/EL2 state. */
int xnu_pe_return_valid(const uint64_t *capture,const uint64_t *pool,const struct xnu_expected *e) {
    return (e->mode==7 || e->mode==8) && xnu_capture_valid(capture,e) && xnu_tables_valid(pool,e);
}
