/* SPDX-License-Identifier: BSD-2-Clause */
/* Runs the libpayload xHCI and mass storage stack in the kernel, as boot/usb_read.c does in the
   probe. libpayload writes zero into every upper address word, and the kernel's memory is above
   4 GiB, so its memory is a fixed pool below 4 GiB that XNU does not manage, mapped uncached.
   The probe experiment showed DMA reaches this range and the SMMU is in bypass. It polls: no
   interrupt is used. Only READ commands are issued. Calls are serialized by the driver. */
#include <libpayload.h>
#include <usb/usb.h>
#include <usb/usbmsc.h>
#include <usb/usbdisk.h>
#include "xhci.h"

extern void kprintf(const char *, ...);
extern unsigned long ml_io_map(unsigned long, unsigned long);
extern unsigned long ml_io_map_wcomb(unsigned long, unsigned long);
extern void IODelay(unsigned);
extern unsigned long long mach_absolute_time(void);
extern void absolutetime_to_nanoseconds(unsigned long long, unsigned long long *);

#define XHCI_BASE 0x0a600000ul
#define XHCI_BYTES 0x100000ul
#define POOL_BASE 0x81000000ul
/* ml_io_map allocates from the 8 MiB ARM io_submap, shared with every other device mapping.
   The probe's stack used at most 176 KiB; the transfer buffer adds 64 KiB. */
#define POOL_BYTES (1ul * 1024 * 1024)
#define MAX_BLOCKS 128u
#define USBSTS_ERRORS ((1u << 2) | (1u << 12)) /* HSE, HCE */

static unsigned long xhci_va, pool_va;
static size_t pool_used;
static usbdev_t *disk;
static uint8_t *transfer;
static uint32_t fatal_count;

void coachz_usb_fatal(void) { fatal_count++; }

void *coachz_usb_p2v(unsigned long pa) {
    if (pa >= XHCI_BASE && pa - XHCI_BASE < XHCI_BYTES) return (void *)(xhci_va + (pa - XHCI_BASE));
    return (void *)(pool_va + (pa - POOL_BASE));
}
unsigned long coachz_usb_v2p(const void *va) { return (unsigned long)va - pool_va + POOL_BASE; }

void *coachz_usb_memalign(size_t align, size_t size) {
    size_t at = (pool_used + align - 1) & ~(align - 1);
    if (at > POOL_BYTES || size > POOL_BYTES - at) { coachz_usb_fatal(); return NULL; }
    pool_used = at + size;
    return (void *)(pool_va + at);
}
void *coachz_usb_malloc(size_t size) { return coachz_usb_memalign(16, size); }
void *coachz_usb_calloc(size_t count, size_t size) {
    void *p = coachz_usb_malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
void coachz_usb_free(void *p) { (void)p; }
int dma_coherent(const void *p) { return (unsigned long)p - pool_va < POOL_BYTES; }
void udelay(unsigned us) { IODelay(us); }
void mdelay(unsigned ms) { while (ms--) IODelay(1000); }
static uint64_t now_us(void) {
    unsigned long long ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}
int coachz_usb_gettimeofday(struct timeval *tv, void *zone) {
    uint64_t us = now_us();
    (void)zone;
    tv->tv_sec = (time_t)(us / 1000000);
    tv->tv_usec = (long)(us % 1000000);
    return 0;
}

void usb_poll_prepare(void) {}
void usb_generic_create(usbdev_t *dev) { (void)dev; }
void usb_generic_remove(usbdev_t *dev) { (void)dev; }
void usbdisk_create(usbdev_t *dev) { if (!disk) disk = dev; }
void usbdisk_remove(usbdev_t *dev) { if (disk == dev) disk = NULL; }

static uint32_t usbsts(void) {
    uint32_t caplength = read32((void *)xhci_va) & 0xff;
    return read32((void *)(xhci_va + caplength + 4));
}
static uint64_t le64_at(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

uint32_t coachz_usb_max_blocks(void) { return MAX_BLOCKS; }

int coachz_usb_read(uint64_t block, uint32_t count, void **data) {
    if (!disk || count > MAX_BLOCKS || block + count > 0x7fffffffull) return 0;
    if (readwrite_blocks_512(disk, (int)block, (int)count, cbw_direction_data_in, transfer)) return 0;
    *data = transfer;
    return 1;
}

int coachz_usb_open(uint64_t *blocks) {
    xhci_va = ml_io_map(XHCI_BASE, XHCI_BYTES);
    pool_va = ml_io_map_wcomb(POOL_BASE, POOL_BYTES);
    kprintf("COACHZ USB: mapped %d %d USBSTS 0x%x\n", xhci_va != 0, pool_va != 0, xhci_va ? usbsts() : 0);
    if (!xhci_va || !pool_va) return 0;
    memset((void *)pool_va, 0, POOL_BYTES);
    transfer = coachz_usb_memalign(4096, MAX_BLOCKS * 512);
    hci_t *controller = xhci_init(XHCI_BASE);
    kprintf("COACHZ USB: controller %d USBSTS 0x%x\n", controller != NULL, usbsts());
    if (!controller) return 0;
    uint64_t start = now_us();
    while (!disk && now_us() - start < 20000000 && !(usbsts() & USBSTS_ERRORS)) {
        usb_poll();
        mdelay(10);
    }
    kprintf("COACHZ USB: disk %d after %llu ms USBSTS 0x%x fatal %u pool %lu\n", disk != NULL,
        (now_us() - start) / 1000, usbsts(), fatal_count, (unsigned long)pool_used);
    if (!disk) return 0;
    *blocks = MSC_INST(disk)->numblocks;
    kprintf("COACHZ USB: blocks %llu size %u speed %d\n", (unsigned long long)*blocks, MSC_INST(disk)->blocksize, (int)disk->speed);
    if (MSC_INST(disk)->blocksize != 512) return 0;
    static const struct { int lba; unsigned at; } probes[] = { {0, 504}, {1, 0}, {2048, 0} };
    for (unsigned i = 0; i < 3; i++) {
        void *data = NULL;
        int ok = coachz_usb_read((uint64_t)probes[i].lba, 1, &data);
        kprintf("COACHZ USB: lba %d ok %d value 0x%llx\n", probes[i].lba, ok, ok ? (unsigned long long)le64_at((uint8_t *)data + probes[i].at) : 0ull);
        if (!ok) return 0;
    }
    return 1;
}
