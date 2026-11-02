/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The QMUX side of the baseband bridge: iPadOS's ABM speaks QMUX on the qmux1 and qmux2 channels of what it takes for a PCIe
 * Qualcomm modem; this modem serves QMI over QRTR. Portable and host-tested; no kernel code.
 *
 * Framing (the first six bytes of Apple's QMuxHeader, the standard QMUX header): I/F type 1, length (excluding the I/F byte, little
 * endian), control flags (0x00 from the host, 0x80 from a service), service type, client id, then the QMI SDU. For every service but
 * CTL the SDU header (flags, 16-bit transaction, message id, length) is exactly QMI's header on QRTR, so a request goes to the modem as
 * the frame minus six bytes and an answer comes back with the six bytes added (libqmi's QRTR backend does the same).
 *
 * CTL (service 0, 8-bit transaction) is answered here, as libqmi's QRTR backend answers it: GetClientId allocates a client id per
 * channel and service and ties it to one local QRTR port, ReleaseClientId frees it and asks for a QRTR DEL_CLIENT, Sync releases every
 * client of that channel; GetVersionInfo lists the modem's services. Apple's QMI parser also knows ConfigPowerSaveMode(Ext) and the
 * RevokeClientId indication; any other CTL request is accepted with a success result and counted, so the kernel can log what ABM asked.
 *
 * Apple's private services (AUDIO 0xe1 to SFT 0xea, numbers from libQMIParser's serviceTypeFromString) do not exist on this modem. A
 * client of one gets an id like any other; its requests come back to the caller as CBQ_PRIVATE so the bridge can implement a message
 * (cbq_private_answer, cbq_private_indication), and cbq_private_unsupported answers the rest with QMI_ERR_NOT_SUPPORTED.
 *
 * The bridge also takes modem clients of its own (channel CBQ_INTERNAL), for what the modem needs and ABM never asks, as the data port
 * setup of coachz_bb_data.h. Their answers come back from cbq_from_modem with that channel and are not ABM's.
 */
#ifndef COACHZ_BB_QMUX_H
#define COACHZ_BB_QMUX_H

#include <stdint.h>

#define CBQ_IF_TYPE 0x01u
#define CBQ_HDR 6u                    /* I/F type, length (2), control flags, service type, client id */
#define CBQ_FLAG_SERVICE 0x80u
#define CBQ_CTL_HDR 6u                /* flags, transaction (1), message id (2), TLV length (2) */
#define CBQ_SDU_HDR 7u                /* flags, transaction (2), message id (2), TLV length (2) */
#define CBQ_CTL_RESPONSE 0x01u
#define CBQ_CTL_INDICATION 0x02u
#define CBQ_QMI_REQUEST 0x00u
#define CBQ_QMI_RESPONSE 0x02u
#define CBQ_QMI_INDICATION 0x04u

#define CBQ_CTL_GET_VERSION_INFO 0x0021u
#define CBQ_CTL_GET_CLIENT_ID 0x0022u
#define CBQ_CTL_RELEASE_CLIENT_ID 0x0023u
#define CBQ_CTL_REVOKE_CLIENT_ID 0x0024u
#define CBQ_CTL_SYNC 0x0027u

/* qmi-errors.h */
#define CBQ_ERR_MALFORMED_MESSAGE 1u
#define CBQ_ERR_CLIENT_IDS_EXHAUSTED 5u
#define CBQ_ERR_INVALID_CLIENT_ID 7u
#define CBQ_ERR_MISSING_ARGUMENT 17u
#define CBQ_ERR_INVALID_SERVICE_TYPE 31u
#define CBQ_ERR_NOT_SUPPORTED 94u

#define CBQ_CHANNELS 2u                /* qmux1, qmux2 */
#define CBQ_INTERNAL CBQ_CHANNELS      /* clients the bridge itself holds */
#define CBQ_MAX_SERVERS 64u
#define CBQ_MAX_CLIENTS 64u
#define CBQ_PORT_BASE 0x4100u          /* local QRTR ports: base + client slot (Wi-Fi and IPA use 0x4000..0x4003) */

static inline int cbq_private_service(uint32_t service) { return service >= 0xe1u && service <= 0xeau; }

struct cbq_server { uint32_t service, version, instance, node, port; uint8_t present; };
struct cbq_client { uint8_t used, channel, service, cid, private_service, release_pending; };
struct cbq_bridge {
    struct cbq_server server[CBQ_MAX_SERVERS];
    struct cbq_client client[CBQ_MAX_CLIENTS];
    uint8_t last_cid[CBQ_CHANNELS + 1][256];
    uint32_t ctl_other, dropped;
};

enum cbq_kind {
    CBQ_DROP,       /* nothing to send (malformed, unknown client, indication from the host); counted in dropped */
    CBQ_TO_HOST,    /* `out` holds `len` bytes: one QMUX frame for `channel` */
    CBQ_TO_MODEM,   /* send the input frame from offset CBQ_HDR (`len` bytes) from local port `local_port` to node:port */
    CBQ_PRIVATE,    /* a request to a private service: answer it, or cbq_private_unsupported */
};
struct cbq_result { enum cbq_kind kind; uint32_t len, node, port, local_port; uint8_t channel, service, cid; };

static inline void cbq_init(struct cbq_bridge *b) { __builtin_memset(b, 0, sizeof *b); }

static inline uint16_t cbq_rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline void cbq_wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* ------------------------------------------------------------------ servers, from QRTR NEW_SERVER / DEL_SERVER */
/* QRTR packs the version in the low byte and the instance above it. The lowest instance of a service is the one used, as qmicli's
   QRTR backend takes the first it finds. */
static inline int cbq_server_new(struct cbq_bridge *b, uint32_t service, uint32_t version_instance, uint32_t node, uint32_t port)
{
    struct cbq_server *free_slot = 0;
    for (unsigned i = 0; i < CBQ_MAX_SERVERS; i++) {
        struct cbq_server *s = &b->server[i];
        if (s->present && s->service == service && s->node == node && s->port == port) return 1;
        if (!s->present && !free_slot) free_slot = s;
    }
    if (!free_slot || service > 0xffu) return 0;   /* QMUX has an 8-bit service type */
    *free_slot = (struct cbq_server){ service, version_instance & 0xffu, version_instance >> 8, node, port, 1 };
    return 1;
}
static inline void cbq_server_del(struct cbq_bridge *b, uint32_t service, uint32_t node, uint32_t port)
{
    for (unsigned i = 0; i < CBQ_MAX_SERVERS; i++) {
        struct cbq_server *s = &b->server[i];
        if (s->present && s->service == service && s->node == node && s->port == port) s->present = 0;
    }
}
static inline const struct cbq_server *cbq_server_find(const struct cbq_bridge *b, uint32_t service)
{
    const struct cbq_server *best = 0;
    for (unsigned i = 0; i < CBQ_MAX_SERVERS; i++) {
        const struct cbq_server *s = &b->server[i];
        if (s->present && s->service == service && (!best || s->instance < best->instance)) best = s;
    }
    return best;
}

/* ------------------------------------------------------------------ clients */
static inline int cbq_client_find(const struct cbq_bridge *b, uint32_t channel, uint32_t service, uint32_t cid)
{
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; i++) {
        const struct cbq_client *c = &b->client[i];
        if (c->used && c->channel == channel && c->service == service && c->cid == cid) return (int)i;
    }
    return -1;
}
/* client ids 1..254 per channel and service, round robin after the last one given (0 is CTL's, 255 broadcast) */
static inline int cbq_client_alloc(struct cbq_bridge *b, uint32_t channel, uint32_t service)
{
    int slot = -1;
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; i++)
        if (!b->client[i].used && !b->client[i].release_pending) { slot = (int)i; break; }
    if (slot < 0) return -1;
    uint32_t cid = b->last_cid[channel][service];
    for (unsigned tries = 0; tries < 254u; tries++) {
        cid = cid % 254u + 1u;
        if (cbq_client_find(b, channel, service, cid) < 0) {
            b->last_cid[channel][service] = (uint8_t)cid;
            b->client[slot] = (struct cbq_client){ 1, (uint8_t)channel, (uint8_t)service, (uint8_t)cid, (uint8_t)cbq_private_service(service), 0 };
            return slot;
        }
    }
    return -1;
}
/* A freed modem client keeps its slot (and so its QRTR port) until the caller has sent DEL_CLIENT for it. */
static inline void cbq_client_free(struct cbq_bridge *b, unsigned slot)
{
    struct cbq_client *c = &b->client[slot];
    c->release_pending = !c->private_service;
    c->used = 0;
}
/* the next port that needs a QRTR DEL_CLIENT, or 0; call until 0 after any CTL answer */
static inline uint32_t cbq_next_release(struct cbq_bridge *b)
{
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; i++)
        if (b->client[i].release_pending) { b->client[i].release_pending = 0; return CBQ_PORT_BASE + i; }
    return 0;
}

/* ------------------------------------------------------------------ frames */
static inline uint32_t cbq_frame_header(uint8_t *out, uint32_t sdu_len, uint32_t service, uint32_t cid)
{
    out[0] = CBQ_IF_TYPE;
    cbq_wr16(out + 1, CBQ_HDR - 1u + sdu_len);
    out[3] = CBQ_FLAG_SERVICE;
    out[4] = (uint8_t)service;
    out[5] = (uint8_t)cid;
    return CBQ_HDR;
}
static inline uint32_t cbq_tlv_result(uint8_t *p, uint32_t error)
{
    p[0] = 0x02; cbq_wr16(p + 1, 4);
    cbq_wr16(p + 3, error ? 1u : 0u);
    cbq_wr16(p + 5, error);
    return 7;
}

/* CTL answer: [QMUX][flags 1, txn, id, len][result][optional TLV 0x01 of `n` bytes] */
static inline uint32_t cbq_ctl_answer(uint8_t *out, uint32_t room, uint32_t txn, uint32_t id, uint32_t error, const uint8_t *tlv1, uint32_t n)
{
    const uint32_t tlvs = 7u + (tlv1 ? 3u + n : 0u), total = CBQ_HDR + CBQ_CTL_HDR + tlvs;
    if (total > room) return 0;
    cbq_frame_header(out, CBQ_CTL_HDR + tlvs, 0, 0);
    uint8_t *s = out + CBQ_HDR;
    s[0] = CBQ_CTL_RESPONSE;
    s[1] = (uint8_t)txn;
    cbq_wr16(s + 2, id);
    cbq_wr16(s + 4, tlvs);
    uint8_t *t = s + CBQ_CTL_HDR;
    t += cbq_tlv_result(t, error);
    if (tlv1) {
        t[0] = 0x01; cbq_wr16(t + 1, n);
        for (uint32_t i = 0; i < n; i++) t[3 + i] = tlv1[i];
    }
    return total;
}
/* the CTL Sync indication a modem sends when its QMUX restarts */
static inline uint32_t cbq_ctl_sync_indication(uint8_t *out, uint32_t room)
{
    if (room < CBQ_HDR + CBQ_CTL_HDR) return 0;
    cbq_frame_header(out, CBQ_CTL_HDR, 0, 0);
    uint8_t *s = out + CBQ_HDR;
    s[0] = CBQ_CTL_INDICATION; s[1] = 0;
    cbq_wr16(s + 2, CBQ_CTL_SYNC);
    cbq_wr16(s + 4, 0);
    return CBQ_HDR + CBQ_CTL_HDR;
}

/* find TLV `type` in a TLV area; returns its value and length, or 0 */
static inline const uint8_t *cbq_tlv(const uint8_t *p, uint32_t n, uint32_t type, uint32_t *len)
{
    while (n >= 3) {
        const uint32_t l = cbq_rd16(p + 1);
        if (3u + l > n) return 0;
        if (p[0] == type) { *len = l; return p + 3; }
        p += 3u + l; n -= 3u + l;
    }
    return 0;
}

static inline struct cbq_result cbq_ctl(struct cbq_bridge *b, uint32_t channel, const uint8_t *s, uint32_t n, uint8_t *out, uint32_t room)
{
    struct cbq_result r = { CBQ_TO_HOST, 0, 0, 0, 0, (uint8_t)channel, 0, 0 };
    const uint32_t txn = s[1], id = cbq_rd16(s + 2), tlen = cbq_rd16(s + 4);
    const uint8_t *tlvs = s + CBQ_CTL_HDR, *v;
    uint32_t vlen = 0;
    if (s[0] != 0 || CBQ_CTL_HDR + tlen > n) { b->dropped++; r.kind = CBQ_DROP; return r; }
    switch (id) {
    case CBQ_CTL_GET_CLIENT_ID: {
        v = cbq_tlv(tlvs, tlen, 0x01, &vlen);
        if (!v || vlen < 1) { r.len = cbq_ctl_answer(out, room, txn, id, CBQ_ERR_MISSING_ARGUMENT, 0, 0); break; }
        const uint32_t service = v[0];
        if (!service || (!cbq_private_service(service) && !cbq_server_find(b, service))) {
            r.len = cbq_ctl_answer(out, room, txn, id, CBQ_ERR_INVALID_SERVICE_TYPE, 0, 0);
            break;
        }
        const int slot = cbq_client_alloc(b, channel, service);
        if (slot < 0) { r.len = cbq_ctl_answer(out, room, txn, id, CBQ_ERR_CLIENT_IDS_EXHAUSTED, 0, 0); break; }
        const uint8_t info[2] = { (uint8_t)service, b->client[slot].cid };
        r.service = (uint8_t)service; r.cid = info[1]; r.local_port = CBQ_PORT_BASE + (uint32_t)slot;
        r.len = cbq_ctl_answer(out, room, txn, id, 0, info, 2);
        break;
    }
    case CBQ_CTL_RELEASE_CLIENT_ID: {
        v = cbq_tlv(tlvs, tlen, 0x01, &vlen);
        if (!v || vlen < 2) { r.len = cbq_ctl_answer(out, room, txn, id, CBQ_ERR_MISSING_ARGUMENT, 0, 0); break; }
        const int slot = cbq_client_find(b, channel, v[0], v[1]);
        if (slot < 0) { r.len = cbq_ctl_answer(out, room, txn, id, CBQ_ERR_INVALID_CLIENT_ID, 0, 0); break; }
        cbq_client_free(b, (unsigned)slot);
        r.service = v[0]; r.cid = v[1]; r.local_port = CBQ_PORT_BASE + (uint32_t)slot;
        r.len = cbq_ctl_answer(out, room, txn, id, 0, v, 2);
        break;
    }
    case CBQ_CTL_SYNC:
        for (unsigned i = 0; i < CBQ_MAX_CLIENTS; i++)
            if (b->client[i].used && b->client[i].channel == channel) cbq_client_free(b, i);
        r.len = cbq_ctl_answer(out, room, txn, id, 0, 0, 0);
        break;
    case CBQ_CTL_GET_VERSION_INFO: {
        uint8_t list[1 + 5 * 32];
        uint32_t count = 0;
        for (unsigned i = 0; i < CBQ_MAX_SERVERS && count < 32; i++) {
            const struct cbq_server *s2 = &b->server[i];
            if (!s2->present || cbq_server_find(b, s2->service) != s2) continue;
            uint8_t *e = list + 1 + 5 * count++;
            e[0] = (uint8_t)s2->service; cbq_wr16(e + 1, s2->version); cbq_wr16(e + 3, 0);
        }
        list[0] = (uint8_t)count;
        r.len = cbq_ctl_answer(out, room, txn, id, 0, list, 1 + 5 * count);
        break;
    }
    default:   /* ConfigPowerSaveMode and anything else: accepted, nothing here sleeps */
        b->ctl_other++;
        r.len = cbq_ctl_answer(out, room, txn, id, 0, 0, 0);
        break;
    }
    if (!r.len) { b->dropped++; r.kind = CBQ_DROP; }
    return r;
}

/* One write from ABM on a QMUX channel (one frame per write, as MHI delivers one transfer). */
static inline struct cbq_result cbq_from_host(struct cbq_bridge *b, uint32_t channel, const uint8_t *f, uint32_t n, uint8_t *out, uint32_t room)
{
    struct cbq_result r = { CBQ_DROP, 0, 0, 0, 0, (uint8_t)channel, 0, 0 };
    if (channel >= CBQ_CHANNELS || n < CBQ_HDR + CBQ_CTL_HDR || f[0] != CBQ_IF_TYPE || cbq_rd16(f + 1) + 1u != n || f[3] != 0) {
        b->dropped++;
        return r;
    }
    const uint32_t service = f[4], cid = f[5];
    if (!service) return cbq_ctl(b, channel, f + CBQ_HDR, n - CBQ_HDR, out, room);
    const int slot = cbq_client_find(b, channel, service, cid);
    const uint8_t *s = f + CBQ_HDR;
    if (slot < 0 || n < CBQ_HDR + CBQ_SDU_HDR || CBQ_SDU_HDR + cbq_rd16(s + 5) > n - CBQ_HDR || s[0] != CBQ_QMI_REQUEST) {
        b->dropped++;
        return r;
    }
    r.service = (uint8_t)service; r.cid = (uint8_t)cid; r.local_port = CBQ_PORT_BASE + (uint32_t)slot;
    r.len = n - CBQ_HDR;
    if (b->client[slot].private_service) { r.kind = CBQ_PRIVATE; return r; }
    const struct cbq_server *srv = cbq_server_find(b, service);
    if (!srv) { b->dropped++; return r; }
    r.kind = CBQ_TO_MODEM;
    r.node = srv->node;
    r.port = srv->port;
    return r;
}

/* A QMI message from the modem to one of the bridge's client ports: the QMUX frame for ABM. */
static inline struct cbq_result cbq_from_modem(struct cbq_bridge *b, uint32_t local_port, uint32_t node, const uint8_t *sdu, uint32_t n, uint8_t *out, uint32_t room)
{
    struct cbq_result r = { CBQ_DROP, 0, node, 0, local_port, 0, 0, 0 };
    const uint32_t slot = local_port - CBQ_PORT_BASE;
    if (local_port < CBQ_PORT_BASE || slot >= CBQ_MAX_CLIENTS || !b->client[slot].used || b->client[slot].private_service ||
        n < CBQ_SDU_HDR || CBQ_SDU_HDR + cbq_rd16(sdu + 5) > n || CBQ_HDR + n > room) {
        b->dropped++;
        return r;
    }
    const struct cbq_client *c = &b->client[slot];
    const struct cbq_server *srv = cbq_server_find(b, c->service);
    if (!srv || srv->node != node) { b->dropped++; return r; }
    cbq_frame_header(out, n, c->service, c->cid);
    for (uint32_t i = 0; i < n; i++) out[CBQ_HDR + i] = sdu[i];
    r.kind = CBQ_TO_HOST; r.len = CBQ_HDR + n; r.channel = c->channel; r.service = c->service; r.cid = c->cid;
    return r;
}

/* A private-service message from the bridge: `type` CBQ_QMI_RESPONSE (result `error`, then `tlvs`) or CBQ_QMI_INDICATION (`tlvs` only,
   transaction 0). */
static inline uint32_t cbq_private_message(uint32_t type, uint32_t txn, uint32_t message, uint32_t service, uint32_t cid, uint32_t error,
                                           const uint8_t *tlvs, uint32_t tlv_len, uint8_t *out, uint32_t room)
{
    const uint32_t body = (type == CBQ_QMI_RESPONSE ? 7u : 0u) + tlv_len, total = CBQ_HDR + CBQ_SDU_HDR + body;
    if (room < total || body > 0xffffu - CBQ_HDR - CBQ_SDU_HDR) return 0;
    cbq_frame_header(out, CBQ_SDU_HDR + body, service, cid);
    uint8_t *s = out + CBQ_HDR, *t = s + CBQ_SDU_HDR;
    s[0] = (uint8_t)type;
    cbq_wr16(s + 1, txn);
    cbq_wr16(s + 3, message);
    cbq_wr16(s + 5, body);
    if (type == CBQ_QMI_RESPONSE) t += cbq_tlv_result(t, error);
    for (uint32_t i = 0; i < tlv_len; i++) t[i] = tlvs[i];
    return total;
}
/* The answer to a private-service request: same message id and transaction. */
static inline uint32_t cbq_private_answer(const uint8_t *request_sdu, uint32_t service, uint32_t cid, uint32_t error, const uint8_t *tlvs,
                                          uint32_t tlv_len, uint8_t *out, uint32_t room)
{
    return cbq_private_message(CBQ_QMI_RESPONSE, cbq_rd16(request_sdu + 1), cbq_rd16(request_sdu + 3), service, cid, error, tlvs, tlv_len, out, room);
}
static inline uint32_t cbq_private_indication(uint32_t message, uint32_t service, uint32_t cid, const uint8_t *tlvs, uint32_t tlv_len,
                                              uint8_t *out, uint32_t room)
{
    return cbq_private_message(CBQ_QMI_INDICATION, 0, message, service, cid, 0, tlvs, tlv_len, out, room);
}
/* The answer to a private-service request nobody implements: QMI_ERR_NOT_SUPPORTED. */
static inline uint32_t cbq_private_unsupported(const uint8_t *request_sdu, uint32_t service, uint32_t cid, uint8_t *out, uint32_t room)
{
    return cbq_private_answer(request_sdu, service, cid, CBQ_ERR_NOT_SUPPORTED, 0, 0, out, room);
}

#endif /* COACHZ_BB_QMUX_H */
