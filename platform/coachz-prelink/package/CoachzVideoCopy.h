/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The decoder reads firmware-owned DMA memory after FBD and writes a locked pixel buffer. Use temporal, explicit accesses here:
 * the pinned libsystem_platform memmove uses LDNP for these row sizes. k211 observed three source/destination hash mismatches before
 * frame emission. This makes the transfer boundary explicit; it does not establish which cache/order mechanism caused that run. */
#ifndef COACHZ_VIDEO_COPY_H
#define COACHZ_VIDEO_COPY_H
#include <stddef.h>
#include <stdint.h>

typedef uint64_t coachz_video_copy_word __attribute__((__may_alias__));

static inline void coachz_video_copy_acquire(void) {
#if defined(__aarch64__)
    __asm__ volatile("dmb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static inline void coachz_video_copy_publish(void) {
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
}

/* No libc call, non-temporal access, overread, or padding write. Different source/destination alignments use byte accesses. */
static inline void coachz_video_copy_row(volatile uint8_t *dst, const volatile uint8_t *src, size_t bytes) {
    while (bytes && (((uintptr_t)src | (uintptr_t)dst) & 7u)) {
        *dst++ = *src++;
        --bytes;
    }
    while (bytes >= sizeof(coachz_video_copy_word)) {
        *(volatile coachz_video_copy_word *)dst = *(const volatile coachz_video_copy_word *)src;
        dst += sizeof(coachz_video_copy_word);
        src += sizeof(coachz_video_copy_word);
        bytes -= sizeof(coachz_video_copy_word);
    }
    while (bytes--) *dst++ = *src++;
}

typedef struct {
    uint64_t bytes, zero_bytes;
    uint32_t rows, first_row, first_byte;
} CoachzVideoCopyDiff;

/* Count differing bytes from values already read. No pixel access or early termination on mismatches. */
static inline int coachz_video_compare_word(CoachzVideoCopyDiff *diff, uint64_t sv, uint64_t dv,
                                            uint32_t bytes, uint32_t row, uint32_t x) {
    if (sv == dv) return 0;
    for (uint32_t b = 0; b < bytes; ++b) {
        if ((uint8_t)(sv >> (8 * b)) == (uint8_t)(dv >> (8 * b))) continue;
        if (!diff->bytes) { diff->first_row = row; diff->first_byte = x + b; }
        ++diff->bytes;
        if (!(uint8_t)(dv >> (8 * b))) ++diff->zero_bytes;
    }
    return 1;
}

/* Inspect every visible byte on sampled pictures, without logging pixel values. Load four aligned words from each view before
 * branching on equality, instead of serializing every uncached eight-byte read behind the preceding comparison. All observations,
 * byte counts and first-difference order remain present; the transfer functions above are unchanged. */
static inline CoachzVideoCopyDiff coachz_video_compare_plane(const volatile uint8_t *src, size_t src_stride,
                                                            const volatile uint8_t *dst, size_t dst_stride,
                                                            uint32_t width_bytes, uint32_t height) {
    CoachzVideoCopyDiff diff = {0, 0, 0, UINT32_MAX, UINT32_MAX};
    for (uint32_t row = 0; row < height; ++row) {
        const volatile uint8_t *s = src + (size_t)row * src_stride, *d = dst + (size_t)row * dst_stride;
        uint32_t x = 0;
        int different = 0;
        while (x < width_bytes && (((uintptr_t)(s + x) | (uintptr_t)(d + x)) & 7u)) {
            const uint64_t sv = s[x], dv = d[x];
            different |= coachz_video_compare_word(&diff, sv, dv, 1, row, x++);
        }
        while (width_bytes - x >= 32) {
            const uint64_t s0 = *(const volatile coachz_video_copy_word *)(s + x);
            const uint64_t s1 = *(const volatile coachz_video_copy_word *)(s + x + 8);
            const uint64_t s2 = *(const volatile coachz_video_copy_word *)(s + x + 16);
            const uint64_t s3 = *(const volatile coachz_video_copy_word *)(s + x + 24);
            const uint64_t d0 = *(const volatile coachz_video_copy_word *)(d + x);
            const uint64_t d1 = *(const volatile coachz_video_copy_word *)(d + x + 8);
            const uint64_t d2 = *(const volatile coachz_video_copy_word *)(d + x + 16);
            const uint64_t d3 = *(const volatile coachz_video_copy_word *)(d + x + 24);
            if ((s0 ^ d0) | (s1 ^ d1) | (s2 ^ d2) | (s3 ^ d3)) {
                different |= coachz_video_compare_word(&diff, s0, d0, 8, row, x);
                different |= coachz_video_compare_word(&diff, s1, d1, 8, row, x + 8);
                different |= coachz_video_compare_word(&diff, s2, d2, 8, row, x + 16);
                different |= coachz_video_compare_word(&diff, s3, d3, 8, row, x + 24);
            }
            x += 32;
        }
        while (width_bytes - x >= 8) {
            const uint64_t sv = *(const volatile coachz_video_copy_word *)(s + x);
            const uint64_t dv = *(const volatile coachz_video_copy_word *)(d + x);
            different |= coachz_video_compare_word(&diff, sv, dv, 8, row, x);
            x += 8;
        }
        while (x < width_bytes) {
            const uint64_t sv = s[x], dv = d[x];
            different |= coachz_video_compare_word(&diff, sv, dv, 1, row, x++);
        }
        diff.rows += (uint32_t)different;
    }
    return diff;
}
#endif
