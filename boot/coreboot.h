/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef PROBE_COREBOOT_H
#define PROBE_COREBOOT_H
#include <stddef.h>
#include <stdint.h>
#define CB_MAX_RANGES 64
#define DISPLAY_MAX_FRAME (64u*1024u*1024u)
#define COACHZ_BACKLIGHT_PORT 0x03d0c000u
struct cb_range { uint64_t base,size;uint32_t type; };
struct cb_display {
    uint64_t physical;
    uint32_t width,height,stride,bpp,orientation,bl_port,bl_polarity,bl_value;
    uint32_t ranges,table_bytes,table_entries;
    struct cb_range memory[CB_MAX_RANGES];
};
uint16_t cb_checksum(const uint8_t *,size_t);
int cb_display_parse(const uint8_t *window,size_t capacity,uint64_t window_pa,
                     uint64_t first_pa,size_t first_bytes,struct cb_display *out);
int cb_display_ram(const struct cb_display *,uint64_t,size_t);
#endif
