/* SPDX-License-Identifier: BSD-2-Clause */
/* Read-only USB experiment: run the libpayload xHCI and mass storage stack from the probe,
   read three sectors whose content is known, stop the controller, return to Linux.
   CPU access to the controller and its DMA are separate questions, so the SMMU and xHCI
   status are logged around the reads, and the data is checked against what the medium holds.
   Only READ commands are issued. The caches are off, so DMA memory is coherent. */
#include <libpayload.h>
#include <usb/usb.h>
#include <usb/usbmsc.h>
#include <usb/usbdisk.h>
#include "xhci.h"
#include "xhci_private.h"

#define OFFSET(member, field) __builtin_offsetof(__typeof__(*((xhci_t *)0)->member), field)
_Static_assert(OFFSET(capreg, rtsoff) == 0x18, "xHCI capability registers");
_Static_assert(OFFSET(opreg, crcr_lo) == 0x18 && OFFSET(opreg, dcbaap_lo) == 0x30 && OFFSET(opreg, prs) == 0x400, "xHCI operational registers");
_Static_assert(OFFSET(hcrreg, intrrs) == 0x20, "xHCI runtime registers");

void probe_value(const char *, uint64_t);

#define XHCI_BASE 0x0a600000ul
#define SMMU_BASE 0x15000000ul
#define HEAP_BYTES (8u * 1024u * 1024u)

static uint8_t image_heap[HEAP_BYTES] __attribute__((aligned(4096)));
static uint8_t *heap;
static size_t heap_used;
static usbdev_t *disk;
static uint64_t ticks_per_us;
static uint32_t fatal_count;

void usb_read_fatal(void) { fatal_count++; }

void *memset(void *d, int c, size_t n) {
    volatile uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    volatile uint8_t *p = d;
    const volatile uint8_t *q = s;
    while (n--) *p++ = *q++;
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return *p - *q;
    return 0;
}
void *memalign(size_t align, size_t size) {
    size_t at = (heap_used + align - 1) & ~(align - 1);
    if (at > HEAP_BYTES || size > HEAP_BYTES - at) { usb_read_fatal(); return NULL; }
    heap_used = at + size;
    return heap + at;
}
void *malloc(size_t size) { return memalign(16, size); }
void *calloc(size_t count, size_t size) {
    void *p = malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
void free(void *p) { (void)p; }
void *dma_malloc(size_t size) { return memalign(16, size); }
void *dma_memalign(size_t align, size_t size) { return memalign(align, size); }
int dma_coherent(const void *p) { (void)p; return 1; }

static uint64_t now(void) {
    uint64_t t;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
    return t;
}
void udelay(unsigned us) { uint64_t start = now(); while (now() - start < us * ticks_per_us) {} }
void mdelay(unsigned ms) { while (ms--) udelay(1000); }
int gettimeofday(struct timeval *tv, void *zone) {
    uint64_t us = now() / ticks_per_us;
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

#define HIGH_ARENA 0xa1000000ul
#define USBSTS_ERRORS ((1u << 2) | (1u << 12)) /* HSE, HCE */

int fdt_range_clear(const void *, uint64_t, uint64_t);

static uint32_t reg32(unsigned long at) { return read32((void *)at); }
static unsigned long operational(void) { return XHCI_BASE + (reg32(XHCI_BASE) & 0xff); }

/* probe_value() with the attempt letter in the name; no tables of string pointers here. */
static void value(char attempt, const char *suffix, uint64_t v) {
    char name[48] = "USB ";
    unsigned at = 4;
    name[at++] = attempt;
    name[at++] = ' ';
    while (*suffix && at < sizeof(name) - 1) name[at++] = *suffix++;
    name[at] = 0;
    probe_value(name, v);
}
static void port_status(char attempt) {
    value(attempt, "PORTSC1=", reg32(operational() + 0x400));
    value(attempt, "PORTSC2=", reg32(operational() + 0x410));
}
static uint64_t le64_at(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void read_sector(char attempt, const char *result, const char *data, int lba, unsigned at, uint8_t *sector) {
    value(attempt, result, (uint64_t)(uint32_t)readwrite_blocks_512(disk, lba, 1, cbw_direction_data_in, sector));
    value(attempt, data, le64_at(sector + at));
}

/* Returns 1 when the sectors were read; 0 when the controller failed or found no disk. */
static int attempt_read(char attempt, uint8_t *arena) {
    heap = arena;
    heap_used = 0;
    disk = NULL;
    value(attempt, "arena=", (uintptr_t)arena);
    hci_t *controller = xhci_init(XHCI_BASE);
    value(attempt, "controller=", controller != NULL);
    if (!controller) return 0;
    value(attempt, "USBSTS init=", reg32(operational() + 4));
    value(attempt, "USBCMD=", reg32(operational()));
    value(attempt, "DCBAAP=", reg32(operational() + 0x30));
    port_status(attempt);
    uint64_t start = now();
    while (!disk && now() - start < 20000000 * ticks_per_us && !(reg32(operational() + 4) & USBSTS_ERRORS)) {
        usb_poll();
        mdelay(10);
    }
    uint32_t state = reg32(operational() + 4);
    value(attempt, "waited ms=", (now() - start) / (1000 * ticks_per_us));
    value(attempt, "USBSTS poll=", state);
    port_status(attempt);
    value(attempt, "disk=", disk != NULL);
    int done = 0;
    if (disk && !(state & USBSTS_ERRORS)) {
        uint8_t *sector = dma_memalign(4096, 4096);
        value(attempt, "address=", (uint64_t)disk->address);
        value(attempt, "speed=", (uint64_t)disk->speed);
        value(attempt, "blocks=", MSC_INST(disk)->numblocks);
        value(attempt, "block size=", MSC_INST(disk)->blocksize);
        read_sector(attempt, "lba0 rc=", "lba0 tail=", 0, 504, sector);
        read_sector(attempt, "lba1 rc=", "lba1 head=", 1, 0, sector);
        read_sector(attempt, "lba2048 rc=", "lba2048 head=", 2048, 0, sector);
        done = 1;
    }
    value(attempt, "SMMU sGFSR=", reg32(SMMU_BASE + 0x48));
    value(attempt, "heap used=", heap_used);
    usb_exit();
    return done;
}

void usb_read_run(const void *dt) {
    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    ticks_per_us = frequency / 1000000;
    probe_value("USB CAPLENGTH=", reg32(XHCI_BASE) & 0xff);
    probe_value("USB HCSPARAMS1=", reg32(XHCI_BASE + 4));
    probe_value("USB HCSPARAMS2=", reg32(XHCI_BASE + 8));
    probe_value("USB HCCPARAMS1=", reg32(XHCI_BASE + 16));
    probe_value("USB USBSTS=", reg32(operational() + 4));
    probe_value("USB PAGESIZE=", reg32(operational() + 8));
    probe_value("USB GSBUSCFG0=", reg32(XHCI_BASE + 0xc100));
    probe_value("USB GSBUSCFG1=", reg32(XHCI_BASE + 0xc104));
    probe_value("USB GCTL=", reg32(XHCI_BASE + 0xc110));
    probe_value("USB GSNPSID=", reg32(XHCI_BASE + 0xc120));
    probe_value("USB GUSB2PHYCFG=", reg32(XHCI_BASE + 0xc200));
    probe_value("USB GUSB3PIPECTL=", reg32(XHCI_BASE + 0xc2c0));
    probe_value("USB SMMU sCR0=", reg32(SMMU_BASE));
    probe_value("USB SMMU sGFSR=", reg32(SMMU_BASE + 0x48));
    if (!attempt_read('A', image_heap)) {
        int clear = fdt_range_clear(dt, HIGH_ARENA, HEAP_BYTES);
        probe_value("USB high arena clear=", clear);
        if (clear) {
            memset((void *)HIGH_ARENA, 0, HEAP_BYTES);
            attempt_read('B', (uint8_t *)HIGH_ARENA);
        }
    }
    probe_value("USB fatal count=", fatal_count);
    probe_value("USB done=", 1);
}
