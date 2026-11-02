/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Portable decisions of mainline drivers/net/ipa for the middle of ipa_setup on IPA v4.2: ipa_endpoint_program's register
 * values for the AP endpoints, the GSI TRE format (gsi_trans.c), the immediate command payloads (ipa_cmd.c), ipa_cmd_init's
 * checks, and the command lists of ipa_mem_setup and ipa_table_setup with the table area ipa_table_init builds.
 * Register numbers come from the generated coachz_ipa_v42.h. Linux 7.4-rc1 (6628e7d4c6b2); the code is our own.
 */
#ifndef COACHZ_IPA_CMD_PROTO_H
#define COACHZ_IPA_CMD_PROTO_H

#include <stdint.h>
#include "coachz_gsi_proto.h"

static inline void cic_le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void cic_le32(uint8_t *p, uint32_t v) { cic_le16(p, v); cic_le16(p + 2, v >> 16); }
static inline void cic_le64(uint8_t *p, uint64_t v) { cic_le32(p, (uint32_t)v); cic_le32(p + 4, (uint32_t)(v >> 32)); }
static inline uint32_t cic_rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t cic_rd64(const uint8_t *p) { return cic_rd32(p) | (uint64_t)cic_rd32(p + 4) << 32; }

/* enum ipa_endpoint_name -> endpoint id (ipa->name_map) */
static inline uint32_t cic_endpoint_id(unsigned name) { return cipa_endpoints[name].endpoint; }

/* ------------------------------------------------------------------ ipa_endpoint_program (v4.2, AP endpoints) */
/* Linux buffer facts behind the RX aggregation byte limit (arm64, 4 KiB pages): NET_SKB_PAD = 64 (L1_CACHE_BYTES),
   IPA_RX_BUFFER_OVERHEAD = PAGE_SIZE - SKB_MAX_ORDER(NET_SKB_PAD, 0) = NET_SKB_PAD + SKB_DATA_ALIGN(sizeof(struct
   skb_shared_info)) = 64 + 320, IPA_MTU = ETH_DATA_LEN. Debian's LAN RX aggregation register confirms the result (6 KB). */
#define CIC_NET_SKB_PAD 64u
#define CIC_RX_BUFFER_OVERHEAD 384u
#define CIC_MTU 1500u
#define CIC_AGGR_GRANULARITY_US 500u
#define CIC_QMAP_HEADER 4u        /* struct rmnet_map_header */
#define CIC_QMAP_UL_CSUM 4u       /* struct rmnet_map_ul_csum_header */
#define CIC_QMAP_MUX_ID_OFFSET 1u /* offsetof(rmnet_map_header, mux_id) */
#define CIC_QMAP_PKT_LEN_OFFSET 2u
#define CIC_QMAP_METADATA_MASK 0x000000ffu

struct cic_write { uint32_t offset, value; };

static inline uint32_t cic_aggr_size_kb(uint32_t buffer_size, int hard_limit)
{
    uint32_t size = buffer_size - CIC_NET_SKB_PAD;
    if (!hard_limit) size -= CIC_MTU + CIC_RX_BUFFER_OVERHEAD;
    return size / 1024u;
}

/* The registers ipa_endpoint_program writes for one AP endpoint, in its order; returns the count. HOL_BLOCK: no v4.2 AP RX
   endpoint has holb_drop, so only the disable (HOL_BLOCK_EN = 0) is written. Suspend/delay: nothing on v4.0+/v4.2+. */
static inline unsigned cic_endpoint_program(const struct cipa_endpoint *e, struct cic_write *w)
{
    const uint32_t id = e->endpoint;
    unsigned n = 0;
#define CIC_W(reg, v) do { w[n].offset = CIPA_##reg + id * CIPA_##reg##_STRIDE; w[n].value = (v); n++; } while (0)
    uint32_t v = 0;
    if (e->checksum) {   /* init_cfg: UL offload with the metadata header offset in words, DL offload */
        if (e->toward_ipa)
            v = cip_encode(CIPA_ENDP_INIT_CFG_CS_METADATA_HDR_OFFSET, CIC_QMAP_HEADER / 4u) | cip_encode(CIPA_ENDP_INIT_CFG_CS_OFFLOAD_EN, 1);
        else
            v = cip_encode(CIPA_ENDP_INIT_CFG_CS_OFFLOAD_EN, 2);
    }
    CIC_W(ENDP_INIT_CFG, v);
    if (e->toward_ipa) CIC_W(ENDP_INIT_NAT, 0);   /* NAT bypass */
    v = 0;                                          /* init_hdr */
    if (e->qmap) {
        const uint32_t size = CIC_QMAP_HEADER + (e->checksum && e->toward_ipa ? CIC_QMAP_UL_CSUM : 0u);
        v = cip_encode(CIPA_ENDP_INIT_HDR_HDR_LEN, size);
        if (!e->toward_ipa)
            v |= cip_encode(CIPA_ENDP_INIT_HDR_HDR_OFST_METADATA, CIC_QMAP_MUX_ID_OFFSET) | CIPA_ENDP_INIT_HDR_HDR_OFST_PKT_SIZE_VALID |
                 cip_encode(CIPA_ENDP_INIT_HDR_HDR_OFST_PKT_SIZE, CIC_QMAP_PKT_LEN_OFFSET);
        v |= CIPA_ENDP_INIT_HDR_HDR_OFST_METADATA_VALID;
    }
    CIC_W(ENDP_INIT_HDR, v);
    v = 0;                                          /* init_hdr_ext */
    if (e->qmap) {
        v |= CIPA_ENDP_INIT_HDR_EXT_HDR_ENDIANNESS;
        if (!e->toward_ipa) v |= CIPA_ENDP_INIT_HDR_EXT_HDR_TOTAL_LEN_OR_PAD_VALID | CIPA_ENDP_INIT_HDR_EXT_HDR_PAYLOAD_LEN_INC_PADDING;
    }
    if (!e->toward_ipa) v |= cip_encode(CIPA_ENDP_INIT_HDR_EXT_HDR_PAD_TO_ALIGNMENT, e->rx_pad_align);
    CIC_W(ENDP_INIT_HDR_EXT, v);
    if (!e->toward_ipa)   /* metadata mask: cpu_to_be32(0xff) as the register's little-endian word */
        CIC_W(ENDP_INIT_HDR_METADATA_MASK, e->qmap ? __builtin_bswap32(CIC_QMAP_METADATA_MASK) : 0u);
    if (e->toward_ipa)   /* init_mode: DMA to the named endpoint, else basic */
        CIC_W(ENDP_INIT_MODE, e->dma_mode ? cip_encode(CIPA_ENDP_INIT_MODE_ENDP_MODE, 3) |
                                            cip_encode(CIPA_ENDP_INIT_MODE_DEST_PIPE_INDEX, cic_endpoint_id(e->dma_endpoint)) : 0u);
    v = 0;                                          /* init_aggr: BYPASS 0, ENABLE_AGGR 1 (GENERIC 4), ENABLE_DEAGGR 2 (QCMAP 6) */
    if (e->aggregation) {
        if (!e->toward_ipa) {
            v = cip_encode(CIPA_ENDP_INIT_AGGR_AGGR_EN, 1) | cip_encode(CIPA_ENDP_INIT_AGGR_AGGR_TYPE, 4) |
                cip_encode(CIPA_ENDP_INIT_AGGR_BYTE_LIMIT, cic_aggr_size_kb(e->rx_buffer_size, e->rx_aggr_hard_limit));
            if (e->rx_aggr_time_limit)
                v |= cip_encode(CIPA_ENDP_INIT_AGGR_TIME_LIMIT, (e->rx_aggr_time_limit + CIC_AGGR_GRANULARITY_US / 2u) / CIC_AGGR_GRANULARITY_US);
            if (e->rx_aggr_close_eof) v |= CIPA_ENDP_INIT_AGGR_SW_EOF_ACTIVE;
        } else {
            v = cip_encode(CIPA_ENDP_INIT_AGGR_AGGR_EN, 2) | cip_encode(CIPA_ENDP_INIT_AGGR_AGGR_TYPE, 6);
        }
    }
    CIC_W(ENDP_INIT_AGGR, v);
    if (!e->toward_ipa) CIC_W(ENDP_INIT_HOL_BLOCK_EN, 0);
    if (e->toward_ipa) CIC_W(ENDP_INIT_DEAGGR, 0);
    CIC_W(ENDP_INIT_RSRC_GRP, cip_encode(CIPA_ENDP_INIT_RSRC_GRP_ENDP_RSRC_GRP, e->resource_group));
    if (e->toward_ipa)
        CIC_W(ENDP_INIT_SEQ, cip_encode(CIPA_ENDP_INIT_SEQ_SEQ_TYPE, e->tx_seq_type) | cip_encode(CIPA_ENDP_INIT_SEQ_SEQ_REP_TYPE, e->tx_seq_rep_type));
    v = 0;                                          /* status */
    if (e->status_enable) {
        v = CIPA_ENDP_STATUS_STATUS_EN;
        if (e->toward_ipa) v |= cip_encode(CIPA_ENDP_STATUS_STATUS_ENDP, cic_endpoint_id(e->tx_status_endpoint));
    }
    CIC_W(ENDP_STATUS, v);
#undef CIC_W
    return n;
}
#define CIC_ENDPOINT_WRITES_MAX 16u

/* ------------------------------------------------------------------ gsi_trans.c: TREs */
#define CIC_CMD_NONE 0x0u
#define CIC_CMD_IP_V4_FILTER_INIT 0x3u
#define CIC_CMD_IP_V6_FILTER_INIT 0x4u
#define CIC_CMD_IP_V4_ROUTING_INIT 0x7u
#define CIC_CMD_IP_V6_ROUTING_INIT 0x8u
#define CIC_CMD_HDR_INIT_LOCAL 0x9u
#define CIC_CMD_REGISTER_WRITE 0xcu
#define CIC_CMD_IP_PACKET_INIT 0x10u
#define CIC_CMD_DMA_SHARED_MEM 0x13u
#define CIC_CMD_IP_PACKET_TAG_STATUS 0x14u
#define CIC_TRE_CHAIN 0x1u
#define CIC_TRE_IEOT  0x200u
#define CIC_TRE_BEI   0x400u
#define CIC_RE_XFER     0x2u
#define CIC_RE_IMMD_CMD 0x3u

/* gsi_trans_tre_fill: {addr le64, len or opcode le16, 0, flags le32}; the last TRE interrupts at end of transfer,
   blocked (BEI) on TX channels; the others chain */
static inline void cic_tre(uint8_t tre[16], uint64_t addr, uint32_t len, uint32_t opcode, int last, int bei)
{
    cic_le64(tre, addr);
    cic_le16(tre + 8, opcode == CIC_CMD_NONE ? len : opcode);
    cic_le16(tre + 10, 0);
    uint32_t flags = (opcode == CIC_CMD_NONE ? CIC_RE_XFER : CIC_RE_IMMD_CMD) << 16;
    flags |= last ? CIC_TRE_IEOT | (bei ? CIC_TRE_BEI : 0u) : CIC_TRE_CHAIN;
    cic_le32(tre + 12, flags);
}
/* struct gsi_event: xfer_ptr le64 names the TRE it completes; chid at byte 15 */
static inline uint32_t cic_event_tre(const uint8_t ev[16]) { return cic_rd32(ev); }
static inline uint32_t cic_event_chid(const uint8_t ev[16]) { return ev[15]; }
static inline uint32_t cic_event_code(const uint8_t ev[16]) { return ev[11]; }

/* ------------------------------------------------------------------ ipa_cmd.c: immediate commands */
/* One command: opcode as it goes in the TRE (v4.0+ register writes carry the clear option there) and its payload */
struct cic_cmd { uint32_t opcode; uint8_t len; uint8_t payload[24]; };

/* ipa_cmd_hdr_init_local_add: table address, then flags {size 11:0, IPA-local offset 27:12}, reserved */
static inline struct cic_cmd cic_hdr_init_local(uint32_t mem_offset, uint32_t offset, uint32_t size, uint64_t addr)
{
    struct cic_cmd c = { CIC_CMD_HDR_INIT_LOCAL, 16, { 0 } };
    cic_le64(c.payload, addr);
    cic_le32(c.payload + 8, (size & 0xfffu) | ((mem_offset + offset) & 0xffffu) << 12);
    return c;
}
/* ipa_cmd_dma_shared_mem_add: {clear_after_read 0, size, local_addr, flags (bit 0: read from IPA)}, system address */
static inline struct cic_cmd cic_dma_shared_mem(uint32_t mem_offset, uint32_t offset, uint32_t size, uint64_t addr, int toward_ipa)
{
    struct cic_cmd c = { CIC_CMD_DMA_SHARED_MEM, 16, { 0 } };
    cic_le16(c.payload + 2, size);
    cic_le16(c.payload + 4, mem_offset + offset);
    cic_le16(c.payload + 6, toward_ipa ? 0u : 1u);
    cic_le64(c.payload + 8, addr);
    return c;
}
/* ipa_cmd_register_write_add, v4.0+: the clear option (hps 0, full 2) in opcode bits 10:9, offset bits 19:16 in flags 14:11 */
static inline struct cic_cmd cic_register_write(uint32_t offset, uint32_t value, uint32_t mask, int clear_full)
{
    struct cic_cmd c = { CIC_CMD_REGISTER_WRITE | (clear_full ? 2u : 0u) << 9, 16, { 0 } };
    cic_le16(c.payload, ((offset >> 16) & 0xfu) << 11);
    cic_le16(c.payload + 2, offset & 0xffffu);
    cic_le32(c.payload + 4, value);
    cic_le32(c.payload + 8, mask);
    return c;
}
/* ipa_cmd_table_init_add: hash rules address, flags {hash size 11:0, hash addr 27:12, nhash size 39:28, nhash addr 55:40},
   non-hash rules address; v4.2 has no hashed tables, so the hash half is zero */
static inline struct cic_cmd cic_table_init(uint32_t opcode, uint32_t mem_offset, uint32_t offset, uint32_t size, uint64_t addr)
{
    struct cic_cmd c = { opcode, 24, { 0 } };
    cic_le64(c.payload + 8, (uint64_t)(size & 0xfffu) << 28 | (uint64_t)((mem_offset + offset) & 0xffffu) << 40);
    cic_le64(c.payload + 16, addr);
    return c;
}
static inline struct cic_cmd cic_ip_packet_init(uint32_t endpoint)
{
    struct cic_cmd c = { CIC_CMD_IP_PACKET_INIT, 8, { 0 } };
    c.payload[0] = (uint8_t)(endpoint & 0x1fu);
    return c;
}
static inline struct cic_cmd cic_ip_packet_tag_status(void) { struct cic_cmd c = { CIC_CMD_IP_PACKET_TAG_STATUS, 8, { 0 } }; return c; }
/* ipa_cmd_transfer_add: a zero-filled payload-sized data transfer */
static inline struct cic_cmd cic_transfer(void) { struct cic_cmd c = { CIC_CMD_NONE, 24, { 0 } }; return c; }

/* ipa_cmd_pipeline_clear_add: no-op register write with full pipeline clear, then a packet straight to LAN RX with a
   tagged status (ipa_cmd_pipeline_clear_count = 4) */
static inline unsigned cic_pipeline_clear(struct cic_cmd *out)
{
    out[0] = cic_register_write(0, 0, 0, 1);
    out[1] = cic_ip_packet_init(cic_endpoint_id(CIPA_EP_AP_LAN_RX));
    out[2] = cic_ip_packet_tag_status();
    out[3] = cic_transfer();
    return 4;
}

/* ipa_cmd_init: the header region and the highest endpoint status register must fit the command fields */
static inline int cic_cmd_init_valid(uint32_t mem_offset)
{
    const struct ci_region *h = &ci_v42_mem[CI_MODEM_HEADER];
    if (h->offset > 0xffffu || mem_offset > 0xffffu - h->offset || h->size > 0xfffu) return 0;
    const uint32_t status = CIPA_ENDP_STATUS + (CIPA_EP_COUNT - 1u) * CIPA_ENDP_STATUS_STRIDE, max = 0xfffffu;
    return status <= max && mem_offset <= max - status;
}

/* ------------------------------------------------------------------ ipa_mem_setup */
/* Header memory (the modem's; v4.2 has no AP header region) from the zero buffer, then zero the two processing context
   regions and the modem region; afterwards LOCAL_PKT_PROC_CNTXT = mem_offset + the modem processing context offset. */
static inline unsigned cic_mem_setup(uint32_t mem_offset, uint64_t zero_addr, struct cic_cmd *out)
{
    unsigned n = 0;
    out[n++] = cic_hdr_init_local(mem_offset, ci_v42_mem[CI_MODEM_HEADER].offset, ci_v42_mem[CI_MODEM_HEADER].size, zero_addr);
    static const unsigned zero[] = { CI_MODEM_PROC_CTX, CI_AP_PROC_CTX, CI_MODEM };
    for (unsigned i = 0; i < 3; i++)
        if (ci_v42_mem[zero[i]].size)
            out[n++] = cic_dma_shared_mem(mem_offset, ci_v42_mem[zero[i]].offset, ci_v42_mem[zero[i]].size, zero_addr, 1);
    return n;
}
static inline uint32_t cic_local_pkt_proc_cntxt(uint32_t mem_offset)
{
    return cip_encode(CIPA_LOCAL_PKT_PROC_CNTXT_IPA_BASE_ADDR, mem_offset + ci_v42_mem[CI_MODEM_PROC_CTX].offset);
}

/* ------------------------------------------------------------------ ipa_table_init / ipa_table_setup */
/* filtered: endpoints with filter support (AP and modem), from ipa_endpoint_init */
static inline uint64_t cic_filtered(void)
{
    uint64_t f = 0;
    for (unsigned i = 0; i < sizeof cipa_endpoints / sizeof cipa_endpoints[0]; i++)
        if (cip_endpoint_defined(&cipa_endpoints[i]) && cipa_endpoints[i].filter) f |= 1ull << cipa_endpoints[i].endpoint;
    return f;
}
static inline uint32_t cic_filter_count(void) { return ci_v42_mem[CI_V4_FILTER].size / 8u - 1u; }   /* bitmap in entry 0 */
static inline uint32_t cic_route_count(void) { return ci_v42_mem[CI_V4_ROUTE].size / 8u; }
/* ipa_table_mem_valid for v4.2 (no hashing) */
static inline int cic_table_valid(void)
{
    const struct ci_region *f4 = &ci_v42_mem[CI_V4_FILTER], *f6 = &ci_v42_mem[CI_V6_FILTER];
    const struct ci_region *r4 = &ci_v42_mem[CI_V4_ROUTE], *r6 = &ci_v42_mem[CI_V6_ROUTE];
    const uint64_t f = cic_filtered();
    if (f4->size != f6->size || r4->size != r6->size || f4->size / 8u < 2u || r4->size / 8u < 2u) return 0;
    if (!f || (uint32_t)__builtin_popcountll(f) > cic_filter_count()) return 0;
    if ((cic_filter_count() + 1u) * 8u > 0xfffu || cic_route_count() * 8u > 0xfffu) return 0;
    if (f4->size / 8u < 1u + (uint32_t)__builtin_popcountll(f)) return 0;
    return cic_route_count() >= CIPA_MODEM_ROUTE_COUNT + 1u;
}
/* The table area: the zero rule, the filter bitmap (shifted left one before v5.0), then every entry the zero rule's address.
   It must be 128-byte aligned. */
static inline uint32_t cic_table_bytes(void)
{
    const uint32_t count = cic_filter_count() > cic_route_count() ? cic_filter_count() : cic_route_count();
    return 8u + (1u + count) * 8u;
}
static inline void cic_table_fill(uint8_t *table, uint64_t table_addr)
{
    const uint32_t count = cic_filter_count() > cic_route_count() ? cic_filter_count() : cic_route_count();
    cic_le64(table, 0);
    cic_le64(table + 8, cic_filtered() << 1);
    for (uint32_t i = 0; i < count; i++) cic_le64(table + 16 + 8u * i, table_addr);
}
/* ipa_table_setup: per table (v4 filter, v6 filter, v4 route, v6 route) an init command; filter tables also zero the rest
   of their region. Filter tables start at the bitmap (skip 1), routes at the first zero-rule pointer (skip 2). */
static inline unsigned cic_table_setup(uint32_t mem_offset, uint64_t table_addr, uint64_t zero_addr, struct cic_cmd *out)
{
    static const struct { uint32_t opcode; unsigned region; int filter; } t[4] = {
        { CIC_CMD_IP_V4_FILTER_INIT, CI_V4_FILTER, 1 }, { CIC_CMD_IP_V6_FILTER_INIT, CI_V6_FILTER, 1 },
        { CIC_CMD_IP_V4_ROUTING_INIT, CI_V4_ROUTE, 0 }, { CIC_CMD_IP_V6_ROUTING_INIT, CI_V6_ROUTE, 0 },
    };
    unsigned n = 0;
    for (unsigned i = 0; i < 4; i++) {
        const struct ci_region *m = &ci_v42_mem[t[i].region];
        const uint32_t count = t[i].filter ? 1u + (uint32_t)__builtin_popcountll(cic_filtered()) : m->size / 8u;
        const uint32_t size = count * 8u;
        out[n++] = cic_table_init(t[i].opcode, mem_offset, m->offset, size, table_addr + (t[i].filter ? 8u : 16u));
        if (t[i].filter) out[n++] = cic_dma_shared_mem(mem_offset, m->offset + size, m->size - size, zero_addr, 1);
    }
    return n;
}

#endif /* COACHZ_IPA_CMD_PROTO_H */
