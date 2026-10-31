/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * Optional SC7180 TSENS sampler. Not enabled in the active kernel build.
 * See LICENSE for NativeQcomTsens attribution and SOURCES.md for differences.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include "coachz_tsens.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
void coachz_thermal_start(void);
void coachz_thermal_poll(void);

struct ct_mapping { uintptr_t srot, tm; };
static struct ct_mapping mappings[CT_BANKS];
static int active;

static int
thermal_read(void *context, enum ct_block block, uint32_t offset, uint32_t *value)
{
    const struct ct_mapping *map = context;
    const uintptr_t base = block == CT_SROT ? map->srot : map->tm;
    const uint32_t limit = block == CT_SROT ? 8 : CT_WINDOW;
    if (!base || (offset & 3) || offset > limit - 4) return -1;
    *value = *(volatile uint32_t *)(base + offset);
    __asm__ volatile ("dmb oshld" ::: "memory");
    return 0;
}

static void
thermal_report(int detail)
{
    for (unsigned bank = 0; bank < CT_BANKS; ++bank) {
        const struct ct_reader reader = {&mappings[bank], thermal_read};
        struct ct_sample sample;
        int32_t peak = 0;
        ct_collect(mappings[bank].srot && mappings[bank].tm ? &reader : NULL,
            ct_banks[bank].mask, &sample);
        if (ct_peak(&sample, &peak)) {
            printf("COACHZ thermal: bank %u version 0x%x control 0x%x ready 0x%x requested 0x%x enabled 0x%x valid 0x%x peak %d mC\n",
                bank, sample.version, sample.control, sample.ready, (unsigned)sample.requested,
                (unsigned)sample.enabled, (unsigned)sample.valid, peak);
        } else {
            printf("COACHZ thermal: bank %u version 0x%x control 0x%x ready 0x%x requested 0x%x enabled 0x%x no-valid-temperature\n",
                bank, sample.version, sample.control, sample.ready,
                (unsigned)sample.requested, (unsigned)sample.enabled);
        }
        /* The first report explains every channel; periodic reports stay short. */
        if (detail) for (unsigned sensor = 0; sensor < CT_SENSORS; ++sensor) {
            if (!(sample.requested & (1u << sensor))) continue;
            if (sample.status[sensor] == CT_OK)
                printf("COACHZ thermal: bank %u sensor %u raw 0x%x temperature %d mC\n",
                    bank, sensor, sample.raw[sensor], sample.milli_c[sensor]);
            else
                printf("COACHZ thermal: bank %u sensor %u %s raw 0x%x\n",
                    bank, sensor, ct_status_name(sample.status[sensor]), sample.raw[sensor]);
        }
    }
}

/* Both entry points run only on coachz_sample_thread, with no second thread. */
void
coachz_thermal_start(void)
{
    static int started;
    unsigned mode = 0;
    if (started || !PE_parse_boot_argn("coachz-thermal", &mode, sizeof(mode)) || mode != 1) return;
    started = 1;
    DTEntry node;
    const void *property = NULL;
    unsigned size = 0;
    const char compatible[] = "google,coachz-tsens-v2";
    if (SecureDTLookupEntry(NULL, "/arm-io/coachz-thermal", &node) != kSuccess ||
        SecureDTGetProperty(node, "compatible", &property, &size) != kSuccess ||
        size != sizeof(compatible) || memcmp(property, compatible, sizeof(compatible))) {
        printf("COACHZ thermal: explicit Coachz ADT node missing, not mapped\n"); return;
    }
    /* Physical TM/SROT tuples, little-endian u64 pairs. Only these fixed banks
     * are supported; an ADT cannot turn this sampler into arbitrary MMIO.
     */
    if (SecureDTGetProperty(node, "reg", &property, &size) != kSuccess || size != 64) {
        printf("COACHZ thermal: reg must contain four base/size pairs\n"); return;
    }
    uint64_t ranges[8];
    memcpy(ranges, property, sizeof(ranges));
    for (unsigned bank = 0; bank < CT_BANKS; ++bank) {
        if (ranges[4 * bank] != ct_banks[bank].tm || ranges[4 * bank + 1] != CT_WINDOW ||
            ranges[4 * bank + 2] != ct_banks[bank].srot || ranges[4 * bank + 3] != CT_WINDOW) {
            printf("COACHZ thermal: bank %u differs from SC7180 layout, not mapped\n", bank); return;
        }
    }
    for (unsigned bank = 0; bank < CT_BANKS; ++bank) {
        mappings[bank].srot = (uintptr_t)ml_io_map((vm_offset_t)ct_banks[bank].srot, 8);
        mappings[bank].tm = (uintptr_t)ml_io_map((vm_offset_t)ct_banks[bank].tm, CT_WINDOW);
    }
    /* Small uncached mappings retained for the kernel lifetime. No register
     * writes, controller reset, clock votes, calibration or interrupt changes.
     */
    active = 1;
    thermal_report(1);
}

void
coachz_thermal_poll(void)
{
    if (active) thermal_report(0);
}
