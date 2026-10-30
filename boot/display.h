/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef PROBE_DISPLAY_H
#define PROBE_DISPLAY_H
#include "coreboot.h"
#define DISPLAY_READS 13
struct display_state { uint64_t span,allocation;uint32_t consistent,allocated,window,conflict,ram; };
int display_analyze(const struct cb_display *,const uint32_t *,uint64_t,uint64_t,struct display_state *);
int display_read32(uint64_t,uint32_t *);
void display_discover(const void *,uint64_t,uint64_t);
#endif
