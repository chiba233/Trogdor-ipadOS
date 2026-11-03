/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * eSIM for the baseband bridge: Apple's VINYL "Tape" commands carried to this machine's eUICC. Portable state machine; no kernel code. Hardware acceptance pending.
 *
 * CommCenter (EURSimCommandDriver) takes each SIM slot's eSIM capability from TLV 0x21 of the UIM Get Card Status (0x2f) response, one
 * byte per slot (1 Vinyl, 2 Tape, 3 Hydra, 4 virtual Hydra, anything else none), and expects two. A modem that is not Apple's leaves it
 * out, so the bridge adds it. Tape is GSMA SGP.22: VinylQMICommandDriver sends each ES10 command as whole DER on VINYL (0xe5) message
 * 0x1002 (TLV 0x01 {command, transaction, slot}, TLV 0x10 {flag, segment count, segment index, at most 1500 bytes}, each segment after
 * the previous one's response) and takes the eUICC's answer from indication 0x1003 (TLV 0x01 {u8 command, u16 status (1 failure), u16 reserved,
 * u32 transaction, two error bytes}, then TLV 0x10 {segment count, index from 0, data} per segment). 0x1004 answers the EID in its response
 * (TLV 0x11, 16 bytes), 0x1005 the radio releases the eUICC is told about (TLV 0x10 transaction, 0x11 six bytes, 0x12 three bytes).
 *
 * The eUICC side follows ChromeOS's Hermes on this same modem (modem_qrtr.cc, apdu.cc; BSD): UIM Get Slot Status (0x47) finds the
 * physical slot whose card is an eUICC and its EID; while it is not the active slot, Switch Slot (0x46) makes it active, the modem
 * gets six seconds to settle (kSimRefreshDelay) and the slot that was active is restored afterwards; Open Logical Channel (0x42) on
 * ISD-R; each command as STORE DATA (CLA 0x80, INS 0xE2, P1 0x11 or 0x91 on the last block, P2 the block number, at most 255 bytes) by
 * Send APDU (0x3b, intermediate procedure bytes on), and 61xx answered with GET RESPONSE. A Bound Profile Package (BF36) is split as
 * SGP.22 2.5.5 does before ES10b.LoadBoundProfilePackage: BF36 header with BF23, A0, A1 header, each 88, A2, A3 header, each 86.
 *
 * Challenge/authenticate/prepare keep the same ISD-R channel and slot for the owning VINYL client until
 * load/cancel/clear, an error, disconnect or 180 seconds without a command. Ordinary UIM stays queued during that session.
 * GetSessionData/ClearSessionData are modem-local recovery-record operations, not ES10 APDUs. This bridge has no stored records.
 *
 * Which CommCenter slot is the eUICC: the logical slot it is in while active, otherwise slot two. While the bridge has switched for
 * a command the capability bytes stay as they were before the switch.
 */
#ifndef COACHZ_BB_ESIM_H
#define COACHZ_BB_ESIM_H

#include <stdint.h>
#include "coachz_bb_qmux.h"
#include "coachz_bb_data.h"

#define CBE_SVC_UIM 0x0bu
#define CBE_SVC_VINYL 0xe5u
#define CBE_UIM_GET_CARD_STATUS 0x002fu
#define CBE_UIM_CARD_STATUS_CHANGE 0x0032u
#define CBE_UIM_SEND_APDU 0x003bu
#define CBE_UIM_OPEN_LOGICAL_CHANNEL 0x0042u
#define CBE_UIM_CLOSE_LOGICAL_CHANNEL 0x003fu
#define CBE_UIM_SWITCH_SLOT 0x0046u
#define CBE_UIM_GET_SLOT_STATUS 0x0047u
#define CBE_UIM_CAPABILITY_TLV 0x21u
#define CBE_ERR_NO_MEMORY 2u
#define CBE_ERR_INTERNAL 3u
#define CBE_ERR_DEVICE_IN_USE 23u
#define CBE_ERR_NO_EFFECT 26u
#define CBE_ERR_INVALID_ARG 48u

#define CBE_VINYL_COMMAND 0x1000u
#define CBE_VINYL_GET_CSN 1u
/* Apple legacy Vinyl CSN is not the GSMA EID, and is unavailable on this Tape transport. */
#define CBE_ERR_VINYL_NOCAPABLE_UICC 0x0204u

#define CBE_VINYL_TAPE 0x1002u
#define CBE_VINYL_TAPE_INFO 0x1003u
#define CBE_VINYL_TAPE_EID 0x1004u
#define CBE_VINYL_TAPE_CAPS 0x1005u

#define CBE_CAP_NONE 0u
#define CBE_CAP_TAPE 2u

/* vinyl::TapeCommand */
#define CBE_TAPE_CHALLENGE 0x03u
#define CBE_TAPE_PREPARE_DOWNLOAD 0x05u
#define CBE_TAPE_LOAD_BPP 0x06u
#define CBE_TAPE_AUTHENTICATE 0x0bu
#define CBE_TAPE_CANCEL 0x0cu
#define CBE_TAPE_ENABLE 0x0eu
#define CBE_TAPE_DISABLE 0x0fu
#define CBE_TAPE_DELETE 0x10u
#define CBE_TAPE_MEMORY_RESET 0x11u
#define CBE_TAPE_GET_SESSION 0x15u
#define CBE_TAPE_CLEAR_SESSION 0x16u
#define CBE_TAPE_EID 0xffu              /* the bridge's own GET EID, for a 0x1004 it cannot answer from the slot status */

#define CBE_SLOTS 4u
#define CBE_JOBS 8u
#define CBE_SEGMENTS 512u
#define CBE_CHUNK 1500u                 /* the segment size CommCenter uses itself (sendOutgoingTapeData_sync) */
#define CBE_APDU_DATA 255u
#define CBE_TIMEOUT_MS 10000u
#define CBE_SETTLE_MS 6000u
#define CBE_SESSION_IDLE_MS 180000u
#define CBE_EVENTS 32u
#define CBE_DEFERRED UINT32_MAX
/* Answers to read-only ES10 queries, kept so a repeated query does not switch the physical slot away from the SIM in use:
 * on hardware each switch dropped LTE about 2 s later, and the SIM state change it caused made CommCenter query again. */
#define CBE_CACHE_ENTRIES 4u
#define CBE_CACHE_REQ 64u

static const uint8_t cbe_isdr_aid[16] = { 0xa0, 0x00, 0x00, 0x05, 0x59, 0x10, 0x10, 0xff, 0xff, 0xff, 0xff, 0x89, 0x00, 0x00, 0x01, 0x00 };
static const uint8_t cbe_get_eid[] = { 0xbf, 0x3e, 0x03, 0x5c, 0x01, 0x5a };
/* GSM, UTRAN, three CDMA2000, E-UTRAN (EPC); then NR (EPC), NR (5GC), E-UTRAN (5GC): this modem has no CDMA and no NR */
static const uint8_t cbe_releases[6] = { 12, 12, 0, 0, 0, 12 };
static const uint8_t cbe_releases_5g[3] = { 0, 0, 0 };

/* ------------------------------------------------------------------ slots */
struct cbe_slots {
    uint8_t known, count, euicc, active, logical, eid_len;   /* euicc, active: physical slot from 1, 0 none; logical: the active one's */
    uint8_t present[CBE_SLOTS], is_euicc[CBE_SLOTS], is_active[CBE_SLOTS], logical_of[CBE_SLOTS];
    uint8_t eid[16];
};

/* UIM Get Slot Status response: 1 when parsed. TLV 0x10 {n, {u32 card, u32 state, u8 logical, u8 iccid[len]}}, 0x11 {n, {u32 protocol,
   u8 apps, u8 atr[len], u8 is_euicc}}, 0x12 {n, {u8 eid[len]}} (Hermes uim_cmd.qmi get_slots_resp). */
static inline int cbe_parse_slots(struct cbe_slots *s, const uint8_t *sdu, uint32_t n)
{
    uint32_t l1 = 0, l2 = 0, l3 = 0;
    const uint8_t *st = cbd_find(sdu, n, 0x10, &l1), *info = cbd_find(sdu, n, 0x11, &l2), *eid = cbd_find(sdu, n, 0x12, &l3);
    struct cbe_slots t;
    __builtin_memset(&t, 0, sizeof t);
    if (cbd_error(sdu, n) || !st || !l1) return 0;
    t.count = st[0] < CBE_SLOTS ? st[0] : CBE_SLOTS;
    for (uint32_t i = 0, at = 1; i < st[0]; i++) {
        if (at + 10u > l1 || at + 10u + st[at + 9] > l1) return 0;
        if (i < CBE_SLOTS) {
            t.present[i] = cbd_rd32(st + at) == 2u;
            t.is_active[i] = cbd_rd32(st + at + 4) == 1u;
            t.logical_of[i] = st[at + 8];
        }
        at += 10u + st[at + 9];
    }
    if (info && l2) {
        for (uint32_t i = 0, at = 1; i < info[0]; i++) {
            if (at + 6u > l2 || at + 7u + info[at + 5] > l2) return 0;
            if (i < CBE_SLOTS) t.is_euicc[i] = info[at + 6 + info[at + 5]] != 0;
            at += 7u + info[at + 5];
        }
    }
    for (uint32_t i = 0; i < t.count; i++) {
        if (t.is_active[i] && !t.active) { t.active = (uint8_t)(i + 1); t.logical = t.logical_of[i] ? t.logical_of[i] : 1; }
        if (t.is_euicc[i] && t.present[i] && !t.euicc) t.euicc = (uint8_t)(i + 1);
    }
    if (eid && l3 && t.euicc) {
        for (uint32_t i = 0, at = 1; i < eid[0]; i++) {
            if (at + 1u > l3 || at + 1u + eid[at] > l3) break;
            if (i + 1u == t.euicc && eid[at] == 16u) { t.eid_len = 16; __builtin_memcpy(t.eid, eid + at + 1, 16); }
            at += 1u + eid[at];
        }
    }
    t.known = 1;
    *s = t;
    return 1;
}

/* ------------------------------------------------------------------ BPP segments (SGP.22 2.5.5) */
/* One BER-TLV header at p: tag (one byte, or 0x1f-class two), definite length up to three bytes. 0 when malformed. */
static inline uint32_t cbe_der(const uint8_t *p, uint32_t n, uint32_t *tag, uint32_t *len)
{
    uint32_t at = 0;
    if (n < 2) return 0;
    *tag = p[at++];
    if ((*tag & 0x1fu) == 0x1fu) { if (n < 3) return 0; *tag = *tag << 8 | p[at++]; }
    const uint32_t l = p[at++];
    if (l < 0x80u) *len = l;
    else if (l == 0x81u && at + 1u <= n) *len = p[at++];
    else if (l == 0x82u && at + 2u <= n) { *len = (uint32_t)p[at] << 8 | p[at + 1]; at += 2; }
        else if (l == 0x83u && at + 3u <= n) { *len = (uint32_t)p[at] << 16 | (uint32_t)p[at + 1] << 8 | p[at + 2]; at += 3; }
            else return 0;
    return at + *len <= n ? at : 0;
}

struct cbe_seg { uint32_t off, len; };
/* The STORE DATA segments of a command: a Bound Profile Package split as SGP.22 says, anything else whole. 0 when a BF36 is malformed. */
static inline uint32_t cbe_segments(const uint8_t *in, uint32_t n, struct cbe_seg *seg, uint32_t max)
{
    uint32_t tag, len, count = 0;
    uint32_t h = cbe_der(in, n, &tag, &len);
    if (!h || h + len != n) return 0;
    if (tag != 0xbf36u) { if (!max) return 0; seg[0] = (struct cbe_seg){ 0, n }; return 1; }
    const uint32_t end = h + len;
    uint32_t at = h;
    while (at < end) {
        uint32_t t, l;
        const uint32_t hh = cbe_der(in + at, end - at, &t, &l);
        if (!hh || count + 1u > max) return 0;
        if (t == 0xbf23u && count == 0) {
            seg[count++] = (struct cbe_seg){ 0, at + hh + l };            /* BF36 header and the whole BF23 */
        } else if (t == 0xa1u || t == 0xa3u) {
            seg[count++] = (struct cbe_seg){ at, hh };                    /* the header alone, then each element */
            for (uint32_t e = at + hh; e < at + hh + l;) {
                uint32_t et, el;
                const uint32_t eh = cbe_der(in + e, at + hh + l - e, &et, &el);
                if (!eh || count + 1u > max) return 0;
                seg[count++] = (struct cbe_seg){ e, eh + el };
                e += eh + el;
            }
        } else if (count) {
            seg[count++] = (struct cbe_seg){ at, hh + l };                /* A0, A2 whole */
        } else {
            return 0;
        }
        at += hh + l;
    }
    return count;
}

/* ------------------------------------------------------------------ the engine */
enum cbe_step { CBE_S_IDLE, CBE_S_SLOTS, CBE_S_SWITCH, CBE_S_SETTLE, CBE_S_RESLOT, CBE_S_OPEN, CBE_S_APDU, CBE_S_CLOSE, CBE_S_RESTORE, CBE_S_RESTORE_SETTLE };
enum cbe_job_state { CBE_J_FREE, CBE_J_RECEIVING, CBE_J_READY };
enum cbe_event_kind {
    CBE_EV_SLOTS,        /* a: count, b: eUICC physical slot, c: active physical slot, d: EID length */
    CBE_EV_SLOTS_FAILED, /* a: QMI error (0xffff none) */
    CBE_EV_REQUEST,      /* a: command, b: transaction, c: CommCenter slot, d: bytes */
    CBE_EV_SWITCH,       /* a: physical slot, b: logical slot, c: QMI error */
    CBE_EV_OPEN,         /* a: QMI error, b: channel */
    CBE_EV_DONE,         /* a: command, b: status, c: SW1 << 8 | SW2, d: bytes out; e: segments */
    CBE_EV_FAILED,       /* a: command, b: step, c: QMI error or SW, d: reason (cbe_reason) */
    CBE_EV_CAPS,         /* a: slot one, b: slot two: the capability bytes changed */
    CBE_EV_LEGACY_CSN,   /* a: command, b: CommCenter slot, c: QMI result; no card identifier */
    CBE_EV_SESSION,      /* a: 1 held, 2 released; b: slot, c: trigger command, d: reason (0 explicit, 1 idle, 2 disconnect, 3 error) */
    CBE_EV_CACHE,        /* a: command, b: 1 answered from the cache, 2 stored, 3 cleared; c: bytes (stored, answered) or entries (cleared) */
};
enum cbe_reason { CBE_R_NO_SLOTS = 1, CBE_R_NO_EUICC, CBE_R_SWITCH_LIMIT, CBE_R_QMI, CBE_R_SW, CBE_R_TIMEOUT, CBE_R_ROOM, CBE_R_INACTIVE, CBE_R_SEGMENTS };
struct cbe_event { uint32_t kind, a, b, c, d, e; };

struct cbe_job {
    uint8_t state, cmd, slot, channel, cid;
    uint16_t qmi_txn;                 /* CBE_TAPE_EID: the 0x1004 request to answer */
    uint32_t txid, total, next, off, len, cap, seq, deadline;
};

struct cbe {
struct cbe_slots slots;
uint8_t caps[2], frozen[2], freeze;
uint8_t *in, *resp;               /* caller's buffers: the commands' bytes, one answer */
uint32_t in_cap, in_used, resp_cap, resp_len, seq;
struct cbe_job job[CBE_JOBS];
/* the command in progress */
enum cbe_step step;
int active;                       /* job index, -1 none */
uint8_t channel_id, channel_open, switched, restore_phys, get_more, sw1, sw2, route_failed;
uint32_t deadline, wait_txn, wait_msg, txn, switches;
uint8_t session, session_channel, session_cid, session_slot;
uint32_t session_deadline;
struct cbe_seg seg[CBE_SEGMENTS];
uint32_t seg_count, seg_index, frag_pos, block;
/* the request for the modem, when req_len is set */
uint8_t req[600];
uint32_t req_len;
/* what goes to CommCenter: one result (indications, or the 0x1004 answer) */
struct cbe_job done;
uint8_t done_pending, done_status, done_sw1, done_sw2;
uint32_t done_chunk, done_len;
struct cbe_event ev[CBE_EVENTS];
uint32_t ev_count, ev_lost, card_status_fixed;
/* read-only query answers; each entry's bytes at cache + index * (cache_cap / CBE_CACHE_ENTRIES) */
uint8_t *cache;
uint32_t cache_cap, cache_next;
struct { uint8_t used, cmd, sw1, sw2; uint32_t req_len, resp_len; uint8_t req[CBE_CACHE_REQ]; } cached[CBE_CACHE_ENTRIES];
};

static inline void cbe_init(struct cbe *e, uint8_t *in, uint32_t in_cap, uint8_t *resp, uint32_t resp_cap)
{
__builtin_memset(e, 0, sizeof *e);
e->in = in; e->in_cap = in_cap; e->resp = resp; e->resp_cap = resp_cap;
e->active = -1;
}

static inline void cbe_event(struct cbe *e, uint32_t kind, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t x)
{
if (e->ev_count == CBE_EVENTS) { e->ev_lost++; return; }
e->ev[e->ev_count++] = (struct cbe_event){ kind, a, b, c, d, x };
}
/* The next event for the log, oldest first: 1 when there was one. */
static inline int cbe_take_event(struct cbe *e, struct cbe_event *out)
{
if (!e->ev_count) return 0;
*out = e->ev[0];
for (uint32_t i = 1; i < e->ev_count; i++) e->ev[i - 1] = e->ev[i];
e->ev_count--;
return 1;
}

/* The capability bytes CommCenter gets now. */
static inline void cbe_caps(const struct cbe *e, uint8_t caps[2])
{
caps[0] = caps[1] = CBE_CAP_NONE;
if (e->freeze) { caps[0] = e->frozen[0]; caps[1] = e->frozen[1]; return; }
if (!e->slots.known || !e->slots.euicc) return;
if (e->slots.active == e->slots.euicc && e->slots.logical >= 1 && e->slots.logical <= 2) caps[e->slots.logical - 1] = CBE_CAP_TAPE;
else caps[1] = CBE_CAP_TAPE;
}
static inline void cbe_caps_note(struct cbe *e)
{
uint8_t c[2];
cbe_caps(e, c);
if (c[0] != e->caps[0] || c[1] != e->caps[1]) { e->caps[0] = c[0]; e->caps[1] = c[1]; cbe_event(e, CBE_EV_CAPS, c[0], c[1], 0, 0, 0); }
}

/* Both card-status responses and indications replace the complete Vinyl capability vector in CommCenter.
 * Keep TLV 0x21 on both, or a hotplug indication silently removes Tape support until the next query.
 * Returns the new SDU length, 0 to leave it as it is. Card/application status bytes are never changed. */
static inline uint32_t cbe_card_status_fix(struct cbe *e, uint32_t service, const uint8_t *sdu, uint32_t n, uint8_t *out, uint32_t room)
{
if (service != CBE_SVC_UIM || n < 7u || !cbd_sdu_ok(sdu, n) || !e->slots.known) return 0;
const uint32_t message = cbd_rd16(sdu + 3);
if (!((sdu[0] == CBQ_QMI_RESPONSE && message == CBE_UIM_GET_CARD_STATUS && !cbd_error(sdu, n)) ||
      (sdu[0] == CBQ_QMI_INDICATION && message == CBE_UIM_CARD_STATUS_CHANGE))) return 0;
uint8_t caps[2];
cbe_caps(e, caps);
const uint32_t end = 7u + cbd_rd16(sdu + 5);
if (room < end + 5u) return 0;
uint32_t w = 7u;
for (uint32_t i = 0; i < 7u; i++) out[i] = sdu[i];
for (uint32_t at = 7u; at < end; at += 3u + cbd_rd16(sdu + at + 1))
if (sdu[at] != CBE_UIM_CAPABILITY_TLV) w += cbd_tlv(out + w, room - w, sdu[at], sdu + at + 3, cbd_rd16(sdu + at + 1));
w += cbd_tlv(out + w, room - w, CBE_UIM_CAPABILITY_TLV, caps, 2);
cbd_wr16(out + 5, w - 7u);
e->card_status_fixed++;
return w;
}

/* ------------------------------------------------------------------ requests for the modem (into e->req) */
static inline void cbe_send(struct cbe *e, uint32_t message, uint32_t tlv_len, uint32_t now)
{
e->txn = (e->txn + 1u) & 0xffffu;
if (!e->txn) e->txn = 1;
e->req_len = cbd_request(e->req, e->txn, message, tlv_len);
e->wait_txn = e->txn;
e->wait_msg = message;
e->deadline = now + CBE_TIMEOUT_MS;
}
static inline void cbe_send_slot_status(struct cbe *e, uint32_t now) { cbe_send(e, CBE_UIM_GET_SLOT_STATUS, 0, now); }
static inline void cbe_send_switch(struct cbe *e, uint32_t physical, uint32_t now)
{
uint8_t *t = e->req + 7;
const uint8_t logical = e->slots.logical ? e->slots.logical : 1;
uint32_t w = cbd_tlv(t, 32, 0x01, &logical, 1);
w += cbd_tlv_u32(t + w, 32, 0x02, physical);
cbe_send(e, CBE_UIM_SWITCH_SLOT, w, now);
e->switches++;
}
static inline void cbe_send_open(struct cbe *e, uint32_t now)
{
uint8_t *t = e->req + 7, aid[17];
const uint8_t logical = e->slots.logical ? e->slots.logical : 1;
aid[0] = sizeof cbe_isdr_aid;
__builtin_memcpy(aid + 1, cbe_isdr_aid, sizeof cbe_isdr_aid);
uint32_t w = cbd_tlv(t, 32, 0x01, &logical, 1);
w += cbd_tlv(t + w, 32, 0x10, aid, sizeof aid);
cbe_send(e, CBE_UIM_OPEN_LOGICAL_CHANNEL, w, now);
}
/* Send APDU: TLV 0x01 slot, 0x02 {u16 length, APDU}, 0x10 channel, 0x11 procedure bytes (0: intermediate ones reported) */
static inline void cbe_send_apdu(struct cbe *e, const uint8_t *apdu, uint32_t n, uint32_t now)
{
uint8_t *t = e->req + 7;
const uint8_t logical = e->slots.logical ? e->slots.logical : 1;
const uint8_t cmd = e->active >= 0 ? e->job[e->active].cmd : 0;
/* Hermes disables intermediate bytes for refresh-producing operations. */
const uint8_t procedure = cmd == CBE_TAPE_ENABLE || cmd == CBE_TAPE_DISABLE ||
    cmd == CBE_TAPE_DELETE || cmd == CBE_TAPE_MEMORY_RESET ? 1 : 0;
uint32_t w = cbd_tlv(t, 8, 0x01, &logical, 1);
t[w] = 0x02; cbd_wr16(t + w + 1, n + 2u); cbd_wr16(t + w + 3, n);
for (uint32_t i = 0; i < n; i++) t[w + 5 + i] = apdu[i];
w += 5u + n;
w += cbd_tlv(t + w, 8, 0x10, &e->channel_id, 1);
w += cbd_tlv(t + w, 8, 0x11, &procedure, 1);
cbe_send(e, CBE_UIM_SEND_APDU, w, now);
}
/* The next STORE DATA block of the current segment, or GET RESPONSE */
static inline void cbe_send_block(struct cbe *e, uint32_t now)
{
    uint8_t apdu[261];
    if (e->get_more) {
        /* Same short GET RESPONSE encoding as Hermes CommandApdu. */
        apdu[0] = 0x80; apdu[1] = 0xc0; apdu[2] = 0x91; apdu[3] = 0; apdu[4] = e->sw2;
        cbe_send_apdu(e, apdu, e->sw2 ? 5u : 4u, now);
        return;
    }
    const struct cbe_job *j = &e->job[e->active];
    const struct cbe_seg *s = &e->seg[e->seg_index];
    const uint32_t left = s->len - e->frag_pos, take = left < CBE_APDU_DATA ? left : CBE_APDU_DATA;
    const uint8_t *bytes = j->cmd == CBE_TAPE_EID ? cbe_get_eid : e->in + j->off;
    apdu[0] = 0x80; apdu[1] = 0xe2; apdu[2] = take == left ? 0x91 : 0x11;
    apdu[3] = (uint8_t)e->block++; apdu[4] = (uint8_t)take;
    __builtin_memcpy(apdu + 5, bytes + s->off + e->frag_pos, take);
    e->frag_pos += take;
    /* Hermes uses Le=256 (encoded 0) on the final STORE DATA fragment. */
    if (take == left) apdu[5 + take] = 0;
    cbe_send_apdu(e, apdu, 5u + take + (take == left), now);
}

static inline void cbe_send_close(struct cbe *e, uint32_t now)
{
    const uint8_t logical = e->slots.logical ? e->slots.logical : 1;
    uint32_t w = cbd_tlv(e->req + 7, 16, 0x01, &logical, 1);
    w += cbd_tlv(e->req + 7 + w, 16, 0x11, &e->channel_id, 1);
    e->step = CBE_S_CLOSE;
    cbe_send(e, CBE_UIM_CLOSE_LOGICAL_CHANNEL, w, now);
}
static inline void cbe_restore(struct cbe *e, uint32_t now)
{
    e->session = 0;
    if (e->switched && e->restore_phys) {
        e->step = CBE_S_RESTORE;
        cbe_send_switch(e, e->restore_phys, now);
    } else {
        e->step = CBE_S_IDLE; e->freeze = 0;
        cbe_caps_note(e);
    }
}
/* Drop the channel lease only when no APDU is executing. Cleanup still finishes before queued SIM traffic resumes. */
static inline void cbe_session_release(struct cbe *e, uint32_t now, uint32_t reason)
{
    if (!e->session || e->active >= 0 || e->step != CBE_S_IDLE) return;
    cbe_event(e, CBE_EV_SESSION, 2, e->session_slot, 0, reason, 0);
    e->session = 0;
    if (e->channel_open) cbe_send_close(e, now);
    else cbe_restore(e, now);
}
static inline int cbe_session_owner(const struct cbe *e, const struct cbe_job *j)
{
    return j->channel == e->session_channel && j->cid == e->session_cid && j->slot == e->session_slot;
}
static inline int cbe_session_terminal(uint32_t cmd)
{
    return cmd == CBE_TAPE_LOAD_BPP || cmd == CBE_TAPE_CANCEL || cmd == CBE_TAPE_CLEAR_SESSION ||
        cmd == CBE_TAPE_ENABLE || cmd == CBE_TAPE_DISABLE || cmd == CBE_TAPE_DELETE || cmd == CBE_TAPE_MEMORY_RESET;
}
/* ------------------------------------------------------------------ read-only query cache */
static inline void cbe_set_cache(struct cbe *e, uint8_t *buf, uint32_t cap) { e->cache = buf; e->cache_cap = buf ? cap : 0; }
/* SGP.22 queries whose answer only another ES10 command can change: GetEUICCInfo1 (BF20), GetEUICCInfo2 (BF22),
 * GetProfilesInfo (BF2D), EuiccConfiguredAddresses (BF3C), GetEID (BF3E). Never the challenge (BF2E) or notifications. */
static inline int cbe_cacheable(const uint8_t *req, uint32_t n)
{
    if (n < 2u || n > CBE_CACHE_REQ || req[0] != 0xbfu) return 0;
    return req[1] == 0x20u || req[1] == 0x22u || req[1] == 0x2du || req[1] == 0x3cu || req[1] == 0x3eu;
}
static inline int cbe_cache_find(const struct cbe *e, uint32_t cmd, const uint8_t *req, uint32_t n)
{
    for (uint32_t i = 0; i < CBE_CACHE_ENTRIES; i++)
        if (e->cached[i].used && e->cached[i].cmd == cmd && e->cached[i].req_len == n && !__builtin_memcmp(e->cached[i].req, req, n))
            return (int)i;
    return -1;
}
/* A hit fills the answer as the card gave it, with its status word. */
static inline int cbe_cache_take(struct cbe *e, const struct cbe_job *j)
{
    const uint8_t *req = e->in + j->off;
    if (!e->cache || e->switched || e->channel_open || !cbe_cacheable(req, j->len)) return 0;
    const int i = cbe_cache_find(e, j->cmd, req, j->len);
    if (i < 0 || e->cached[i].resp_len > e->resp_cap) return 0;
    __builtin_memcpy(e->resp, e->cache + (uint32_t)i * (e->cache_cap / CBE_CACHE_ENTRIES), e->cached[i].resp_len);
    e->resp_len = e->cached[i].resp_len; e->sw1 = e->cached[i].sw1; e->sw2 = e->cached[i].sw2; e->seg_count = 0;
    cbe_event(e, CBE_EV_CACHE, j->cmd, 1, e->resp_len, 0, 0);
    return 1;
}
/* After a command reached the card: keep a clean read-only answer; anything else may change the answers, so drop them all. */
static inline void cbe_cache_note(struct cbe *e, const struct cbe_job *j, uint32_t status)
{
    if (!e->cache || j->cmd == CBE_TAPE_EID || j->cmd == CBE_TAPE_GET_SESSION || j->cmd == CBE_TAPE_CLEAR_SESSION) return;
    const uint8_t *req = e->in + j->off;
    if (!cbe_cacheable(req, j->len)) {
        uint32_t had = 0;
        for (uint32_t i = 0; i < CBE_CACHE_ENTRIES; i++) { had += e->cached[i].used; e->cached[i].used = 0; }
        if (had) cbe_event(e, CBE_EV_CACHE, j->cmd, 3, had, 0, 0);
        return;
    }
    const uint32_t room = e->cache_cap / CBE_CACHE_ENTRIES;
    if (status || e->sw1 != 0x90u || e->sw2 || !e->resp_len || e->resp_len > room || cbe_cache_find(e, j->cmd, req, j->len) >= 0) return;
    uint32_t i = 0;
    while (i < CBE_CACHE_ENTRIES && e->cached[i].used) i++;
    if (i == CBE_CACHE_ENTRIES) i = e->cache_next++ % CBE_CACHE_ENTRIES;
    __builtin_memcpy(e->cache + i * room, e->resp, e->resp_len);
    __builtin_memcpy(e->cached[i].req, req, j->len);
    e->cached[i].used = 1; e->cached[i].cmd = j->cmd; e->cached[i].sw1 = e->sw1; e->cached[i].sw2 = e->sw2;
    e->cached[i].req_len = j->len; e->cached[i].resp_len = e->resp_len;
    cbe_event(e, CBE_EV_CACHE, j->cmd, 2, e->resp_len, 0, 0);
}

static inline void cbe_finish(struct cbe *e, uint32_t status, uint32_t reason, uint32_t detail, uint32_t now)
{
    if (e->active >= 0) {
        e->done = e->job[e->active]; e->job[e->active].state = CBE_J_FREE;
        e->done_status = (uint8_t)status; e->done_sw1 = e->sw1; e->done_sw2 = e->sw2;
        e->done_pending = 1; e->done_chunk = 0; e->done_len = status ? 0 : e->resp_len;
        cbe_cache_note(e, &e->done, status);
        if (status) cbe_event(e, CBE_EV_FAILED, e->done.cmd, e->step, detail, reason, 0);
        else cbe_event(e, CBE_EV_DONE, e->done.cmd, 0, (uint32_t)e->sw1 << 8 | e->sw2, e->resp_len, e->seg_count);
        e->active = -1;
    }
    e->req_len = e->wait_msg = 0;
    const uint32_t cmd = e->done.cmd;
    if (!status && e->channel_open && !cbe_session_terminal(cmd) &&
        (e->session || cmd == CBE_TAPE_CHALLENGE || cmd == CBE_TAPE_AUTHENTICATE || cmd == CBE_TAPE_PREPARE_DOWNLOAD)) {
        if (!e->session) {
            e->session_channel = e->done.channel; e->session_cid = e->done.cid; e->session_slot = e->done.slot;
            cbe_event(e, CBE_EV_SESSION, 1, e->done.slot, cmd, 0, 0);
        }
        e->session = 1; e->session_deadline = now + CBE_SESSION_IDLE_MS;
        e->step = CBE_S_IDLE; return;
    }
    if (e->session) cbe_event(e, CBE_EV_SESSION, 2, e->session_slot, cmd, status ? 3 : 0, 0);
    e->session = 0;
    if (e->channel_open) cbe_send_close(e, now);
    else cbe_restore(e, now);
}

static inline void cbe_begin(struct cbe *e, uint32_t now)
{
    struct cbe_job *j = &e->job[e->active];
    if (e->route_failed) { cbe_finish(e, 1, CBE_R_INACTIVE, e->slots.active, now); return; }
    if (!e->slots.known || !e->slots.euicc || !e->slots.active) {
        cbe_finish(e, 1, CBE_R_NO_EUICC, 0, now); return;
    }
    uint8_t caps[2]; cbe_caps(e, caps);
    if (j->slot < 1 || j->slot > 2 || caps[j->slot - 1] != CBE_CAP_TAPE) {
        cbe_finish(e, 1, CBE_R_INACTIVE, j->slot, now); return;
    }
    j->deadline = now + 180000u;
    e->resp_len = e->seg_index = e->frag_pos = e->block = 0;
    e->get_more = e->sw1 = e->sw2 = 0;
    /* No BB recovery records are stored here. Match the driver's explicit empty-session result,
     * without inventing eUICC data or forwarding an opaque recovery token as DER. */
    if (j->cmd == CBE_TAPE_GET_SESSION || j->cmd == CBE_TAPE_CLEAR_SESSION) {
        cbe_finish(e, 0, 0, 0, now); return;
    }
    if (cbe_cache_take(e, j)) { cbe_finish(e, 0, 0, 0, now); return; }
    if (j->cmd == CBE_TAPE_EID) {
        e->seg[0] = (struct cbe_seg){ 0, sizeof cbe_get_eid }; e->seg_count = 1;
    } else {
        e->seg_count = cbe_segments(e->in + j->off, j->len, e->seg, CBE_SEGMENTS);
    }
    if (!e->seg_count) { cbe_finish(e, 1, CBE_R_SEGMENTS, 0, now); return; }
    /* A short APDU numbers at most 256 fragments in one STORE DATA sequence. */
    for (uint32_t i = 0; i < e->seg_count; i++)
        if (e->seg[i].len > 256u * CBE_APDU_DATA) { cbe_finish(e, 1, CBE_R_SEGMENTS, i, now); return; }
    if (e->session && e->channel_open && cbe_session_owner(e, j) && e->slots.active == e->slots.euicc) {
        e->step = CBE_S_APDU; cbe_send_block(e, now); return;
    }
    if (e->slots.active != e->slots.euicc) {
        e->restore_phys = e->slots.active;
        e->frozen[0] = caps[0]; e->frozen[1] = caps[1]; e->freeze = 1;
        /* Restore even if the switch response times out: its request may have landed. */
        e->switched = 1; e->step = CBE_S_SWITCH;
        cbe_send_switch(e, e->slots.euicc, now);
    } else {
        e->switched = 0; e->step = CBE_S_OPEN; cbe_send_open(e, now);
    }
}

/* First fit in the caller's pool; in-flight or receiving jobs keep their bytes. */
static inline int cbe_reserve(struct cbe *e, uint32_t room, uint32_t *off)
{
    uint32_t at = 0;
    for (uint32_t pass = 0; pass <= CBE_JOBS; pass++) {
        if (at > e->in_cap || room > e->in_cap - at) return 0;
        int hit = 0;
        for (uint32_t i = 0; i < CBE_JOBS; i++) {
            const struct cbe_job *j = &e->job[i];
            if (j->state && at < j->off + j->cap && j->off < at + room) {
                at = j->off + j->cap; hit = 1; break;
            }
        }
        if (!hit) { *off = at; return 1; }
    }
    return 0;
}

/* Accept a VINYL request. Return 0 for an unimplemented message, else one QMUX response in out.
 * Segment acknowledgements mean buffered, not that the eUICC accepted the ES10 operation.
 * Completion is a later Tape indication; no profile/key material is interpreted here. */
static inline uint32_t cbe_host(struct cbe *e, uint32_t channel, uint32_t cid,
    const uint8_t *sdu, uint32_t n, uint8_t *out, uint32_t room, uint32_t now)
{
    if (!cbd_sdu_ok(sdu, n) || sdu[0] != CBQ_QMI_REQUEST) return 0;
    const uint32_t message = cbd_rd16(sdu + 3);
    if (message == CBE_VINYL_COMMAND) {
        uint32_t len = 0;
        const uint8_t *command = cbd_find(sdu, n, 0x02, &len);
        uint32_t error = CBQ_ERR_MALFORMED_MESSAGE;
        if (command && len == 6u) {
            if (command[5] < 1 || command[5] > 2) error = CBE_ERR_INVALID_ARG;
            else if (command[0] != CBE_VINYL_GET_CSN) return 0;
            else {
                /* VinylQMICommandDriver::queryCsn_sync accepts this exact error as no legacy CSN:
                 * asVinylDriverError(0x204) -> 6; handleClientStarted_sync's callback then emits
                 * Ready for this slot. Generic NOT_SUPPORTED (0x5e) leaves the driver unready.
                 * Do not invent a CSN from the EID or report a successful legacy Vinyl operation. */
                error = CBE_ERR_VINYL_NOCAPABLE_UICC;
                cbe_event(e, CBE_EV_LEGACY_CSN, command[0], command[5], error, 0, 0);
            }
        }
        return cbq_private_answer(sdu, CBE_SVC_VINYL, cid, error, 0, 0, out, room);
    }
    if (message != CBE_VINYL_TAPE && message != CBE_VINYL_TAPE_EID && message != CBE_VINYL_TAPE_CAPS) return 0;
    uint32_t l = 0, error = 0, w = 0;
    const uint8_t *h = cbd_find(sdu, n, 0x01, &l);
    uint8_t extra[48];
    if (!h || l != (message == CBE_VINYL_TAPE ? 6u : 5u)) error = CBQ_ERR_MALFORMED_MESSAGE;
    /* Device capabilities are not slot-specific: the native writer leaves this byte zero. */
    else if (message != CBE_VINYL_TAPE_CAPS &&
        (h[message == CBE_VINYL_TAPE ? 5 : 4] < 1 || h[message == CBE_VINYL_TAPE ? 5 : 4] > 2))
        error = CBE_ERR_INVALID_ARG;
    else if (message == CBE_VINYL_TAPE_CAPS) {
        w = cbd_tlv(extra, sizeof extra, 0x10, h, 4);
        w += cbd_tlv(extra + w, sizeof extra - w, 0x11, cbe_releases, sizeof cbe_releases);
        w += cbd_tlv(extra + w, sizeof extra - w, 0x12, cbe_releases_5g, sizeof cbe_releases_5g);
    } else if (message == CBE_VINYL_TAPE_EID && e->slots.known && e->slots.eid_len == 16) {
        uint8_t caps[2]; cbe_caps(e, caps);
        if (caps[h[4] - 1] != CBE_CAP_TAPE) error = CBE_ERR_INVALID_ARG;
        else w = cbd_tlv(extra, sizeof extra, 0x11, e->slots.eid, 16);
    } else {
        const uint8_t cmd = message == CBE_VINYL_TAPE_EID ? CBE_TAPE_EID : h[0];
        const uint32_t txid = cbd_rd32(h + (message == CBE_VINYL_TAPE ? 1 : 0));
        const uint8_t slot = h[message == CBE_VINYL_TAPE ? 5 : 4];
        uint32_t plen = 0;
        const uint8_t *payload = message == CBE_VINYL_TAPE ? cbd_find(sdu, n, 0x10, &plen) : 0;
        const uint32_t total = payload && plen >= 5 ? cbd_rd16(payload + 1) : 1;
        const uint32_t index = payload && plen >= 5 ? cbd_rd16(payload + 3) : 0;
        const uint32_t bytes = payload && plen >= 5 ? plen - 5 : 0;
        int found = -1, free_job = -1;
        for (uint32_t i = 0; i < CBE_JOBS; i++) {
            struct cbe_job *j = &e->job[i];
            if (!j->state) free_job = (int)i;
            else if (j->channel == channel && j->cid == cid && j->txid == txid) found = (int)i;
        }
        const int empty_session = message == CBE_VINYL_TAPE && cmd == CBE_TAPE_GET_SESSION && !payload;
        if (slot < 1 || slot > 2 || !total || total > CBE_SEGMENTS || index >= total || bytes > CBE_CHUNK ||
            (message == CBE_VINYL_TAPE && !empty_session &&
             (cmd == CBE_TAPE_EID || !payload || plen < 5 || !bytes || (index + 1 < total && bytes != CBE_CHUNK))))
            error = CBE_ERR_INVALID_ARG;
        else if (found < 0 && index) error = CBE_ERR_INVALID_ARG;
        else {
            if (found < 0) {
                uint32_t off = 0, cap = message == CBE_VINYL_TAPE ? total * CBE_CHUNK : 0;
                if (free_job < 0 || !cbe_reserve(e, cap, &off)) error = CBE_ERR_DEVICE_IN_USE;
                else {
                    found = free_job;
                    struct cbe_job *j = &e->job[found]; __builtin_memset(j, 0, sizeof *j);
                    j->state = CBE_J_RECEIVING; j->cmd = cmd; j->txid = txid; j->slot = slot;
                    j->channel = (uint8_t)channel; j->cid = (uint8_t)cid; j->qmi_txn = cbd_rd16(sdu + 1);
                    j->off = off; j->cap = cap; j->total = total; j->seq = ++e->seq;
                }
            }
            if (!error) {
                struct cbe_job *j = &e->job[found];
                if (j->state != CBE_J_RECEIVING || j->cmd != cmd || j->slot != slot || j->total != total || j->next != index)
                    error = CBE_ERR_INVALID_ARG;
                else {
                    if (bytes) __builtin_memcpy(e->in + j->off + j->len, payload + 5, bytes);
                    j->len += bytes; j->next++; j->deadline = now + 30000u;
                    if (j->next == j->total) {
                        j->state = CBE_J_READY;
                        cbe_event(e, CBE_EV_REQUEST, cmd, txid, slot, j->len, total);
                    }
                    /* 0x1004 is answered after its real EID read, not acknowledged twice. */
                    if (message == CBE_VINYL_TAPE_EID) return CBE_DEFERRED;
                }
            }
        }
    }
    return cbq_private_answer(sdu, CBE_SVC_VINYL, cid, error, extra, error ? 0 : w, out, room);
}

static inline void cbe_tick(struct cbe *e, uint32_t now)
{
    if (e->active >= 0 && (int32_t)(now - e->job[e->active].deadline) >= 0) {
        cbe_finish(e, 1, CBE_R_TIMEOUT, 0, now); return;
    }
    for (uint32_t i = 0; i < CBE_JOBS; i++)
        if (e->job[i].state == CBE_J_RECEIVING && (int32_t)(now - e->job[i].deadline) >= 0)
            e->job[i].state = CBE_J_FREE;
    if (e->step == CBE_S_SETTLE && (int32_t)(now - e->deadline) >= 0) {
        e->step = CBE_S_RESLOT; cbe_send_slot_status(e, now);
    } else if (e->step == CBE_S_RESTORE_SETTLE && (int32_t)(now - e->deadline) >= 0) {
        e->slots.known = 0; e->step = CBE_S_RESLOT; cbe_send_slot_status(e, now);
    } else if (e->step != CBE_S_IDLE && e->step != CBE_S_SETTLE && e->step != CBE_S_RESTORE_SETTLE &&
               (int32_t)(now - e->deadline) >= 0) {
        e->req_len = e->wait_msg = 0;
        if (e->step == CBE_S_CLOSE) {
            e->done_status = 1; cbe_event(e, CBE_EV_FAILED, e->done.cmd, e->step, 0, CBE_R_TIMEOUT, 0);
            e->channel_open = 0; cbe_restore(e, now);
        }
        else if (e->active >= 0) cbe_finish(e, 1, CBE_R_TIMEOUT, 0, now);
        else { if (e->switched) e->route_failed = 1; e->done_status = 1; e->slots.known = 0; e->freeze = 0; e->switched = 0; e->step = CBE_S_IDLE; e->deadline = now + 5000u; }
    }
    if (e->step != CBE_S_IDLE) return;
    if (e->session && (int32_t)(now - e->session_deadline) >= 0) {
        cbe_session_release(e, now, 1); return;
    }
    if (e->done_pending) return;
    int next = -1;
    for (uint32_t i = 0; i < CBE_JOBS; i++)
        if (e->job[i].state == CBE_J_READY && (!e->session || cbe_session_owner(e, &e->job[i])) && (next < 0 || (int32_t)(e->job[i].seq - e->job[next].seq) < 0)) next = (int)i;
    if (next >= 0) {
        e->active = next; e->job[next].deadline = now + 180000u;
        if (e->slots.known) cbe_begin(e, now);
        else { e->step = CBE_S_SLOTS; cbe_send_slot_status(e, now); }
    } else if (!e->session && !e->slots.known && (int32_t)(now - e->deadline) >= 0) {
        e->step = CBE_S_SLOTS; cbe_send_slot_status(e, now);
    }
}

/* Consume only the response to this engine's private UIM client/transaction. */
static inline int cbe_answer(struct cbe *e, const uint8_t *sdu, uint32_t n, uint32_t now)
{
    if (!cbd_sdu_ok(sdu, n) || sdu[0] != CBQ_QMI_RESPONSE || !e->wait_msg ||
        cbd_rd16(sdu + 1) != e->wait_txn || cbd_rd16(sdu + 3) != e->wait_msg) return 0;
    const uint32_t error = cbd_error(sdu, n), step = e->step;
    e->req_len = e->wait_msg = 0;
    if (step == CBE_S_CLOSE) {
        if (error) { e->done_status = 1; cbe_event(e, CBE_EV_FAILED, e->done.cmd, step, error, CBE_R_QMI, 0); }
        e->channel_open = 0; cbe_restore(e, now); return 1;
    }
    if (step == CBE_S_RESTORE) {
        if (error && error != CBE_ERR_NO_EFFECT) {
            cbe_event(e, CBE_EV_FAILED, e->done.cmd, step, error, CBE_R_QMI, 0);
            e->route_failed = 1; e->done_status = 1; e->slots.known = 0; e->freeze = 0; e->switched = 0; e->step = CBE_S_IDLE; e->deadline = now + 5000u;
        } else { e->step = CBE_S_RESTORE_SETTLE; e->deadline = now + CBE_SETTLE_MS; }
        return 1;
    }
    if (error && !(step == CBE_S_SWITCH && error == CBE_ERR_NO_EFFECT)) {
        if (e->active >= 0) cbe_finish(e, 1, CBE_R_QMI, error, now);
        else {
            cbe_event(e, CBE_EV_SLOTS_FAILED, error, 0, 0, 0, 0);
            if (e->done_pending) e->done_status = 1;
            if (e->switched) e->route_failed = 1;
            e->slots.known = 0; e->freeze = e->switched = 0;
            e->step = CBE_S_IDLE; e->deadline = now + 5000u;
        }
        return 1;
    }
    if (step == CBE_S_SLOTS || step == CBE_S_RESLOT) {
        if (!cbe_parse_slots(&e->slots, sdu, n)) {
            if (e->active >= 0) cbe_finish(e, 1, CBE_R_NO_SLOTS, 0, now);
            else {
                if (e->done_pending) e->done_status = 1;
                if (e->switched) e->route_failed = 1;
                e->step = CBE_S_IDLE; e->slots.known = 0; e->freeze = 0; e->switched = 0; e->deadline = now + 5000u;
            }
            return 1;
        }
        cbe_event(e, CBE_EV_SLOTS, e->slots.count, e->slots.euicc, e->slots.active, e->slots.eid_len, 0);
        if (e->active < 0) {
            if (e->route_failed && e->slots.active == e->restore_phys) e->route_failed = 0;
            if (e->switched && e->slots.active != e->restore_phys) {
                e->route_failed = 1; e->done_status = 1;
                cbe_event(e, CBE_EV_FAILED, e->done.cmd, step, e->slots.active, CBE_R_INACTIVE, 0);
            }
            e->step = CBE_S_IDLE; e->switched = e->freeze = 0;
            if (e->route_failed) { e->slots.known = 0; e->deadline = now + 5000u; }
            cbe_caps_note(e);
        } else if (step == CBE_S_SLOTS) { cbe_caps_note(e); cbe_begin(e, now); }
        else if (e->slots.active != e->slots.euicc) cbe_finish(e, 1, CBE_R_INACTIVE, e->slots.active, now);
        else { e->step = CBE_S_OPEN; cbe_send_open(e, now); }
    } else if (step == CBE_S_SWITCH) {
        cbe_event(e, CBE_EV_SWITCH, e->slots.euicc, e->slots.logical, error, 0, 0);
        e->step = CBE_S_SETTLE; e->deadline = now + CBE_SETTLE_MS;
    } else if (step == CBE_S_OPEN) {
        uint32_t l = 0; const uint8_t *ch = cbd_find(sdu, n, 0x10, &l);
        if (!ch || l != 1 || !ch[0]) { cbe_finish(e, 1, CBE_R_QMI, 0, now); return 1; }
        e->channel_id = ch[0]; e->channel_open = 1;
        cbe_event(e, CBE_EV_OPEN, 0, ch[0], 0, 0, 0);
        e->step = CBE_S_APDU; cbe_send_block(e, now);
    } else if (step == CBE_S_APDU) {
        uint32_t l = 0; const uint8_t *a = cbd_find(sdu, n, 0x10, &l);
        if (!a || l < 4 || cbd_rd16(a) + 2u != l) { cbe_finish(e, 1, CBE_R_QMI, 0, now); return 1; }
        const uint32_t bytes = cbd_rd16(a) - 2u;
        e->sw1 = a[2 + bytes]; e->sw2 = a[3 + bytes];
        if (bytes > e->resp_cap - e->resp_len) { cbe_finish(e, 1, CBE_R_ROOM, bytes, now); return 1; }
        __builtin_memcpy(e->resp + e->resp_len, a + 2, bytes); e->resp_len += bytes;
        if (e->sw1 == 0x61) { e->get_more = 1; cbe_send_block(e, now); return 1; }
        if (e->sw1 != 0x90 || e->sw2) { cbe_finish(e, 1, CBE_R_SW, (uint32_t)e->sw1 << 8 | e->sw2, now); return 1; }
        e->get_more = 0;
        if (e->frag_pos < e->seg[e->seg_index].len) { cbe_send_block(e, now); return 1; }
        if (++e->seg_index < e->seg_count) {
            e->frag_pos = e->block = e->resp_len = 0; cbe_send_block(e, now); return 1;
        }
        if (e->job[e->active].cmd == CBE_TAPE_EID) {
            uint32_t tag = 0, len = 0, h = cbe_der(e->resp, e->resp_len, &tag, &len);
            uint32_t at = tag == 0xbf3eu ? h : 0;
            h = cbe_der(e->resp + at, e->resp_len - at, &tag, &len);
            if (!h || tag != 0x5a || len != 16) { cbe_finish(e, 1, CBE_R_SW, 0, now); return 1; }
            __builtin_memcpy(e->slots.eid, e->resp + at + h, 16); e->slots.eid_len = 16;
        }
        cbe_finish(e, 0, 0, 0, now);
    }
    return 1;
}

/* One completed result, emitted in <=1500-byte Tape segments. The caller must retain the
 * returned frame before calling again. No response/APDU/profile/EID bytes go to the log. */
static inline uint32_t cbe_output(struct cbe *e, uint8_t *channel, uint8_t *out, uint32_t room)
{
    if (!e->done_pending || e->step != CBE_S_IDLE) return 0;
    uint8_t tlvs[CBE_CHUNK + 32]; uint32_t w = 0, n;
    *channel = e->done.channel;
    if (e->done.cmd == CBE_TAPE_EID) {
        if (!e->done_status && e->slots.eid_len == 16) w = cbd_tlv(tlvs, sizeof tlvs, 0x11, e->slots.eid, 16);
        n = cbq_private_message(CBQ_QMI_RESPONSE, e->done.qmi_txn, CBE_VINYL_TAPE_EID, CBE_SVC_VINYL,
            e->done.cid, e->done_status ? CBE_ERR_INTERNAL : 0, tlvs, w, out, room);
        if (n) e->done_pending = 0;
        return n;
    }
    /* IndTapeIndResult is 11 wire bytes, not the legacy Info result's 8-byte layout.
     * The native parser reads u16 at 1, u16 at 3, u32 transaction at 5, SW bytes at 9/10. */
    uint8_t info[11] = { 0 };
    info[0] = e->done.cmd; cbd_wr16(info + 1, e->done_status);
    cbd_wr32(info + 5, e->done.txid);
    info[9] = e->done_status ? e->done_sw1 : 0; info[10] = e->done_status ? e->done_sw2 : 0;
    w = cbd_tlv(tlvs, sizeof tlvs, 0x01, info, sizeof info);
    /* The native handler explicitly accepts a successful GetSessionData with no payload TLV. */
    if (e->done.cmd == CBE_TAPE_GET_SESSION || e->done.cmd == CBE_TAPE_CLEAR_SESSION) {
        n = cbq_private_indication(CBE_VINYL_TAPE_INFO, CBE_SVC_VINYL, e->done.cid, tlvs, w, out, room);
        if (n) e->done_pending = 0;
        return n;
    }
    const uint32_t count = e->done_len ? (e->done_len + CBE_CHUNK - 1u) / CBE_CHUNK : 1;
    const uint32_t at = e->done_chunk * CBE_CHUNK;
    const uint32_t bytes = e->done_len - at < CBE_CHUNK ? e->done_len - at : CBE_CHUNK;
    tlvs[w] = 0x10; cbd_wr16(tlvs + w + 1, 4u + bytes);
    cbd_wr16(tlvs + w + 3, count); cbd_wr16(tlvs + w + 5, e->done_chunk);
    if (bytes) __builtin_memcpy(tlvs + w + 7, e->resp + at, bytes);
    w += 7u + bytes;
    n = cbq_private_indication(CBE_VINYL_TAPE_INFO, CBE_SVC_VINYL, e->done.cid, tlvs, w, out, room);
    if (n && ++e->done_chunk >= count) e->done_pending = 0;
    return n;
}

#endif /* COACHZ_BB_ESIM_H */
