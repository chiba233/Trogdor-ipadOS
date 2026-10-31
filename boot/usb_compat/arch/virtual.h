/* SPDX-License-Identifier: BSD-2-Clause */
/* The probe runs with the MMU off: virtual is physical. */
#ifndef USB_COMPAT_VIRTUAL_H
#define USB_COMPAT_VIRTUAL_H
#define virt_to_phys(virt) ((unsigned long)(virt))
#define phys_to_virt(phys) ((void *)(unsigned long)(phys))
#define virt_to_bus(virt) virt_to_phys(virt)
#define bus_to_virt(bus) phys_to_virt(bus)
#endif
