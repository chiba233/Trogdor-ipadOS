/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "coachz_motion.h"

struct fixture {
    uint32_t versions, calls, data_reads;
    int fail_data, short_data;
    int32_t odr[2];
    uint8_t data_flags;
};

static void put32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i));
}
static void put16(uint8_t *p, int n) {
    p[0] = (uint8_t)n; p[1] = (uint8_t)((uint16_t)n >> 8);
}

static int command(void *context, uint16_t cmd, uint8_t version, const void *arguments,
    uint32_t size, void *response, uint32_t room, uint32_t *length) {
    struct fixture *f = context;
    const uint8_t *a = arguments;
    uint8_t *r = response;
    assert(room == CM_REPLY);
    memset(r, 0, room);
    ++f->calls;
    if (cmd == 0x0008) {
        assert(!version && size == 1 && a[0] == CM_COMMAND);
        put32(r, f->versions); *length = 4; return 0;
    }
    assert(cmd == CM_COMMAND && version == 1 && size == 13);
    for (unsigned i = 2; i < size; ++i) {
        int query = (a[0] == CM_ODR || a[0] == CM_RANGE) && i >= 5 && i < 9;
        assert(a[i] == (query ? 0xff : 0)); // No accidental setter or nonzero padding.
    }
    if (a[0] == CM_DUMP) {
        assert(a[1] == 0); // Count only, never request a FIFO or DUMP vector batch.
        r[0] = 1; r[1] = 2; *length = 2; return 0;
    }
    assert(a[1] < 2);
    unsigned id = a[1];
    switch (a[0]) {
    case CM_INFO:
        // Reverse the usual indices: the client must use INFO, not index guessing.
        r[0] = id ? CM_ACCEL : CM_GYRO; r[1] = CM_LID; r[2] = CM_BMI260;
        *length = 3; return 0;
    case CM_ODR: put32(r, (uint32_t)f->odr[id]); *length = 4; return 0;
    case CM_RANGE: put32(r, id ? 4 : 1000); *length = 4; return 0;
    case CM_DATA:
        ++f->data_reads;
        if (f->fail_data) return 7 | (3 << 8); // Shared transport's EC result error encoding.
        r[0] = f->data_flags; r[1] = 0; // Actual handler doesn't fill sensor_num, even for id 1.
        put16(r + 2, id ? 8192 : -16384);
        put16(r + 4, id ? -8192 : 32767);
        put16(r + 6, id ? -32768 : 0);
        *length = f->short_data ? 7 : 8; return 0;
    default: assert(!"unexpected or mutating subcommand"); return -99;
    }
}

int main(void) {
    struct fixture f = {0};
    f.versions = 0x1e; f.odr[0] = 0; f.odr[1] = 10000;
    const struct cm_bus bus = {&f, command};
    struct cm_topology topology;
    struct cm_sample sample;
    assert(!cm_discover(&bus, &topology));
    assert(topology.scanned == 2 && topology.reported == 2);
    assert(topology.sensors[0].type == CM_GYRO && topology.sensors[1].type == CM_ACCEL);
    assert(cm_supported(&topology.sensors[0]) && cm_supported(&topology.sensors[1]));
    puts("PASS supported command version and INFO-based discovery");

    cm_collect(&bus, &topology.sensors[1], &sample);
    assert(sample.status == CM_AVAILABLE && sample.range == 4 && sample.odr_millihz == 10000);
    assert(sample.raw[0] == 8192 && sample.raw[1] == -8192 && sample.raw[2] == -32768);
    assert(sample.scaled[0] == 1000000 && sample.scaled[1] == -1000000 && sample.scaled[2] == -4000000);
    puts("PASS zero DATA flags and ignored unfilled echo with signed acceleration conversion");

    unsigned reads = f.data_reads;
    cm_collect(&bus, &topology.sensors[0], &sample);
    assert(sample.status == CM_INACTIVE && f.data_reads == reads);
    f.odr[0] = 25000;
    cm_collect(&bus, &topology.sensors[0], &sample);
    assert(sample.status == CM_AVAILABLE && sample.scaled[0] == -500000000);
    assert(sample.scaled[1] == 999969482 && sample.scaled[2] == 0);
    puts("PASS inactive gyro is not enabled; active gyro uses queried range and dps units");

    f.fail_data = 1;
    cm_collect(&bus, &topology.sensors[1], &sample);
    assert(sample.status == CM_ERROR && sample.error == (7 | (3 << 8)));
    assert(!sample.raw[0] && !sample.scaled[0]);
    f.fail_data = 0; f.short_data = 1;
    cm_collect(&bus, &topology.sensors[1], &sample);
    assert(sample.status == CM_ERROR && sample.error == CM_SHORT);
    f.short_data = 0; f.data_flags = 2;
    cm_collect(&bus, &topology.sensors[1], &sample);
    assert(sample.status == CM_ERROR && sample.error == CM_FRAME);
    puts("PASS errors and non-vector replies never reuse prior axes");

    topology.sensors[1].chip = 99;
    unsigned calls = f.calls;
    cm_collect(&bus, &topology.sensors[1], &sample);
    assert(sample.status == CM_UNSUPPORTED && f.calls == calls);
    f.versions = 1u << 4;
    assert(cm_discover(&bus, &topology) == CM_VERSION);
    puts("PASS unknown chip and unsupported version are explicit, with no guessed ABI");
    puts("PASS every range/ODR request uses NO_VALUE and no shared event is consumed");
    return 0;
}
