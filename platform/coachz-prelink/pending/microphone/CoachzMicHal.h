/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* User space only: keep floating-point code out of kernel headers. */
#ifndef COACHZ_MIC_HAL_H
#define COACHZ_MIC_HAL_H
#include "CoachzMicShared.h"

/* Bounded read for the existing HAL's mono Float32 input. No spin in RT code.
 * Returns 1 for a consistent copy (possibly silence), 0 for a raced copy.
 * Unsupported ABI, stopped input, expired/future samples all become silence.
 * Call only after the kernel driver and BOTH HAL hooks in INTEGRATION.md are
 * installed: this helper alone does not request or authorize capture. */
static inline int coachz_mic_read(const CoachzMicShared *s,
    const int16_t *ring, uint64_t frame, uint32_t count, float *out)
{
    if (!out) return 0;
    for (uint32_t i = 0; i < count; ++i) out[i] = 0.0f;
    if (!s || !ring || s->magic != COACHZ_MIC_MAGIC || s->version != COACHZ_MIC_VERSION) return 0;
    const uint32_t seq = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    if (seq & 1u) return 0;
    const uint32_t state = __atomic_load_n(&s->state, __ATOMIC_RELAXED);
    const uint32_t capacity = __atomic_load_n(&s->ring_frames, __ATOMIC_RELAXED);
    const uint64_t first = __atomic_load_n(&s->first, __ATOMIC_RELAXED);
    const uint64_t written = __atomic_load_n(&s->written, __ATOMIC_RELAXED);
    if (state == CoachzMicRunning && capacity && written >= first && written - first <= capacity) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t t = frame + i;
            if (t >= frame && t >= first && t < written)
                out[i] = __atomic_load_n(&ring[t % capacity], __ATOMIC_RELAXED) / 32768.0f;
        }
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (seq == __atomic_load_n(&s->seq, __ATOMIC_RELAXED)) return 1;
    for (uint32_t i = 0; i < count; ++i) out[i] = 0.0f;
    return 0;
}
#endif
