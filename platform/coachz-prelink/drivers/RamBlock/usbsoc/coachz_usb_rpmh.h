/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * What the USB bring-up needs from the shared RPMh client (drivers/RamBlock/mmc/coachz_rpmh_iface.h, implemented in
 * coachz-prelink-wifi.patch). Two additions, both implemented there:
 *
 *  COACHZ_CX_USB              a fifth CX consumer (usb_1 required-opps nom); COACHZ_CX_CONSUMERS becomes 5.
 *  coachz_rpmh_vreg_request   a per-consumer regulator vote with the regulator core's aggregation: the LDO is enabled
 *                             while any consumer enables it, at the highest millivolts any enabled consumer asked for,
 *                             with the device tree's initial mode (HPM) sent once, and the rpmh-regulator rules for
 *                             the VRM writes (voltage cached until the first enable, acknowledged when raised or
 *                             enabled). Needed because ldoa4 also supplies the DSI PLL, CSI, UFS and refgen: a plain
 *                             write from one driver would override what another relies on. Returns 0 on success.
 */
#ifndef COACHZ_USB_RPMH_H
#define COACHZ_USB_RPMH_H

#include <stdint.h>

enum { COACHZ_CX_USB = 4 };
enum { COACHZ_VREG_USB_HSPHY = 1, COACHZ_VREG_USB_SSPHY = 2 };

int coachz_rpmh_cx_request(unsigned consumer, unsigned level);
int coachz_rpmh_vreg_request(unsigned consumer, const char *resource, uint32_t mv, int hpm, int enable);

#endif /* COACHZ_USB_RPMH_H */
