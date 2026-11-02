/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * An AP RX endpoint of IPA v4.2 as mainline runs it, and the end of ipa_setup: ipa_endpoint_enable_one for AP_LAN_RX (start
 * its GSI channel, TX_SUSPEND for the endpoint, replenish), then ipa_endpoint_default_route_set. Not in the kernel build.
 * After this mainline's ipa_setup calls ipa_qmi_setup, which is the handshake in coachz_ipa_qmi.h.
 *
 * Buffers: rx.buffer_size (8 KiB) each from one DMA block (write-combined or uncached, zeroed is not needed), the data
 * placed after NET_SKB_PAD of headroom as ipa_endpoint_replenish_one does. One TRE per buffer; tre_buffer[] records which
 * buffer each ring entry holds (gsi_trans map). Up to gsi_channel_tre_max (251) are queued; a smaller block queues fewer,
 * as a failed page allocation stops mainline's loop.
 *
 * Completion (gsi_channel_poll -> gsi_channel_update -> ipa_endpoint_trans_complete): coachz_ipa_rx_poll reads
 * EV_CH_E_CNTXT_4, takes each new event's TRE and received length, parses the status structures in that buffer, requeues
 * the buffer, acknowledges the events and re-enables the ring's IEOB interrupt. Call it when coachz_gsi_interrupt reports
 * the ring in ieob_pending, or from a poll. AP_LAN_RX has no network device in mainline (only the modem RX endpoint gets
 * one), so delivered packets are counted and dropped here too; a tagged status from the command endpoint ends a pipeline
 * clear. An endpoint without status (AP_MODEM_RX) hands each whole buffer to `deliver`, as ipa_endpoint_skb_build does.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include "coachz_ipa_kernel.h"
#include "coachz_ipa_rx_proto.h"


static inline void cir_dma_wmb(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dmb oshst" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
static inline void cir_dma_rmb(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dmb oshld" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

int coachz_ipa_rx_init(struct coachz_ipa_rx *rx, struct coachz_ipa *ipa, struct coachz_gsi *gsi, unsigned name,
    void *buf_virt, uint64_t buf_phys, uint32_t bytes)
{
    __builtin_memset(rx, 0, sizeof *rx);
    rx->ipa = ipa;
    rx->gsi = gsi;
    rx->e = &cipa_endpoints[name];
    rx->channel = rx->e->channel;
    const struct cgs_channel *c = &gsi->t.ch[rx->channel];
    if (!c->defined || c->toward_ipa || c->tre_count > 256u) { printf("COACHZ ipa: endpoint %u is not an AP RX channel\n", rx->e->endpoint); return -EINVAL; }
    const uint32_t size = cir_buffer_size(rx->e);
    rx->buffers = bytes / size;
    if (rx->buffers > 256u) rx->buffers = 256u;
    if (!buf_virt || !rx->buffers || (buf_phys & 127u)) { printf("COACHZ ipa: no receive buffers for endpoint %u\n", rx->e->endpoint); return -ENOMEM; }
    rx->evt_ring = c->evt_ring;
    rx->tre_count = c->tre_count;
    rx->capacity = cir_capacity(c, rx->buffers);
    rx->buf_virt = buf_virt;
    rx->buf_phys = buf_phys;
    for (uint32_t i = 0; i < rx->buffers; i++) rx->free_list[i] = (uint16_t)(rx->buffers - 1u - i);
    rx->free_count = rx->buffers;
    return 0;
}

/* ipa_endpoint_replenish: queue buffers while TREs and buffers remain; ring the doorbell per batch, or when the last TRE
   was taken (__gsi_trans_commit). Entries committed without a doorbell wait for the next one, as in mainline. */
static void replenish(struct coachz_ipa_rx *rx)
{
    struct coachz_gsi *g = rx->gsi;
    const uint32_t size = cir_buffer_size(rx->e);
    while (rx->outstanding < rx->capacity && rx->free_count) {
        const uint16_t b = rx->free_list[--rx->free_count];
        uint8_t tre[16];
        cir_tre(tre, rx->buf_phys + (uint64_t)b * size, rx->e);
        __builtin_memcpy(coachz_gsi_ring_entry(g, rx->channel, 0, rx->fill), tre, sizeof tre);
        rx->tre_buffer[rx->fill % rx->tre_count] = b;
        rx->fill++;
        rx->outstanding++;
        if (cir_doorbell(++rx->replenish_count, rx->capacity - rx->outstanding)) {
            cir_dma_wmb();
            coachz_gsi_channel_doorbell(g, rx->channel, rx->fill);
        }
    }
}

/* ipa_endpoint_status_parse on one received buffer */
static void status_parse(struct coachz_ipa_rx *rx, const uint8_t *data, uint32_t total)
{
    uint32_t resid = total;
    while (resid) {
        const struct cir_packet p = cir_status_step(data, resid, rx->e);
        switch (p.kind) {
        case CIR_SHORT:
            printf("COACHZ ipa: short message (%u bytes < %u byte status)\n", resid, CIR_STATUS_SIZE);
            return;
        case CIR_SKIP: rx->skipped++; break;
        case CIR_TAG: rx->tagged++; rx->pipeline_clear_done++; break;
        case CIR_TAG_UNEXPECTED: rx->tagged++; printf("COACHZ ipa: unexpected tagged packet from endpoint %u\n", p.status.src); break;
        case CIR_DROP: rx->dropped++; break;
        case CIR_DELIVER:
            rx->delivered++;
            rx->bytes += p.length;
            if (rx->deliver) rx->deliver(rx->context, data + p.data_offset, p.length, p.status.metadata);
            break;
        }
        rx->packets += p.kind != CIR_SKIP;
        data += p.consumed;
        resid -= p.consumed;
    }
}

unsigned coachz_ipa_rx_poll(struct coachz_ipa_rx *rx)
{
    struct coachz_gsi *g = rx->gsi;
    const struct cgs_channel *c = &g->t.ch[rx->channel];
    const uint32_t ev_base = (uint32_t)(g->ring_phys + g->place.evt_offset[rx->evt_ring]);
    const uint32_t tre_base = (uint32_t)(g->ring_phys + g->place.offset[rx->channel]);
    const uint32_t size = cir_buffer_size(rx->e);
    unsigned completed = 0;
    for (;;) {
        const uint32_t hw = *(volatile uint32_t *)(g->virt + CGSI_EV_CH_E_CNTXT_4 + rx->evt_ring * CGSI_EV_CH_E_CNTXT_4_STRIDE);
        const uint32_t index = (hw - ev_base) / CGS_ELEMENT;
        uint32_t old = g->evt_index[rx->evt_ring];
        if (index == old % c->event_count) break;
        cir_dma_rmb();
        while (old % c->event_count != index) {
            const uint8_t *ev = coachz_gsi_ring_entry(g, rx->channel, 1, old);
            const uint32_t tre = (cic_event_tre(ev) - tre_base) / CGS_ELEMENT, len = cir_event_len(ev);
            const uint16_t b = rx->tre_buffer[tre % rx->tre_count];
            if (cic_event_chid(ev) != rx->channel || len > cir_dma_len(rx->e)) {
                printf("COACHZ ipa: RX event for channel %u, length %u\n", cic_event_chid(ev), len);
            } else if (rx->e->status_enable) {
                status_parse(rx, rx->buf_virt + (uint64_t)b * size + CIC_NET_SKB_PAD, len);
            } else {   /* ipa_endpoint_skb_build: the whole buffer goes up (to rmnet for the modem endpoint) */
                rx->packets++;
                if (rx->deliver) { rx->delivered++; rx->bytes += len; rx->deliver(rx->context, rx->buf_virt + (uint64_t)b * size + CIC_NET_SKB_PAD, len, 0); }
                else rx->dropped++;
            }
            rx->free_list[rx->free_count++] = b;   /* gsi_trans_free: the buffer and its TRE come back */
            rx->outstanding--;
            old++;
            completed++;
        }
        coachz_gsi_evt_ring_doorbell(g, rx->evt_ring, old);
        replenish(rx);                             /* ipa_endpoint_trans_complete -> ipa_endpoint_replenish */
    }
    g->ieob_pending &= ~(1u << rx->evt_ring);
    coachz_gsi_ieob_enable_one(g, rx->evt_ring);
    return completed;
}

/* ipa_endpoint_enable_one for an RX endpoint */
int coachz_ipa_rx_enable(struct coachz_ipa_rx *rx)
{
    const int err = coachz_gsi_channel_start(rx->gsi, rx->channel);
    if (err) { printf("COACHZ ipa: error %d starting RX channel %u for endpoint %u\n", err, rx->channel, rx->e->endpoint); return err; }
    coachz_ipa_suspend_enable(rx->ipa, rx->e->endpoint, 1);
    replenish(rx);   /* ipa_endpoint_replenish_enable: the channel holds no buffers yet */
    return 0;
}

/* the end of ipa_setup: enable the exception endpoint and make it the default route */
int coachz_ipa_setup_lan_rx(struct coachz_ipa_rx *rx)
{
    const int err = coachz_ipa_rx_enable(rx);
    if (err) return err;
    *(volatile uint32_t *)(rx->ipa->reg + CIPA_ROUTE) = cir_default_route(rx->e->endpoint);
    printf("COACHZ ipa: LAN RX endpoint %u up with %u buffers queued, ROUTE 0x%x, IRQ_SUSPEND_EN 0x%x; setup complete\n", rx->e->endpoint,
        rx->outstanding, *(volatile uint32_t *)(rx->ipa->reg + CIPA_ROUTE), *(volatile uint32_t *)(rx->ipa->reg + CIPA_IRQ_SUSPEND_EN));
    return 0;
}
