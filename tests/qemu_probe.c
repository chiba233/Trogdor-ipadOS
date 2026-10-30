/* SPDX-License-Identifier: BSD-2-Clause */
/* CPU-model test only: semihost log, board admission and Linux entry are stubs. */
#include <stddef.h>
#include <stdint.h>
static int checkpoint,transport;
static void text(const char *s) {
    register uint64_t op __asm__("x0")=4;
    register const char *arg __asm__("x1")=s;
    __asm__ volatile("hlt #0xf000":"+r"(op),"+r"(arg)::"memory","cc");
}
static int same(const char *a,const char *b) { while(*a && *a==*b){a++;b++;}return *a==*b; }
void probe_value(const char *name,uint64_t v) {
    const char hex[]="0123456789abcdef";char b[20]="0x";
    for(unsigned i=0;i<16;i++) b[2+i]=hex[(v>>(60-i*4))&15];b[18]='\n';b[19]=0;
    text(name);text(b);
    if(same(name,"XNU checkpoint="))checkpoint=(int)v;
    if(same(name,"XNU diagnostic transport verified="))transport=(int)v;
}
static void finish(unsigned status) {
    uint64_t block[2]={0x20026,status};
    register uint64_t op __asm__("x0")=0x20;
    register uint64_t *arg __asm__("x1")=block;
    __asm__ volatile("hlt #0xf000":"+r"(op),"+r"(arg)::"memory","cc");
    for(;;)__asm__ volatile("wfe");
}
void host_finish(void) {
    uint64_t a,b,c;__asm__ volatile("mrs %0,cntkctl_el1":"=r"(a));
    __asm__ volatile("mrs %0,tpidr_el0":"=r"(b));__asm__ volatile("mrs %0,tpidrro_el0":"=r"(c));
    probe_value("HOST restored CNTKCTL=",a);probe_value("HOST restored TPIDR=",b);probe_value("HOST restored TPIDRRO=",c);
    text("HOST_LINUX_ENTRY_REACHED\n");
    finish((checkpoint||transport) && a==HOST_CNTKCTL && b==HOST_TPIDR && c==HOST_TPIDRRO?0:2);
}
void probe_main(const void *dt,uint64_t incoming,uint64_t sctlr,uint64_t hcr) {
    (void)dt;(void)sctlr;(void)hcr;
    uint64_t seed=HOST_CNTKCTL;__asm__ volatile("msr cntkctl_el1,%0"::"r"(seed):"memory");
    seed=HOST_TPIDR;__asm__ volatile("msr tpidr_el0,%0"::"r"(seed):"memory");
    seed=HOST_TPIDRRO;__asm__ volatile("msr tpidrro_el0,%0;isb"::"r"(seed):"memory");
    uint64_t freq,cpu,el;
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(freq));
    __asm__ volatile("mrs %0,mpidr_el1":"=r"(cpu));
    __asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    text("Trogdor-ipadOS native phase-0 probe\n");
    probe_value("entry EL=",incoming>>2);probe_value("current EL=",el>>2);
    probe_value("HOST EL=",el);probe_value("HOST CNTFRQ=",freq);probe_value("HOST MPIDR=",cpu);
}
void probe_exception(uint64_t esr,uint64_t elr,uint64_t far,uint64_t vector) {
    probe_value("HOST UNEXPECTED ESR=",esr);probe_value("HOST UNEXPECTED ELR=",elr);
    probe_value("HOST UNEXPECTED FAR=",far);probe_value("HOST UNEXPECTED vector=",vector);finish(3);
}
int probe_export_linux_log(void *dt) {
    (void)dt;uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if(el!=8)return 0;
    probe_value("stage=",3);probe_value("handoff EL=",el>>2);
    text("HOST_LOG_EXPORT_REACHED\n");return 1;
}
/* This system test excludes board/FDT admission, already separately tested. */
int xnu_range_allowed(const void *dt,uint64_t lo,uint64_t size) {(void)dt;(void)lo;(void)size;return 1;}
