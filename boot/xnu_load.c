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
    if (len<HEADER || (le32(p+8)<1 || le32(p+8)>7)) return 0;
    uint32_t mode=le32(p+8);
    for (unsigned i=0;i<8;i++) if (p[i]!=(uint8_t)magic[i]) return 0;
    uint32_t count=le32(p+12);
    if (!count || count>64 || !bounded(HEADER,(uint64_t)count*40,len)) return 0;
    uint64_t v[14];for (unsigned i=0;i<14;i++) v[i]=le64(p+16+8*i);
    uint64_t base=v[0],entry=v[1],checkpoint=v[2],pages=v[3];
    uint64_t raw=v[4],rawsize=v[5],adt=v[6],adtsize=v[7],span=v[8],memory=v[9];
    uint64_t phys=(uintptr_t)window,bias=mode>=2?(base&0x1fffff):0;
    uint64_t metadata=HEADER+(uint64_t)count*40;
    if (capacity<XNU_ARENA_BYTES || memory!=XNU_ARENA_BYTES || (phys&0x1fffff) ||
        phys>UINT64_MAX-memory || base>UINT64_MAX-memory || (base&4095) ||
        !span || span>memory || raw<metadata+(mode==7?224u:mode==6?184u:mode==5?112u:mode==4?96u:mode>=2?48u:0u) || (raw&15) ||
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
    uint64_t args=up(span+bias),tree=args+4096,top=up(tree+adtsize);
    if (top>memory || entry<base || checkpoint<base || pages<base ||
        !bounded(pages-base,4096,span) || (pages&4095)) return 0;
    uint64_t extra[6]={0,0,0,0,0,0};
    if (mode>=2) {
        if (!bounded(metadata,48,len) || phys+top>phys+((memory-bias)&~0x1fffffull)) return 0;
        for (unsigned i=0;i<6;i++) extra[i]=le64(p+metadata+i*8);
        if (extra[0]!=0x22a51a251aull || extra[1]!=0x0c0804ff00bb44ffull || extra[5]!=65536 ||
            !bounded(pages-base,65536,span)) return 0;
        for (unsigned i=2;i<5;i++) if (extra[i]<base || extra[i]>=base+span || (extra[i]&15)) return 0;
    }
    uint64_t cinit[6]={0,0,0,0,0,0};
    uint64_t cpu[2]={0,0},platform[9]={0,0,0,0,0,0,0,0,0},pe[5]={0};
    if (mode>=4) {
        if (!bounded(metadata,mode==7?224u:mode==6?184u:mode==5?112u:96u,len)) return 0;
        for (unsigned i=0;i<6;i++) cinit[i]=le64(p+metadata+48+i*8);
        uint64_t stop=mode>=6?0x5b0u:mode==5?0x5a4u:0x594u;
        if (cinit[0]<base || !bounded(cinit[0]-base,stop+4,span)) return 0;
        if (mode!=6 && checkpoint!=cinit[0]+stop) return 0;
        if (mode>=5) {
            cpu[0]=le64(p+metadata+96);cpu[1]=le64(p+metadata+104);
            if (cpu[0]&15) return 0;
        }
        if (mode>=6) {
            for (unsigned i=0;i<9;i++) platform[i]=le64(p+metadata+112+i*8);
            for (unsigned i=4;i<7;i++) if (platform[i]<base || !bounded(platform[i]-base,0x38,span) || (platform[i]&3)) return 0;
            if ((mode==6 && checkpoint!=platform[6]+0x34) || adtsize>65536 || !bounded(platform[7],52,adtsize) ||
                (platform[7]&3) || platform[8]!=platform[7]+52) return 0;
            const uint8_t *node=p+adt+platform[7];
            if (!le32(node) || le32(node+40)!=7) return 0;
            const char name[]="name",target[]="arm-io";
            for (unsigned i=0;i<32;i++) if (node[8+i]!=(i<4?(uint8_t)name[i]:0)) return 0;
            for (unsigned i=0;i<8;i++) if (node[44+i]!=(i<6?(uint8_t)target[i]:0)) return 0;
            for (unsigned i=10;i<12;i++) if (v[i]<platform[8] && platform[7]<v[i]+8) return 0;
        }
        if (mode==7) {
            uint64_t fields[8];
            for (unsigned i=0;i<5;i++) pe[i]=le64(p+metadata+184+i*8);
            if (!xnu_pe_contract(p+adt,(size_t)adtsize,fields) || fields[5]!=frequency ||
                fields[6]!=v[10] || fields[7]!=v[11]) return 0;
            const char line[]="-v debug=0xc cpus=1 keepsyms=1";
            for (unsigned i=0;i<1024;i++) if (p[128+i]!=(i<sizeof(line)?(uint8_t)line[i]:0)) return 0;
        }
        /* Every read/write region is checked before zeroing or copying anything. */
        uint64_t frame=mode>=6?4096u:0xe0u;
        uint64_t starts[18]={extra[3]-frame,cinit[1],cinit[2],cinit[3],cinit[4],cinit[5],pages,cpu[0]-16,cpu[1],platform[0],platform[1],platform[2],platform[3],pe[0],pe[1],pe[2],pe[3],pe[4]};
        uint64_t sizes[18]={frame,0x480,8,8,1,8,65536,0x520,8,0xb0,4,0x1c,8,48,0x208,4,4,4};
        for (unsigned i=0;i<(mode==7?18u:mode==6?13u:mode==5?9u:7u);i++) {
            if (starts[i]<base || !bounded(starts[i]-base,sizes[i],span) || (i!=4 && (starts[i]&(i>=15?3u:7u)))) return 0;
            int inside=0;
            for (unsigned j=0;j<count;j++) {
                const uint8_t *s=p+HEADER+40*j;
                uint64_t off=le64(s),n=le64(s+8),file=le64(s+16),bytes=le64(s+24),prot=le64(s+32);
                if (prot>7 || off>span || n>span-off || bytes>n || !bounded(file,bytes,rawsize)) return 0;
                if (!(prot&4) && starts[i]>=base+off && bounded(starts[i]-base-off,sizes[i],n)) {
                    inside=1;
                    if (i<6 || i==7 || i>=9) for (uint64_t k=0;k<sizes[i];k++) {
                        uint64_t rel=starts[i]-base-off+k;
                        uint8_t want=(i==17 && !k)?1:0;
                        if ((rel<bytes?p[raw+file+rel]:0)!=want) return 0;
                    }
                }
            }
            if (!inside) return 0;
            for (unsigned j=0;j<i;j++) if (starts[i]<starts[j]+sizes[j] && starts[j]<starts[i]+sizes[i]) return 0;
        }
    }
    int executable_entry=0,executable_checkpoint=0,pool=0,stack=0;
    uint64_t last=0;
    for (unsigned i=0;i<count;i++) {
        const uint8_t *s=p+HEADER+40*i;
        uint64_t off=le64(s),n=le64(s+8),file=le64(s+16),bytes=le64(s+24),prot=le64(s+32);
        if (!n || off<last || !bounded(off,n,span) || bytes>n || !bounded(file,bytes,rawsize) || prot>7) return 0;
        last=off+n;
        if (!(prot&4) && pages>=base+off && bounded(pages-base-off,65536,n)) pool=1;
        if (mode==3 && !(prot&4) && n>=0xd0 && extra[3]>=base+off+0xd0 && extra[3]<=base+off+n) {
            uint64_t slot=extra[3]-0x60-base-off;
            if (extra[3]-0xd0<pages+65536 && pages<extra[3]) return 0;
            for (unsigned j=0;j<8;j++) if (slot+j<bytes && p[raw+file+slot+j]) return 0;
            stack=1;
        }
        if (mode>=4 && (prot&4) && cinit[0]>=base+off && bounded(cinit[0]-base-off,12,bytes)) {
            const uint8_t *code=p+raw+file+cinit[0]-base-off;
            if (le32(code)!=0xd10343ff || le32(code+4)!=0xa9076ffc || le32(code+8)!=0xa90867fa) return 0;
            stack=1;
        }
        if ((prot&4) && entry>=base+off && bounded(entry-base-off,4,bytes)) executable_entry=1;
        if ((prot&4) && checkpoint>=base+off && bounded(checkpoint-base-off,4,bytes)) {
            if (le32(p+raw+file+checkpoint-base-off)!=(0xd4000002u|(0x584eu<<5))) return 0;
            if (mode==3 && (checkpoint-base-off<8 ||
                le32(p+raw+file+checkpoint-base-off-4)!=0xf9403bf3 ||
                le32(p+raw+file+checkpoint-base-off-8)!=0xd10343ff)) return 0;
            executable_checkpoint=1;
        }
    }
    if ((mode>=3 && !stack) || (mode>=2 && !pool) || last!=span || !executable_entry || !executable_checkpoint || (entry&3) || (checkpoint&3)) return 0;
    /* All writes stay in the Image-owned arena, after every input is checked. */
    volatile uint64_t *zero=window;for (uint64_t i=0;i<top/8;i++) zero[i]=0;
    for (unsigned i=0;i<count;i++) {
        const uint8_t *s=p+HEADER+40*i;
        copy((uint8_t *)window+bias+le64(s),p+raw+le64(s+16),(size_t)le64(s+24));
    }
    uint8_t *a=(uint8_t *)window+args,*t=(uint8_t *)window+tree;
    a[0]=2;a[2]=2;
    wr64(a+8,base-bias);wr64(a+16,phys);wr64(a+24,memory);wr64(a+32,phys+top);
    wr64(a+0x60,base-bias+tree);
    for (unsigned i=0;i<4;i++) a[0x68+i]=(uint8_t)(adtsize>>(8*i));
    copy(a+0x6c,p+128,1024);wr64(a+0x478,memory);
    copy(t,p+adt,(size_t)adtsize);wr64(t+v[10],phys);wr64(t+v[11],memory);
    if (mode==5) {
        volatile uint8_t *seed=(volatile uint8_t *)window+bias+(cpu[0]-base);
        for (unsigned i=0;i<0x500;i++) seed[i]=0xa5;
    }
    out->entry=phys+bias+entry-base;out->args=phys+args;
    expected->pc=phys+bias+checkpoint-base;expected->args=out->args;
    expected->virt=base;expected->phys=phys+bias;expected->memory=memory-bias;
    expected->pages=phys+bias+pages-base;expected->flags=0;
    expected->mode=mode;expected->tcr=extra[0];expected->mair=extra[1];
    expected->vbar=extra[2];expected->sp0=extra[3]-(mode==6?0x200u:mode>=3?0xd0u:0u);expected->sp1=extra[4];expected->end=phys+top;
    expected->c_entry=cinit[0];expected->const_args=cinit[1];expected->boot_args=cinit[2];
    expected->slide=cinit[3];expected->static_cache=cinit[4];expected->guard=cinit[5];
    expected->cpu_data=cpu[0];expected->rtclock=cpu[1];
    expected->pe_state=platform[0];expected->protection=platform[1];expected->dt_root=platform[2];expected->dt_cursor=platform[3];
    expected->pe_init=platform[4];expected->identify=platform[5];expected->soc=platform[6];expected->node=platform[7];expected->cursor=platform[8];
    expected->tree=base-bias+tree;expected->tree_size=adtsize;
    expected->soc_data=pe[0];expected->pe_info=pe[1];expected->config_bits=pe[2];
    expected->debug_enabled=pe[3];expected->degradation=pe[4];
    expected->adt_checksum=mode==7?xnu_pe_checksum(t,(size_t)adtsize):0;
    return 1;
}

int xnu_capture_valid(const uint64_t *c,const struct xnu_expected *e) {
    uint64_t high_pc=e->virt+(e->pc-e->phys);
    uint64_t stack_pa=e->phys+(e->sp0+0x70-e->virt);
    if (e->mode<1 || e->mode>7 || !e->pc || e->pc>UINT64_MAX-4 ||
        c[0]!=(e->mode>=3?high_pc+4:e->pc+4) ||
        (c[1]&15)!=(e->mode>=2?4u:5u) ||
        (c[2]&0x1005)!=(e->mode>=2?0x1005u:0u) || c[3]!=(e->mode==6?0:e->mode>=4?e->const_args:e->args) ||
        c[4]!=e->virt || c[5]!=e->phys || c[6]!=e->memory ||
        c[7]!=e->pages || c[8]!=e->flags) return 0;
    if (e->mode==1) return 1;
    if (c[9]!=e->tcr || c[10]!=e->pages || c[11]!=e->pages+4096 || c[12]!=e->mair ||
        c[13]!=e->sp0 || c[14]!=e->sp1 || c[15]!=e->vbar || (c[16]&1) || (c[17]&1) ||
        (c[16]&0x0000fffffffff000ull)!=(e->args&0x0000fffffffff000ull) ||
        (c[17]&0x0000fffffffff000ull)!=((e->pc+4)&0x0000fffffffff000ull)) return 0;
    if (e->mode==2) return 1;
    if (e->mode>=4) {
        uint64_t line=c[25];
        return c[18]==(e->mode==7?0xffffffffu:e->mode==6?1:e->mode==5?e->cpu_data:e->const_args) &&
            c[19]==(e->mode==7?e->cpu_data:e->mode==6?e->const_args:e->mode==5?e->cpu_data:e->virt+(e->args-e->phys)) &&
            c[20]==0x300000 && c[21]==0 && c[22]==(e->mode>=5?high_pc:high_pc-8) &&
            (e->mode<5 || !(c[2]&2)) && c[24]==e->sp0+(e->mode==6?0x30u:0xc0u) &&
            !(c[23]&1) && (c[23]&0x0000fffffffff000ull)==(stack_pa&0x0000fffffffff000ull) &&
            line>=4 && line<=131072 && !(line&(line-1)) && c[26]==XNU_ARENA_BYTES/line;
    }
    return c[18]==e->virt+(e->args-e->phys) && c[19]==0 && c[20]==0x300000 &&
        c[21]==0 && c[22]==high_pc-8 && !(c[23]&1) &&
        (c[23]&0x0000fffffffff000ull)==(stack_pa&0x0000fffffffff000ull);
}
