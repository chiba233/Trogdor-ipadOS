/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The shared RPMh client the SDHCI driver calls. It is NOT implemented in this directory: the apps RSC (DRV2) active
 * TCS and the cmd-db lookup already exist in coachz-prelink-wifi.patch (wifi_rpmh_send, wifi_db_find), and one owner
 * must serialize every driver's TCS use. Whoever integrates this driver implements these four functions once, over
 * that code, for all Coachz drivers (Wi-Fi, SDHCI, later GPU/USB). Semantics follow Linux 7.4-rc1:
 *
 *  coachz_cmd_db_address   cmd_db_read_addr(): the RPMh address of a resource name ("ldoa19", "cx.lvl"), 0 if absent.
 *  coachz_cmd_db_aux       cmd_db_read_aux_data(): the aux bytes of a resource (the ARC level table for "cx.lvl").
 *  coachz_rpmh_write       rpmh_write() (wait != 0) or rpmh_write_async() in RPMH_ACTIVE_ONLY_STATE: one TCS batch of
 *                          (address, data) pairs. Returns 0 on success.
 *  coachz_rpmh_cx_request  rpmhpd_set_performance_state() on "cx": the vote must be the maximum over every consumer,
 *                          because the apps DRV holds one value per resource and a lower write would drop the level the
 *                          display, USB or GPU rely on. level is an RPMH_REGULATOR_LEVEL_* value (64 low SVS, 256 nom);
 *                          0 withdraws this consumer. Returns 0 on success.
 */
#ifndef COACHZ_RPMH_IFACE_H
#define COACHZ_RPMH_IFACE_H

#include <stdint.h>

/* 3 is the modem's proxy vote and 4 usb_1 (usbsoc/coachz_usb_rpmh.h). */
enum { COACHZ_CX_SDCC1 = 1, COACHZ_CX_SDCC2 = 2 };

uint32_t coachz_cmd_db_address(const char *resource);
int coachz_cmd_db_aux(const char *resource, const uint8_t **aux, uint32_t *bytes);
int coachz_rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count, int wait);
int coachz_rpmh_cx_request(unsigned consumer, unsigned level);

#endif /* COACHZ_RPMH_IFACE_H */
