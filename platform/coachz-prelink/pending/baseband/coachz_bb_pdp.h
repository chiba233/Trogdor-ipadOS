/* SPDX-License-Identifier: BSD-2-Clause */
/* WDS lifecycle observation, not a dialler. Wire facts: libqmi data/qmi-service-wds.json.
 * Four native contexts use mux 1..4; IPv4 and IPv6 have distinct WDS clients on the same mux.
 * The control bridge owns this state under its lock. No addresses or APNs are retained here. */
#ifndef COACHZ_BB_PDP_H
#define COACHZ_BB_PDP_H
#include "coachz_bb_data.h"
#define CBP_INTERFACES 4u
#define CBP_START 0x20u
#define CBP_STOP 0x21u
#define CBP_STATUS 0x22u
#define CBP_SETTINGS 0x2du
#define CBP_FAMILY 0x4du
struct cbp_client {
    uint16_t bind_txn, family_txn, start_txn, stop_txn;
    uint8_t bind_pending, family_pending, start_pending, stop_pending;
    uint8_t mux, next_mux, family, next_family, active, settings;
    uint32_t handle;
};
static inline uint32_t cbp_family_bit(uint32_t family) { return family == 4u ? 1u : family == 6u ? 2u : 0u; }
static inline uint32_t cbp_byte(const uint8_t *sdu, uint32_t n, uint32_t type)
{
    uint32_t len = 0;
    const uint8_t *v = cbd_find(sdu, n, type, &len);
    return v && len == 1u ? *v : 0u;
}
static inline int cbp_mux_ok(uint32_t mux) { return mux >= 1u && mux <= CBP_INTERFACES; }
static inline int cbp_observe(struct cbp_client *c, const uint8_t *sdu, uint32_t n)
{
    if (!cbd_sdu_ok(sdu, n)) return 0;
    const uint32_t id = cbd_rd16(sdu + 3), txn = cbd_rd16(sdu + 1);
    if (sdu[0] == 0u) {
        if (id == CBD_WDS_BIND_MUX_DATA_PORT) {
            c->bind_txn = (uint16_t)txn;
            c->next_mux = (uint8_t)cbp_byte(sdu, n, 0x11);
            c->bind_pending = 1;
        } else if (id == CBP_FAMILY) {
            c->family_txn = (uint16_t)txn;
            c->next_family = (uint8_t)cbp_byte(sdu, n, 1);
            c->family_pending = 1;
        } else if (id == CBP_START) {
            c->start_txn = (uint16_t)txn;
            c->start_pending = 1;
            const uint32_t family = cbp_byte(sdu, n, 0x19);
            if (cbp_family_bit(family)) c->family = (uint8_t)family;
        } else if (id == CBP_STOP) {
            c->stop_txn = (uint16_t)txn;
            c->stop_pending = 1;
        } else return 0;
        return 1;
    }
    if (sdu[0] == 2u) {
        const uint32_t error = cbd_error(sdu, n);
        if (id == CBD_WDS_BIND_MUX_DATA_PORT && c->bind_pending && txn == c->bind_txn) {
            c->bind_pending = 0;
            if (!error) { c->mux = c->next_mux; c->active = c->settings = 0; c->handle = 0; }
        } else if (id == CBP_FAMILY && c->family_pending && txn == c->family_txn) {
            c->family_pending = 0;
            if (!error) c->family = c->next_family;
        } else if (id == CBP_START && c->start_pending && txn == c->start_txn) {
            c->start_pending = 0;
            if (!error) {
                const uint32_t handle = cbd_u32(sdu, n, 1);
                if (handle != 0xffffffffu) { c->handle = handle; c->active = 1; }
            }
        } else if (id == CBP_STOP && c->stop_pending && txn == c->stop_txn) {
            c->stop_pending = 0;
            if (!error) { c->active = c->settings = 0; c->handle = 0; }
        } else if (id == CBP_SETTINGS) {
            if (!error) {
                uint32_t len;
                c->settings = cbd_find(sdu, n, 0x1e, &len) && len == 4u ? 1u : 0u;
                if (cbd_find(sdu, n, 0x25, &len) && len == 17u) c->settings |= 2u;
            }
        } else if (id == CBP_STATUS && !error) {
            c->active = cbp_byte(sdu, n, 1) == 2u;
            if (!c->active) c->settings = 0;
        } else return 0;
        return 1;
    }
    if (sdu[0] == 4u && id == CBP_STATUS) {
        uint32_t len;
        const uint8_t *v = cbd_find(sdu, n, 1, &len);
        if (!v || len != 2u) return 0;
        c->active = v[0] == 2u;
        if (!c->active) { c->settings = 0; c->handle = 0; }
        return 1;
    }
    return 0;
}
/* A disconnect on one address family must not tear down the other family's interface. */
static inline uint32_t cbp_active_families(const struct cbp_client *clients, uint32_t n, uint32_t mux)
{
    uint32_t mask = 0;
    for (uint32_t i = 0; i < n; i++)
        if (clients[i].mux == mux && clients[i].active) mask |= cbp_family_bit(clients[i].family);
    return mask;
}
#endif
