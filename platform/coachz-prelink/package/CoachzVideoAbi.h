/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The interface between the video decode service (CoachzVenus.cpp, IOKit class CoachzVenus) and the VideoToolbox decoder that iOS loads
 * from /System/Library/VideoDecoders/AVD.videodecoder (CoachzVideoDecoder.c).
 *
 * The kernel runs the whole stateful decoder as mainline Linux's vdec.c does (session, sequence change, DPB, capture buffers); a client
 * only feeds bitstream and takes decoded NV12 or negotiated P010 pictures. One user client is one decoder session.
 *
 * Memory: type CoachzVideoMemoryInput + i maps input buffer i (Annex B bitstream, written by the client), type
 * CoachzVideoMemoryOutput + i maps output buffer i of the current output generation (linear NV12/P010: the luma plane at 0 with `stride` bytes
 * a line and `scanlines` lines, the interleaved CbCr plane right after it, `stride` bytes a line), read by the client after a frame event
 * names it. Both are write-combined: write input sequentially, read output in long runs.
 *
 * Methods (scalars in, events out):
 *   Create(codec, width, height[, caps])    -> structure out CoachzVideoInfo: the input buffers. The session is started (vdec_start_output).
 *   QueueInput(index, bytes, token, flags)   an input buffer with `bytes` of bitstream; `token` comes back with the picture decoded from it.
 *   ReturnOutput(generation, index)          gives a delivered output buffer back to the decoder.
 *   Drain()                                  the EOS buffer (vdec_decoder_cmd STOP); a CoachzVideoEventDrained follows the last picture.
 *   Flush()                                  drops queued input and pictures (vdec_stop_output's HFI_FLUSH_ALL); CoachzVideoEventFlushed after.
 *   WaitEvent(timeout_ms)                    -> structure out: up to COACHZ_VIDEO_EVENTS_PER_WAIT CoachzVideoEvent, kIOReturnTimeout if none.
 *   Destroy()                                the session is released (vdec_session_release); closing the client does the same.
 *   SetFrameRate(fps)                         media-window rate rounded to an integer in 1..480, for clock/bandwidth scaling.
 *   AuditMemory(address, bytes)              -> CoachzVideoMapAudit: read-only page metadata from this client's task, bounded to 64 MiB.
 *                                            No wiring, faults, cache maintenance or physical addresses are returned. */
#ifndef COACHZ_VIDEO_ABI_H
#define COACHZ_VIDEO_ABI_H
#include <stdint.h>

#define COACHZ_VIDEO_CLASS "CoachzVenus"
#define COACHZ_VIDEO_VERSION 1u
#define COACHZ_VIDEO_AUDIT_VERSION 1u
#define COACHZ_VIDEO_MAX_INPUTS 16u
#define COACHZ_VIDEO_MAX_OUTPUTS 32u
#define COACHZ_VIDEO_EVENTS_PER_WAIT 32u

enum {CoachzVideoMemoryInput = 0x100, CoachzVideoMemoryOutput = 0x200};
enum {CoachzVideoCreate = 0, CoachzVideoQueueInput = 1, CoachzVideoReturnOutput = 2, CoachzVideoDrain = 3, CoachzVideoFlush = 4,
      CoachzVideoWaitEvent = 5, CoachzVideoDestroy = 6, CoachzVideoSetFrameRate = 7, CoachzVideoAuditMemory = 8};
enum {CoachzVideoClientP010 = 1u << 0}; /* optional Create caps; a three-scalar client remains NV12-only */
enum {CoachzVideoOutputP010 = 1u << 8}; /* Outputs.flags: stride is bytes, scanlines is rows; 16-bit samples, ten MSBs used */
/* HFI codec numbers (hfi_helper.h HFI_VIDEO_CODEC_*). */
enum {CoachzVideoH264 = 0x2, CoachzVideoVP8 = 0x1000, CoachzVideoHEVC = 0x2000, CoachzVideoVP9 = 0x4000};
/* QueueInput flags. */
enum {CoachzVideoInputEndOfFrame = 1u << 0, CoachzVideoInputCodecConfig = 1u << 1};

enum {
    CoachzVideoEventInputDone = 1,      /* index: the input buffer is free again; status: the firmware's error for it */
    CoachzVideoEventOutputs = 2,        /* a new output generation: count, bytes, stride, scanlines, width, height, crop */
    CoachzVideoEventFrame = 3,          /* index of the current generation, token, crop, flags; status nonzero: decode error */
    CoachzVideoEventDrained = 4,        /* every picture before the Drain has been delivered */
    CoachzVideoEventFlushed = 5,
    CoachzVideoEventError = 6,          /* the session failed (status: HFI error); every later call fails */
};
/* Frame flags. */
enum {CoachzVideoFrameKey = 1u << 0, CoachzVideoFrameCorrupt = 1u << 1, CoachzVideoFrameDropped = 1u << 2,
      CoachzVideoFrameNoToken = 1u << 3, CoachzVideoFrameSample = 1u << 4,
      CoachzVideoEventTiming = 1u << 5}; /* Frame/InputDone: the otherwise unused Outputs fields carry timing below. */

typedef struct {
    uint32_t version;
    uint32_t input_count, input_bytes;
    uint32_t session_id; /* opaque diagnostic identity; formerly reserved, layout unchanged */
} CoachzVideoInfo;

typedef struct {
    /* attribute_mask bit n means PP cache metadata value n; bit 31 includes every value >= 31. It does not encode PTE alias types. */
    uint32_t version, pages, unmapped, unmanaged, changed, attribute_mask;
} CoachzVideoMapAudit;

typedef struct {
    uint32_t kind, index, generation, status;
    uint64_t token;
    uint32_t flags, filled, offset;
    uint32_t count, bytes, stride, scanlines;   /* Outputs. Timing Frame/InputDone: count|bytes<<32 = message ticks,
                                               * stride|scanlines<<32 = ETB command-begin ticks (0 when association unavailable).
                                               * Same absolute timebase as mach_absolute_time. These include software queueing,
                                               * HFI transport and reordering; they are not hardware-only decode times. */
    uint32_t width, height;                     /* coded size */
    uint32_t crop_left, crop_top, crop_width, crop_height;
} CoachzVideoEvent;

#endif
