/* SPDX-License-Identifier: BSD-2-Clause */
#include "xnu_checkpoint.h"
extern uint8_t _xnu_payload_start[],_xnu_payload_end[],_xnu_window[],_image_end[];
void probe_value(const char *,uint64_t);
struct xnu_expected xnu_expected;
#define expected xnu_expected
uint64_t xnu_capture[27];
struct xnu_handoff xnu_prepare(void) {
    struct xnu_handoff out={0,0};uint64_t cpu,freq,el;
    __asm__ volatile("mrs %0, mpidr_el1":"=r"(cpu));
    __asm__ volatile("mrs %0, cntfrq_el0":"=r"(freq));
    __asm__ volatile("mrs %0, CurrentEL":"=r"(el));
    /* x19 is saved in the EL2 snapshot before any XNU code executes. */
    extern uint64_t xnu_saved[10];
    int allowed=(el==4 && (uintptr_t)_image_end<(1ull<<38) && (size_t)(_image_end-_xnu_window)==XNU_ARENA_BYTES &&
        xnu_range_allowed((const void *)(uintptr_t)xnu_saved[0],(uintptr_t)_xnu_window,XNU_ARENA_BYTES));
    int loaded=allowed && xnu_load(_xnu_payload_start,(size_t)(_xnu_payload_end-_xnu_payload_start),
        _xnu_window,XNU_ARENA_BYTES,cpu&0xff00ffffffull,freq,&out,&expected);
    probe_value("XNU copied=",(uint64_t)loaded);
    if (loaded) __asm__ volatile("dsb sy; ic iallu; dsb sy; isb":::"memory");
    return out;
}
int xnu_checkpoint_complete(void) {
    uint64_t *c=xnu_capture;
    int good=xnu_capture_valid(c,&expected);
    if (expected.mode>=2) {
        int tables=good && xnu_tables_valid((const uint64_t *)(uintptr_t)expected.pages,&expected);
        probe_value("XNU MMU enabled=",(c[2]&0x1005)==0x1005);
        probe_value("XNU tables verified=",(uint64_t)tables);
        probe_value("XNU translations verified=",(uint64_t)good);
        good=good && tables;
    }
    if (expected.mode==3) {
        probe_value("XNU high VA verified=",(uint64_t)good);
        probe_value("XNU stack read verified=",(uint64_t)good);
        probe_value("XNU C entry verified=",(uint64_t)good);
        probe_value("XNU args VA=",c[18]);
    }
    if (expected.mode>=4) {
        int data=good && xnu_cinit_valid(&expected);
        good=good && data;
        probe_value("XNU C body verified=",(uint64_t)good);
        probe_value("XNU stack writes verified=",(uint64_t)data);
        probe_value("XNU args copy verified=",(uint64_t)data);
        probe_value("XNU cache maintenance verified=",(uint64_t)good);
        probe_value("XNU args VA=",expected.virt+expected.args-expected.phys);
        probe_value("XNU cache line bytes=",c[25]);
        probe_value("XNU cache lines=",c[26]);
    }
    if (expected.mode>=5) {
        int cpu=good && xnu_cpu_valid(&expected);
        good=good && cpu;
        probe_value("XNU CPU data verified=",(uint64_t)cpu);
    }
    if (expected.mode==6) {
        int platform=good && xnu_platform_valid(&expected);
        good=good && platform;
        probe_value("XNU platform state verified=",(uint64_t)platform);
        probe_value("XNU ADT binding verified=",(uint64_t)platform);
        probe_value("XNU ADT lookup verified=",(uint64_t)platform);
    }
    probe_value("XNU checkpoint=",(uint64_t)good);
    probe_value("XNU boot_args verified=",(uint64_t)good);
    probe_value("XNU current EL=",((c[1]&15)==4 || (c[1]&15)==5)?1:0);
    probe_value("XNU ELR=",c[0]);probe_value("XNU args PA=",expected.args);
    return good;
}
