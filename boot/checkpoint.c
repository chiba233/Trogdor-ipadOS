/* SPDX-License-Identifier: BSD-2-Clause */
#include "checkpoint.h"

unsigned checkpoint_delay(unsigned current_el, unsigned target_el,
                          unsigned stage, int32_t version, uint64_t freq) {
    if (stage != 3 || current_el != target_el || version < 2 ||
        freq < 1000 || freq > 1000000000) return 0;
    if (target_el == 2) return 3;
    if (target_el == 1) return 10;
    return 0;
}

#if defined(__aarch64__) && !defined(CHECKPOINT_HOST_TEST)
static uint64_t psci(uint64_t function) {
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = 0;
    register uint64_t x2 __asm__("x2") = 0;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                         "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
    return x0;
}
static uint64_t counter(void) {
    uint64_t ticks;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(ticks) : : "memory");
    return ticks;
}
int checkpoint_run(unsigned current_el, unsigned target_el, unsigned stage) {
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    int32_t version = (int32_t)psci(0x84000000); /* PSCI_VERSION */
    unsigned seconds = checkpoint_delay(current_el, target_el, stage, version, freq);
    if (!seconds) return 4;
    uint64_t start = counter(), duration = freq * seconds;
    while (counter() - start < duration) __asm__ volatile("yield");
    __asm__ volatile("dsb sy" : : : "memory");
    psci(0x84000009); /* SYSTEM_RESET, same interface as factory Depthcharge. */
    return 5;
}
#endif
