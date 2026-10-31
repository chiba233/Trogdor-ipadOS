/* SPDX-License-Identifier: BSD-2-Clause */
/* The few libpayload services its USB stack needs, on top of boot/usb_read.c. */
#ifndef USB_COMPAT_LIBPAYLOAD_H
#define USB_COMPAT_LIBPAYLOAD_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <libpayload-config.h>
#include <kconfig.h>
#include <endian.h>
#include <arch/barrier.h>
#include <arch/virtual.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) MIN(a, b)
#define max(a, b) MAX(a, b)
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __unused __attribute__((unused))
#define __weak __attribute__((weak))
#define printf(...) ((void)0)
#define fatal(...) usb_read_fatal()
#define assert(x) do { if (!(x)) usb_read_fatal(); } while (0)
void usb_read_fatal(void);
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
int memcmp(const void *, const void *, size_t);
void *malloc(size_t);
void *calloc(size_t, size_t);
void *memalign(size_t, size_t);
void free(void *);
void *dma_malloc(size_t);
void *dma_memalign(size_t, size_t);
int dma_coherent(const void *);
static inline int dma_initialized(void) { return 1; }
int usb_exit(void);
void usb_poll(void);
void udelay(unsigned);
void mdelay(unsigned);
typedef long time_t;
struct timeval { time_t tv_sec; long tv_usec; };
int gettimeofday(struct timeval *, void *);
#define div_round_up(a, b) (((a) + (b) - 1) / (b))
#define htonl(x) __builtin_bswap32(x)
#define ntohl(x) __builtin_bswap32(x)
#define htonw(x) __builtin_bswap16(x)
#define ntohw(x) __builtin_bswap16(x)
static inline u32 read32(const volatile void *a) { return *(const volatile u32 *)a; }
static inline void write32(volatile void *a, u32 v) { *(volatile u32 *)a = v; }
static inline void *xzalloc(size_t n) { return calloc(1, n); }
static inline void *xmalloc(size_t n) { return malloc(n); }
#endif
