// SPDX-License-Identifier: BSD-2-Clause-Patent
// The VideoToolbox decoder for the Venus video codec, built as /System/Library/VideoDecoders/AVD.videodecoder: VideoToolbox's
// VTLoadVideoDecoders dlopens that path (the shared cache has no image for it) and calls AVDRegister, which registers the codec types
// here with VTRegisterVideoDecoder, as H264H8Register does.
//
// The decoder object is a CoreMedia derived object of VideoToolbox's decoder class, laid out as the 21H461 cache's own decoders are
// (kParavirtualizedVideoDecoder_BaseClass / _VideoDecoderClass, H264H8's _kH264VideoDecoderVTable): base class version 1, decoder
// class version 3 with StartSession(decoder, session, format), DecodeFrame(decoder, frame, sample, flags, info out),
// CopySupportedPropertyDictionary, SetProperties, CopySerializableProperties, CanAcceptFormatDescription, FinishDelayedFrames,
// StartTileSession, DecodeTile, FinishDelayedTiles. VTDecoderSessionEmitDecodedFrame(session, frame, status, info, image) hands each
// picture back. The macOS SDK has no header for any of this; the signatures are read from those callers and callees.
//
// The kernel (CoachzVenus.cpp, CoachzVideoAbi.h) runs the whole decoder; this side turns length-prefixed NAL units into Annex B (the
// parameter sets before the first picture and after a flush), queues each sample with a token, and copies each decoded NV12/P010 picture
// into a pixel buffer of the session's pool.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <time.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <os/log.h>
#include "CoachzVideoAbi.h"
#include "CoachzVideoProbe.h"
#include "CoachzVideoCopy.h"

typedef struct OpaqueVTVideoDecoderSession *VTVideoDecoderSession;
typedef struct OpaqueVTVideoDecoderFrame *VTVideoDecoderFrame;
typedef CFTypeRef VTVideoDecoderRef;

typedef struct {
    uint32_t version;
    size_t derivedStorageSize;
    Boolean (*equal)(CFTypeRef, CFTypeRef);
    OSStatus (*invalidate)(CFTypeRef);
    void (*finalize)(CFTypeRef);
    CFStringRef (*copyDebugDescription)(CFTypeRef);
    OSStatus (*copyProperty)(CFTypeRef, CFStringRef, CFAllocatorRef, void *);
    OSStatus (*setProperty)(CFTypeRef, CFStringRef, CFTypeRef);
    void *reserved[6];
} DecoderBaseClass;
typedef struct {
    uint32_t version;
    OSStatus (*startSession)(VTVideoDecoderRef, VTVideoDecoderSession, CMVideoFormatDescriptionRef);
    OSStatus (*decodeFrame)(VTVideoDecoderRef, VTVideoDecoderFrame, CMSampleBufferRef, VTDecodeFrameFlags, VTDecodeInfoFlags *);
    OSStatus (*copySupportedPropertyDictionary)(VTVideoDecoderRef, CFDictionaryRef *);
    OSStatus (*setProperties)(VTVideoDecoderRef, CFDictionaryRef);
    OSStatus (*copySerializableProperties)(VTVideoDecoderRef, CFAllocatorRef, CFDictionaryRef *);
    Boolean (*canAcceptFormatDescription)(VTVideoDecoderRef, CMFormatDescriptionRef);
    OSStatus (*finishDelayedFrames)(VTVideoDecoderRef);
    void *startTileSession, *decodeTile, *finishDelayedTiles;
} DecoderClass;
typedef struct {
    const void *reserved;
    const DecoderBaseClass *base;
    const DecoderClass *decoder;
    const void *more;
} DecoderVTable;

extern OSStatus VTRegisterVideoDecoder(FourCharCode, OSStatus (*)(FourCharCode, CFAllocatorRef, VTVideoDecoderRef *));
extern const void *VTVideoDecoderGetClassID(void);
extern OSStatus CMDerivedObjectCreate(CFAllocatorRef, const DecoderVTable *, const void *, CFTypeRef *);
extern void *CMBaseObjectGetDerivedStorage(CFTypeRef);
extern OSStatus VTDecoderSessionSetPixelBufferAttributes(VTVideoDecoderSession, CFDictionaryRef);
extern CVPixelBufferPoolRef VTDecoderSessionGetPixelBufferPool(VTVideoDecoderSession);
extern OSStatus VTDecoderSessionEmitDecodedFrame(VTVideoDecoderSession, VTVideoDecoderFrame, OSStatus, VTDecodeInfoFlags, CVImageBufferRef);

enum {kErrNoDecoder = -12906, kErrMalfunction = -12911, kErrBadData = -12909, kErrNotAvailable = -12783, kErrPropertyNotFound = -12782};
#define PENDING 128u

enum PerfStage {
    PerfRate, PerfBlockCopy, PerfInputWait, PerfAnnexB, PerfInputHash, PerfQueue,
    PerfEventWait, PerfFirmwareToEbd, PerfEbdToBatch, PerfEbdBatchWait, PerfInputRelease, PerfFirmwareToFbd, PerfFbdToBatch, PerfFbdBatchWait,
    PerfPool, PerfPixelLock, PerfSnapshot,
    PerfPixelCopy, PerfLockedAudit, PerfPixelUnlock, PerfAttachments, PerfReturn, PerfPending, PerfPreEmitAudit, PerfEmit,
    PerfFrame, PerfReport, PerfStages
};
typedef struct { uint64_t count, ticks, maximum; } PerfCounter;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    io_connect_t conn;
    VTVideoDecoderSession session;
    CMVideoFormatDescriptionRef format;
    FourCharCode codec;
    uint32_t hfi, width, height, sample_bytes, fps;
    int nal_len;                         // 0: the sample is raw (VP8, VP9)
    uint8_t *params; size_t params_len;  // Annex B parameter sets
    bool need_params;
    CoachzVideoInfo info;
    uint8_t *input[COACHZ_VIDEO_MAX_INPUTS];
    bool input_busy[COACHZ_VIDEO_MAX_INPUTS];
    uint32_t generation, nopb, stride, scanlines, crop_left, crop_top, crop_width, crop_height;
    mach_vm_address_t opb[COACHZ_VIDEO_MAX_OUTPUTS];
    mach_vm_size_t opb_size[COACHZ_VIDEO_MAX_OUTPUTS];
    struct {
        uint64_t token, input_hash;
        VTVideoDecoderFrame frame;
        CMTime pts, duration;
        uint32_t input_bytes;
        bool used, hidden;
    } pending[PENDING];
    uint64_t next_token;
    uint32_t drained, flushed;           // counts of the events, for waiters
    OSStatus error;
    bool thread_running, stop, invalidated, legacy_service;
    pthread_t thread;
    CFDictionaryRef attachments;
    uint8_t *scratch; size_t scratch_len;
    uint64_t frames, drops, decodes, hidden, finishes, accepts, property_calls;
    CMTime rate_duration;
    uint32_t rate_samples;
    bool rate_started;
    PerfCounter perf[PerfStages];
    uint64_t perf_frames, perf_slow_frames, perf_slow_inputs, slow_ticks;
    uint64_t perf_missing_fbd, perf_missing_ebd, perf_missing_etb, perf_invalid_timing;
    uint64_t perf_cpu_ns, perf_cpu_samples, perf_cpu_failed;
    size_t audit_bytes;                 // event-thread-owned snapshots, including pictures awaiting emit
    uint32_t decode_flags, output_changes;
} Decoder;

static os_log_t gLog;
static void note(const char *what, long value) { os_log_error(gLog, "AVD coachz: %{public}s %ld", what, value); }

// Counters cover every call, independently of expensive picture sampling. Updates allocate/log nothing and do not change pixels,
// ownership or scheduling. Cumulative fields may be read while another callback updates them; they are not an atomic snapshot.
static void perf_record(Decoder *d, enum PerfStage stage, uint64_t elapsed) {
    PerfCounter *p = &d->perf[stage];
    __atomic_fetch_add(&p->ticks, elapsed, __ATOMIC_RELAXED);
    uint64_t maximum = __atomic_load_n(&p->maximum, __ATOMIC_RELAXED);
    while (elapsed > maximum && !__atomic_compare_exchange_n(&p->maximum, &maximum, elapsed, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    __atomic_fetch_add(&p->count, 1, __ATOMIC_RELAXED);
}
static uint64_t perf_end(Decoder *d, enum PerfStage stage, uint64_t begin) {
    const uint64_t end = mach_absolute_time();
    if (end >= begin) perf_record(d, stage, end - begin);
    return end;
}
static void perf_report(Decoder *d) {
    const uint64_t begin = mach_absolute_time();
    static const char *const names[PerfStages] = {"rate", "block-copy", "input-wait", "annex-b", "input-hash", "queue-input",
        "event-wait", "etb-to-ebd-message", "ebd-message-to-batch", "ebd-batch-wait", "release-input", "etb-to-fbd-message", "fbd-message-to-batch",
        "fbd-batch-wait", "pixel-pool", "pixel-lock", "snapshot", "pixel-copy",
        "locked-audit", "pixel-unlock", "attachments", "return-output", "pending-match", "pre-emit-audit", "emit-callback", "frame-event", "report-log"};
    const uint64_t frames = __atomic_load_n(&d->perf_frames, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < PerfStages; ++i) {
        PerfCounter *p = &d->perf[i];
        const uint64_t count = __atomic_load_n(&p->count, __ATOMIC_RELAXED);
        if (count) os_log_error(gLog, "AVD coachz: perf session 0x%x frames %llu stage %{public}s n %llu ticks %llu max %llu cumulative-live 1",
            d->info.session_id, frames, names[i], count, __atomic_load_n(&p->ticks, __ATOMIC_RELAXED),
            __atomic_load_n(&p->maximum, __ATOMIC_RELAXED));
    }
    os_log_error(gLog, "AVD coachz: perf coverage session 0x%x frames %llu missing-fbd %llu missing-ebd %llu missing-etb %llu invalid %llu",
        d->info.session_id, frames, __atomic_load_n(&d->perf_missing_fbd, __ATOMIC_RELAXED),
        __atomic_load_n(&d->perf_missing_ebd, __ATOMIC_RELAXED), __atomic_load_n(&d->perf_missing_etb, __ATOMIC_RELAXED),
        __atomic_load_n(&d->perf_invalid_timing, __ATOMIC_RELAXED));
    os_log_error(gLog, "AVD coachz: perf cpu session 0x%x frames %llu samples %llu nanoseconds %llu failed %llu",
        d->info.session_id, frames, __atomic_load_n(&d->perf_cpu_samples, __ATOMIC_RELAXED),
        __atomic_load_n(&d->perf_cpu_ns, __ATOMIC_RELAXED), __atomic_load_n(&d->perf_cpu_failed, __ATOMIC_RELAXED));
    thread_extended_info_data_t scheduling = {0};
    mach_msg_type_number_t scheduling_count = THREAD_EXTENDED_INFO_COUNT;
    const kern_return_t scheduling_status = thread_info(pthread_mach_thread_np(pthread_self()), THREAD_EXTENDED_INFO,
        (thread_info_t)&scheduling, &scheduling_count);
    int relative = 0;
    qos_class_t qos = QOS_CLASS_UNSPECIFIED;
    const int qos_status = pthread_get_qos_class_np(pthread_self(), &qos, &relative);
    const bool event_worker = d->thread_running && pthread_equal(pthread_self(), d->thread);
    os_log_error(gLog, "AVD coachz: perf scheduling session 0x%x status 0x%x qos-status %d qos %u relative %d policy %d priority %d current %d event-worker %u",
        d->info.session_id, scheduling_status, qos_status, (unsigned)qos, relative,
        scheduling.pth_policy, scheduling.pth_priority, scheduling.pth_curpri, (unsigned)event_worker);
    perf_end(d, PerfReport, begin);
}

static bool thread_cpu_ns(uint64_t *value) {
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) return false;
    *value = (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
    return true;
}

static void perf_event(Decoder *d, const CoachzVideoEvent *e, bool frame, uint64_t received) {
    const uint64_t handled = mach_absolute_time();
    if (handled >= received) perf_record(d, frame ? PerfFbdBatchWait : PerfEbdBatchWait, handled - received);
    if (!(e->flags & CoachzVideoEventTiming)) {
        __atomic_fetch_add(frame ? &d->perf_missing_fbd : &d->perf_missing_ebd, 1, __ATOMIC_RELAXED);
        return;
    }
    const uint64_t message = (uint64_t)e->count | (uint64_t)e->bytes << 32;
    const uint64_t submitted = (uint64_t)e->stride | (uint64_t)e->scanlines << 32;
    if (message && message <= received) {
        perf_record(d, frame ? PerfFbdToBatch : PerfEbdToBatch, received - message);
        if (submitted && submitted <= message) {
            // The endpoint is the kernel message timestamp, not the later time at which the client observes it.
            perf_record(d, frame ? PerfFirmwareToFbd : PerfFirmwareToEbd, message - submitted);
        } else if (!submitted) __atomic_fetch_add(&d->perf_missing_etb, 1, __ATOMIC_RELAXED);
        else __atomic_fetch_add(&d->perf_invalid_timing, 1, __ATOMIC_RELAXED);
    } else __atomic_fetch_add(&d->perf_invalid_timing, 1, __ATOMIC_RELAXED);
}

static Decoder *storage(VTVideoDecoderRef d) { return (Decoder *)CMBaseObjectGetDerivedStorage(d); }

// A private, cacheable copy of every visible byte before the transfer. Its lifetime extends through the pre-emit check; the firmware
// output is returned earlier and MUST NOT be read again at that point. Diagnostics never replace pixels, retry copies or drop frames.
#define AUDIT_BYTE_BUDGET (128u * 1024u * 1024u)
typedef struct {
    uint32_t generation, index, y_bytes, uv_bytes, y_rows, uv_rows;
    uint64_t token, capture_begin, capture_end, copy_end;
    kern_return_t return_status;
    size_t bytes;
    uint8_t pixels[];
} PictureAudit;

static void audit_free(Decoder *d, PictureAudit *a) {
    if (a) { d->audit_bytes -= a->bytes; free(a); }
}
static PictureAudit *audit_capture(Decoder *d, const CoachzVideoEvent *e, const uint8_t *sy, const uint8_t *suv,
                                   uint32_t y_bytes, uint32_t uv_bytes, uint32_t height) {
    if (!(e->flags & CoachzVideoFrameSample)) return NULL;
    const uint32_t uv_rows = (height + 1) / 2;
    const uint64_t visible = (uint64_t)y_bytes * height + (uint64_t)uv_bytes * uv_rows;
    if (!visible || visible > AUDIT_BYTE_BUDGET - sizeof(PictureAudit) ||
        d->audit_bytes > AUDIT_BYTE_BUDGET - sizeof(PictureAudit) - visible) {
        os_log_error(gLog, "AVD coachz: audit unavailable session 0x%x gen %u token %llu reason budget bytes %llu outstanding %zu",
                     d->info.session_id, e->generation, e->token, visible, d->audit_bytes);
        return NULL;
    }
    const size_t bytes = sizeof(PictureAudit) + (size_t)visible;
    PictureAudit *a = malloc(bytes);
    if (!a) { note("audit snapshot allocation failed", (long)bytes); return NULL; }
    *a = (PictureAudit){.generation = e->generation, .index = e->index, .token = e->token,
        .y_bytes = y_bytes, .uv_bytes = uv_bytes, .y_rows = height, .uv_rows = uv_rows, .bytes = bytes,
        .capture_begin = mach_absolute_time(), .return_status = KERN_FAILURE};
    for (uint32_t row = 0; row < height; ++row)
        coachz_video_copy_row(a->pixels + (size_t)row * y_bytes, sy + (size_t)row * d->stride, y_bytes);
    uint8_t *uv = a->pixels + (size_t)y_bytes * height;
    for (uint32_t row = 0; row < uv_rows; ++row)
        coachz_video_copy_row(uv + (size_t)row * uv_bytes, suv + (size_t)row * d->stride, uv_bytes);
    coachz_video_copy_publish();
    a->capture_end = mach_absolute_time();
    d->audit_bytes += bytes;
    return a;
}
static void audit_observe(Decoder *d, PictureAudit *a, const char *stage, const uint8_t *sy, const uint8_t *suv,
                           const uint8_t *y, size_t ys, const uint8_t *uv, size_t uvs) {
    const uint8_t *expected_uv = a->pixels + (size_t)a->y_bytes * a->y_rows;
    const uint64_t begin = mach_absolute_time();
    const CoachzVideoCopyDiff src_y = coachz_video_compare_plane(a->pixels, a->y_bytes, sy, d->stride, a->y_bytes, a->y_rows);
    const CoachzVideoCopyDiff src_uv = coachz_video_compare_plane(expected_uv, a->uv_bytes, suv, d->stride, a->uv_bytes, a->uv_rows);
    const CoachzVideoCopyDiff dst_y = coachz_video_compare_plane(a->pixels, a->y_bytes, y, ys, a->y_bytes, a->y_rows);
    const CoachzVideoCopyDiff dst_uv = coachz_video_compare_plane(expected_uv, a->uv_bytes, uv, uvs, a->uv_bytes, a->uv_rows);
    const uint64_t end = mach_absolute_time();
    os_log_error(gLog, "AVD coachz: audit session 0x%x gen %u token %llu index %u stage %{public}s ticks %llu..%llu capture %llu..%llu copied %llu src-diff %llu/%llu dst-diff %llu/%llu dst-rows %u/%u first %u:%u/%u:%u dst-zero %llu/%llu",
                 d->info.session_id, a->generation, a->token, a->index, stage, begin, end, a->capture_begin, a->capture_end, a->copy_end,
                 src_y.bytes, src_uv.bytes, dst_y.bytes, dst_uv.bytes, dst_y.rows, dst_uv.rows,
                 dst_y.first_row, dst_y.first_byte, dst_uv.first_row, dst_uv.first_byte, dst_y.zero_bytes, dst_uv.zero_bytes);
}
static void audit_before_emit(Decoder *d, PictureAudit *a, CVPixelBufferRef pb) {
    if (!a || !pb) return;
    // Relocking is itself an observable synchronization point. Report it, and never access an unlocked base address or the returned OPB.
    const CVReturn lock = CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    if (lock) { note("audit pre-emit lock failed", lock); return; }
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    if (y && uv && ys >= a->y_bytes && uvs >= a->uv_bytes &&
        CVPixelBufferGetHeightOfPlane(pb, 0) >= a->y_rows && CVPixelBufferGetHeightOfPlane(pb, 1) >= a->uv_rows) {
        const uint8_t *expected_uv = a->pixels + (size_t)a->y_bytes * a->y_rows;
        const CoachzVideoCopyDiff dy = coachz_video_compare_plane(a->pixels, a->y_bytes, y, ys, a->y_bytes, a->y_rows);
        const CoachzVideoCopyDiff duv = coachz_video_compare_plane(expected_uv, a->uv_bytes, uv, uvs, a->uv_bytes, a->uv_rows);
        const uint64_t ticks = mach_absolute_time();
        const CVReturn unlock = CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        os_log_error(gLog, "AVD coachz: audit session 0x%x gen %u token %llu index %u stage pre-emit-relocked ticks %llu return 0x%x unlock %d dst-diff %llu/%llu rows %u/%u first %u:%u/%u:%u zero %llu/%llu",
                     d->info.session_id, a->generation, a->token, a->index, ticks, a->return_status, unlock,
                     dy.bytes, duv.bytes, dy.rows, duv.rows, dy.first_row, dy.first_byte, duv.first_row, duv.first_byte,
                     dy.zero_bytes, duv.zero_bytes);
    } else {
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        note("audit pre-emit layout invalid", 0);
    }
}

// Frames are taken from the pending table under the lock and handed to VideoToolbox after it is dropped: a client's output callback may
// decode the next frame from inside the emit.
typedef struct { VTVideoDecoderFrame frame; OSStatus status; VTDecodeInfoFlags flags; CVPixelBufferRef image; PictureAudit *audit; } Emit;
typedef struct { Emit e[PENDING]; unsigned n; } Emits;
typedef struct { uint64_t audit_ticks, emit_ticks; unsigned emits; } DeliveryCost;
static void take_locked(Decoder *d, unsigned slot, OSStatus status, VTDecodeInfoFlags flags, CVPixelBufferRef image, Emits *out) {
    if (out->n < PENDING) out->e[out->n++] = (Emit){d->pending[slot].frame, status, flags, image ? CVPixelBufferRetain(image) : NULL, NULL};
    d->pending[slot].used = false;
}
static void drop_all_locked(Decoder *d, OSStatus status, Emits *out) {
    for (unsigned i = 0; i < PENDING; i++)
        if (d->pending[i].used) { take_locked(d, i, status, kVTDecodeInfo_FrameDropped, NULL, out); d->drops++; }
}
static DeliveryCost deliver(Decoder *d, Emits *out) {
    DeliveryCost cost = {0};
    for (unsigned i = 0; i < out->n; i++) {
        Emit *e = &out->e[i];
        if (!d->invalidated && d->session) {
            if (e->audit) {
                const uint64_t begin = mach_absolute_time();
                audit_before_emit(d, e->audit, e->image);
                cost.audit_ticks += perf_end(d, PerfPreEmitAudit, begin) - begin;
            }
            const uint64_t begin = mach_absolute_time();
            const OSStatus result = VTDecoderSessionEmitDecodedFrame(d->session, e->frame, e->status, e->flags, e->image);
            const uint64_t end = perf_end(d, PerfEmit, begin);
            cost.emit_ticks += end - begin; ++cost.emits;
            if (e->audit) os_log_error(gLog, "AVD coachz: emit session 0x%x gen %u token %llu ticks %llu..%llu result %d frame-status %d",
                d->info.session_id, e->audit->generation, e->audit->token, begin, end, result, e->status);
        }
        audit_free(d, e->audit);
        if (e->image) CVPixelBufferRelease(e->image);
    }
    out->n = 0;
    return cost;
}

static void audit_mapping(Decoder *d, const CoachzVideoEvent *e, const char *plane, const void *base,
                            size_t stride, uint32_t width_bytes, uint32_t rows) {
    CoachzVideoMapAudit a = {0};
    size_t size = sizeof a;
    const uint64_t span = (uint64_t)(rows - 1) * stride + width_bytes;
    const uint64_t in[] = {(uintptr_t)base, span};
    const kern_return_t result = IOConnectCallMethod(d->conn, CoachzVideoAuditMemory, in, 2, NULL, 0,
                                                    NULL, NULL, &a, &size);
    os_log_error(gLog, "AVD coachz: mapping session 0x%x gen %u token %llu index %u plane %{public}s status 0x%x result-bytes %zu version %u span %llu pages %u unmapped %u unmanaged %u changed %u attr-mask 0x%x",
                 d->info.session_id, e->generation, e->token, e->index, plane, result, size, a.version, span,
                 a.pages, a.unmapped, a.unmanaged, a.changed, a.attribute_mask);
}

static void unmap_outputs(Decoder *d) {
    for (unsigned i = 0; i < d->nopb; i++)
        if (d->opb[i]) IOConnectUnmapMemory64(d->conn, CoachzVideoMemoryOutput + i, mach_task_self(), d->opb[i]);
    memset(d->opb, 0, sizeof d->opb);
    d->nopb = 0;
}
static OSStatus pool_attributes(Decoder *d);
static void outputs_changed(Decoder *d, const CoachzVideoEvent *e) {
    unmap_outputs(d);
    const uint32_t sample_bytes = e->flags & CoachzVideoOutputP010 ? 2 : 1;
    const uint32_t width = e->crop_width ? e->crop_width : e->width, height = e->crop_height ? e->crop_height : e->height;
    if (d->sample_bytes != sample_bytes || d->width != width || d->height != height) {
        d->sample_bytes = sample_bytes; d->width = width; d->height = height;
        const OSStatus status = pool_attributes(d);
        if (status != noErr) {
            note("pixel buffer attributes", status);
            pthread_mutex_lock(&d->lock); d->error = status; pthread_cond_broadcast(&d->cond); pthread_mutex_unlock(&d->lock);
            return;
        }
    }
    d->generation = e->generation; d->stride = e->stride; d->scanlines = e->scanlines;
    d->crop_left = e->crop_left; d->crop_top = e->crop_top; d->crop_width = e->crop_width; d->crop_height = e->crop_height;
    uint32_t count = e->count > COACHZ_VIDEO_MAX_OUTPUTS ? COACHZ_VIDEO_MAX_OUTPUTS : e->count;
    for (uint32_t i = 0; i < count; i++) {
        mach_vm_address_t at = 0; mach_vm_size_t size = 0;
        if (IOConnectMapMemory64(d->conn, CoachzVideoMemoryOutput + i, mach_task_self(), &at, &size, kIOMapAnywhere) != kIOReturnSuccess) {
            note("output not mapped", (long)i);
            at = 0;
        }
        d->opb[i] = at; d->opb_size[i] = size;
    }
    d->nopb = count;
    ++d->output_changes;
    os_log_error(gLog, "AVD coachz: outputs session 0x%x gen %u %ux%u sample-bytes %u stride %u rows %u buffers %u",
                 d->info.session_id, e->generation, width, height, sample_bytes, e->stride, e->scanlines, count);
}

// The NV12/P010 picture into a 420v/x420 pixel buffer of the session's pool, the crop rectangle only.
typedef struct { uint64_t pool, lock, snapshot, copy, audit, unlock, attachments; } PictureCost;
static CVPixelBufferRef picture(Decoder *d, const CoachzVideoEvent *e, PictureAudit **audit, PictureCost *cost) {
    *audit = NULL;
    if (e->index >= d->nopb || !d->opb[e->index]) return NULL;
    const uint8_t *base = (const uint8_t *)(uintptr_t)d->opb[e->index] + e->offset;
    const size_t need = (size_t)d->stride * d->scanlines + (size_t)d->stride * ((d->scanlines + 1) / 2);
    if (e->offset + need > d->opb_size[e->index]) return NULL;
    uint64_t begin = mach_absolute_time();
    CVPixelBufferPoolRef pool = VTDecoderSessionGetPixelBufferPool(d->session);
    CVPixelBufferRef pb = NULL;
    const CVReturn allocated = pool ? CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb) : kCVReturnInvalidPoolAttributes;
    cost->pool = perf_end(d, PerfPool, begin) - begin;
    if (allocated != kCVReturnSuccess || !pb) return NULL;
    const OSType format = d->sample_bytes == 2 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    if (CVPixelBufferGetPixelFormatType(pb) != format || !CVPixelBufferIsPlanar(pb) || CVPixelBufferGetPlaneCount(pb) != 2) {
        note("unexpected pixel pool format", CVPixelBufferGetPixelFormatType(pb)); CVPixelBufferRelease(pb); return NULL;
    }
    begin = mach_absolute_time();
    const CVReturn locked = CVPixelBufferLockBaseAddress(pb, 0);
    cost->lock = perf_end(d, PerfPixelLock, begin) - begin;
    if (locked != kCVReturnSuccess) { CVPixelBufferRelease(pb); return NULL; }
    const uint32_t cw = e->crop_width ? e->crop_width : d->width, ch = e->crop_height ? e->crop_height : d->height;
    if (!coachz_video_visible_end(d->stride, d->scanlines, e->crop_left, e->crop_top, cw, ch, d->sample_bytes)) {
        CVPixelBufferUnlockBaseAddress(pb, 0); CVPixelBufferRelease(pb); return NULL;
    }
    size_t w = CVPixelBufferGetWidthOfPlane(pb, 0), h = CVPixelBufferGetHeightOfPlane(pb, 0);
    if (w > cw) w = cw;
    if (h > ch) h = ch;
    if (w > d->stride / d->sample_bytes - e->crop_left) w = d->stride / d->sample_bytes - e->crop_left;
    if (h + e->crop_top > d->scanlines) h = d->scanlines - e->crop_top;
    uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    const uint8_t *sy = base + (size_t)e->crop_top * d->stride + e->crop_left * d->sample_bytes;
    const uint8_t *suv = base + (size_t)d->stride * d->scanlines + (size_t)(e->crop_top / 2) * d->stride + (e->crop_left & ~1u) * d->sample_bytes;
    const size_t y_bytes = w * d->sample_bytes, uv_bytes = ((w + 1) & ~(size_t)1) * d->sample_bytes;
    if (!y || !uv || y_bytes > ys || uv_bytes > uvs || (h + 1) / 2 > CVPixelBufferGetHeightOfPlane(pb, 1)) {
        CVPixelBufferUnlockBaseAddress(pb, 0); CVPixelBufferRelease(pb); return NULL;
    }
    coachz_video_copy_acquire();
    begin = mach_absolute_time();
    *audit = audit_capture(d, e, sy, suv, (uint32_t)y_bytes, (uint32_t)uv_bytes, (uint32_t)h);
    uint64_t observed = mach_absolute_time();
    cost->snapshot = observed - begin;
    if (e->flags & CoachzVideoFrameSample) perf_record(d, PerfSnapshot, cost->snapshot);
    begin = observed;
    uint32_t pre_y = 0, pre_uv = 0, pre_zy = 0, pre_zuv = 0;
    if (e->flags & CoachzVideoFrameSample) {
        pre_y = coachz_video_probe_plane(sy, d->stride, (uint32_t)y_bytes, (uint32_t)h, &pre_zy);
        pre_uv = coachz_video_probe_plane(suv, d->stride, (uint32_t)uv_bytes, (uint32_t)((h + 1) / 2), &pre_zuv);
    }
    observed = mach_absolute_time();
    cost->audit = observed - begin;
    begin = observed;
    for (size_t r = 0; r < h; r++) coachz_video_copy_row(y + r * ys, sy + r * d->stride, y_bytes);
    for (size_t r = 0; r < (h + 1) / 2; r++) coachz_video_copy_row(uv + r * uvs, suv + r * d->stride, uv_bytes);
    coachz_video_copy_publish();
    observed = perf_end(d, PerfPixelCopy, begin);
    cost->copy = observed - begin;
    begin = observed;
    if (*audit) {
        (*audit)->copy_end = observed;
        // All four full comparisons finish before this observation's first log call.
        audit_observe(d, *audit, "copied-locked", sy, suv, y, ys, uv, uvs);
        const uintptr_t page_mask = (uintptr_t)vm_page_size - 1;
        os_log_error(gLog, "AVD coachz: audit layout session 0x%x gen %u token %llu index %u Y %zu bytes/%zu rows stride %zu UV %zu bytes/%zu rows stride %zu source-stride %u align64 src %u/%u dst %u/%u page-size %llu page-offset src %u/%u dst %u/%u",
                     d->info.session_id, e->generation, e->token, e->index, y_bytes, h, ys, uv_bytes, (h + 1) / 2, uvs, d->stride,
                     (unsigned)((uintptr_t)sy & 63), (unsigned)((uintptr_t)suv & 63),
                     (unsigned)((uintptr_t)y & 63), (unsigned)((uintptr_t)uv & 63),
                     (unsigned long long)vm_page_size,
                     (unsigned)((uintptr_t)sy & page_mask), (unsigned)((uintptr_t)suv & page_mask),
                     (unsigned)((uintptr_t)y & page_mask), (unsigned)((uintptr_t)uv & page_mask));
    }
    if (e->flags & CoachzVideoFrameSample) {
        uint32_t szy, szuv, dzy, dzuv;
        const uint32_t hy = coachz_video_probe_plane(sy, d->stride, (uint32_t)y_bytes, (uint32_t)h, &szy);
        const uint32_t huv = coachz_video_probe_plane(suv, d->stride, (uint32_t)uv_bytes, (uint32_t)((h + 1) / 2), &szuv);
        const uint32_t dy = coachz_video_probe_plane(y, (uint32_t)ys, (uint32_t)y_bytes, (uint32_t)h, &dzy);
        const uint32_t duv = coachz_video_probe_plane(uv, (uint32_t)uvs, (uint32_t)uv_bytes, (uint32_t)((h + 1) / 2), &dzuv);
        os_log_error(gLog, "AVD coachz: sample session 0x%x gen %u token %llu index %u %ux%u src Y %08x UV %08x zero %u/%u dst Y %08x UV %08x zero %u/%u",
                     d->info.session_id, e->generation, e->token, e->index, (uint32_t)w, (uint32_t)h, hy, huv, szy, szuv, dy, duv, dzy, dzuv);
        const CoachzVideoCopyDiff ydiff = coachz_video_compare_plane(sy, d->stride, y, ys, (uint32_t)y_bytes, (uint32_t)h);
        const CoachzVideoCopyDiff uvdiff = coachz_video_compare_plane(suv, d->stride, uv, uvs, (uint32_t)uv_bytes, (uint32_t)((h + 1) / 2));
        os_log_error(gLog, "AVD coachz: copy session 0x%x gen %u token %llu temporal pre Y %08x UV %08x sampled-source-changed %u/%u dst-stride %zu/%zu diff Y %llu bytes/%u rows first %u:%u zero %llu UV %llu bytes/%u rows first %u:%u zero %llu",
                     d->info.session_id, e->generation, e->token, pre_y, pre_uv, (unsigned)(pre_y != hy), (unsigned)(pre_uv != huv), ys, uvs,
                     (unsigned long long)ydiff.bytes, ydiff.rows, ydiff.first_row, ydiff.first_byte, (unsigned long long)ydiff.zero_bytes,
                     (unsigned long long)uvdiff.bytes, uvdiff.rows, uvdiff.first_row, uvdiff.first_byte, (unsigned long long)uvdiff.zero_bytes);
    }
    if (*audit) {
        audit_observe(d, *audit, "after-logging-locked", sy, suv, y, ys, uv, uvs);
        const CoachzVideoPlaneDigest hy = coachz_video_digest_plane((*audit)->pixels, (uint32_t)y_bytes, (uint32_t)y_bytes, (uint32_t)h);
        const CoachzVideoPlaneDigest huv = coachz_video_digest_plane((*audit)->pixels + y_bytes * h, (uint32_t)uv_bytes, (uint32_t)uv_bytes, (uint32_t)((h + 1) / 2));
        os_log_error(gLog, "AVD coachz: full snapshot session 0x%x gen %u token %llu index %u Y %016llx UV %016llx zero-bytes %llu/%llu zero-rows %u/%u",
                     d->info.session_id, e->generation, e->token, e->index, hy.hash, huv.hash, hy.zero_bytes, huv.zero_bytes, hy.zero_rows, huv.zero_rows);
        // These calls read existing translations only. Keep them after the first full comparison and observe their interval separately.
        audit_mapping(d, e, "source-Y", sy, d->stride, (uint32_t)y_bytes, (uint32_t)h);
        audit_mapping(d, e, "source-UV", suv, d->stride, (uint32_t)uv_bytes, (uint32_t)((h + 1) / 2));
        audit_mapping(d, e, "target-Y", y, ys, (uint32_t)y_bytes, (uint32_t)h);
        audit_mapping(d, e, "target-UV", uv, uvs, (uint32_t)uv_bytes, (uint32_t)((h + 1) / 2));
        audit_observe(d, *audit, "after-map-query-locked", sy, suv, y, ys, uv, uvs);
    }
    observed = mach_absolute_time();
    cost->audit += observed - begin;
    if (e->flags & CoachzVideoFrameSample) perf_record(d, PerfLockedAudit, cost->audit);
    begin = observed;
    const CVReturn unlock = CVPixelBufferUnlockBaseAddress(pb, 0);
    cost->unlock = perf_end(d, PerfPixelUnlock, begin) - begin;
    if (*audit) os_log_error(gLog, "AVD coachz: unlock session 0x%x gen %u token %llu index %u ticks %llu status %d",
                            d->info.session_id, e->generation, e->token, e->index, mach_absolute_time(), unlock);
    begin = mach_absolute_time();
    if (d->attachments) CVBufferSetAttachments(pb, d->attachments, kCVAttachmentMode_ShouldPropagate);
    cost->attachments = perf_end(d, PerfAttachments, begin) - begin;
    return pb;
}

static int find_pending_locked(Decoder *d, uint64_t token) {
    for (unsigned i = 0; i < PENDING; i++) if (d->pending[i].used && d->pending[i].token == token) return (int)i;
    return -1;
}
static void frame_event(Decoder *d, const CoachzVideoEvent *e, Emits *out, uint64_t received) {
    const uint64_t begin = mach_absolute_time();
    uint64_t cpu_begin = 0, cpu_end = 0;
    const bool cpu_begin_valid = thread_cpu_ns(&cpu_begin);
    if (e->index != UINT32_MAX) perf_event(d, e, true, received);
    const bool has_token = !(e->flags & CoachzVideoFrameNoToken);
    CVPixelBufferRef pb = NULL;
    PictureAudit *audit = NULL;
    PictureCost cost = {0};
    uint64_t return_ticks = 0;
    CMTime pts = kCMTimeInvalid, duration = kCMTimeInvalid;
    uint64_t input_hash = 0;
    uint32_t input_bytes = 0;
    bool have_metadata = false;
    if (e->index != ~0u && e->generation == d->generation) {
        pb = picture(d, e, &audit, &cost);
        const uint64_t before_return = mach_absolute_time();
        const kern_return_t returned = IOConnectCallScalarMethod(d->conn, CoachzVideoReturnOutput, (const uint64_t[]){e->generation, e->index}, 2, NULL, NULL);
        return_ticks = perf_end(d, PerfReturn, before_return) - before_return;
        if (audit) audit->return_status = returned;
    } else if (e->index != ~0u) {
        const uint64_t before_return = mach_absolute_time();
        IOConnectCallScalarMethod(d->conn, CoachzVideoReturnOutput, (const uint64_t[]){e->generation, e->index}, 2, NULL, NULL);
        return_ticks = perf_end(d, PerfReturn, before_return) - before_return;
    }
    const uint64_t before_pending = mach_absolute_time();
    pthread_mutex_lock(&d->lock);
    const int slot = has_token ? find_pending_locked(d, e->token) : -1;
    if (slot >= 0) {
        if (e->flags & CoachzVideoFrameSample) {
            pts = d->pending[slot].pts; duration = d->pending[slot].duration;
            input_hash = d->pending[slot].input_hash; input_bytes = d->pending[slot].input_bytes;
            have_metadata = true;
        }
        if (pb && !d->pending[slot].hidden) {
            const unsigned before = out->n;
            take_locked(d, (unsigned)slot, e->status || (e->flags & CoachzVideoFrameCorrupt) ? kErrBadData : noErr, 0, pb, out); d->frames++;
            if (out->n > before) { out->e[before].audit = audit; audit = NULL; }
        }
        else { take_locked(d, (unsigned)slot, noErr, kVTDecodeInfo_FrameDropped, NULL, out); d->drops++; }
        // A sample the firmware never answers (no picture of its own) would wait for the next drain: one far older than a picture just
        // delivered cannot still be in the DPB.
        for (unsigned i = 0; i < PENDING; i++)
            if (d->pending[i].used && d->pending[i].token + 48 < e->token) { take_locked(d, i, noErr, kVTDecodeInfo_FrameDropped, NULL, out); d->drops++; }
    }
    pthread_mutex_unlock(&d->lock);
    const uint64_t pending_ticks = perf_end(d, PerfPending, before_pending) - before_pending;
    if (e->flags & CoachzVideoFrameSample) os_log_error(gLog,
        "AVD coachz: frame metadata session 0x%x gen %u token %llu matched %u pts %lld/%d flags 0x%x duration %lld/%d flags 0x%x queued-bytes %u input-hash %016llx",
        d->info.session_id, e->generation, e->token, have_metadata, pts.value, pts.timescale, pts.flags,
        duration.value, duration.timescale, duration.flags, input_bytes, input_hash);
    audit_free(d, audit);
    if (pb) CVPixelBufferRelease(pb);
    const DeliveryCost delivered = deliver(d, out);
    const bool cpu_valid = cpu_begin_valid && thread_cpu_ns(&cpu_end) && cpu_end >= cpu_begin;
    const uint64_t cpu_ns = cpu_valid ? cpu_end - cpu_begin : 0;
    if (cpu_valid) {
        __atomic_fetch_add(&d->perf_cpu_ns, cpu_ns, __ATOMIC_RELAXED);
        __atomic_fetch_add(&d->perf_cpu_samples, 1, __ATOMIC_RELAXED);
    } else __atomic_fetch_add(&d->perf_cpu_failed, 1, __ATOMIC_RELAXED);
    const uint64_t end = perf_end(d, PerfFrame, begin);
    const uint64_t frames = __atomic_add_fetch(&d->perf_frames, 1, __ATOMIC_RELAXED);
    const uint64_t message = e->flags & CoachzVideoEventTiming ? (uint64_t)e->count | (uint64_t)e->bytes << 32 : 0;
    const uint64_t etb = e->flags & CoachzVideoEventTiming ? (uint64_t)e->stride | (uint64_t)e->scanlines << 32 : 0;
    const uint64_t from = message && message <= begin ? message : begin;
    if (d->slow_ticks && end - from >= d->slow_ticks) {
        const uint64_t n = __atomic_add_fetch(&d->perf_slow_frames, 1, __ATOMIC_RELAXED);
        if (n <= 8 || !(n & (n - 1))) os_log_error(gLog,
            "AVD coachz: slow frame session 0x%x gen %u token %llu n %llu ticks %llu..%llu etb %llu message %llu batch %llu pool %llu lock %llu snapshot %llu copy %llu audit %llu unlock %llu attachments %llu return %llu pending %llu pre-emit %llu emit %llu emits %u sampled %u cpu-ns %llu cpu-valid %u",
            d->info.session_id, e->generation, e->token, n, begin, end, etb, message, received, cost.pool, cost.lock, cost.snapshot, cost.copy, cost.audit,
            cost.unlock, cost.attachments, return_ticks, pending_ticks, delivered.audit_ticks, delivered.emit_ticks, delivered.emits,
            (unsigned)((e->flags & CoachzVideoFrameSample) != 0), cpu_ns, (unsigned)cpu_valid);
    }
    if (!(frames % 120)) perf_report(d);
}

static void input_done(Decoder *d, const CoachzVideoEvent *e, uint64_t received) {
    const uint64_t begin = mach_absolute_time();
    perf_event(d, e, false, received);
    pthread_mutex_lock(&d->lock);
    if (e->index < COACHZ_VIDEO_MAX_INPUTS) d->input_busy[e->index] = false;
    pthread_cond_broadcast(&d->cond);
    pthread_mutex_unlock(&d->lock);
    perf_end(d, PerfInputRelease, begin);
}
static bool event_boundary(uint32_t kind) {
    return kind == CoachzVideoEventOutputs || kind == CoachzVideoEventDrained ||
           kind == CoachzVideoEventFlushed || kind == CoachzVideoEventError;
}

static void *event_thread(void *arg) {
    Decoder *d = arg;
    CoachzVideoEvent events[COACHZ_VIDEO_EVENTS_PER_WAIT];
    Emits *out = calloc(1, sizeof *out);
    if (!out) return NULL;
    while (!d->stop) {
        size_t size = sizeof events;
        const uint64_t timeout = 200;
        const uint64_t before_wait = mach_absolute_time();
        const kern_return_t kr = IOConnectCallMethod(d->conn, CoachzVideoWaitEvent, &timeout, 1, NULL, 0, NULL, NULL, events, &size);
        const uint64_t received = perf_end(d, PerfEventWait, before_wait);
        if (kr == kIOReturnTimeout) continue;
        if (kr != kIOReturnSuccess) {
            pthread_mutex_lock(&d->lock);
            if (!d->error) { d->error = kErrMalfunction; note("wait failed", kr); }
            drop_all_locked(d, kErrMalfunction, out);
            pthread_cond_broadcast(&d->cond);
            pthread_mutex_unlock(&d->lock);
            deliver(d, out);
            break;
        }
        const size_t count = size / sizeof events[0];
        bool released[COACHZ_VIDEO_EVENTS_PER_WAIT] = {0};
        size_t prepared = 0;
        for (size_t i = 0; i < count; i++) {
            if (i == prepared) {
                size_t end = i;
                while (end < count && !event_boundary(events[end].kind)) ++end;
                // EBD already transferred input ownership back to the driver. Wake input producers before a preceding frame's
                // copy/audit/client callback can block them, as hfi_session_etb_done/vb2_buffer_done does independently of capture.
                // Never move a completion across Outputs, Drain, Flush or Error; pending frames/control keep their original order.
                for (size_t j = i; j < end; ++j) if (events[j].kind == CoachzVideoEventInputDone) {
                    input_done(d, &events[j], received); released[j] = true;
                }
                prepared = end == i ? i + 1 : end;
            }
            const CoachzVideoEvent *e = &events[i];
            switch (e->kind) {
            case CoachzVideoEventInputDone:
                if (!released[i]) input_done(d, e, received);
                break;
            case CoachzVideoEventOutputs:
                outputs_changed(d, e);
                pthread_mutex_lock(&d->lock);
                if (d->error) drop_all_locked(d, d->error, out);
                pthread_mutex_unlock(&d->lock);
                deliver(d, out);
                break;
            case CoachzVideoEventFrame: frame_event(d, e, out, received); break;
            case CoachzVideoEventDrained:
            case CoachzVideoEventFlushed:
                pthread_mutex_lock(&d->lock);
                drop_all_locked(d, noErr, out);
                if (e->kind == CoachzVideoEventDrained) d->drained++; else d->flushed++;
                pthread_mutex_unlock(&d->lock);
                deliver(d, out);
                pthread_mutex_lock(&d->lock);
                pthread_cond_broadcast(&d->cond);
                pthread_mutex_unlock(&d->lock);
                break;
            case CoachzVideoEventError:
                pthread_mutex_lock(&d->lock);
                d->error = kErrMalfunction;
                note("session failed", (long)e->status);
                drop_all_locked(d, kErrMalfunction, out);
                pthread_cond_broadcast(&d->cond);
                pthread_mutex_unlock(&d->lock);
                deliver(d, out);
                break;
            default: break;
            }
        }
    }
    free(out);
    return NULL;
}

// ---- format description: codec, size, parameter sets ----
static bool append(uint8_t **buf, size_t *len, const uint8_t *data, size_t n) {
    uint8_t *grown = realloc(*buf, *len + 4 + n);
    if (!grown) return false;
    grown[*len] = 0; grown[*len + 1] = 0; grown[*len + 2] = 0; grown[*len + 3] = 1;
    memcpy(grown + *len + 4, data, n);
    *buf = grown; *len += 4 + n;
    return true;
}
static bool parameter_sets(Decoder *d) {
    size_t count = 0;
    int nal = 0;
    if (d->hfi == CoachzVideoH264) {
        if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(d->format, 0, NULL, NULL, &count, &nal) != noErr) return false;
        for (size_t i = 0; i < count; i++) {
            const uint8_t *ps = NULL; size_t n = 0;
            if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(d->format, i, &ps, &n, NULL, NULL) != noErr || !append(&d->params, &d->params_len, ps, n)) return false;
        }
    } else if (d->hfi == CoachzVideoHEVC) {
        // hev1 may carry its parameter sets in the samples only (ISO/IEC 14496-15 8.4.1): then there is nothing to prepend.
        if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(d->format, 0, NULL, NULL, &count, &nal) != noErr) {
            if (d->codec != 'hev1') return false;
            count = 0; nal = 4;
        }
        for (size_t i = 0; i < count; i++) {
            const uint8_t *ps = NULL; size_t n = 0;
            if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(d->format, i, &ps, &n, NULL, NULL) != noErr || !append(&d->params, &d->params_len, ps, n)) return false;
        }
    }
    d->nal_len = nal;
    return nal == 1 || nal == 2 || nal == 4;
}
static void color_attachments(Decoder *d) {
    const CFStringRef keys[] = {kCVImageBufferColorPrimariesKey, kCVImageBufferTransferFunctionKey, kCVImageBufferYCbCrMatrixKey,
                                kCVImageBufferChromaLocationTopFieldKey, kCVImageBufferChromaLocationBottomFieldKey};
    CFMutableDictionaryRef out = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!out) return;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        CFTypeRef v = CMFormatDescriptionGetExtension(d->format, keys[i]);
        if (v) CFDictionarySetValue(out, keys[i], v);
    }
    d->attachments = out;
}
static OSStatus pool_attributes(Decoder *d) {
    const int32_t format = d->sample_bytes == 2 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    const int32_t w = (int32_t)d->width, h = (int32_t)d->height;
    CFNumberRef nf = CFNumberCreate(NULL, kCFNumberSInt32Type, &format), nw = CFNumberCreate(NULL, kCFNumberSInt32Type, &w),
                nh = CFNumberCreate(NULL, kCFNumberSInt32Type, &h);
    CFDictionaryRef surface = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *keys[] = {kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey};
    const void *values[] = {nf, nw, nh, surface};
    CFDictionaryRef attributes = nf && nw && nh && surface ? CFDictionaryCreate(NULL, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : NULL;
    const OSStatus status = attributes ? VTDecoderSessionSetPixelBufferAttributes(d->session, attributes) : kErrMalfunction;
    if (attributes) CFRelease(attributes);
    if (nf) CFRelease(nf);
    if (nw) CFRelease(nw);
    if (nh) CFRelease(nh);
    if (surface) CFRelease(surface);
    return status;
}

static void close_connection(Decoder *d) {
    if (d->thread_running) {
        d->stop = true;
        pthread_join(d->thread, NULL);
        d->thread_running = false;
    }
    if (d->conn) {
        perf_report(d);
        os_log_error(gLog, "AVD coachz: close session 0x%x decode %llu flags 0x%x hidden %llu finish %llu accept %llu properties %llu outputs %u frames %llu drops %llu drained %u flushed %u error %d",
                     d->info.session_id, d->decodes, d->decode_flags, d->hidden, d->finishes, d->accepts, d->property_calls,
                     d->output_changes, d->frames, d->drops, d->drained, d->flushed, d->error);
        IOConnectCallScalarMethod(d->conn, CoachzVideoDestroy, NULL, 0, NULL, NULL);
        unmap_outputs(d);
        for (uint32_t i = 0; i < d->info.input_count && i < COACHZ_VIDEO_MAX_INPUTS; i++)
            if (d->input[i]) IOConnectUnmapMemory64(d->conn, CoachzVideoMemoryInput + i, mach_task_self(), (mach_vm_address_t)(uintptr_t)d->input[i]);
        memset(d->input, 0, sizeof d->input);
        IOServiceClose(d->conn);
        d->conn = 0;
    }
}

static OSStatus start_session(VTVideoDecoderRef obj, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format) {
    Decoder *d = storage(obj);
    d->session = session;
    if (d->format) CFRelease(d->format);
    d->format = (CMVideoFormatDescriptionRef)CFRetain(format);
    const CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(format);
    d->width = (uint32_t)dims.width; d->height = (uint32_t)dims.height;
    d->sample_bytes = 1; d->fps = 30;
    d->rate_duration = kCMTimeZero; d->rate_samples = 0; d->rate_started = false;
    mach_timebase_info_data_t timebase = {0};
    if (mach_timebase_info(&timebase) == KERN_SUCCESS && timebase.numer && timebase.denom)
        d->slow_ticks = UINT64_C(50000000) * timebase.denom / timebase.numer;
    if (dims.width < 1 || dims.height < 1 || dims.width > 4096 || dims.height > 4096) return kErrNoDecoder;
    const FourCharCode sub = CMFormatDescriptionGetMediaSubType(format);
    if (sub == kCMVideoCodecType_H264) d->hfi = CoachzVideoH264;
    else if (sub == kCMVideoCodecType_HEVC || sub == 'hev1') d->hfi = CoachzVideoHEVC;
    else if (sub == kCMVideoCodecType_VP9) d->hfi = CoachzVideoVP9;
    else return kErrNoDecoder;
    if (d->hfi != CoachzVideoVP9 && !parameter_sets(d)) { note("no parameter sets", (long)sub); return kErrBadData; }
    d->need_params = true;
    color_attachments(d);
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(COACHZ_VIDEO_CLASS));
    if (!service) return kErrNoDecoder;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &d->conn);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) { note("open", kr); d->conn = 0; return kErrNoDecoder; }
    const uint64_t create[4] = {d->hfi, d->width, d->height, CoachzVideoClientP010};
    size_t size = sizeof d->info;
    kr = IOConnectCallMethod(d->conn, CoachzVideoCreate, create, 4, NULL, 0, NULL, NULL, &d->info, &size);
    if (kr == kIOReturnBadArgument) { // an older service supports the original three-scalar, NV12-only Create
        d->legacy_service = true;
        size = sizeof d->info;
        kr = IOConnectCallMethod(d->conn, CoachzVideoCreate, create, 3, NULL, 0, NULL, NULL, &d->info, &size);
    }
    if (kr != KERN_SUCCESS || size != sizeof d->info || d->info.version != COACHZ_VIDEO_VERSION) { note("create", kr); close_connection(d); return kErrNoDecoder; }
    for (uint32_t i = 0; i < d->info.input_count && i < COACHZ_VIDEO_MAX_INPUTS; i++) {
        mach_vm_address_t at = 0; mach_vm_size_t len = 0;
        if (IOConnectMapMemory64(d->conn, CoachzVideoMemoryInput + i, mach_task_self(), &at, &len, kIOMapAnywhere) != kIOReturnSuccess ||
            len < d->info.input_bytes) { note("input not mapped", (long)i); close_connection(d); return kErrMalfunction; }
        d->input[i] = (uint8_t *)(uintptr_t)at;
    }
    const OSStatus attributes_status = pool_attributes(d);
    if (attributes_status != noErr) { note("pixel buffer attributes", attributes_status); close_connection(d); return attributes_status; }
    d->stop = false;
    if (pthread_create(&d->thread, NULL, event_thread, d)) { close_connection(d); return kErrMalfunction; }
    d->thread_running = true;
    os_log_error(gLog, "AVD coachz: start session 0x%x codec 0x%x %ux%u audit-version %u", d->info.session_id, d->hfi, d->width, d->height,
                 COACHZ_VIDEO_AUDIT_VERSION);
    os_log_error(gLog, "AVD coachz: perf clock session 0x%x version 1 ns-numer %u ns-denom %u slow-ticks %llu",
                 d->info.session_id, timebase.numer, timebase.denom, d->slow_ticks);
    return noErr;
}

// Length-prefixed NAL units to Annex B in place at out; 0 when it does not fit or the lengths are wrong.
static size_t annex_b(const Decoder *d, const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    size_t at = 0, o = 0;
    while (at + (size_t)d->nal_len <= n) {
        size_t len = 0;
        for (int i = 0; i < d->nal_len; i++) len = len << 8 | in[at + (size_t)i];
        at += (size_t)d->nal_len;
        if (!len || len > n - at || o + 4 + len > cap) return 0;
        out[o] = 0; out[o + 1] = 0; out[o + 2] = 0; out[o + 3] = 1;
        memcpy(out + o + 4, in + at, len);
        o += 4 + len; at += len;
    }
    return at == n ? o : 0;
}

// vdec_s_parm configures a stream rate, not a new rate for every quantized sample duration. k212/k213 alternate 533/16000 and
// 534/16000 for the same 30 fps stream. Per-sample ceil turned those into hundreds of 31/30 fps load_scale calls. Start promptly,
// then estimate over at most 16 samples or half a media second. CMTimeAdd preserves differing timescales; missing timing retains
// the last valid rate. Integer rounding represents the window; the kernel still selects an OPP at or above its calculated load.
static OSStatus update_frame_rate(Decoder *d, CMTime duration) {
    if (CMTIME_IS_NUMERIC(duration) && duration.value > 0 && duration.timescale > 0) {
        const CMTime span = d->rate_samples ? CMTimeAdd(d->rate_duration, duration) : duration;
        if (!CMTIME_IS_NUMERIC(span) || span.value <= 0 || span.timescale <= 0) {
            d->rate_duration = kCMTimeZero; d->rate_samples = 0;
            return noErr;
        }
        d->rate_duration = span;
        ++d->rate_samples;
        if (d->rate_started && d->rate_samples < 16 && CMTimeCompare(span, CMTimeMake(1, 2)) < 0) return noErr;
        const uint64_t numerator = (uint64_t)span.timescale * d->rate_samples, denominator = (uint64_t)span.value;
        const uint64_t rate = numerator / denominator + (numerator % denominator >= denominator / 2 + denominator % 2);
        d->rate_duration = kCMTimeZero; d->rate_samples = 0; d->rate_started = true;
        if (rate >= 1 && rate <= 480 && (uint32_t)rate != d->fps) {
            const uint64_t fps = rate;
            const kern_return_t scaled = IOConnectCallScalarMethod(d->conn, CoachzVideoSetFrameRate, &fps, 1, NULL, NULL);
            if (scaled != KERN_SUCCESS && scaled != kIOReturnUnsupported && !(d->legacy_service && scaled == kIOReturnBadArgument)) {
                note("frame rate", scaled); return kErrMalfunction;
            }
            d->fps = (uint32_t)rate;
        }
    }
    return noErr;
}

static OSStatus decode_frame(VTVideoDecoderRef obj, VTVideoDecoderFrame frame, CMSampleBufferRef sample, VTDecodeFrameFlags flags,
                             VTDecodeInfoFlags *info) {
    Decoder *d = storage(obj);
    if (info) *info = 0;
    if (!d->conn) return kErrMalfunction;
    const uint64_t input_begin = mach_absolute_time();
    uint64_t begin = input_begin;
    const CMTime duration = CMSampleBufferGetDuration(sample);
    pthread_mutex_lock(&d->lock);
    const OSStatus rate_status = update_frame_rate(d, duration);
    pthread_mutex_unlock(&d->lock);
    const uint64_t rate_ticks = perf_end(d, PerfRate, begin) - begin;
    if (rate_status) return rate_status;
    begin = mach_absolute_time();
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    const size_t n = block ? CMBlockBufferGetDataLength(block) : 0;
    if (!n) return kErrBadData;
    if (d->scratch_len < n) {
        uint8_t *grown = realloc(d->scratch, n);
        if (!grown) return kErrMalfunction;
        d->scratch = grown; d->scratch_len = n;
    }
    const OSStatus copied = CMBlockBufferCopyDataBytes(block, 0, n, d->scratch);
    const uint64_t block_ticks = perf_end(d, PerfBlockCopy, begin) - begin;
    if (copied != kCMBlockBufferNoErr) return kErrBadData;
    begin = mach_absolute_time();
    pthread_mutex_lock(&d->lock);
    d->decodes++; d->decode_flags |= flags;
    if (flags & kVTDecodeFrame_DoNotOutputFrame) d->hidden++;
    int slot = -1;
    for (unsigned tries = 0; slot < 0 && !d->error && tries < 50; tries++) {
        for (uint32_t i = 0; i < d->info.input_count && i < COACHZ_VIDEO_MAX_INPUTS; i++) if (!d->input_busy[i]) { slot = (int)i; break; }
        if (slot < 0) {
            struct timespec ts = {0, 20 * 1000 * 1000};
            pthread_cond_timedwait_relative_np(&d->cond, &d->lock, &ts);
        }
    }
    int pend = -1;
    for (unsigned i = 0; slot >= 0 && i < PENDING; i++) if (!d->pending[i].used) { pend = (int)i; break; }
    const OSStatus error = d->error;
    if (slot >= 0 && pend >= 0 && !error) {
        d->input_busy[slot] = true;
        d->pending[pend].used = true; d->pending[pend].frame = frame; d->pending[pend].token = ++d->next_token;
        d->pending[pend].pts = CMSampleBufferGetPresentationTimeStamp(sample);
        d->pending[pend].duration = duration;
        d->pending[pend].input_hash = 0; d->pending[pend].input_bytes = 0;
        d->pending[pend].hidden = (flags & kVTDecodeFrame_DoNotOutputFrame) != 0;
    }
    const uint64_t token = d->next_token;
    pthread_mutex_unlock(&d->lock);
    const uint64_t wait_ticks = perf_end(d, PerfInputWait, begin) - begin;
    if (error) return error;
    if (slot < 0 || pend < 0) { note("no free input", (long)slot); return kErrMalfunction; }
    uint8_t *dst = d->input[slot];
    begin = mach_absolute_time();
    size_t at = 0;
    if (d->need_params && d->params_len <= d->info.input_bytes) { memcpy(dst, d->params, d->params_len); at = d->params_len; }
    const size_t body = d->nal_len ? annex_b(d, d->scratch, n, dst + at, d->info.input_bytes - at)
                                   : (n <= d->info.input_bytes - at ? (memcpy(dst + at, d->scratch, n), n) : 0);
    const uint64_t convert_ticks = perf_end(d, PerfAnnexB, begin) - begin;
    uint64_t hash_ticks = 0, queue_ticks = 0;
    kern_return_t kr = KERN_FAILURE;
    if (body) {
        coachz_video_copy_publish();
        const uint32_t bytes = (uint32_t)(at + body);
        begin = mach_absolute_time();
        const CoachzVideoPlaneDigest input = coachz_video_digest_plane(dst, bytes, bytes, 1);
        hash_ticks = perf_end(d, PerfInputHash, begin) - begin;
        pthread_mutex_lock(&d->lock);
        if (d->pending[pend].used && d->pending[pend].token == token) {
            d->pending[pend].input_hash = input.hash; d->pending[pend].input_bytes = bytes;
        }
        pthread_mutex_unlock(&d->lock);
        const uint64_t args[4] = {(uint64_t)slot, at + body, token, CoachzVideoInputEndOfFrame};
        begin = mach_absolute_time();
        kr = IOConnectCallScalarMethod(d->conn, CoachzVideoQueueInput, args, 4, NULL, NULL);
        queue_ticks = perf_end(d, PerfQueue, begin) - begin;
    }
    if (kr != KERN_SUCCESS) {
        pthread_mutex_lock(&d->lock);
        d->input_busy[slot] = false;
        d->pending[pend].used = false;
        pthread_mutex_unlock(&d->lock);
        if (!body) note("sample does not convert or fit", (long)n);
        else note("queue", kr);
        return body ? kErrMalfunction : kErrBadData;
    }
    d->need_params = false;
    const uint64_t input_end = mach_absolute_time();
    if (d->slow_ticks && input_end - input_begin >= d->slow_ticks) {
        const uint64_t count = __atomic_add_fetch(&d->perf_slow_inputs, 1, __ATOMIC_RELAXED);
        if (count <= 8 || !(count & (count - 1))) os_log_error(gLog,
            "AVD coachz: slow input session 0x%x token %llu n %llu ticks %llu..%llu rate %llu block %llu input-wait %llu annex-b %llu hash %llu queue %llu bytes %zu",
            d->info.session_id, token, count, input_begin, input_end, rate_ticks, block_ticks, wait_ticks, convert_ticks, hash_ticks, queue_ticks, at + body);
    }
    if (info) *info |= kVTDecodeInfo_Asynchronous;
    return noErr;
}

// Waits until the kernel reports what it was asked (drained or flushed) or the session failed.
static void wait_count(Decoder *d, uint32_t *counter, uint32_t before) {
    pthread_mutex_lock(&d->lock);
    for (unsigned i = 0; i < 300 && *counter == before && !d->error && d->thread_running; i++) {
        struct timespec ts = {0, 10 * 1000 * 1000};
        pthread_cond_timedwait_relative_np(&d->cond, &d->lock, &ts);
    }
    Emits *out = calloc(1, sizeof *out);
    if (*counter == before && out) drop_all_locked(d, noErr, out);
    pthread_mutex_unlock(&d->lock);
    if (out) { deliver(d, out); free(out); }
}
static OSStatus finish_delayed_frames(VTVideoDecoderRef obj) {
    Decoder *d = storage(obj);
    if (!d->conn) return noErr;
    pthread_mutex_lock(&d->lock);
    d->finishes++;
    const uint32_t before = d->drained;
    pthread_mutex_unlock(&d->lock);
    os_log_error(gLog, "AVD coachz: finish session 0x%x drained-before %u", d->info.session_id, before);
    if (IOConnectCallScalarMethod(d->conn, CoachzVideoDrain, NULL, 0, NULL, NULL) == KERN_SUCCESS) wait_count(d, &d->drained, before);
    return d->error;
}
static Boolean can_accept(VTVideoDecoderRef obj, CMFormatDescriptionRef format) {
    Decoder *d = storage(obj);
    pthread_mutex_lock(&d->lock); d->accepts++; pthread_mutex_unlock(&d->lock);
    const Boolean accept = d->format && CMFormatDescriptionEqual(d->format, format);
    os_log_error(gLog, "AVD coachz: can-accept session 0x%x result %u", d->info.session_id, (unsigned)accept);
    return accept;
}
static OSStatus copy_supported(VTVideoDecoderRef obj, CFDictionaryRef *out) {
    const void *keys[] = {kVTPropertyTypeKey, kVTPropertyReadWriteStatusKey};
    const void *values[] = {kVTPropertyType_Boolean, kVTPropertyReadWriteStatus_ReadOnly};
    CFDictionaryRef readonly = CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *names[] = {kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder};
    const void *descriptions[] = {readonly};
    *out = CFDictionaryCreate(NULL, names, descriptions, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(readonly);
    return *out ? noErr : kErrMalfunction;
}
static OSStatus copy_property(CFTypeRef obj, CFStringRef key, CFAllocatorRef allocator, void *out) {
    if (CFEqual(key, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder)) {
        *(CFTypeRef *)out = CFRetain(kCFBooleanTrue);
        return noErr;
    }
    return kErrNotAvailable;
}
static OSStatus set_property(CFTypeRef obj, CFStringRef key, CFTypeRef value) {
    Decoder *d = storage(obj);
    pthread_mutex_lock(&d->lock); const bool trace = ++d->property_calls <= 8; pthread_mutex_unlock(&d->lock);
    char name[128];
    if (trace && key && CFStringGetCString(key, name, sizeof name, kCFStringEncodingUTF8))
        os_log_error(gLog, "AVD coachz: set-property session 0x%x key %{public}s", d->info.session_id, name);
    return kErrPropertyNotFound;
}
static OSStatus invalidate(CFTypeRef obj) {
    Decoder *d = storage(obj);
    pthread_mutex_lock(&d->lock);
    d->invalidated = true;
    pthread_mutex_unlock(&d->lock);
    close_connection(d);
    return noErr;
}
static void finalize(CFTypeRef obj) {
    Decoder *d = storage(obj);
    invalidate(obj);
    if (d->format) CFRelease(d->format);
    if (d->attachments) CFRelease(d->attachments);
    free(d->params);
    free(d->scratch);
    pthread_mutex_destroy(&d->lock);
    pthread_cond_destroy(&d->cond);
}
static CFStringRef copy_description(CFTypeRef obj) {
    Decoder *d = storage(obj);
    return CFStringCreateWithFormat(NULL, NULL, CFSTR("<Coachz Venus decoder %p codec 0x%x %ux%u, %llu frames, %llu dropped>"), obj,
                                    d->hfi, d->width, d->height, d->frames, d->drops);
}

static const DecoderBaseClass kBase = {1, sizeof(Decoder), NULL, invalidate, finalize, copy_description, copy_property, set_property, {0}};
static const DecoderClass kClass = {3, start_session, decode_frame, copy_supported, NULL, NULL, can_accept, finish_delayed_frames,
                                    NULL, NULL, NULL};
static const DecoderVTable kVTable = {NULL, &kBase, &kClass, NULL};

static OSStatus create_instance(FourCharCode codec, CFAllocatorRef allocator, VTVideoDecoderRef *out) {
    *out = NULL;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(COACHZ_VIDEO_CLASS));
    if (!service) return kErrNoDecoder;   // no codec on this boot (no firmware, or its bring-up failed)
    IOObjectRelease(service);
    CFTypeRef obj = NULL;
    const OSStatus status = CMDerivedObjectCreate(allocator, &kVTable, VTVideoDecoderGetClassID(), &obj);
    if (status != noErr || !obj) return status ? status : kErrMalfunction;
    Decoder *d = storage(obj);
    memset(d, 0, sizeof *d);
    pthread_mutex_init(&d->lock, NULL);
    pthread_cond_init(&d->cond, NULL);
    d->codec = codec;
    *out = obj;
    return noErr;
}

__attribute__((visibility("default"))) void AVDRegister(void);
__attribute__((visibility("default"))) void AVDRegister(void) {
    gLog = os_log_create("org.trogdor.coachz", "video");
    const FourCharCode codecs[] = {kCMVideoCodecType_H264, kCMVideoCodecType_HEVC, 'hev1', kCMVideoCodecType_VP9};
    for (size_t i = 0; i < sizeof codecs / sizeof codecs[0]; i++) {
        const OSStatus status = VTRegisterVideoDecoder(codecs[i], create_instance);
        if (status) note("register", status);
    }
}
