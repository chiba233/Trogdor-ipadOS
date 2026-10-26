/* SPDX-License-Identifier: BSD-2-Clause */
#include <stddef.h>
#include <stdint.h>
#include "fdt.h"
#include "checkpoint.h"
#ifndef PROBE_RESET_EL
#define PROBE_RESET_EL 0
#endif
#ifndef PROBE_UART
#define PROBE_UART 0
#endif

struct probe_record {
    uint64_t magic;
    uint32_t version, stage;
    uint64_t fdt, incoming_el, incoming_sctlr, incoming_hcr, current_el;
    uint64_t esr, elr, far, vector;
    uint32_t used, uart_ready;
    char text[8192];
};
volatile struct probe_record probe_log;
static int uart_enabled;

#if PROBE_UART
#define UART_BASE 0x00a88000u
static uint32_t rd(unsigned off) { return *(volatile uint32_t *)(uintptr_t)(UART_BASE + off); }
static void wr(unsigned off, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)(UART_BASE + off) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static int idle(void) {
    for (unsigned i = 0; i < 100000; i++) if (!(rd(0x40) & 1)) return 1;
    return 0;
}
static int uart_available(void) {
    /* Reuse only an already configured UART; no clock/GPIO/firmware setup. */
    return ((rd(0x68) >> 8) & 0xff) == 2 && (rd(0x48) & 1) && rd(0x268) == 8;
}
static void uart_byte(char c) {
    if (!uart_enabled) return;
    if (!idle()) { uart_enabled = 0; return; }
    wr(0x270, 1);          /* UART_TX_TRANS_LEN */
    wr(0x600, 0x08000000); /* UART_START_TX */
    wr(0x700, (uint8_t)c); /* GENI_TX_FIFO */
    if (!idle()) uart_enabled = 0;
}
#else
static int uart_available(void) { return 0; }
static void uart_byte(char c) { (void)c; }
#endif

static void put(char c) {
    uint32_t used = probe_log.used;
    if (used + 1 < sizeof(probe_log.text)) {
        probe_log.text[used] = c;
        probe_log.text[used + 1] = 0;
        probe_log.used = used + 1;
    }
    if (c == '\n') uart_byte('\r');
    uart_byte(c);
}
static void text(const char *s) { while (*s) put(*s++); }
static void hex(uint64_t v) {
    const char digits[] = "0123456789abcdef";
    text("0x");
    for (int shift = 60; shift >= 0; shift -= 4) put(digits[(v >> shift) & 15]);
}
static void item(const char *name, uint64_t value) { text(name); hex(value); put('\n'); }
static void property(const void *dt, const char *path, const char *name) {
    uint32_t n;
    const uint8_t *p = fdt_property(dt, FDT_MAX_SIZE, path, name, &n);
    text(path); put(':'); text(name); text(" = ");
    if (!p) { text("<absent>\n"); return; }
    /* Raw bounded hex preserves strings, cell widths and byte ordering. */
    const char digits[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n && i < 128; i++) {
        put(digits[p[i] >> 4]); put(digits[p[i] & 15]);
    }
    if (n > 128) text("...");
    put('\n');
}
void probe_exception(uint64_t esr, uint64_t elr, uint64_t far, uint64_t vector) {
    uart_enabled = 0;
    probe_log.stage = 255;
    probe_log.esr = esr; probe_log.elr = elr;
    probe_log.far = far; probe_log.vector = vector;
    text("EXCEPTION (memory log only)\n");
    item("ESR=", esr); item("ELR=", elr); item("FAR=", far);
    __asm__ volatile("dsb sy" ::: "memory");
    for (;;) __asm__ volatile("wfe");
}
void probe_main(const void *dt, uint64_t incoming_el, uint64_t sctlr, uint64_t hcr) {
    uint64_t current, freq, midr, mpidr;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(current));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    probe_log.magic = 0x54524f4750524f42ull;
    probe_log.version = 1; probe_log.stage = 1;
    probe_log.fdt = (uintptr_t)dt; probe_log.incoming_el = incoming_el >> 2;
    probe_log.incoming_sctlr = sctlr; probe_log.incoming_hcr = hcr;
    probe_log.current_el = current >> 2;
    text("Trogdor-ipadOS native phase-0 probe\n");
    item("entry EL=", incoming_el >> 2); item("current EL=", current >> 2);
    item("entry SCTLR=", sctlr); item("entry HCR=", hcr);
    item("CNTFRQ=", freq); item("MIDR=", midr); item("MPIDR=", mpidr);
    item("FDT PA=", (uintptr_t)dt);
    if (!dt || ((uintptr_t)dt & 7) || !fdt_valid(dt, FDT_MAX_SIZE)) {
        text("FDT invalid; stopped before peripheral access\n");
        probe_log.stage = 2;
        return;
    }
    property(dt, "/", "model"); property(dt, "/", "compatible");
    property(dt, "/memory", "reg");
    property(dt, "/chosen", "linux,initrd-start");
    property(dt, "/chosen", "linux,initrd-end");
    property(dt, "/firmware/coreboot", "reg");
    property(dt, "/firmware/coreboot", "board-id");
    property(dt, "/firmware/coreboot", "sku-id");
    text("phase-0 complete; no XNU loaded\n");
    probe_log.stage = 3;
    /* All state is already in RAM if UART probing itself faults. */
    uart_enabled = uart_available();
    probe_log.uart_ready = (uint32_t)uart_enabled;
    uint32_t count = probe_log.used;
    for (uint32_t i = 0; i < count && uart_enabled; i++) {
        char c = probe_log.text[i];
        if (c == '\n') uart_byte('\r');
        uart_byte(c);
    }
#if PROBE_RESET_EL
    text("diagnostic checkpoint; requesting timed reset\n");
    probe_log.stage = (uint32_t)checkpoint_run((unsigned)(current >> 2),
                                              PROBE_RESET_EL, probe_log.stage);
#endif
    __asm__ volatile("dsb sy" ::: "memory");
}
