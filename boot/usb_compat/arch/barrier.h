/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef USB_COMPAT_BARRIER_H
#define USB_COMPAT_BARRIER_H
#define mb() __asm__ volatile("dsb sy" ::: "memory")
#define rmb() mb()
#define wmb() mb()
#endif
