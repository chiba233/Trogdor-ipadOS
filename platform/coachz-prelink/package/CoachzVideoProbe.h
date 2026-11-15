/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Picture diagnostics shared by the kernel and decoder. No pixel values are logged. The legacy probe samples eight rows; the full
 * digest visits every visible byte of a selected picture. Neither a digest nor equal sampled rows proves source immutability. */
#ifndef COACHZ_VIDEO_PROBE_H
#define COACHZ_VIDEO_PROBE_H
#include <stdint.h>

/* End of the last visible UV row, relative to the FBD offset. Zero means the crop does not fit the NV12/P010 layout. filled_len is
 * recorded separately: it is firmware metadata, not proof that these bytes were written. */
static inline uint64_t coachz_video_visible_end(uint32_t stride, uint32_t scanlines, uint32_t left, uint32_t top,
                                              uint32_t width, uint32_t height, uint32_t sample_bytes) {
    const uint64_t y_left = (uint64_t)left * sample_bytes, y_width = (uint64_t)width * sample_bytes;
    const uint64_t uv_left = (uint64_t)(left & ~1u) * sample_bytes, uv_width = (((uint64_t)width + 1) & ~1ull) * sample_bytes;
    if ((sample_bytes != 1 && sample_bytes != 2) || !width || !height || y_left > stride || y_width > stride - y_left ||
        top > scanlines || height > scanlines - top || uv_left + uv_width > stride) return 0;
    return (uint64_t)stride * scanlines + ((uint64_t)(top / 2) + (height + 1ull) / 2 - 1) * stride + uv_left + uv_width;
}

static inline uint32_t coachz_video_probe_plane(const volatile uint8_t *base, uint32_t stride, uint32_t width, uint32_t height,
                                              uint32_t *zero_rows) {
    uint32_t hash = 2166136261u;
    *zero_rows = 0;
    if (!width || !height) return hash;
    for (uint32_t n = 0; n < 8; ++n) {
        const uint32_t row = (uint32_t)((uint64_t)(height - 1) * n / 7);
        uint8_t any = 0;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t value = base[(uint64_t)row * stride + x];
            any |= value;
            hash = (hash ^ value) * 16777619u;
        }
        if (!any) ++*zero_rows;
    }
    return hash;
}

typedef struct {
    uint64_t hash, zero_bytes;
    uint32_t zero_rows;
} CoachzVideoPlaneDigest;
typedef uint64_t coachz_video_probe_word __attribute__((__may_alias__));

/* A fixed-size fold lets the compiler keep the eight FNV steps in registers. Preserve byte order and exact zero counts. */
static inline void coachz_video_digest_word(CoachzVideoPlaneDigest *d, uint64_t word) {
    for (uint32_t b = 0; b < 8; ++b) {
        const uint8_t v = (uint8_t)(word >> (8 * b));
        d->zero_bytes += v == 0;
        d->hash = (d->hash ^ v) * UINT64_C(1099511628211);
    }
}

static inline CoachzVideoPlaneDigest coachz_video_digest_plane(const volatile uint8_t *base, uint32_t stride,
                                                              uint32_t width, uint32_t height) {
    CoachzVideoPlaneDigest d = {UINT64_C(14695981039346656037), 0, 0};
    for (uint32_t row = 0; row < height; ++row) {
        uint8_t any = 0;
        const volatile uint8_t *p = base + (uint64_t)row * stride;
        uint32_t x = 0;
        while (x < width && ((uintptr_t)(p + x) & 7u)) {
            const uint8_t v = p[x++]; any |= v;
            d.zero_bytes += v == 0; d.hash = (d.hash ^ v) * UINT64_C(1099511628211);
        }
        // Explicit temporal, aligned reads, all within the visible row. Issue independent reads before the dependent FNV chain
        // instead of waiting for that chain between each uncached word. No SIMD, non-temporal access or extra pixel read is needed.
        while (width - x >= 32) {
            const uint64_t w0 = *(const volatile coachz_video_probe_word *)(p + x);
            const uint64_t w1 = *(const volatile coachz_video_probe_word *)(p + x + 8);
            const uint64_t w2 = *(const volatile coachz_video_probe_word *)(p + x + 16);
            const uint64_t w3 = *(const volatile coachz_video_probe_word *)(p + x + 24);
            any |= (w0 | w1 | w2 | w3) != 0;
            coachz_video_digest_word(&d, w0); coachz_video_digest_word(&d, w1);
            coachz_video_digest_word(&d, w2); coachz_video_digest_word(&d, w3);
            x += 32;
        }
        while (width - x >= 8) {
            const uint64_t word = *(const volatile coachz_video_probe_word *)(p + x);
            any |= word != 0;
            coachz_video_digest_word(&d, word);
            x += 8;
        }
        while (x < width) {
            const uint8_t v = p[x++]; any |= v;
            d.zero_bytes += v == 0; d.hash = (d.hash ^ v) * UINT64_C(1099511628211);
        }
        d.zero_rows += any == 0;
    }
    return d;
}
#endif
