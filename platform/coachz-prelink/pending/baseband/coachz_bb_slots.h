/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef COACHZ_BB_SLOTS_H
#define COACHZ_BB_SLOTS_H

#include "coachz_bb_data.h"

/* CommCenter's two card endpoints against the modem's active logical slot.
 *
 * CommCenter keeps its eSIM (Tape) management on endpoint two (coachz_bb_esim.h), while this modem reports whichever card is active
 * in its logical slot one: with the eUICC selected, the enabled profile's card and Ready applications arrive as card one and the
 * eSIM's capability as endpoint two, and CommCenter ties the subscription to the wrong card. While the eUICC is the selected radio
 * card this adapter moves the card records and the card byte of each application index so the active card sits on endpoint two,
 * and maps CommCenter's endpoint-two slot and card-session selectors back to the modem's logical slot. Provisioning session numbers
 * (primary and secondary GW/1x) are a separate namespace and are never renumbered: they follow the application indexes.
 * With the physical card selected nothing is translated (that mapping is the identity, and is the route accepted on hardware).
 *
 * Wire layout (libqmi data/qmi-service-uim.json, "UIM Card Status"): four u16 application indexes (low byte application, high byte
 * card, 0xffff none), a counted card array of {card state, UPIN state, UPIN retries, UPUK retries, error code, counted applications},
 * each application 6 bytes, a counted AID, then 7 bytes. Slot selectors: TLV 0x01 Slot (Power Off/On SIM 0x30/0x31, Send APDU 0x3b,
 * Logical Channel 0x3f, Open Logical Channel 0x42); TLV 0x01 Session, first byte its type (0x20-0x28, 0x2a, 0x2c, 0x34, 0x44; Change
 * Provisioning Session 0x38, whose TLV 0x10 Application Information starts with its slot). Session types 4 to 9 are the
 * non-provisioning, card and logical-channel sessions on slot one (even) and two (odd). */
struct cbl_view { uint8_t active, logical, present[2]; };   /* active: CommCenter endpoint of the active card; logical: modem slot */

#define CBL_ERR_DEVICE_NOT_READY 52u   /* QMI_PROTOCOL_ERROR_DEVICE_NOT_READY: a request for the endpoint whose card is not active */

static inline uint32_t cbl_card_bytes(const uint8_t *p, uint32_t size)
{
    if (size < 6u) return 0;
    uint32_t at = 6;
    for (uint32_t i = 0; i < p[5]; i++) {
        if (size - at < 7u) return 0;
        const uint32_t bytes = 14u + p[at + 6];
        if (bytes > size - at) return 0;
        at += bytes;
    }
    return at;
}

static inline int cbl_valid(const struct cbl_view *v)
{
    return v->active >= 1 && v->active <= 2 && v->logical >= 1 && v->logical <= 2;
}

/* 1 when the endpoints and the modem's slots do not line up and the adapter has work to do. */
static inline int cbl_swapped(const struct cbl_view *v)
{
    return cbl_valid(v) && v->active != v->logical;
}

/* TLV 0x10 Card Status mapped to endpoints: the value's new length, or 0 for malformed input (then left as it is). The endpoint
 * with no modem record gets a 6-byte record carrying only physical presence (card state present or absent, no applications). */
static inline uint32_t cbl_card_status(const struct cbl_view *v, const uint8_t *in, uint32_t size, uint8_t *out, uint32_t room)
{
    if (!cbl_valid(v) || size < 9u) return 0;
    const uint32_t cards = in[8];
    if (!cards || cards > 2u || v->logical > cards) return 0;
    uint32_t offset[2] = { 0, 0 }, bytes[2] = { 0, 0 }, at = 9;
    for (uint32_t i = 0; i < cards; i++) {
        offset[i] = at;
        bytes[i] = cbl_card_bytes(in + at, size - at);
        if (!bytes[i]) return 0;
        at += bytes[i];
    }
    if (at != size) return 0;
    const uint32_t total = 9u + bytes[0] + bytes[1] + (cards == 1 ? 6u : 0u);
    if (room < total) return 0;
    __builtin_memcpy(out, in, 9);
    out[8] = 2;
    const uint32_t swap = (v->active - 1u) ^ (v->logical - 1u);
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t p = 2u * i;
        if (cbd_rd16(in + p) == 0xffffu) continue;
        if (in[p + 1] >= cards) return 0;
        out[p + 1] ^= (uint8_t)swap;
    }
    at = 9;
    for (uint32_t endpoint = 0; endpoint < 2; endpoint++) {
        const uint32_t logical = endpoint ^ swap;
        if (logical < cards) {
            __builtin_memcpy(out + at, in + offset[logical], bytes[logical]);
            at += bytes[logical];
        } else {
            __builtin_memset(out + at, 0, 6);
            out[at] = v->present[endpoint] ? 1 : 0;
            at += 6;
        }
    }
    return at;
}

/* The other card-status TLVs are counted arrays with one entry per card, in TLV 0x10's card order, and CommCenter takes them by
 * position as well: libCommCenterMCommandDrivers' sHandleIccids keys TLV 0x17's ICCIDs by the entry's index, so an array left in the
 * modem's order gives the active eUICC's ICCID to endpoint one and none to endpoint two (EURSimCommandDriver then finds no SIM for
 * the eSIM plan, never switches its subscription, and instance kOne stays with the physical card). Entry sizes, read from this
 * modem's own responses: 0x11 hot swap (1 byte: 0 not supported, 2 present, 3 not present), 0x12 validity (1), 0x15 SIM busy (1),
 * 0x16 (4), 0x17 {ICCID length, ICCID, ATR length, ATR, EID length, EID}. */
#define CBL_ENTRY_MAX 72u
static inline int cbl_is_card_array(uint32_t type)
{
    return type == 0x11u || type == 0x12u || type == 0x15u || type == 0x16u || type == 0x17u;
}
static inline uint32_t cbl_entry_bytes(uint32_t type, const uint8_t *p, uint32_t size)
{
    if (type == 0x11u || type == 0x12u || type == 0x15u) return size >= 1u ? 1u : 0u;
    if (type == 0x16u) return size >= 4u ? 4u : 0u;
    if (type != 0x17u) return 0;
    uint32_t at = 0;
    for (uint32_t field = 0; field < 3; field++) {
        if (at >= size || p[at] > size - at - 1u) return 0;
        at += 1u + p[at];
    }
    return at <= CBL_ENTRY_MAX ? at : 0;
}
/* One such array mapped to endpoints as cbl_card_status maps the cards; fill[e] is endpoint e's entry when the modem has no card
 * for it. The value's new length, 0 for malformed input or a missing fill (then left as it is). */
static inline uint32_t cbl_card_array(const struct cbl_view *v, uint32_t type, const uint8_t *in, uint32_t size,
    const uint8_t *const fill[2], const uint32_t fill_len[2], uint8_t *out, uint32_t room)
{
    if (!cbl_valid(v) || !cbl_is_card_array(type) || size < 1u) return 0;
    const uint32_t n = in[0];
    if (!n || n > 2u || v->logical > n) return 0;
    uint32_t offset[2] = { 0, 0 }, bytes[2] = { 0, 0 }, at = 1;
    for (uint32_t i = 0; i < n; i++) {
        offset[i] = at;
        bytes[i] = cbl_entry_bytes(type, in + at, size - at);
        if (!bytes[i]) return 0;
        at += bytes[i];
    }
    if (at != size || room < 1u) return 0;
    const uint32_t swap = (v->active - 1u) ^ (v->logical - 1u);
    uint32_t w = 1;
    out[0] = 2;
    for (uint32_t endpoint = 0; endpoint < 2; endpoint++) {
        const uint32_t logical = endpoint ^ swap;
        const uint8_t *src = logical < n ? in + offset[logical] : fill[endpoint];
        const uint32_t len = logical < n ? bytes[logical] : fill_len[endpoint];
        if (!src || !len || len > room - w) return 0;
        __builtin_memcpy(out + w, src, len);
        w += len;
    }
    return w;
}

enum cbl_request_result { CBL_UNCHANGED, CBL_CHANGED, CBL_INACTIVE, CBL_MALFORMED };

/* A UIM request from CommCenter: only its slot or card-session selector (and Change Provisioning Session's application slot)
 * changes. CBL_INACTIVE: it names the endpoint whose card is not the active one, which the modem cannot reach. */
static inline enum cbl_request_result cbl_uim_request(const struct cbl_view *v, const uint8_t *in, uint32_t size, uint8_t *out,
    uint32_t room)
{
    if (!cbl_valid(v) || !cbd_sdu_ok(in, size) || size > room) return CBL_MALFORMED;
    const uint32_t msg = cbd_rd16(in + 3);
    /* 0x34 is Authenticate (QMI UIM; not in libqmi), whose TLV 0x01 is the same session information. */
    const int session = (msg >= 0x20u && msg <= 0x28u) || msg == 0x2au || msg == 0x2cu || msg == 0x34u || msg == 0x38u || msg == 0x44u;
    const int slot = msg == 0x30u || msg == 0x31u || msg == 0x3bu || msg == 0x3fu || msg == 0x42u;
    if (!session && !slot) return CBL_UNCHANGED;
    uint32_t len = 0;
    const uint8_t *selector = cbd_find(in, size, 1, &len);
    if (!selector || len < (msg == 0x38u ? 2u : 1u)) return CBL_MALFORMED;
    uint8_t mapped = selector[0];
    if (session) {
        if (mapped <= 3u) {
            /* Provisioning sessions follow the published application indexes. */
        } else if (mapped <= 9u) {
            const uint8_t endpoint = (uint8_t)(1u + ((mapped - 4u) & 1u));
            if (endpoint != v->active) return CBL_INACTIVE;
            mapped = (uint8_t)(mapped - (endpoint - 1u) + (v->logical - 1u));
        } else return CBL_INACTIVE;
    } else {
        if (mapped != v->active) return CBL_INACTIVE;
        mapped = v->logical;
    }
    const uint8_t *app = 0;
    uint32_t app_len = 0;
    if (msg == 0x38u) {
        app = cbd_find(in, size, 0x10, &app_len);
        if (app && (app_len < 2u || app[0] != v->active)) return CBL_INACTIVE;
    }
    if (mapped == selector[0] && (!app || app[0] == v->logical)) return CBL_UNCHANGED;
    __builtin_memcpy(out, in, size);
    out[selector - in] = mapped;
    if (app) out[app - in] = v->logical;
    return CBL_CHANGED;
}
#endif
