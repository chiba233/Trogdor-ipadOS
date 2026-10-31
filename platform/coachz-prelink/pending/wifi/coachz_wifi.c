/* SPDX-License-Identifier: BSD-2-Clause */
/* WCN3990 / SC7180 first-stage SMEM inventory. Not in the kernel build.
 * coachz-wifi=1 plus an explicit /arm-io/coachz-wifi ADT node enables it.
 * Reads only the public table, the global partition and APPS<->MODEM.
 * This does not start MPSS, allocate shared items, acknowledge GLINK,
 * access the radio, submit DMA, register a MAC or claim WLAN readiness.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include "coachz_wifi_smem.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
void coachz_wifi_start(void);

struct wifi_mapping { uintptr_t base; };

/* Reserved shared memory is mapped uncached. Use aligned word loads even for
 * byte/halfword protocol fields, as on the existing strict-alignment probes.
 */
static int
wifi_read(void *context, uint32_t offset, void *out, uint32_t bytes)
{
    const struct wifi_mapping *mapping = context;
    uint8_t *p = out;
    if (!cw_span(CW_SMEM_SIZE, offset, bytes)) return -1;
    for (uint32_t i = 0; i < bytes;) {
        const uint32_t at = offset + i;
        const uint32_t word = *(volatile uint32_t *)(mapping->base + (at & ~3u));
        __asm__ volatile ("dmb oshld" ::: "memory");
        unsigned lane = at & 3u;
        while (i < bytes && lane < 4) p[i++] = (uint8_t)(word >> (8 * lane++));
    }
    return 0;
}

static void
wifi_partition(const char *name, int present, const struct cw_partition *p)
{
    if (!present) { printf("COACHZ wifi: %s partition absent\n", name); return; }
    printf("COACHZ wifi: %s hosts %u/%u offset 0x%x size 0x%x uncached 0x%x cached 0x%x line %u\n",
        name, p->host0, p->host1, p->offset, p->bytes, p->uncached, p->cached, p->cacheline);
}

static void
wifi_modem_state(const struct cw_reader *r, const struct cw_smem *layout)
{
    struct cw_item item;
    struct cw_smp2p state;
    int status = layout->has_global ? cw_smem_find(r, &layout->global, CW_SMP2P_IN, &item) : CW_ABSENT;
    if (!status) status = cw_smp2p_read(r, &item, &state);
    if (status) { printf("COACHZ wifi: incoming SMP2P item %u status %d\n", CW_SMP2P_IN, status); return; }
    printf("COACHZ wifi: SMP2P version %u features 0x%x flags 0x%x entries %u slave %d state 0x%x (snapshot)\n",
        state.version, state.features, state.flags, state.valid, state.has_slave, state.state);
}

static void
wifi_glink_state(const struct cw_reader *r, const struct cw_smem *layout)
{
    const uint16_t ids[] = {CW_GLINK_DESC, CW_GLINK_TX, CW_GLINK_RX};
    struct cw_item item[3] = {{0}};
    int status[3];
    for (unsigned i = 0; i < 3; i++) {
        status[i] = layout->has_modem ? cw_smem_find(r, &layout->modem, ids[i], &item[i]) : CW_ABSENT;
        printf("COACHZ wifi: GLINK item %u status %d offset 0x%x size 0x%x\n", ids[i], status[i], item[i].offset, item[i].bytes);
    }
    if (!status[0] && item[0].bytes >= 16) {
        uint8_t descriptor[16];
        if (cw_read(r, item[0].offset, descriptor, sizeof(descriptor))) return;
        const uint32_t tx_tail = cw_u32(descriptor), tx_head = cw_u32(descriptor + 4);
        const uint32_t rx_tail = cw_u32(descriptor + 8), rx_head = cw_u32(descriptor + 12);
        const int bounded = !status[1] && !status[2] && item[1].bytes && item[2].bytes &&
            tx_tail < item[1].bytes && tx_head < item[1].bytes && rx_tail < item[2].bytes && rx_head < item[2].bytes;
        printf("COACHZ wifi: GLINK tx %u/%u rx %u/%u bounded %d (not consumed)\n", tx_tail, tx_head, rx_tail, rx_head, bounded);
    }
}

/* Call from the existing sampler thread after IOKit has started, once. */
void
coachz_wifi_start(void)
{
    static int started;
    unsigned mode = 0;
    if (started || !PE_parse_boot_argn("coachz-wifi", &mode, sizeof(mode)) || mode != 1) return;
    started = 1;
    DTEntry node;
    const void *property = NULL;
    unsigned size = 0;
    const char compatible[] = "google,coachz-wcn3990";
    if (SecureDTLookupEntry(NULL, "/arm-io/coachz-wifi", &node) != kSuccess ||
        SecureDTGetProperty(node, "compatible", &property, &size) != kSuccess || size != sizeof(compatible) ||
        memcmp(property, compatible, sizeof(compatible))) {
        printf("COACHZ wifi: explicit Coachz ADT node missing, not mapped\n"); return;
    }
    if (SecureDTGetProperty(node, "smem-region", &property, &size) != kSuccess || size != 16) {
        printf("COACHZ wifi: smem-region must contain a physical base and size, not mapped\n"); return;
    }
    const uint8_t *range = property;
    if (cw_u64(range) != CW_SMEM_BASE || cw_u64(range + 8) != CW_SMEM_SIZE) {
        printf("COACHZ wifi: SMEM differs from this SC7180 layout, not mapped\n"); return;
    }
    const boot_args *args = PE_state.bootArgs;
    if (!args || !cw_disjoint(CW_SMEM_BASE, CW_SMEM_SIZE, args->physBase, args->memSize)) {
        printf("COACHZ wifi: SMEM overlaps the XNU memory window or boot args are unavailable, not mapped\n"); return;
    }
    static struct wifi_mapping mapping;
    mapping.base = (uintptr_t)ml_io_map((vm_offset_t)CW_SMEM_BASE, CW_SMEM_SIZE);
    if (!mapping.base) { printf("COACHZ wifi: SMEM map failed\n"); return; }
    /* Kept for the kernel lifetime; no pointer escapes this module. */
    const struct cw_reader reader = {&mapping, CW_SMEM_SIZE, wifi_read};
    struct cw_smem layout;
    const int status = cw_smem_layout(&reader, &layout);
    printf("COACHZ wifi: SMEM v12 inventory status %d table entries %u\n", status, layout.count);
    if (status) return;
    wifi_partition("global", layout.has_global, &layout.global);
    wifi_partition("apps-modem", layout.has_modem, &layout.modem);
    wifi_modem_state(&reader, &layout);
    wifi_glink_state(&reader, &layout);
    printf("COACHZ wifi: inventory complete; MPSS/WLFW liveness and radio not tested\n");
}
