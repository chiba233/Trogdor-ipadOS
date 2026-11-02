/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ipa_modem_start and the cellular data path on IPA v4.2, as mainline Linux runs it (ipa_modem.c for rmnet_ipa0, the rmnet
 * QMAP layer above it). Not in the kernel build; drivers/RamBlock/coachz_modem.c includes it together with
 * coachz_ipa_cmd.c and coachz_ipa_rx.c in one translation unit, so its static names are prefixed cim_.
 *
 * Mainline: after the QMI handshake ipa_modem_start registers rmnet_ipa0 (raw IP, MTU 1500). Opening it enables AP_MODEM_TX
 * (start GSI channel 0) and AP_MODEM_RX (channel 3: the endpoint's TX_SUSPEND interrupt and receive buffers). rmnet then
 * adds a data device per mux id; ModemManager or qmicli binds a mux id to the data call over QMI (WDS/WDA), which is not
 * here yet. This file keeps the same split: coachz_ipa_modem_start/open/stop for rmnet_ipa0, coachz_ipa_modem_mux_add for
 * an rmnet data device, coachz_ipa_modem_xmit for its egress and a deliver callback for its ingress. Attaching those to an
 * XNU ifnet is the integrating side's work.
 *
 * TX: each frame is copied into a DMA bounce slot (mainline maps the skb; it linearizes when fragments exceed the TLV
 * FIFO, so one TRE per frame is a path mainline also takes) and committed with IEOT and BEI; ipa_endpoint_skb_tx commits
 * with a doorbell unless more frames follow, and this rings per frame. Completions are found as gsi_channel_update does:
 * EV_CH_E_CNTXT_4 and each event's TRE pointer, then the event ring doorbell. Slots and TREs are limited by
 * gsi_channel_tre_max (505 for channel 0) and the slots the DMA block holds.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include "coachz_ipa_kernel.h"
#include "coachz_ipa_modem_proto.h"

#define CIM_SLOT 2048u   /* QMAP and UL checksum headers plus a 1500-byte packet, rounded up */

struct coachz_ipa_modem {
    struct coachz_ipa *ipa;
    struct coachz_gsi *gsi;
    struct coachz_ipa_rx *rx;            /* AP_MODEM_RX, initialised with coachz_ipa_rx_init(..., CIPA_EP_AP_MODEM_RX, ...) */
    uint32_t state, open, data_format;
    uint32_t tx_channel, tx_evt_ring, tx_tre_count, tx_capacity, tx_fill, tx_outstanding;
    uint8_t *slot_virt;
    uint64_t slot_phys;
    uint32_t slots;
    uint8_t mux_present[CIM_MAX_LOGICAL_EP];
    void (*mux_deliver)(void *context, uint32_t mux_id, const uint8_t *ip, uint32_t length, uint32_t version);
    void *mux_context;
    uint64_t tx_packets, tx_bytes, tx_dropped, tx_busy, rx_frames, rx_packets, rx_bytes, rx_dropped;
};
int coachz_ipa_modem_init(struct coachz_ipa_modem *m, struct coachz_ipa *ipa, struct coachz_gsi *gsi, struct coachz_ipa_rx *rx,
    void *slot_virt, uint64_t slot_phys, uint32_t bytes);
int coachz_ipa_modem_start(struct coachz_ipa_modem *m);
int coachz_ipa_modem_open(struct coachz_ipa_modem *m);
int coachz_ipa_modem_stop(struct coachz_ipa_modem *m);
int coachz_ipa_modem_mux_add(struct coachz_ipa_modem *m, uint32_t mux_id);
int coachz_ipa_modem_xmit(struct coachz_ipa_modem *m, uint32_t mux_id, const uint8_t *ip, uint32_t length);
unsigned coachz_ipa_modem_tx_poll(struct coachz_ipa_modem *m);

static inline void cim_dma_wmb(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dmb oshst" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
static inline void cim_dma_rmb(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dmb oshld" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

/* rmnet ingress: deaggregate one received buffer, strip QMAP header and DL checksum trailer, hand IP packets up by mux id */
static void cim_rx_deliver(void *context, const uint8_t *data, uint32_t length, uint32_t metadata)
{
    struct coachz_ipa_modem *m = context;
    (void)metadata;
    m->rx_frames++;
    uint32_t resid = length;
    while (resid) {
        const struct cim_packet p = cim_ingress_next(data, resid, m->data_format);
        if (!p.consumed) { if (resid) m->rx_dropped++; break; }   /* rmnet_map_deaggregate returns NULL: rest dropped */
        if (cim_ingress_deliverable(&p, m->data_format, m->mux_present) && m->mux_deliver) {
            m->rx_packets++;
            m->rx_bytes += p.ip_len;
            m->mux_deliver(m->mux_context, p.mux_id, data + p.ip_offset, p.ip_len, p.ip_version);
        } else {
            m->rx_dropped++;
        }
        data += p.consumed;
        resid -= p.consumed;
    }
}

int coachz_ipa_modem_init(struct coachz_ipa_modem *m, struct coachz_ipa *ipa, struct coachz_gsi *gsi, struct coachz_ipa_rx *rx,
    void *slot_virt, uint64_t slot_phys, uint32_t bytes)
{
    __builtin_memset(m, 0, sizeof *m);
    m->ipa = ipa;
    m->gsi = gsi;
    m->rx = rx;
    m->data_format = CIM_DATA_FORMAT;
    if (!cim_format_matches_endpoints(m->data_format) || rx->e != &cipa_endpoints[CIPA_EP_AP_MODEM_RX]) {
        printf("COACHZ modem: endpoints do not match the MAPv4 data format\n");
        return -EINVAL;
    }
    m->tx_channel = cipa_endpoints[CIPA_EP_AP_MODEM_TX].channel;
    const struct cgs_channel *c = &gsi->t.ch[m->tx_channel];
    if (!c->defined || !c->toward_ipa) return -EINVAL;
    m->tx_evt_ring = c->evt_ring;
    m->tx_tre_count = c->tre_count;
    m->slots = bytes / CIM_SLOT;
    if (m->slots > c->tre_count) m->slots = c->tre_count;
    if (!slot_virt || !m->slots || (slot_phys & 127u)) { printf("COACHZ modem: no TX buffers\n"); return -ENOMEM; }
    m->tx_capacity = m->slots < cgs_tre_max(c) ? m->slots : cgs_tre_max(c);
    m->slot_virt = slot_virt;
    m->slot_phys = slot_phys;
    rx->deliver = cim_rx_deliver;
    rx->context = m;
    return 0;
}

/* ipa_modem_start: STOPPED -> RUNNING with rmnet_ipa0 registered (here: ready to open) */
int coachz_ipa_modem_start(struct coachz_ipa_modem *m)
{
    if (m->state != CIM_STOPPED) return 0;   /* silently ignore attempts when running or changing state */
    m->state = CIM_STARTING;
    m->state = CIM_RUNNING;
    printf("COACHZ modem: rmnet_ipa0 registered (raw IP, MTU %u; rmnet data devices MTU %u, data format 0x%x)\n", CIM_IPA_MTU,
        cim_data_mtu(m->data_format), m->data_format);
    return 0;
}

/* ipa_open: enable AP_MODEM_TX, then AP_MODEM_RX */
int coachz_ipa_modem_open(struct coachz_ipa_modem *m)
{
    if (m->state != CIM_RUNNING || m->open) return m->open ? 0 : -ENXIO;
    int err = coachz_gsi_channel_start(m->gsi, m->tx_channel);
    if (err) { printf("COACHZ modem: error %d starting TX channel %u\n", err, m->tx_channel); return err; }
    if ((err = coachz_ipa_rx_enable(m->rx))) return err;   /* (mainline disables TX again here; channel stop is not ported yet) */
    m->open = 1;
    printf("COACHZ modem: rmnet_ipa0 up: TX channel %u (%u slots), RX endpoint %u with %u buffers, IRQ_SUSPEND_EN 0x%x\n", m->tx_channel,
        m->tx_capacity, m->rx->e->endpoint, m->rx->outstanding, *(volatile uint32_t *)(m->ipa->reg + CIPA_IRQ_SUSPEND_EN));
    return 0;
}

/* ipa_modem_stop without an open device; stopping open endpoints needs channel stop/reset, not ported yet */
int coachz_ipa_modem_stop(struct coachz_ipa_modem *m)
{
    if (m->state == CIM_STOPPED) return 0;
    if (m->state != CIM_RUNNING) return -EBUSY;
    if (m->open) { printf("COACHZ modem: stopping an open rmnet_ipa0 is not supported yet\n"); return -EBUSY; }
    m->state = CIM_STOPPED;
    return 0;
}

/* rmnet newlink: a data device for one mux id */
int coachz_ipa_modem_mux_add(struct coachz_ipa_modem *m, uint32_t mux_id)
{
    if (mux_id >= CIM_MAX_LOGICAL_EP) return -EINVAL;
    if (m->mux_present[mux_id]) return -EBUSY;   /* "MUX ID already exists" */
    m->mux_present[mux_id] = 1;
    return 0;
}

/* gsi_channel_update for the TX channel: release the slots and TREs of completed frames */
unsigned coachz_ipa_modem_tx_poll(struct coachz_ipa_modem *m)
{
    struct coachz_gsi *g = m->gsi;
    const struct cgs_channel *c = &g->t.ch[m->tx_channel];
    const uint32_t ev_base = (uint32_t)(g->ring_phys + g->place.evt_offset[m->tx_evt_ring]);
    const uint32_t tre_base = (uint32_t)(g->ring_phys + g->place.offset[m->tx_channel]);
    const uint32_t hw = *(volatile uint32_t *)(g->virt + CGSI_EV_CH_E_CNTXT_4 + m->tx_evt_ring * CGSI_EV_CH_E_CNTXT_4_STRIDE);
    const uint32_t index = (hw - ev_base) / CGS_ELEMENT;
    uint32_t old = g->evt_index[m->tx_evt_ring];
    unsigned completed = 0;
    if (index == old % c->event_count) {
        g->ieob_pending &= ~(1u << m->tx_evt_ring);
        coachz_gsi_ieob_enable_one(g, m->tx_evt_ring);   /* napi_complete: interrupts again */
        return 0;
    }
    cim_dma_rmb();
    while (old % c->event_count != index) {
        const uint8_t *ev = coachz_gsi_ring_entry(g, m->tx_channel, 1, old);
        const uint32_t tre = (cic_event_tre(ev) - tre_base) / CGS_ELEMENT;
        /* frames complete in order: everything up to this TRE is done */
        const uint32_t oldest = m->tx_fill - m->tx_outstanding;
        const uint32_t done = (tre - oldest % m->tx_tre_count + m->tx_tre_count) % m->tx_tre_count + 1u;
        if (cic_event_chid(ev) != m->tx_channel || done > m->tx_outstanding) {
            printf("COACHZ modem: TX event for channel %u, TRE %u\n", cic_event_chid(ev), tre);
        } else {
            m->tx_outstanding -= done;
            completed += done;
        }
        old++;
    }
    coachz_gsi_evt_ring_doorbell(g, m->tx_evt_ring, old);
    g->ieob_pending &= ~(1u << m->tx_evt_ring);
    coachz_gsi_ieob_enable_one(g, m->tx_evt_ring);
    return completed;
}

/* rmnet_egress_handler + ipa_start_xmit + ipa_endpoint_skb_tx for one IP packet on a mux id */
int coachz_ipa_modem_xmit(struct coachz_ipa_modem *m, uint32_t mux_id, const uint8_t *ip, uint32_t length)
{
    if (!m->open || mux_id >= CIM_MAX_LOGICAL_EP || !m->mux_present[mux_id]) { m->tx_dropped++; return -ENXIO; }
    if (m->tx_outstanding >= m->tx_capacity) coachz_ipa_modem_tx_poll(m);
    if (m->tx_outstanding >= m->tx_capacity) { m->tx_busy++; return -EBUSY; }   /* NETDEV_TX_BUSY */
    const uint32_t index = m->tx_fill, slot = index % m->slots;
    uint8_t *frame = m->slot_virt + (uint64_t)slot * CIM_SLOT;
    const uint32_t len = cim_egress(frame, CIM_SLOT, ip, length, mux_id, m->data_format);
    if (!cim_tx_acceptable(len, CIM_SLOT)) { m->tx_dropped++; return -E2BIG; }   /* dropped, as -E2BIG is */
    uint8_t tre[16];
    cim_tx_tre(tre, m->slot_phys + (uint64_t)slot * CIM_SLOT, len);
    __builtin_memcpy(coachz_gsi_ring_entry(m->gsi, m->tx_channel, 0, index), tre, sizeof tre);
    m->tx_fill++;
    m->tx_outstanding++;
    cim_dma_wmb();
    coachz_gsi_channel_doorbell(m->gsi, m->tx_channel, m->tx_fill);
    m->tx_packets++;
    m->tx_bytes += length;
    return 0;
}
