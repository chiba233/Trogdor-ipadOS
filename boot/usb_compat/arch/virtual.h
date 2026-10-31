/* SPDX-License-Identifier: BSD-2-Clause */
/* The probe runs with the MMU off: virtual is physical. The kernel maps a fixed pool. */
#ifndef USB_COMPAT_VIRTUAL_H
#define USB_COMPAT_VIRTUAL_H
#if COACHZ_USB_KERNEL
void *coachz_usb_p2v(unsigned long);
unsigned long coachz_usb_v2p(const void *);
#define virt_to_phys(virt) coachz_usb_v2p((const void *)(virt))
#define phys_to_virt(phys) coachz_usb_p2v((unsigned long)(phys))
#else
#define virt_to_phys(virt) ((unsigned long)(virt))
#define phys_to_virt(phys) ((void *)(unsigned long)(phys))
#endif
#define virt_to_bus(virt) virt_to_phys(virt)
#define bus_to_virt(bus) phys_to_virt(bus)
#endif
