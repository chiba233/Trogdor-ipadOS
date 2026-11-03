/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The data port as this modem has it. Portable and host-tested; no kernel code.
 *
 * Apple's PCIe modem carries data over MHI, and CommCenter names that port in its requests: WDS Bind Mux Data Port (0x00a2) and QOS
 * Bind Data Port (0x002b) carry the peripheral endpoint {type 3 (PCIe), interface 4} or only a mux id. Here data goes through the IPA,
 * whose port the modem knows as {type 4 (embedded), interface 1}, and the modem refuses a mux id without it (QMI_ERR_INVALID_ARG).
 * ModemManager does the same for its "ipa" driver (mm_port_net_driver_to_qmi_endpoint_type, initialize_endpoint_info): it names the
 * embedded endpoint in every bind, and before the first one it opens the hardware port with DPM Open Port (the IPA endpoint ids
 * Linux shows as modem/tx_endpoint_id and rx_endpoint_id, swapped as the modem sees them) and sets the data format with WDA Set Data
 * Format: raw IP, QMAP aggregation both ways, at most 32 datagrams and 32768 bytes per downlink aggregate. Of its QMAP versions
 * (QMAPv5, QMAPv4, QMAP, each tried until the modem reports it) IPA v4.2 takes MAPv4 (ipa_sysfs.c ipa_offload_string), so the
 * bridge asks for QMAPv4 and then QMAP.
 *
 * Message layouts are libqmi's (qmi-service-wds, -wda, -dpm); the QOS bind is Qualcomm's QOS 0x002b, whose endpoint TLV is 0x10 beside
 * CommCenter's mux id (0x11) and data port (0x12).
 */
#ifndef COACHZ_BB_DATA_H
#define COACHZ_BB_DATA_H

#include <stdint.h>

#define CBD_SVC_WDS 0x01u
#define CBD_SVC_QOS 0x04u
#define CBD_SVC_WDA 0x1au
#define CBD_SVC_DPM 0x2fu
#define CBD_WDS_BIND_MUX_DATA_PORT 0x00a2u
#define CBD_QOS_BIND_DATA_PORT 0x002bu
#define CBD_WDA_SET_DATA_FORMAT 0x0020u
#define CBD_WDA_GET_DATA_FORMAT 0x0021u
#define CBD_DPM_OPEN_PORT 0x0020u

#define CBD_EP_EMBEDDED 4u
#define CBD_EP_INTERFACE 1u
#define CBD_IPA_AP_MODEM_TX 1u        /* IPA v4.2 endpoint ids (ipa_data-v4.2.c), as coachz_ipa_qmi.h has them */
#define CBD_IPA_AP_MODEM_RX 9u
#define CBD_LLP_RAW_IP 2u
#define CBD_DAP_QMAP 5u
#define CBD_DAP_QMAPV4 8u
#define CBD_DL_MAX_DATAGRAMS 32u
#define CBD_DL_MAX_SIZE 32768u

static const uint32_t cbd_daps[] = { CBD_DAP_QMAPV4, CBD_DAP_QMAP };

static inline uint16_t cbd_rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t cbd_rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void cbd_wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void cbd_wr32(uint8_t *p, uint32_t v) { cbd_wr16(p, v); cbd_wr16(p + 2, v >> 16); }

/* TLV writer: returns the bytes written, 0 when it does not fit */
static inline uint32_t cbd_tlv(uint8_t *p, uint32_t room, uint32_t type, const uint8_t *value, uint32_t len)
{
    if (room < 3u + len) return 0;
    p[0] = (uint8_t)type;
    cbd_wr16(p + 1, len);
    for (uint32_t i = 0; i < len; i++) p[3 + i] = value[i];
    return 3u + len;
}
static inline uint32_t cbd_tlv_u32(uint8_t *p, uint32_t room, uint32_t type, uint32_t v)
{
    uint8_t b[4];
    cbd_wr32(b, v);
    return cbd_tlv(p, room, type, b, 4);
}
static inline uint32_t cbd_tlv_endpoint(uint8_t *p, uint32_t room, uint32_t type)
{
    uint8_t b[8];
    cbd_wr32(b, CBD_EP_EMBEDDED);
    cbd_wr32(b + 4, CBD_EP_INTERFACE);
    return cbd_tlv(p, room, type, b, 8);
}

/* A whole SDU (flags, transaction, message, length, TLVs) whose TLVs are all well formed. */
static inline int cbd_sdu_ok(const uint8_t *sdu, uint32_t n)
{
    if (n < 7u || 7u + cbd_rd16(sdu + 5) > n) return 0;
    uint32_t at = 7u, end = 7u + cbd_rd16(sdu + 5);
    while (at < end) {
        if (end - at < 3u || 3u + cbd_rd16(sdu + at + 1) > end - at) return 0;
        at += 3u + cbd_rd16(sdu + at + 1);
    }
    return 1;
}
static inline const uint8_t *cbd_find(const uint8_t *sdu, uint32_t n, uint32_t type, uint32_t *len)
{
    if (!cbd_sdu_ok(sdu, n)) return 0;
    for (uint32_t at = 7u, end = 7u + cbd_rd16(sdu + 5); at < end; at += 3u + cbd_rd16(sdu + at + 1))
        if (sdu[at] == type) { *len = cbd_rd16(sdu + at + 1); return sdu + at + 3; }
    return 0;
}

/* Where the endpoint goes in a request for this modem: its TLV, and whether to add it beside a mux id (0x11) when it is missing. */
static inline uint32_t cbd_endpoint_tlv(uint32_t service, uint32_t message, int *insert)
{
    *insert = 0;
    if (service == CBD_SVC_WDS && message == CBD_WDS_BIND_MUX_DATA_PORT) { *insert = 1; return 0x10; }
    if (service == CBD_SVC_QOS && message == CBD_QOS_BIND_DATA_PORT) { *insert = 1; return 0x10; }
    if (service == CBD_SVC_WDA && message == CBD_WDA_SET_DATA_FORMAT) return 0x17;
    if (service == CBD_SVC_WDA && message == CBD_WDA_GET_DATA_FORMAT) return 0x10;
    return 0;
}

struct cbd_fix { uint32_t len, before_type, before_interface; uint8_t inserted; };

/* A request SDU with its endpoint for this modem's IPA port. `len` 0: the request goes as it is. before_type and before_interface
   are the endpoint the host named (0 when it named none). */
static inline struct cbd_fix cbd_endpoint_fix(uint32_t service, const uint8_t *sdu, uint32_t n, uint8_t *out, uint32_t room)
{
    struct cbd_fix f = { 0, 0, 0, 0 };
    int insert;
    if (n < 7u || sdu[0] != 0) return f;
    const uint32_t type = cbd_endpoint_tlv(service, cbd_rd16(sdu + 3), &insert);
    uint32_t len = 0;
    if (!type || !cbd_sdu_ok(sdu, n)) return f;
    const uint8_t *ep = cbd_find(sdu, n, type, &len);
    if (ep && len == 8u) {
        f.before_type = cbd_rd32(ep);
        f.before_interface = cbd_rd32(ep + 4);
        if (f.before_type == CBD_EP_EMBEDDED && f.before_interface == CBD_EP_INTERFACE) return f;
    } else if (ep || !insert || !cbd_find(sdu, n, 0x11, &len)) {
        return f;
    }
    const uint32_t end = 7u + cbd_rd16(sdu + 5);
    uint32_t w = 7u;
    if (room < end + 11u) return f;
    for (uint32_t i = 0; i < 7u; i++) out[i] = sdu[i];
    for (uint32_t at = 7u; at < end; at += 3u + cbd_rd16(sdu + at + 1)) {
        const uint32_t l = cbd_rd16(sdu + at + 1);
        if (sdu[at] == type) { w += cbd_tlv_endpoint(out + w, room - w, type); continue; }
        w += cbd_tlv(out + w, room - w, sdu[at], sdu + at + 3, l);
    }
    if (!ep) { w += cbd_tlv_endpoint(out + w, room - w, type); f.inserted = 1; }
    cbd_wr16(out + 5, w - 7u);
    f.len = w;
    return f;
}

static inline uint32_t cbd_request(uint8_t *sdu, uint32_t txn, uint32_t message, uint32_t tlv_len)
{
    sdu[0] = 0;
    cbd_wr16(sdu + 1, txn);
    cbd_wr16(sdu + 3, message);
    cbd_wr16(sdu + 5, tlv_len);
    return 7u + tlv_len;
}
/* DPM Open Port with the IPA hardware port: {embedded, 1, rx endpoint = AP_MODEM_TX, tx endpoint = AP_MODEM_RX} */
static inline uint32_t cbd_dpm_open(uint8_t *sdu, uint32_t room, uint32_t txn)
{
    uint8_t port[17];
    if (room < 7u + 20u) return 0;
    port[0] = 1;
    cbd_wr32(port + 1, CBD_EP_EMBEDDED);
    cbd_wr32(port + 5, CBD_EP_INTERFACE);
    cbd_wr32(port + 9, CBD_IPA_AP_MODEM_TX);
    cbd_wr32(port + 13, CBD_IPA_AP_MODEM_RX);
    return cbd_request(sdu, txn, CBD_DPM_OPEN_PORT, cbd_tlv(sdu + 7, room - 7u, 0x11, port, sizeof port));
}
/* WDA Set Data Format: raw IP, `dap` both ways, the downlink limits, the embedded endpoint */
static inline uint32_t cbd_wda_set(uint8_t *sdu, uint32_t room, uint32_t txn, uint32_t dap)
{
    uint32_t w = 0;
    if (room < 7u + 5u * 7u + 11u) return 0;
    uint8_t *t = sdu + 7;
    const uint32_t r = room - 7u;
    w += cbd_tlv_u32(t + w, r - w, 0x11, CBD_LLP_RAW_IP);
    w += cbd_tlv_u32(t + w, r - w, 0x12, dap);
    w += cbd_tlv_u32(t + w, r - w, 0x13, dap);
    w += cbd_tlv_u32(t + w, r - w, 0x15, CBD_DL_MAX_DATAGRAMS);
    w += cbd_tlv_u32(t + w, r - w, 0x16, CBD_DL_MAX_SIZE);
    w += cbd_tlv_endpoint(t + w, r - w, 0x17);
    return cbd_request(sdu, txn, CBD_WDA_SET_DATA_FORMAT, w);
}

/* The QMI error of a response, or 0xffff when it carries no result */
static inline uint32_t cbd_error(const uint8_t *sdu, uint32_t n)
{
    uint32_t len = 0;
    const uint8_t *r = cbd_find(sdu, n, 0x02, &len);
    if (!r || len < 4u) return 0xffffu;
    return cbd_rd16(r) ? cbd_rd16(r + 2) : 0;
}
static inline uint32_t cbd_u32(const uint8_t *sdu, uint32_t n, uint32_t type)
{
    uint32_t len = 0;
    const uint8_t *v = cbd_find(sdu, n, type, &len);
    return v && len >= 4u ? cbd_rd32(v) : 0xffffffffu;
}

/* The setup ModemManager runs before its first bind, as a sequence of requests and answers. */
enum cbd_step { CBD_IDLE, CBD_DPM, CBD_WDA, CBD_DONE };
struct cbd_setup {
    enum cbd_step step;
    uint32_t txn, dap_index, dpm_error, wda_error;
    uint32_t llp, ul_dap, dl_dap, dl_datagrams, dl_size;   /* as the modem reports them, 0xffffffff when it did not */
    uint8_t has_dpm, has_wda;
};
/* Next request: its service in *service and its SDU in `sdu`; 0 when there is none. Call at the start and after each answer. */
static inline uint32_t cbd_next(struct cbd_setup *s, uint8_t *sdu, uint32_t room, uint32_t *service)
{
    s->txn = (s->txn + 1u) & 0xffffu;
    if (!s->txn) s->txn = 1;
    if (s->step == CBD_IDLE) {
        s->dpm_error = s->wda_error = 0xffffu;
        s->llp = s->ul_dap = s->dl_dap = s->dl_datagrams = s->dl_size = 0xffffffffu;
        s->step = CBD_DPM;
        if (s->has_dpm) { *service = CBD_SVC_DPM; return cbd_dpm_open(sdu, room, s->txn); }
    }
    if (s->step == CBD_DPM) {
        s->step = CBD_WDA;
        s->dap_index = 0;
        if (s->has_wda) { *service = CBD_SVC_WDA; return cbd_wda_set(sdu, room, s->txn, cbd_daps[0]); }
    } else if (s->step == CBD_WDA && s->has_wda && (s->wda_error || s->dl_dap != cbd_daps[s->dap_index]) &&
               s->dap_index + 1u < sizeof cbd_daps / sizeof cbd_daps[0]) {
        s->dap_index++;
        *service = CBD_SVC_WDA;
        return cbd_wda_set(sdu, room, s->txn, cbd_daps[s->dap_index]);
    }
    s->step = CBD_DONE;
    return 0;
}
/* An answer for the request in flight. 1 when it was that one. */
static inline int cbd_answer(struct cbd_setup *s, uint32_t service, const uint8_t *sdu, uint32_t n)
{
    if (n < 7u || sdu[0] != 0x02 || cbd_rd16(sdu + 1) != s->txn) return 0;
    if (s->step == CBD_DPM && service == CBD_SVC_DPM && cbd_rd16(sdu + 3) == CBD_DPM_OPEN_PORT) {
        s->dpm_error = cbd_error(sdu, n);
        return 1;
    }
    if (s->step == CBD_WDA && service == CBD_SVC_WDA && cbd_rd16(sdu + 3) == CBD_WDA_SET_DATA_FORMAT) {
        s->wda_error = cbd_error(sdu, n);
        s->llp = cbd_u32(sdu, n, 0x11);
        s->ul_dap = cbd_u32(sdu, n, 0x12);
        s->dl_dap = cbd_u32(sdu, n, 0x13);
        s->dl_datagrams = cbd_u32(sdu, n, 0x15);
        s->dl_size = cbd_u32(sdu, n, 0x16);
        return 1;
    }
    return 0;
}

/*
 * Baseband activation (BSP 0xe2, ActivationRegister 0xb00c, QMIMav13ActivationCommandDriver): CommCenter registers, then takes the
 * state from the indication of the same id, TLV 0x01, one byte of bsp::ActivationState (0 kActivated, 1 kReady, 2 kNotReady, 3 kError).
 * An Apple modem reports kActivated once its activation ticket is accepted; until then CommCenter keeps /cc/props/baseband_activated
 * false, which stops operator names, network attach and data contexts. This modem has no activation lock: it is activated.
 *
 * The same indication carries the service provisioned state. Without it CommCenter keeps baseband_service_provisioned_states at
 * kUnknown and gives up the operator name ("not provisioned"), and the interface shows no service while the modem is registered.
 * TLV 0x4f (one byte, parseLegacyTlvs): 2 is kProvisioned, 3 kNotProvisioned. TLV 0x50 and 0x51 (one byte each, slot one and two,
 * parseMutableTlvs): 2 is kCardProvisionedStateProvisioned. Slot two has no card here, so 0x51 is left out.
 *
 * TLV 0x56 and 0x57 (Slot1IsAccepted, Slot2IsAccepted, one byte, nonzero true) are the activation's verdict on each slot's SIM, the
 * SIM lock. While slot one is not accepted CommCenter logs "Baseband activation result is unexpected!" and "BB activated, but there
 * is no SIM" on every activation query and sets the service provisioned state back to kUnknown. This modem has no SIM lock: both
 * slots are accepted.
 *
 * Whether a slot holds a SIM at all is the slot's own record in ActivationInfo (slot one at +0x60, its present flag at +0x110; slot
 * two at +0x110, flag at +0x1c0). parseMutableTlvs fills slot one's record, and sets the flag, from TLV 0x42 (ActivationIccidSlot1,
 * 10 bytes); 0x43 is its IMSI (8 bytes), 0x44 and 0x45 its GIDs, 0x46 to 0x49 the same for slot two. ActivationInfo +0x1c8 is the OR of
 * the two flags, and pp reads it: without 0x42 every activation query ends in "BB activated, but there is no SIM". pp then hands the
 * slot's ICCID and IMSI to carrier bundle matching (kCTPostponementInfoICCID, kCTPostponementInfoIMSI) through
 * createActivationStringFromData, which prints the bytes as hex and drops a trailing F: both are BCD in reading order, padded with F.
 * With the ICCID alone matching answers "There is no CarrierID or SIM Info present", the bundle technology stays kUnknown and the
 * carrier bundle is never set up (no APN, no data context). So both, as read from the card, go into the indication once known.
 */
#define CBD_SVC_BSP 0xe2u
#define CBD_BSP_ACTIVATION_REGISTER 0xb00cu
#define CBD_ACTIVATED 0u
#define CBD_PROVISIONED 2u
#define CBD_ACCEPTED 1u
#define CBD_ICCID_BYTES 10u
#define CBD_IMSI_BYTES 8u
#define CBD_ACTIVATION_TLVS 20u
#define CBD_ACTIVATION_SIM_TLVS (CBD_ACTIVATION_TLVS + 3u + CBD_ICCID_BYTES + 3u + CBD_IMSI_BYTES)
static inline uint32_t cbd_activation_state(uint8_t *tlvs, uint32_t room, const uint8_t *iccid, const uint8_t *imsi)
{
    const uint8_t state = CBD_ACTIVATED, provisioned = CBD_PROVISIONED, accepted = CBD_ACCEPTED;
    const uint32_t need = CBD_ACTIVATION_TLVS + (iccid ? 3u + CBD_ICCID_BYTES : 0u) + (iccid && imsi ? 3u + CBD_IMSI_BYTES : 0u);
    if (room < need) return 0;
    uint32_t w = cbd_tlv(tlvs, room, 0x01, &state, 1);
    w += cbd_tlv(tlvs + w, room - w, 0x4f, &provisioned, 1);
    w += cbd_tlv(tlvs + w, room - w, 0x50, &provisioned, 1);
    w += cbd_tlv(tlvs + w, room - w, 0x56, &accepted, 1);
    w += cbd_tlv(tlvs + w, room - w, 0x57, &accepted, 1);
    if (iccid) w += cbd_tlv(tlvs + w, room - w, 0x42, iccid, CBD_ICCID_BYTES);
    if (iccid && imsi) w += cbd_tlv(tlvs + w, room - w, 0x43, imsi, CBD_IMSI_BYTES);
    return w;
}

/* CommCenter reads EF_ICCID and EF_IMSI itself: UIM Read Transparent (0x20), TLV 0x02 {u16 file id, u8 path length, path}. The answer
 * carries TLV 0x11 {u16 content length, content} (libqmi qmi-service-uim.json). Files are stored as on the card (TS 31.102). */
#define CBD_SVC_UIM 0x0bu
#define CBD_UIM_READ_TRANSPARENT 0x0020u
#define CBD_EF_ICCID 0x2fe2u
#define CBD_EF_IMSI 0x6f07u
static inline uint32_t cbd_uim_read_file(const uint8_t *sdu, uint32_t n)
{
    uint32_t len = 0;
    if (!cbd_sdu_ok(sdu, n) || sdu[0] != 0u || cbd_rd16(sdu + 3) != CBD_UIM_READ_TRANSPARENT) return 0;
    const uint8_t *file = cbd_find(sdu, n, 0x02, &len);
    return file && len >= 3u ? cbd_rd16(file) : 0u;
}
/* The content of a successful read, at most room bytes: its length, or 0. */
static inline uint32_t cbd_uim_read_content(const uint8_t *sdu, uint32_t n, uint8_t *out, uint32_t room)
{
    uint32_t len = 0;
    if (!cbd_sdu_ok(sdu, n) || sdu[0] != 2u || cbd_rd16(sdu + 3) != CBD_UIM_READ_TRANSPARENT || cbd_error(sdu, n)) return 0;
    const uint8_t *r = cbd_find(sdu, n, 0x11, &len);
    if (!r || len < 2u || cbd_rd16(r) != len - 2u || len - 2u > room) return 0;
    for (uint32_t i = 0; i < len - 2u; i++) out[i] = r[2 + i];
    return len - 2u;
}
/* EF_ICCID keeps each digit pair low nibble first: swap into reading order. */
static inline int cbd_iccid_from_ef(const uint8_t *ef, uint32_t n, uint8_t out[CBD_ICCID_BYTES])
{
    if (n != CBD_ICCID_BYTES) return 0;
    for (uint32_t i = 0; i < CBD_ICCID_BYTES; i++) out[i] = (uint8_t)(ef[i] << 4 | ef[i] >> 4);
    return 1;
}
/* EF_IMSI: a length byte (digit bytes that follow), then digit 1 in the high nibble of the first byte above the parity nibble, then
 * pairs low nibble first, F filling. Repacked in reading order, padded with F. */
static inline int cbd_imsi_from_ef(const uint8_t *ef, uint32_t n, uint8_t out[CBD_IMSI_BYTES])
{
    uint8_t digits[16];
    uint32_t count = 0;
    if (n < 2u || !ef[0] || ef[0] > 8u || n < 1u + ef[0]) return 0;
    for (uint32_t i = 1; i <= ef[0]; i++) {
        const uint8_t lo = ef[i] & 0xfu, hi = ef[i] >> 4;
        if (i > 1u) { if (lo > 9u) break; digits[count++] = lo; }
        if (hi > 9u) break;
        digits[count++] = hi;
    }
    if (count < 6u || count > 15u) return 0;
    for (uint32_t i = 0; i < CBD_IMSI_BYTES; i++) {
        const uint8_t a = 2u * i < count ? digits[2u * i] : 0xfu, b = 2u * i + 1u < count ? digits[2u * i + 1u] : 0xfu;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return 1;
}

#endif /* COACHZ_BB_DATA_H */
