/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Portable decisions of mainline drivers/net/ipa for an AP RX endpoint on IPA v4.2 (AP_LAN_RX first): buffer and TRE layout
 * (ipa_endpoint_replenish_one, gsi_trans_page_add), the replenish doorbell rule (IPA_REPLENISH_BATCH, or no TREs left), the
 * packet status structure and its parsing (ipa_status_extract, ipa_endpoint_status_parse/skip/tag_valid/drop) and the
 * default route (ipa_endpoint_default_route_set). Linux 7.4-rc1 (6628e7d4c6b2); the code is our own.
 */
#ifndef COACHZ_IPA_RX_PROTO_H
#define COACHZ_IPA_RX_PROTO_H

#include <stdint.h>
#include "coachz_ipa_cmd_proto.h"

#define CIR_REPLENISH_BATCH 16u
#define CIR_STATUS_SIZE 32u          /* IPA_STATUS_SIZE: eight le32 words */
#define CIR_DL_CSUM_TRAILER 8u       /* struct rmnet_map_dl_csum_trailer */
#define CIR_RULE_MISS 0x3ffu
#define CIR_EXCEPTION_DEAGGR 1u

/* One receive buffer per transaction: rx.buffer_size bytes, the data DMA'd after NET_SKB_PAD of headroom */
static inline uint32_t cir_buffer_size(const struct cipa_endpoint *e) { return e->rx_buffer_size; }
static inline uint32_t cir_dma_len(const struct cipa_endpoint *e) { return e->rx_buffer_size - CIC_NET_SKB_PAD; }
/* how many buffers may be outstanding: the channel's TRE budget (gsi_channel_tre_max, one TRE each) or the buffers memory
   holds, whichever is smaller (running out of memory is where mainline's replenish loop also stops) */
static inline uint32_t cir_capacity(const struct cgs_channel *c, uint32_t buffers)
{
    const uint32_t tre_max = cgs_tre_max(c);
    return buffers < tre_max ? buffers : tre_max;
}
/* __gsi_trans_commit's doorbell: every IPA_REPLENISH_BATCH commits, or when the last available TRE was just taken */
static inline int cir_doorbell(uint32_t replenish_count, uint32_t avail_after) { return !(replenish_count % CIR_REPLENISH_BATCH) || !avail_after; }
/* an RX TRE: a data transfer, the last (only) one, interrupt at end of transfer, no BEI on RX */
static inline void cir_tre(uint8_t tre[16], uint64_t buffer_addr, const struct cipa_endpoint *e)
{
    cic_tre(tre, buffer_addr + CIC_NET_SKB_PAD, cir_dma_len(e), CIC_CMD_NONE, 1, 0);
}
/* gsi_event.len: the bytes actually received into the buffer */
static inline uint32_t cir_event_len(const uint8_t ev[16]) { return ev[8] | (uint32_t)ev[9] << 8; }

/* ------------------------------------------------------------------ ipa_status_extract, before v5.0 */
struct cir_status {
    uint32_t opcode, exception, mask, length, src, dst, metadata, router_rule;
};
static inline struct cir_status cir_status(const uint8_t *p)
{
    struct cir_status s;
    const uint32_t w0 = cic_rd32(p), w1 = cic_rd32(p + 4), w3 = cic_rd32(p + 12);
    s.opcode = w0 & 0xffu;
    s.exception = (w0 >> 8) & 0xffu;
    s.mask = w0 >> 16;
    s.length = w1 & 0xffffu;
    s.src = (w1 >> 16) & 0x1fu;
    s.dst = (w1 >> 24) & 0x1fu;
    s.metadata = cic_rd32(p + 8);
    s.router_rule = w3 >> 22;
    return s;
}
/* ipa_status_format_packet: PACKET, DROPPED_PACKET, SUSPENDED_PACKET, PACKET_2ND_PASS */
static inline int cir_format_packet(uint32_t opcode) { return opcode == 1u || opcode == 4u || opcode == 8u || opcode == 64u; }

enum cir_kind { CIR_SKIP, CIR_TAG, CIR_TAG_UNEXPECTED, CIR_DROP, CIR_DELIVER, CIR_SHORT };
struct cir_packet { enum cir_kind kind; uint32_t consumed, data_offset, length; struct cir_status status; };

/* One step of ipa_endpoint_status_parse over `resid` bytes at `p` for endpoint `id`. A status without packet data, or for
   another endpoint or another format, is skipped (32 bytes). Otherwise the packet takes the status, its length aligned to
   pad_align, and the DL checksum trailer when checksum offload is on. A status with any mask bit set is a tagged transfer:
   from the command endpoint it ends a pipeline clear (complete(&ipa->completion)); it is dropped either way. Deaggregation
   exceptions and routing misses are dropped; everything else is delivered (data after the status). */
static inline struct cir_packet cir_status_step(const uint8_t *p, uint32_t resid, const struct cipa_endpoint *e)
{
    struct cir_packet r = { CIR_SHORT, resid, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };
    if (resid < CIR_STATUS_SIZE) return r;
    r.status = cir_status(p);
    if (!r.status.length || !cir_format_packet(r.status.opcode) || r.status.dst != e->endpoint) {
        r.kind = CIR_SKIP;
        r.consumed = CIR_STATUS_SIZE;
        return r;
    }
    const uint32_t align = e->rx_pad_align ? e->rx_pad_align : 1u;
    r.consumed = CIR_STATUS_SIZE + (r.status.length + align - 1u) / align * align + (e->checksum ? CIR_DL_CSUM_TRAILER : 0u);
    r.data_offset = CIR_STATUS_SIZE;
    r.length = r.status.length;
    if (r.status.mask)
        r.kind = r.status.src == cic_endpoint_id(CIPA_EP_AP_COMMAND_TX) ? CIR_TAG : CIR_TAG_UNEXPECTED;
    else if (r.status.exception)
        r.kind = r.status.exception == CIR_EXCEPTION_DEAGGR ? CIR_DROP : CIR_DELIVER;
    else
        r.kind = r.status.router_rule == CIR_RULE_MISS ? CIR_DROP : CIR_DELIVER;
    if (r.consumed > resid) r.consumed = resid;   /* mainline would underflow resid here; stop at the buffer's end instead */
    return r;
}

/* ------------------------------------------------------------------ ipa_endpoint_default_route_set */
static inline uint32_t cir_default_route(uint32_t endpoint)
{
    return cip_encode(CIPA_ROUTE_ROUTE_DEF_PIPE, endpoint) | CIPA_ROUTE_ROUTE_DEF_HDR_TABLE |
           cip_encode(CIPA_ROUTE_ROUTE_FRAG_DEF_PIPE, endpoint) | CIPA_ROUTE_ROUTE_DEF_RETAIN_HDR;
}

#endif /* COACHZ_IPA_RX_PROTO_H */
