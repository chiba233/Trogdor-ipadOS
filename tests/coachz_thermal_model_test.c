/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "coachz_tsens.h"

struct fixture {
    uint32_t version, control, ready, raw[CT_SENSORS];
    unsigned reads[CT_SENSORS], total, retry;
    int fail_sensor;
};

static int
read_fixture(void *context, enum ct_block block, uint32_t offset, uint32_t *value)
{
    struct fixture *f = context;
    f->total++;
    if (block == CT_SROT) {
        assert(offset == 0 || offset == 4);
        *value = offset ? f->control : f->version;
    } else if (offset == 0xe4) {
        *value = f->ready;
    } else {
        assert(offset >= 0xa0 && offset < 0xe0 && !(offset & 3));
        unsigned i = (offset - 0xa0) / 4;
        f->reads[i]++;
        if ((int)i == f->fail_sensor) return -1;
        *value = f->raw[i];
        if (!i && f->reads[i] <= f->retry) *value &= ~CT_VALID;
    }
    return 0;
}

static struct fixture
normal(uint16_t mask)
{
    struct fixture f = {0};
    f.version = 0x20030001;
    f.control = ((uint32_t)mask << 3) | 1;
    f.ready = 1;
    f.fail_sensor = -1;
    for (unsigned i = 0; i < CT_SENSORS; ++i) f.raw[i] = CT_VALID | (300 + i);
    return f;
}

int main(void)
{
    struct ct_sample s;
    struct fixture f = normal(0x7fff);
    struct ct_reader r = {&f, read_fixture};
    int32_t peak = 123456;

    assert(ct_milli_c(CT_VALID | 425) == 42500);
    assert(ct_milli_c(CT_VALID | 0xfff) == -100);
    assert(ct_milli_c(CT_VALID | 0xf9c) == -10000);
    assert(ct_milli_c(0x800) == -204800);
    assert(ct_milli_c(0x7ff) == 204700);
    puts("PASS signed 12-bit deci-Celsius conversion");

    for (unsigned bank = 0; bank < CT_BANKS; ++bank) {
        f = normal(ct_banks[bank].mask);
        ct_collect(&r, ct_banks[bank].mask, &s);
        assert(s.valid == ct_banks[bank].mask);
        unsigned count = bank ? 10 : 15;
        assert(f.total == 3 + count);
        for (unsigned i = 0; i < CT_SENSORS; ++i) {
            assert(f.reads[i] == (i < count));
            assert(s.status[i] == (i < count ? CT_OK : CT_NOT_REQUESTED));
        }
    }
    puts("PASS both bank masks and bounded aligned reads");

    f = normal(3);
    f.ready = 8; /* TRDY does not decide: on hardware bank 0 stayed 0x8 while Linux read its temperatures. */
    ct_collect(&r, 3, &s);
    assert(s.valid == 3 && s.status[0] == CT_OK);
    f.control = 1 | (1u << 3); /* Sensor 1 disabled. */
    ct_collect(&r, 3, &s);
    assert(s.valid == 1 && s.status[1] == CT_SENSOR_DISABLED);
    puts("PASS VALID decides, not TRDY, and per-sensor enable qualification");

    f = normal(3);
    f.control &= ~1u;
    ct_collect(&r, 3, &s);
    assert(!s.valid && s.status[0] == CT_CONTROLLER_DISABLED && f.total == 2);
    f = normal(3);
    f.control |= 2;
    ct_collect(&r, 3, &s);
    assert(!s.valid && s.status[0] == CT_RESET && f.total == 2);
    f = normal(3);
    f.version = 0x10000000;
    ct_collect(&r, 3, &s);
    assert(!s.valid && s.status[0] == CT_UNSUPPORTED_VERSION && f.total == 1);
    ct_collect(NULL, 3, &s);
    assert(s.status[0] == CT_UNMAPPED && !ct_peak(&s, &peak) && peak == 123456);
    puts("PASS unavailable hardware has no temperature value");

    f = normal(3);
    f.retry = 2;
    ct_collect(&r, 3, &s);
    assert(s.valid == 3 && f.reads[0] == 3);
    f = normal(3);
    f.retry = 3;
    ct_collect(&r, 3, &s);
    assert(s.valid == 2 && s.status[0] == CT_INVALID && f.reads[0] == 3);
    f = normal(3);
    f.fail_sensor = 0;
    ct_collect(&r, 3, &s);
    assert(s.valid == 2 && s.status[0] == CT_IO_ERROR && f.reads[0] == 1);
    puts("PASS bounded retry and individual read failure");

    f = normal(3);
    f.raw[0] = CT_VALID | 0xf9c; /* -10 C */
    f.raw[1] = CT_VALID | 0xfff; /* -0.1 C; must win over -10 C, not default zero */
    ct_collect(&r, 3, &s);
    assert(ct_peak(&s, &peak) && peak == -100);
    f.raw[0] &= ~(uint32_t)CT_VALID;
    f.raw[1] &= ~(uint32_t)CT_VALID;
    ct_collect(&r, 3, &s);
    assert(!s.valid && !ct_peak(&s, &peak));
    assert(s.milli_c[0] == 0 && s.status[0] == CT_INVALID);
    puts("PASS peak uses only fresh valid readings including negative values");
    return 0;
}
