/* SPDX-License-Identifier: BSD-2-Clause */
#include <stddef.h>
#include <stdint.h>
#ifndef HOST_GUARD_CASE
#define HOST_GUARD_CASE 0
#endif
extern int display_read32_abi(uint64_t,uint32_t *);
extern uint8_t display_read_fault[],display_fault_dispatch[];
extern uint64_t display_read_guard[2];
extern void display_inject(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static void semihost(uint64_t op,const void *arg) {
    register uint64_t a __asm__("x0")=op;register const void *b __asm__("x1")=arg;
    __asm__ volatile("hlt #0xf000":"+r"(a):"r"(b):"memory");
}
static void done(int good) {
    uint64_t status[2]={0x20026,(uint64_t)!good};
    semihost(4,good?"DISPLAY_MODEL_PASS\n":"DISPLAY_MODEL_FAIL\n");semihost(0x20,status);
    for(;;)__asm__ volatile("wfe");
}
#if HOST_GUARD_CASE
static uint64_t wanted_esr,wanted_pc,wanted_far,wanted_spsr,wanted_vector;
#else
static uint64_t root[512] __attribute__((aligned(4096)));
static uint32_t source=0x89abcdef;
#endif
void probe_main(const void *dt,uint64_t el,uint64_t sctlr,uint64_t hcr) {
    (void)dt;(void)sctlr;(void)hcr;if(el!=8)done(0);
#if HOST_GUARD_CASE
    wanted_esr=0x96000005;wanted_pc=(uintptr_t)display_read_fault;wanted_far=0x80000000;wanted_spsr=0x3c5;wanted_vector=4;
    display_read_guard[0]=1;display_read_guard[1]=0x80000000;
    if(HOST_GUARD_CASE==1)wanted_esr|=1<<10;
    if(HOST_GUARD_CASE==2)wanted_esr|=1<<6;
    if(HOST_GUARD_CASE==3)wanted_pc+=4;
    if(HOST_GUARD_CASE==4)wanted_far+=4;
    if(HOST_GUARD_CASE==5)wanted_spsr=0x3c4;
    if(HOST_GUARD_CASE==6)wanted_vector=0;
    if(HOST_GUARD_CASE==7)display_read_guard[0]=0;
    if(HOST_GUARD_CASE==8)display_read_guard[1]+=4;
    if(HOST_GUARD_CASE==9)wanted_esr=0x92000005;
    display_inject(wanted_esr,wanted_pc,wanted_far,wanted_spsr,wanted_vector,0x80000000);
    done(0);
#else
    uint32_t out=0;uint64_t saved[4]={0x12345,0x12340000,0x56780000,0x3c5},after[4];
    __asm__ volatile("msr esr_el1,%0;msr elr_el1,%1;msr far_el1,%2;msr spsr_el1,%3;isb"::"r"(saved[0]),"r"(saved[1]),"r"(saved[2]),"r"(saved[3]):"memory");
    if(display_read32_abi((uintptr_t)&source,&out)!=1 || out!=source || display_read_guard[0] || display_read_guard[1])done(0);
    __asm__ volatile("mrs %0,esr_el1;mrs %1,elr_el1;mrs %2,far_el1;mrs %3,spsr_el1":"=r"(after[0]),"=r"(after[1]),"=r"(after[2]),"=r"(after[3]));
    for(unsigned i=0;i<4;i++)if(after[i]!=saved[i])done(0);
    root[1]=0x40000701;
    uint64_t tcr=25|(1ull<<8)|(1ull<<10)|(3ull<<12)|(25ull<<16)|(1ull<<23),mair=255,table=(uintptr_t)root,ctl;
    __asm__ volatile("dsb sy;msr mair_el1,%1;msr tcr_el1,%2;msr ttbr0_el1,%3;isb;tlbi vmalle1;dsb sy;isb;mrs %0,sctlr_el1":"=r"(ctl):"r"(mair),"r"(tcr),"r"(table):"memory");
    uint64_t on=ctl|1;__asm__ volatile("msr sctlr_el1,%0;isb"::"r"(on):"memory");
    for(unsigned attempt=0;attempt<2;attempt++) {
        out=0xa5a5a5a5;
        if(display_read32_abi(0x80000000,&out)!=0 || out!=0xa5a5a5a5 || display_read_guard[0] || display_read_guard[1])done(0);
        __asm__ volatile("mrs %0,esr_el1;mrs %1,elr_el1;mrs %2,far_el1;mrs %3,spsr_el1":"=r"(after[0]),"=r"(after[1]),"=r"(after[2]),"=r"(after[3]));
        for(unsigned i=0;i<4;i++)if(after[i]!=saved[i])done(0);
    }
    out=0;if(display_read32_abi((uintptr_t)&source,&out)!=1 || out!=source)done(0);
    __asm__ volatile("dsb sy;msr sctlr_el1,%0;isb"::"r"(ctl):"memory");
#endif
}
void probe_exception(uint64_t esr,uint64_t pc,uint64_t far,uint64_t vector) {
#if HOST_GUARD_CASE
    done(esr==wanted_esr && pc==wanted_pc && far==wanted_far && vector==wanted_vector && !display_read_guard[0] && !display_read_guard[1]);
#else
    (void)esr;(void)pc;(void)far;(void)vector;done(0);
#endif
}
int probe_export_linux_log(void *dt) {(void)dt;semihost(4,"DISPLAY_MODEL_HVC_RETURN\n");return 1;}
void host_finish(void) {done(HOST_GUARD_CASE==0);}
