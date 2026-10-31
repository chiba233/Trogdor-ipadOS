/* SPDX-License-Identifier: BSD-2-Clause */
/* Selects the parts of the libpayload USB stack that the read experiment uses. */
#ifndef USB_COMPAT_CONFIG_H
#define USB_COMPAT_CONFIG_H
#define CONFIG_LP_USB 1
#define CONFIG_LP_USB_XHCI 1
#define CONFIG_LP_USB_HUB 1
#define CONFIG_LP_USB_MSC 1
#define CONFIG_LP_USB_GEN_HUB 1
#define CONFIG_LP_USB_HID 0
#define CONFIG_LP_USB_EHCI 0
#define CONFIG_LP_USB_OHCI 0
#define CONFIG_LP_USB_UHCI 0
#define CONFIG_LP_USB_DWC2 0
#define CONFIG_LP_USB_PCI 0
#define CONFIG_LP_DEBUG_USB 0
#define CONFIG_LP_USB_XHCI_MTK_QUIRK 0
#define CONFIG_LP_ARCH_MIPS 0
#define CONFIG_LP_ARCH_ARM64 1
#define CONFIG_LP_ARCH_X86 0
#define CONFIG_LP_ARCH_ARM 0
#endif
