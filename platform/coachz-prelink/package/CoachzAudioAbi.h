/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Adapted from UniXNU-sheng package/ShengAudioAbi.h, revision 08cde20 (BSD-2-Clause-Patent, Copyright (c) 2026, UniXNU contributors).
 * The interface between the Coachz audio service (CoachzAudio.cpp, IOKit class CoachzAudio) and the audio server plug-in that iOS loads
 * (CoachzAudioPlugIn.c). The plug-in opens the service, maps three memory types and starts and stops each direction.
 *
 * Memory type 0 is the control page; 1 the playback ring; 2 the capture ring. Rings hold 16-bit little-endian interleaved samples,
 * ring_frames frames each. Sample time t lives at frame t % ring_frames. Sample time 0 is the frame LPASS DMA plays at host time
 * anchor_host, and one frame lasts ticks_per_frame_q32 / 2^32 host ticks. The DMA runs from the moment the service starts, so one
 * timeline holds for the whole boot, and the service clears each frame once it has played. The plug-in writes at least lead_frames
 * ahead of the play position. */
#ifndef COACHZ_AUDIO_ABI_H
#define COACHZ_AUDIO_ABI_H
#include <stdint.h>

#define COACHZ_AUDIO_CLASS "CoachzAudio"
#define COACHZ_AUDIO_MAGIC 0x435a4155u        /* 'CZAU' */
#define COACHZ_AUDIO_VERSION 1u
#define COACHZ_AUDIO_RATE 48000u
#define COACHZ_AUDIO_OUT_CHANNELS 2u
#define COACHZ_AUDIO_IN_CHANNELS 1u
#define COACHZ_AUDIO_PERIOD_FRAMES 480u        /* 10 ms */
#define COACHZ_AUDIO_RING_FRAMES 65536u        /* 1.37 s: room for the 300 ms lead and the audio server's late IO cycles */

enum {CoachzAudioMemoryControl = 0, CoachzAudioMemoryOutput = 1, CoachzAudioMemoryInput = 2};
enum {CoachzAudioStartOutput = 0, CoachzAudioStopOutput = 1, CoachzAudioStartInput = 2, CoachzAudioStopInput = 3};
/* Scalar input: the address of CoreAudio's host clock frequency constant in the caller (24 MHz, the timer of Apple's chips); the service
 * sets it to this board's timer frequency. */
enum {CoachzAudioSetHostClock = 4};
enum {CoachzAudioOutputRunning = 1u << 0, CoachzAudioInputRunning = 1u << 1, CoachzAudioOutputReady = 1u << 2, CoachzAudioInputReady = 1u << 3};

typedef struct {
    uint32_t magic, version;
    uint32_t rate, out_channels, in_channels, period_frames, ring_frames, lead_frames;
    volatile uint32_t state;                 /* CoachzAudio*Running / *Ready bits */
    uint32_t reserved0;
    uint64_t ticks_per_frame_q32;            /* host ticks per frame, 32.32 fixed point */
    volatile uint64_t out_anchor_host, in_anchor_host;
    volatile uint64_t out_frames, in_frames; /* frames the service consumed / produced so far */
    volatile uint32_t out_late, in_late;     /* periods the service had to send or deliver without fresh data */
    volatile uint32_t out_seq, in_seq;       /* incremented around anchor updates (odd while writing) */
    volatile uint64_t out_written;           /* the plug-in: the sample time after the last frame it wrote */
    volatile uint64_t in_written;            /* the service: the sample time after the last captured frame in the ring */
    volatile uint64_t out_written_host;      /* the plug-in: host time of its last write */
    volatile uint32_t out_holes;             /* the service: written periods that held a run of 64 or more silent frames */
} CoachzAudioControl;

/* Second half of the control page: the plug-in records what the audio server asked for and it does not answer (selector, object id
 * << 24 | scope fourcc's first byte), so the kernel log names them without the unified log. */
#define COACHZ_AUDIO_DIAG_OFFSET 2048u
#define COACHZ_AUDIO_DIAG_SLOTS 32u
typedef struct {
    volatile uint32_t count, loads;
    volatile uint32_t selector[COACHZ_AUDIO_DIAG_SLOTS], where[COACHZ_AUDIO_DIAG_SLOTS];
    volatile uint32_t volume_milli, volume_sets;  /* the output volume (scalar x 1000) and how often the audio server set it */
    volatile uint32_t mix_peak;                   /* the largest output sample before the volume, x 32767 */
    volatile uint32_t in_misses;                  /* input reads that asked for frames the service had not captured yet */
    /* How far ahead of the play position the audio server writes (output time minus now, frames) and how far behind it reads, the
     * least and most of each, and the frames of the last write. */
    volatile int32_t ahead_min, ahead_max, behind_min, behind_max;
    volatile uint32_t io_frames, writes;
    /* Writes that began inside frames already written (another IO context of the audio server mixing into the same ring), and the most
     * frames such a write went back. */
    volatile uint32_t overlaps, overlap_max;
    /* Every frame written; writes that began past the end of the one before (a skipped stretch) and the frames skipped; the time from
     * one write to the next in buckets below 15, 25, 40, 80 and 160 ms and above (one IO cycle of 1024 frames is 21.3 ms). */
    volatile uint32_t frames_total, gaps, gap_frames;
    volatile uint32_t interval[6];
    volatile uint64_t last_end, last_host;
    volatile int32_t skew_min, skew_max;          /* the audio server's current sample time minus the timeline's, least and most */
    /* The audio server's IO thread as the plug-in sees it: current and base priority, scheduling policy, whether it has a time constraint
     * and that constraint's period, computation and constraint (host ticks); the lowest current priority seen. */
    volatile int32_t io_pri, io_base, io_policy, io_tc, io_pri_min;
    volatile uint32_t io_period, io_computation, io_constraint;
    /* CoreAudio's host clock frequency (Hz) after the plug-in asked the service to set it, and that request's result (0 done, -1 the
     * constant was not found, else the kernel's return). */
    volatile uint32_t host_clock;
    volatile int32_t host_clock_result;
} CoachzAudioDiag;
/* After it, every distinct property access in the order the audio server made it: selector, then object << 24 | scope's first byte << 16 |
 * kind (1 set, 0 get) | 2 if it failed. */
#define COACHZ_AUDIO_TRACE_OFFSET 2560u
#define COACHZ_AUDIO_TRACE_SLOTS 128u
typedef struct {
    volatile uint32_t count;
    volatile uint32_t entry[COACHZ_AUDIO_TRACE_SLOTS][2];
} CoachzAudioTrace;

#endif
