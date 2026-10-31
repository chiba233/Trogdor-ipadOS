/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef USB_COMPAT_KCONFIG_H
#define USB_COMPAT_KCONFIG_H
#include <libpayload-config.h>
#define CONFIG(option) CONFIG_##option
#endif
