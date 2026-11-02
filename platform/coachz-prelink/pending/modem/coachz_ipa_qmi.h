/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The AP/modem IPA QMI handshake of mainline drivers/net/ipa (ipa_qmi.c, ipa_qmi_msg.[ch], data/ipa_data-v4.2.c,
 * Linux 7.4-rc1 6628e7d4c6b2), portable: message encodings, the INIT_DRIVER request built from the IPA v4.2 memory
 * layout, and the "ready" rules. No transport: the caller moves the bytes over the QRTR it already runs. Values and
 * order from Linux; the code is our own.
 *
 * The handshake (ipa_qmi.c's DOC):
 *   AP serves IPA_HOST_SERVICE (0x31 v1 instance 1) and looks up the modem's IPA_MODEM_SERVICE (0x31 v1 instance 2).
 *   When the modem service appears, the AP sends INIT_DRIVER (0x21) and waits up to a minute for a success response.
 *   The modem sends DRIVER_INIT_COMPLETE (0x35) once it has loaded the IPA microcontroller (first boot only) and
 *   INDICATION_REGISTER (0x20); the AP answers both, then sends the INIT_COMPLETE indication (0x22).
 *   Ready: INIT_DRIVER succeeded and DRIVER_INIT_COMPLETE answered; on the initial boot also INDICATION_REGISTER
 *   answered and INIT_COMPLETE sent.
 */
#ifndef COACHZ_IPA_QMI_H
#define COACHZ_IPA_QMI_H

#include <stddef.h>
#include <stdint.h>

/* include/linux/soc/qcom/qmi.h header and message types; TLVs are type u8, length u16 LE, value */
#define CI_QMI_REQUEST 0u
#define CI_QMI_RESPONSE 2u
#define CI_QMI_INDICATION 4u
#define CI_QMI_HDR 7u

#define CI_IPA_SERVICE 0x31u
#define CI_IPA_VERSION 1u
#define CI_HOST_INSTANCE 1u
#define CI_MODEM_INSTANCE 2u
/* qmi_interface.c: the QRTR instance word of a service is version | instance << 8 */
#define CI_QRTR_INSTANCE(inst) (CI_IPA_VERSION | ((uint32_t)(inst) << 8))

#define CI_INDICATION_REGISTER 0x20u
#define CI_INIT_DRIVER 0x21u
#define CI_INIT_COMPLETE 0x22u
#define CI_DRIVER_INIT_COMPLETE 0x35u
#define CI_INIT_DRIVER_REQ_MAX 162u
#define CI_INIT_DRIVER_TIMEOUT_MS 60000u
#define CI_PLATFORM_MSM_ANDROID 3u

/* ipa_data-v4.2.c ipa_mem_local_data (offsets within the IPA-local memory) and the constants INIT_DRIVER uses */
enum ci_mem { CI_UC_SHARED, CI_UC_INFO, CI_V4_FILTER, CI_V6_FILTER, CI_V4_ROUTE, CI_V6_ROUTE, CI_MODEM_HEADER,
              CI_MODEM_PROC_CTX, CI_AP_PROC_CTX, CI_PDN_CONFIG, CI_STATS_QUOTA_MODEM, CI_STATS_TETHERING, CI_MODEM, CI_MEM_COUNT };
struct ci_region { uint16_t offset, size; uint8_t canaries; };
static const struct ci_region ci_v42_mem[CI_MEM_COUNT] = {
    { 0x0000, 0x0080, 0 }, { 0x0080, 0x0200, 0 }, { 0x0290, 0x0078, 2 }, { 0x0318, 0x0078, 2 }, { 0x03a0, 0x0078, 2 },
    { 0x0428, 0x0078, 2 }, { 0x04a8, 0x0140, 2 }, { 0x05f0, 0x0200, 2 }, { 0x07f0, 0x0200, 0 }, { 0x09f8, 0x0050, 2 },
    { 0x0a50, 0x0060, 2 }, { 0x0ab0, 0x0140, 0 }, { 0x0bf0, 0x140c, 0 },
};
#define CI_V42_MODEM_ROUTE_COUNT 8u
/* endpoint ids (ipa_data-v4.2.c ipa_gsi_endpoint_data) */
#define CI_EP_AP_MODEM_TX 1u
#define CI_EP_AP_COMMAND_TX 6u
#define CI_EP_AP_LAN_RX 8u
#define CI_EP_AP_MODEM_RX 9u

/* ------------------------------------------------------------------ encoding */
struct ci_buf { uint8_t *p; size_t room, len; int overflow; };
static inline void ci_put(struct ci_buf *b, const void *src, size_t n)
{
    if (b->len + n > b->room) { b->overflow = 1; return; }
    for (size_t i = 0; i < n; i++) b->p[b->len + i] = ((const uint8_t *)src)[i];
    b->len += n;
}
static inline void ci_u8(struct ci_buf *b, uint8_t v) { ci_put(b, &v, 1); }
static inline void ci_u16(struct ci_buf *b, uint16_t v) { uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; ci_put(b, x, 2); }
static inline void ci_u32(struct ci_buf *b, uint32_t v)
{
    uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    ci_put(b, x, 4);
}
static inline void ci_tlv_u8(struct ci_buf *b, uint8_t t, uint8_t v) { ci_u8(b, t); ci_u16(b, 1); ci_u8(b, v); }
static inline void ci_tlv_u32(struct ci_buf *b, uint8_t t, uint32_t v) { ci_u8(b, t); ci_u16(b, 4); ci_u32(b, v); }
static inline void ci_tlv_pair(struct ci_buf *b, uint8_t t, uint32_t a, uint32_t c) { ci_u8(b, t); ci_u16(b, 8); ci_u32(b, a); ci_u32(b, c); }
static inline void ci_header(struct ci_buf *b, uint8_t type, uint16_t txn, uint16_t msg)
{
    b->len = 0;
    b->overflow = 0;
    ci_u8(b, type); ci_u16(b, txn); ci_u16(b, msg); ci_u16(b, 0);
}
static inline size_t ci_finish(struct ci_buf *b)
{
    if (b->overflow || b->len < CI_QMI_HDR) return 0;
    const uint16_t body = (uint16_t)(b->len - CI_QMI_HDR);
    b->p[5] = (uint8_t)body;
    b->p[6] = (uint8_t)(body >> 8);
    return b->len;
}

/* init_modem_driver_req: every field the v4.2 layout defines, in ipa_init_modem_driver_req_ei order. */
static inline size_t ci_init_driver_req(uint8_t *out, size_t room, uint16_t txn, uint32_t mem_offset, int uc_loaded)
{
    struct ci_buf b = { out, room, 0, 0 };
    const struct ci_region *m = ci_v42_mem;
    ci_header(&b, CI_QMI_REQUEST, txn, CI_INIT_DRIVER);
    ci_u8(&b, 0x10); ci_u16(&b, 4); ci_u32(&b, CI_PLATFORM_MSM_ANDROID);   /* SIGNED_4_BYTE_ENUM */
    const uint32_t hdr = mem_offset + m[CI_MODEM_HEADER].offset;
    ci_tlv_pair(&b, 0x11, hdr, hdr + m[CI_MODEM_HEADER].size - 1u);
    ci_tlv_pair(&b, 0x12, mem_offset + m[CI_V4_ROUTE].offset, CI_V42_MODEM_ROUTE_COUNT - 1u);
    ci_tlv_pair(&b, 0x13, mem_offset + m[CI_V6_ROUTE].offset, CI_V42_MODEM_ROUTE_COUNT - 1u);
    ci_tlv_u32(&b, 0x14, mem_offset + m[CI_V4_FILTER].offset);
    ci_tlv_u32(&b, 0x15, mem_offset + m[CI_V6_FILTER].offset);
    ci_tlv_pair(&b, 0x16, mem_offset + m[CI_MODEM].offset, m[CI_MODEM].size);
    ci_tlv_u32(&b, 0x17, CI_EP_AP_MODEM_RX);
    ci_tlv_u8(&b, 0x18, uc_loaded ? 1u : 0u);
    const uint32_t ctx = mem_offset + m[CI_MODEM_PROC_CTX].offset;
    ci_tlv_pair(&b, 0x19, ctx, ctx + m[CI_MODEM_PROC_CTX].size - 1u);
    /* no compression table, no hashed route/filter tables on v4.2 */
    ci_tlv_u32(&b, 0x1f, mem_offset + m[CI_STATS_QUOTA_MODEM].offset);
    ci_tlv_u32(&b, 0x20, mem_offset + m[CI_STATS_QUOTA_MODEM].size);   /* sic: mainline adds mem_offset to the size */
    /* no STATS_DROP region on v4.2 */
    return ci_finish(&b);
}
/* The server's responses (INDICATION_REGISTER, DRIVER_INIT_COMPLETE): result success. */
static inline size_t ci_success_rsp(uint8_t *out, size_t room, uint16_t txn, uint16_t msg)
{
    struct ci_buf b = { out, room, 0, 0 };
    ci_header(&b, CI_QMI_RESPONSE, txn, msg);
    ci_u8(&b, 0x02); ci_u16(&b, 4); ci_u16(&b, 0); ci_u16(&b, 0);
    return ci_finish(&b);
}
/* ipa_server_init_complete: the INIT_COMPLETE indication with a success status */
static inline size_t ci_init_complete_ind(uint8_t *out, size_t room, uint16_t txn)
{
    struct ci_buf b = { out, room, 0, 0 };
    ci_header(&b, CI_QMI_INDICATION, txn, CI_INIT_COMPLETE);
    ci_u8(&b, 0x02); ci_u16(&b, 4); ci_u16(&b, 0); ci_u16(&b, 0);
    return ci_finish(&b);
}

/* ------------------------------------------------------------------ decoding */
struct ci_msg { uint8_t type; uint16_t txn, id; const uint8_t *tlv; uint16_t tlv_len; };
static inline int ci_parse(const uint8_t *p, size_t n, struct ci_msg *m)
{
    if (n < CI_QMI_HDR) return 0;
    m->type = p[0];
    m->txn = (uint16_t)(p[1] | p[2] << 8);
    m->id = (uint16_t)(p[3] | p[4] << 8);
    m->tlv_len = (uint16_t)(p[5] | p[6] << 8);
    m->tlv = p + CI_QMI_HDR;
    return (size_t)m->tlv_len + CI_QMI_HDR <= n;
}
static inline const uint8_t *ci_find(const struct ci_msg *m, uint8_t type, uint16_t *len)
{
    for (uint32_t at = 0; at + 3u <= m->tlv_len;) {
        const uint16_t l = (uint16_t)(m->tlv[at + 1] | m->tlv[at + 2] << 8);
        if (at + 3u + l > m->tlv_len) return NULL;
        if (m->tlv[at] == type) { *len = l; return m->tlv + at + 3; }
        at += 3u + l;
    }
    return NULL;
}
static inline uint32_t ci_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
/* qmi_response_type_v01: 0 when TLV 0x02 says result 0 */
static inline int ci_result(const struct ci_msg *m, uint16_t *error)
{
    uint16_t l;
    const uint8_t *v = ci_find(m, 0x02, &l);
    if (!v || l < 4) { *error = 0xffff; return -1; }
    *error = (uint16_t)(v[2] | v[3] << 8);
    return (v[0] | v[1] << 8) ? -1 : 0;
}
struct ci_init_driver_rsp { int ok; uint16_t error; int has_ctrl, has_default, has_pending; uint32_t ctrl_end_pt, default_end_pt; uint8_t pending; };
static inline int ci_init_driver_rsp_parse(const struct ci_msg *m, struct ci_init_driver_rsp *r)
{
    if (m->type != CI_QMI_RESPONSE || m->id != CI_INIT_DRIVER) return 0;
    uint16_t l;
    const uint8_t *v;
    r->ok = ci_result(m, &r->error) == 0;
    r->has_ctrl = (v = ci_find(m, 0x10, &l)) && l == 4; if (r->has_ctrl) r->ctrl_end_pt = ci_le32(v);
    r->has_default = (v = ci_find(m, 0x11, &l)) && l == 4; if (r->has_default) r->default_end_pt = ci_le32(v);
    r->has_pending = (v = ci_find(m, 0x12, &l)) && l == 1; if (r->has_pending) r->pending = v[0];
    return 1;
}

/* ------------------------------------------------------------------ the handshake state (struct ipa_qmi) */
struct ci_handshake {
    int initial_boot;          /* set by ipa_qmi_setup; cleared once the first boot completes; kept across modem reboots */
    int uc_ready;              /* set by DRIVER_INIT_COMPLETE; kept across modem reboots */
    int modem_ready;           /* INIT_DRIVER answered with success; cleared on teardown */
    int indication_requested;  /* INDICATION_REGISTER answered; cleared on teardown */
    int indication_sent;       /* INIT_COMPLETE sent; cleared on teardown */
    int modem_started;         /* ipa_modem_start was called */
};
static inline void ci_setup(struct ci_handshake *h) { *h = (struct ci_handshake){ 0 }; h->initial_boot = 1; }
/* ipa_qmi_teardown on a modem stop: only the per-boot flags go */
static inline void ci_teardown(struct ci_handshake *h)
{
    h->modem_ready = h->indication_requested = h->indication_sent = h->modem_started = 0;
}
/* ipa_client_new_server: the modem's IPA service appeared (service 0x31, instance word 0x201) */
static inline int ci_is_modem_service(uint32_t service, uint32_t instance) { return service == CI_IPA_SERVICE && instance == CI_QRTR_INSTANCE(CI_MODEM_INSTANCE); }
/* What ipa_qmi_ready asks for after any event. */
struct ci_step { int send_init_complete, start_modem; };
/* ipa_qmi_ready + ipa_qmi_indication. Call after each event; after a successful send of INIT_COMPLETE call
   ci_indication_sent() and this again; when start_modem is set, call ipa_modem_start once. */
static inline struct ci_step ci_ready_step(struct ci_handshake *h)
{
    struct ci_step s = { 0, 0 };
    if (!h->modem_ready || !h->uc_ready) return s;
    if (h->indication_requested && !h->indication_sent) { s.send_init_complete = 1; return s; }
    if (h->initial_boot) {
        if (!h->indication_sent) return s;
        h->initial_boot = 0;
    }
    if (!h->modem_started) { h->modem_started = 1; s.start_modem = 1; }
    return s;
}
static inline void ci_indication_sent(struct ci_handshake *h) { h->indication_sent = 1; }
/* The three events, each after the AP's own send succeeded (ipa_server_* / ipa_client_init_driver_work). */
static inline void ci_on_indication_register_answered(struct ci_handshake *h) { h->indication_requested = 1; }
static inline void ci_on_driver_init_complete_answered(struct ci_handshake *h) { h->uc_ready = 1; }
static inline void ci_on_init_driver_success(struct ci_handshake *h) { h->modem_ready = 1; }

#endif /* COACHZ_IPA_QMI_H */
