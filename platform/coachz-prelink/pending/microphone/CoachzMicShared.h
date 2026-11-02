/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Pending extension; not part of the active CoachzAudio ABI. */
#ifndef COACHZ_MIC_SHARED_H
#define COACHZ_MIC_SHARED_H
#include <stdint.h>

#define COACHZ_MIC_MAGIC 0x435a4d49u /* CZMI */
#define COACHZ_MIC_VERSION 1u
#define COACHZ_MIC_CONTROL_OFFSET 1024u /* before existing diagnostics at 2048 */
#define COACHZ_MIC_RATE 48000u
#define COACHZ_MIC_DMA_BYTES 262144u
#define COACHZ_MIC_DMA_FRAMES (COACHZ_MIC_DMA_BYTES / 8u)
#define COACHZ_MIC_PERIOD_BYTES (COACHZ_MIC_DMA_BYTES / 2u)

enum CoachzMicState {CoachzMicOff, CoachzMicWarming, CoachzMicRunning, CoachzMicFault};
enum CoachzMicError {
    CoachzMicOK, CoachzMicArgument, CoachzMicBusy, CoachzMicParentClock,
    CoachzMicSupply, CoachzMicClockUpdate, CoachzMicBranch, CoachzMicPosition,
    CoachzMicOverrun, CoachzMicDmaFault, CoachzMicStoppedDma
};

/* Single kernel writer. All fields naturally aligned, no packed structures.
 * Mono S16 sample time t is at ring[t % ring_frames]. [first, written) is the
 * ONLY valid interval. A new start/rebase invalidates the previous interval.
 * seq covers both metadata AND sample stores. Readers must recheck it AFTER
 * copying samples, with acquire ordering, and discard a raced copy. */
typedef struct {
    uint32_t magic, version;
    uint32_t seq, state;
    uint32_t error, generation;
    uint32_t rate, ring_frames;
    uint64_t anchor_host, first, written;
    uint64_t frames_copied;
    uint32_t source_channel, dma_current;
    uint32_t irq_periods, irq_faults;
    uint32_t peak[2], nonzero[2]; /* peaks are absolute S16 magnitudes */
} CoachzMicShared;

#endif
