/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The middle of mainline ipa_setup on IPA v4.2, after gsi_setup: ipa_endpoint_setup (program the AP endpoints),
 * ipa_endpoint_enable_one for AP_COMMAND_TX (start its GSI channel), then the command channel's transactions carrying
 * ipa_mem_setup and ipa_table_setup. Not in the kernel build. Enabling AP_LAN_RX, which needs receive buffers, and the
 * default route come next.
 *
 * Transactions (gsi_trans.c): each command takes one TRE on channel 1; its payload sits in a DMA slot tied to the TRE's ring
 * index (the pool is 256 slots of 32 bytes, one per ring entry, so a slot is reused only after its TRE completed). The last
 * TRE has IEOT and, on a TX channel, BEI. Completion is found as gsi_channel_update finds it: EV_CH_E_CNTXT_4 holds the
 * hardware's event write pointer, each new event's xfer_ptr names a TRE, and the event ring doorbell then acknowledges them.
 * Mainline waits without a timeout (gsi_trans_commit_wait); here a second is allowed and a timeout is reported.
 *
 * DMA memory (apps SMMU in bypass, so physical addresses): one write-combined or uncached block of at least
 * CIC_DMA_BYTES, 128-byte aligned: the payload pool, then the 128-byte-aligned table area (ipa_table_init).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include "coachz_ipa_kernel.h"
#include "coachz_ipa_cmd_proto.h"

#define CIC_POOL_SLOT 32u
#define CIC_DMA_BYTES (256u * CIC_POOL_SLOT + 256u)
#define CIC_COMMAND_CHANNEL 1u
#define CIC_WAIT_US 1000000u

struct coachz_ipa_cmd {
    struct coachz_ipa *ipa;
    struct coachz_gsi *gsi;
    uint8_t *dma_virt;
    uint64_t dma_phys;
    uint32_t channel, tre_count, evt_ring;
};
int coachz_ipa_cmd_init(struct coachz_ipa_cmd *c, struct coachz_ipa *ipa, struct coachz_gsi *gsi, void *dma_virt, uint64_t dma_phys, uint32_t bytes);
int coachz_ipa_cmd_run(struct coachz_ipa_cmd *c, const struct cic_cmd *cmds, unsigned count);
int coachz_ipa_setup_commands(struct coachz_ipa_cmd *c);

static inline void ipa_wr(struct coachz_ipa *ipa, uint32_t o, uint32_t v) { *(volatile uint32_t *)(ipa->reg + o) = v; }
static inline uint32_t ipa_rd(struct coachz_ipa *ipa, uint32_t o) { return *(volatile uint32_t *)(ipa->reg + o); }
static inline uint32_t gsi_rd(struct coachz_gsi *g, uint32_t o) { return *(volatile uint32_t *)(g->virt + o); }

/* the ordering iowrite32/ioread32 give Linux between DMA memory and the device: dma_wmb before a doorbell, rmb after a read */
static inline void cic_dma_wmb(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dmb oshst" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
static inline void cic_dma_rmb(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dmb oshld" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

int coachz_ipa_cmd_init(struct coachz_ipa_cmd *c, struct coachz_ipa *ipa, struct coachz_gsi *gsi, void *dma_virt, uint64_t dma_phys, uint32_t bytes)
{
    __builtin_memset(c, 0, sizeof *c);
    c->ipa = ipa;
    c->gsi = gsi;
    c->channel = cipa_endpoints[CIPA_EP_AP_COMMAND_TX].channel;
    const struct cgs_channel *ch = &gsi->t.ch[c->channel];
    if (!ch->defined || !ch->command || ch->tre_count > 256u) { printf("COACHZ ipa: no command channel\n"); return -EINVAL; }
    if (!dma_virt || bytes < CIC_DMA_BYTES || (dma_phys & 127u)) { printf("COACHZ ipa: command DMA block too small or unaligned\n"); return -ENOMEM; }
    if (!cic_cmd_init_valid(ipa->mem_offset) || !cic_table_valid()) { printf("COACHZ ipa: command or table layout invalid\n"); return -EINVAL; }
    c->tre_count = ch->tre_count;
    c->evt_ring = ch->evt_ring;
    c->dma_virt = dma_virt;
    c->dma_phys = dma_phys;
    __builtin_memset(dma_virt, 0, CIC_DMA_BYTES);
    cic_table_fill(c->dma_virt + 256u * CIC_POOL_SLOT, c->dma_phys + 256u * CIC_POOL_SLOT);   /* ipa_table_init */
    return 0;
}

/* gsi_channel_update for the command channel: true once an event names the TRE at ring index `last` */
static int command_done(struct coachz_ipa_cmd *c, uint32_t last)
{
    struct coachz_gsi *g = c->gsi;
    const struct cgs_channel *ch = &g->t.ch[c->channel];
    const uint32_t ev_base = (uint32_t)(g->ring_phys + g->place.evt_offset[c->evt_ring]);
    const uint32_t tre_base = (uint32_t)(g->ring_phys + g->place.offset[c->channel]);
    const uint32_t hw = gsi_rd(g, CGSI_EV_CH_E_CNTXT_4 + c->evt_ring * CGSI_EV_CH_E_CNTXT_4_STRIDE);
    const uint32_t index = (hw - ev_base) / CGS_ELEMENT;
    uint32_t old = g->evt_index[c->evt_ring];
    int done = 0;
    if (index == old % ch->event_count) return 0;
    cic_dma_rmb();
    while (old % ch->event_count != index) {
        const uint8_t *ev = coachz_gsi_ring_entry(g, c->channel, 1, old);
        const uint32_t tre = (cic_event_tre(ev) - tre_base) / CGS_ELEMENT;
        if (cic_event_chid(ev) != c->channel) printf("COACHZ ipa: command event for channel %u\n", cic_event_chid(ev));
        else if (tre == last % c->tre_count) done = 1;
        old++;
    }
    coachz_gsi_evt_ring_doorbell(g, c->evt_ring, old);   /* we have handled these events */
    return done;
}

/* __gsi_trans_commit + gsi_trans_commit_wait for immediate commands (at most IPA_COMMAND_TRANS_TRE_MAX) */
int coachz_ipa_cmd_run(struct coachz_ipa_cmd *c, const struct cic_cmd *cmds, unsigned count)
{
    struct coachz_gsi *g = c->gsi;
    if (!count || count > CGS_COMMAND_TRE_MAX) return -EINVAL;
    const uint32_t first = g->ch_index[c->channel];
    for (unsigned i = 0; i < count; i++) {
        const uint32_t index = first + i, slot = index % 256u;
        uint8_t *payload = c->dma_virt + slot * CIC_POOL_SLOT;
        __builtin_memset(payload, 0, CIC_POOL_SLOT);
        __builtin_memcpy(payload, cmds[i].payload, cmds[i].len);
        uint8_t tre[16];
        cic_tre(tre, c->dma_phys + slot * CIC_POOL_SLOT, cmds[i].len, cmds[i].opcode, i == count - 1u, 1);
        __builtin_memcpy(coachz_gsi_ring_entry(g, c->channel, 0, index), tre, sizeof tre);
    }
    cic_dma_wmb();
    coachz_gsi_channel_doorbell(g, c->channel, first + count);
    const uint32_t last = first + count - 1u;
    for (unsigned waited = 0; waited < CIC_WAIT_US; waited += 10) {
        if (command_done(c, last)) return 0;
        delay(10);
    }
    printf("COACHZ ipa: %u commands (first opcode 0x%x) not completed; channel state 0x%x, event pointer 0x%x\n", count, cmds[0].opcode,
        gsi_rd(g, CGSI_CH_C_CNTXT_0 + c->channel * CGSI_CH_C_CNTXT_0_STRIDE),
        gsi_rd(g, CGSI_EV_CH_E_CNTXT_4 + c->evt_ring * CGSI_EV_CH_E_CNTXT_4_STRIDE));
    return -ETIMEDOUT;
}

/* ipa_endpoint_setup: program every AP endpoint (modem endpoints are the modem's) */
static void endpoint_setup(struct coachz_ipa *ipa)
{
    for (unsigned i = 0; i < sizeof cipa_endpoints / sizeof cipa_endpoints[0]; i++) {
        const struct cipa_endpoint *e = &cipa_endpoints[i];
        if (!cip_endpoint_defined(e) || e->ee != CGS_EE_AP) continue;
        struct cic_write w[CIC_ENDPOINT_WRITES_MAX];
        const unsigned n = cic_endpoint_program(e, w);
        for (unsigned k = 0; k < n; k++) ipa_wr(ipa, w[k].offset, w[k].value);
    }
}

/* ipa_setup from ipa_endpoint_setup through ipa_table_setup */
int coachz_ipa_setup_commands(struct coachz_ipa_cmd *c)
{
    struct cic_cmd cmds[CGS_COMMAND_TRE_MAX];
    endpoint_setup(c->ipa);
    int err = coachz_gsi_channel_start(c->gsi, c->channel);   /* ipa_endpoint_enable_one(AP_COMMAND_TX) */
    if (err) { printf("COACHZ ipa: error %d starting TX channel %u for endpoint %u\n", err, c->channel, cic_endpoint_id(CIPA_EP_AP_COMMAND_TX)); return err; }
    unsigned n = cic_mem_setup(c->ipa->mem_offset, c->ipa->zero_addr, cmds);   /* ipa_mem_setup */
    if ((err = coachz_ipa_cmd_run(c, cmds, n))) return err;
    ipa_wr(c->ipa, CIPA_LOCAL_PKT_PROC_CNTXT, cic_local_pkt_proc_cntxt(c->ipa->mem_offset));
    n = cic_table_setup(c->ipa->mem_offset, c->dma_phys + 256u * CIC_POOL_SLOT, c->ipa->zero_addr, cmds);   /* ipa_table_setup */
    if ((err = coachz_ipa_cmd_run(c, cmds, n))) return err;
    printf("COACHZ ipa: endpoints programmed, command channel up, memory and tables set (LOCAL_PKT_PROC_CNTXT 0x%x, ENDP_INIT_MODE[%u] 0x%x)\n",
        ipa_rd(c->ipa, CIPA_LOCAL_PKT_PROC_CNTXT), cic_endpoint_id(CIPA_EP_AP_COMMAND_TX),
        ipa_rd(c->ipa, CIPA_ENDP_INIT_MODE + cic_endpoint_id(CIPA_EP_AP_COMMAND_TX) * CIPA_ENDP_INIT_MODE_STRIDE));
    return 0;
}
