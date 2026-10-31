/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef PROBE_DISPLAY_H
#define PROBE_DISPLAY_H
#include "coreboot.h"
#define DISPLAY_READS 13
struct display_state { uint64_t span,allocation;uint32_t consistent,allocated,window,conflict,ram; };
int display_analyze(const struct cb_display *,const uint32_t *,uint64_t,uint64_t,struct display_state *);
int display_read32(uint64_t,uint32_t *);
void display_discover(const void *,uint64_t,uint64_t);
struct display_video { uint64_t pa,bytes;uint32_t width,height,stride,ready; };
extern struct display_video display_video;
int display_ec_hello(uint64_t,uint32_t *);
int display_ec_backlight(uint64_t,uint16_t,uint32_t *);
#endif
