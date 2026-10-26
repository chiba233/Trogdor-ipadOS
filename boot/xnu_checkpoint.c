/* SPDX-License-Identifier: BSD-2-Clause */
#include "xnu_checkpoint.h"
extern uint8_t _xnu_payload_start[],_xnu_payload_end[],_xnu_window[],_image_end[];
void probe_value(const char *,uint64_t);
static struct xnu_expected expected;
uint64_t xnu_capture[9];
struct xnu_handoff xnu_prepare(void) {
    struct xnu_handoff out={0,0};uint64_t cpu,freq,el;
    __asm__ volatile("mrs %0, mpidr_el1":"=r"(cpu));
    __asm__ volatile("mrs %0, cntfrq_el0":"=r"(freq));
    __asm__ volatile("mrs %0, CurrentEL":"=r"(el));
    /* x19 is saved in the EL2 snapshot before any XNU code executes. */
    extern uint64_t xnu_saved[10];
    int allowed=(el==4 && (size_t)(_image_end-_xnu_window)==XNU_ARENA_BYTES &&
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
    probe_value("XNU checkpoint=",(uint64_t)good);
    probe_value("XNU boot_args verified=",(uint64_t)good);
    probe_value("XNU current EL=",(c[1]&15)==5?1:0);
    probe_value("XNU ELR=",c[0]);probe_value("XNU args PA=",c[3]);
    return good;
}
