/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Read-only sampling adapted from NativeQcomTsens; see LICENSE and SOURCES.md.
 * Register facts checked against Linux's SC7180 / generic TSENS v2 path.
 */
#ifndef COACHZ_TSENS_H
#define COACHZ_TSENS_H
#include <stdint.h>

#define CT_BANKS 2u
#define CT_SENSORS 16u
#define CT_WINDOW 0x200u
#define CT_READY 1u /* TM + 0xe4, TRDY bit 0. Logged only: on hardware bank 0 stayed 0x8 and bank 1 toggled, so each sensor's VALID bit decides, as in Linux. */
#define CT_VALID (1u << 21)

struct ct_bank { uint64_t tm, srot; uint16_t mask; };
static const struct ct_bank ct_banks[CT_BANKS] = {
    {0x0c263000, 0x0c222000, 0x7fff},
    {0x0c265000, 0x0c223000, 0x03ff}
};

enum ct_status {
    CT_NOT_REQUESTED, CT_UNMAPPED, CT_IO_ERROR, CT_UNSUPPORTED_VERSION,
    CT_CONTROLLER_DISABLED, CT_RESET, CT_SENSOR_DISABLED, CT_NOT_READY,
    CT_INVALID, CT_OK
};
enum ct_block { CT_SROT, CT_TM };
struct ct_reader {
    void *context;
    /* Return zero on success. Aligned 32-bit reads only; no write callback. */
    int (*read)(void *, enum ct_block, uint32_t, uint32_t *);
};
struct ct_sample {
    uint32_t version, control, ready;
    uint16_t requested, enabled, valid;
    uint32_t raw[CT_SENSORS];
    int32_t milli_c[CT_SENSORS]; /* Usable only where status == CT_OK. */
    enum ct_status status[CT_SENSORS];
};

static inline const char *
ct_status_name(enum ct_status status)
{
    switch (status) {
    case CT_NOT_REQUESTED: return "not-requested";
    case CT_UNMAPPED: return "unmapped";
    case CT_IO_ERROR: return "read-error";
    case CT_UNSUPPORTED_VERSION: return "unsupported-version";
    case CT_CONTROLLER_DISABLED: return "controller-disabled";
    case CT_RESET: return "in-reset";
    case CT_SENSOR_DISABLED: return "sensor-disabled";
    case CT_NOT_READY: return "not-ready";
    case CT_INVALID: return "invalid";
    case CT_OK: return "valid";
    }
    return "unknown";
}

static inline void
ct_mark(struct ct_sample *sample, enum ct_status status)
{
    for (unsigned i = 0; i < CT_SENSORS; ++i)
        if (sample->requested & (1u << i)) sample->status[i] = status;
}

static inline int32_t
ct_milli_c(uint32_t raw)
{
    int32_t code = (int32_t)(raw & 0xfff);
    if (code & 0x800) code -= 0x1000;
    return code * 100;
}

/* One fresh bounded snapshot, not a calibration or initialization operation.
 * Disabled/unready sensors are not polled. Invalid data gets at most 3 reads;
 * no sleep or spin-wait for the hardware to become ready.
 */
static inline void
ct_collect(const struct ct_reader *reader, uint16_t mask, struct ct_sample *sample)
{
    *sample = (struct ct_sample){0};
    sample->requested = mask;
    ct_mark(sample, CT_UNMAPPED);
    if (!reader || !reader->read) return;
    if (reader->read(reader->context, CT_SROT, 0, &sample->version)) {
        ct_mark(sample, CT_IO_ERROR); return;
    }
    if ((sample->version >> 28) != 2) {
        ct_mark(sample, CT_UNSUPPORTED_VERSION); return;
    }
    if (reader->read(reader->context, CT_SROT, 4, &sample->control)) {
        ct_mark(sample, CT_IO_ERROR); return;
    }
    sample->enabled = (uint16_t)((sample->control >> 3) & 0xffff);
    if (!(sample->control & 1)) {
        ct_mark(sample, CT_CONTROLLER_DISABLED); return;
    }
    if (sample->control & 2) {
        ct_mark(sample, CT_RESET); return;
    }
    if (reader->read(reader->context, CT_TM, 0xe4, &sample->ready)) {
        ct_mark(sample, CT_IO_ERROR); return;
    }
    for (unsigned i = 0; i < CT_SENSORS; ++i) {
        const uint16_t bit = (uint16_t)(1u << i);
        if (!(mask & bit)) continue;
        if (!(sample->enabled & bit)) { sample->status[i] = CT_SENSOR_DISABLED; continue; }
        sample->status[i] = CT_INVALID;
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            if (reader->read(reader->context, CT_TM, 0xa0 + 4 * i, &sample->raw[i])) {
                sample->status[i] = CT_IO_ERROR; break;
            }
            if (!(sample->raw[i] & CT_VALID)) continue;
            sample->milli_c[i] = ct_milli_c(sample->raw[i]);
            sample->valid |= bit;
            sample->status[i] = CT_OK;
            break;
        }
    }
}

/* A peak over valid requested channels only. No valid channel => no value. */
static inline int
ct_peak(const struct ct_sample *sample, int32_t *milli_c)
{
    int found = 0;
    for (unsigned i = 0; i < CT_SENSORS; ++i) {
        if (!(sample->valid & sample->requested & (1u << i))) continue;
        if (!found || sample->milli_c[i] > *milli_c) *milli_c = sample->milli_c[i];
        found = 1;
    }
    return found;
}
#endif
