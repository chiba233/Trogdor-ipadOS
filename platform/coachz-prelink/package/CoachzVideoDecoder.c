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
// parameter sets before the first picture and after a flush), queues each sample with a token, and copies each decoded NV12 picture
// into a pixel buffer of the session's pool.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <os/log.h>
#include "CoachzVideoAbi.h"

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

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    io_connect_t conn;
    VTVideoDecoderSession session;
    CMVideoFormatDescriptionRef format;
    FourCharCode codec;
    uint32_t hfi, width, height;
    int nal_len;                         // 0: the sample is raw (VP8, VP9)
    uint8_t *params; size_t params_len;  // Annex B parameter sets
    bool need_params;
    CoachzVideoInfo info;
    uint8_t *input[COACHZ_VIDEO_MAX_INPUTS];
    bool input_busy[COACHZ_VIDEO_MAX_INPUTS];
    uint32_t generation, nopb, stride, scanlines, crop_left, crop_top, crop_width, crop_height;
    mach_vm_address_t opb[COACHZ_VIDEO_MAX_OUTPUTS];
    mach_vm_size_t opb_size[COACHZ_VIDEO_MAX_OUTPUTS];
    struct { uint64_t token; VTVideoDecoderFrame frame; bool used, hidden; } pending[PENDING];
    uint64_t next_token;
    uint32_t drained, flushed;           // counts of the events, for waiters
    OSStatus error;
    bool thread_running, stop, invalidated;
    pthread_t thread;
    CFDictionaryRef attachments;
    uint8_t *scratch; size_t scratch_len;
    uint64_t frames, drops;
} Decoder;

static os_log_t gLog;
static void note(const char *what, long value) { os_log_error(gLog, "AVD coachz: %{public}s %ld", what, value); }

static Decoder *storage(VTVideoDecoderRef d) { return (Decoder *)CMBaseObjectGetDerivedStorage(d); }

// Frames are taken from the pending table under the lock and handed to VideoToolbox after it is dropped: a client's output callback may
// decode the next frame from inside the emit.
typedef struct { VTVideoDecoderFrame frame; OSStatus status; VTDecodeInfoFlags flags; CVPixelBufferRef image; } Emit;
typedef struct { Emit e[PENDING]; unsigned n; } Emits;
static void take_locked(Decoder *d, unsigned slot, OSStatus status, VTDecodeInfoFlags flags, CVPixelBufferRef image, Emits *out) {
    if (out->n < PENDING) out->e[out->n++] = (Emit){d->pending[slot].frame, status, flags, image ? CVPixelBufferRetain(image) : NULL};
    d->pending[slot].used = false;
}
static void drop_all_locked(Decoder *d, OSStatus status, Emits *out) {
    for (unsigned i = 0; i < PENDING; i++)
        if (d->pending[i].used) { take_locked(d, i, status, kVTDecodeInfo_FrameDropped, NULL, out); d->drops++; }
}
static void deliver(Decoder *d, Emits *out) {
    for (unsigned i = 0; i < out->n; i++) {
        Emit *e = &out->e[i];
        if (!d->invalidated && d->session) VTDecoderSessionEmitDecodedFrame(d->session, e->frame, e->status, e->flags, e->image);
        if (e->image) CVPixelBufferRelease(e->image);
    }
    out->n = 0;
}

static void unmap_outputs(Decoder *d) {
    for (unsigned i = 0; i < d->nopb; i++)
        if (d->opb[i]) IOConnectUnmapMemory64(d->conn, CoachzVideoMemoryOutput + i, mach_task_self(), d->opb[i]);
    memset(d->opb, 0, sizeof d->opb);
    d->nopb = 0;
}
static void outputs_changed(Decoder *d, const CoachzVideoEvent *e) {
    unmap_outputs(d);
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
}

// The NV12 picture into a 420v pixel buffer of the session's pool, the crop rectangle only.
static CVPixelBufferRef picture(Decoder *d, const CoachzVideoEvent *e) {
    if (e->index >= d->nopb || !d->opb[e->index]) return NULL;
    const uint8_t *base = (const uint8_t *)(uintptr_t)d->opb[e->index] + e->offset;
    const size_t need = (size_t)d->stride * d->scanlines + (size_t)d->stride * ((d->scanlines + 1) / 2);
    if (e->offset + need > d->opb_size[e->index]) return NULL;
    CVPixelBufferPoolRef pool = VTDecoderSessionGetPixelBufferPool(d->session);
    CVPixelBufferRef pb = NULL;
    if (!pool || CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb) != kCVReturnSuccess || !pb) return NULL;
    if (CVPixelBufferLockBaseAddress(pb, 0) != kCVReturnSuccess) { CVPixelBufferRelease(pb); return NULL; }
    const uint32_t cw = e->crop_width ? e->crop_width : d->width, ch = e->crop_height ? e->crop_height : d->height;
    size_t w = CVPixelBufferGetWidthOfPlane(pb, 0), h = CVPixelBufferGetHeightOfPlane(pb, 0);
    if (w > cw) w = cw;
    if (h > ch) h = ch;
    if (w > d->stride - e->crop_left) w = d->stride - e->crop_left;
    if (h + e->crop_top > d->scanlines) h = d->scanlines - e->crop_top;
    uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    const uint8_t *sy = base + (size_t)e->crop_top * d->stride + e->crop_left;
    const uint8_t *suv = base + (size_t)d->stride * d->scanlines + (size_t)(e->crop_top / 2) * d->stride + (e->crop_left & ~1u);
    for (size_t r = 0; r < h; r++) memcpy(y + r * ys, sy + r * d->stride, w);
    for (size_t r = 0; r < (h + 1) / 2; r++) memcpy(uv + r * uvs, suv + r * d->stride, (w + 1) & ~(size_t)1);
    CVPixelBufferUnlockBaseAddress(pb, 0);
    if (d->attachments) CVBufferSetAttachments(pb, d->attachments, kCVAttachmentMode_ShouldPropagate);
    return pb;
}

static int find_pending_locked(Decoder *d, uint64_t token) {
    for (unsigned i = 0; i < PENDING; i++) if (d->pending[i].used && d->pending[i].token == token) return (int)i;
    return -1;
}
static void frame_event(Decoder *d, const CoachzVideoEvent *e, Emits *out) {
    const bool has_token = !(e->flags & CoachzVideoFrameNoToken);
    CVPixelBufferRef pb = NULL;
    if (e->index != ~0u && e->generation == d->generation) {
        pb = picture(d, e);
        IOConnectCallScalarMethod(d->conn, CoachzVideoReturnOutput, (const uint64_t[]){e->generation, e->index}, 2, NULL, NULL);
    } else if (e->index != ~0u) {
        IOConnectCallScalarMethod(d->conn, CoachzVideoReturnOutput, (const uint64_t[]){e->generation, e->index}, 2, NULL, NULL);
    }
    pthread_mutex_lock(&d->lock);
    const int slot = has_token ? find_pending_locked(d, e->token) : -1;
    if (slot >= 0) {
        if (pb && !d->pending[slot].hidden) { take_locked(d, (unsigned)slot, e->status ? kErrBadData : noErr, 0, pb, out); d->frames++; }
        else { take_locked(d, (unsigned)slot, noErr, kVTDecodeInfo_FrameDropped, NULL, out); d->drops++; }
        // A sample the firmware never answers (no picture of its own) would wait for the next drain: one far older than a picture just
        // delivered cannot still be in the DPB.
        for (unsigned i = 0; i < PENDING; i++)
            if (d->pending[i].used && d->pending[i].token + 48 < e->token) { take_locked(d, i, noErr, kVTDecodeInfo_FrameDropped, NULL, out); d->drops++; }
    }
    pthread_mutex_unlock(&d->lock);
    if (pb) CVPixelBufferRelease(pb);
    deliver(d, out);
}

static void *event_thread(void *arg) {
    Decoder *d = arg;
    CoachzVideoEvent events[COACHZ_VIDEO_EVENTS_PER_WAIT];
    Emits *out = calloc(1, sizeof *out);
    if (!out) return NULL;
    while (!d->stop) {
        size_t size = sizeof events;
        const uint64_t timeout = 200;
        const kern_return_t kr = IOConnectCallMethod(d->conn, CoachzVideoWaitEvent, &timeout, 1, NULL, 0, NULL, NULL, events, &size);
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
        for (size_t i = 0; i < size / sizeof events[0]; i++) {
            const CoachzVideoEvent *e = &events[i];
            switch (e->kind) {
            case CoachzVideoEventInputDone:
                pthread_mutex_lock(&d->lock);
                if (e->index < COACHZ_VIDEO_MAX_INPUTS) d->input_busy[e->index] = false;
                pthread_cond_broadcast(&d->cond);
                pthread_mutex_unlock(&d->lock);
                break;
            case CoachzVideoEventOutputs: outputs_changed(d, e); break;
            case CoachzVideoEventFrame: frame_event(d, e, out); break;
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
static void pool_attributes(Decoder *d) {
    const int32_t format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, w = (int32_t)d->width, h = (int32_t)d->height;
    CFNumberRef nf = CFNumberCreate(NULL, kCFNumberSInt32Type, &format), nw = CFNumberCreate(NULL, kCFNumberSInt32Type, &w),
                nh = CFNumberCreate(NULL, kCFNumberSInt32Type, &h);
    CFDictionaryRef surface = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *keys[] = {kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey};
    const void *values[] = {nf, nw, nh, surface};
    CFDictionaryRef attributes = CFDictionaryCreate(NULL, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (attributes) { VTDecoderSessionSetPixelBufferAttributes(d->session, attributes); CFRelease(attributes); }
    CFRelease(nf); CFRelease(nw); CFRelease(nh); CFRelease(surface);
}

static void close_connection(Decoder *d) {
    if (d->thread_running) {
        d->stop = true;
        pthread_join(d->thread, NULL);
        d->thread_running = false;
    }
    if (d->conn) {
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
    const uint64_t create[3] = {d->hfi, d->width, d->height};
    size_t size = sizeof d->info;
    kr = IOConnectCallMethod(d->conn, CoachzVideoCreate, create, 3, NULL, 0, NULL, NULL, &d->info, &size);
    if (kr != KERN_SUCCESS || size != sizeof d->info || d->info.version != COACHZ_VIDEO_VERSION) { note("create", kr); close_connection(d); return kErrNoDecoder; }
    for (uint32_t i = 0; i < d->info.input_count && i < COACHZ_VIDEO_MAX_INPUTS; i++) {
        mach_vm_address_t at = 0; mach_vm_size_t len = 0;
        if (IOConnectMapMemory64(d->conn, CoachzVideoMemoryInput + i, mach_task_self(), &at, &len, kIOMapAnywhere) != kIOReturnSuccess ||
            len < d->info.input_bytes) { note("input not mapped", (long)i); close_connection(d); return kErrMalfunction; }
        d->input[i] = (uint8_t *)(uintptr_t)at;
    }
    pool_attributes(d);
    d->stop = false;
    if (pthread_create(&d->thread, NULL, event_thread, d)) { close_connection(d); return kErrMalfunction; }
    d->thread_running = true;
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

static OSStatus decode_frame(VTVideoDecoderRef obj, VTVideoDecoderFrame frame, CMSampleBufferRef sample, VTDecodeFrameFlags flags,
                             VTDecodeInfoFlags *info) {
    Decoder *d = storage(obj);
    if (info) *info = 0;
    if (!d->conn) return kErrMalfunction;
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    const size_t n = block ? CMBlockBufferGetDataLength(block) : 0;
    if (!n) return kErrBadData;
    if (d->scratch_len < n) {
        uint8_t *grown = realloc(d->scratch, n);
        if (!grown) return kErrMalfunction;
        d->scratch = grown; d->scratch_len = n;
    }
    if (CMBlockBufferCopyDataBytes(block, 0, n, d->scratch) != kCMBlockBufferNoErr) return kErrBadData;
    pthread_mutex_lock(&d->lock);
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
        d->pending[pend].hidden = (flags & kVTDecodeFrame_DoNotOutputFrame) != 0;
    }
    const uint64_t token = d->next_token;
    pthread_mutex_unlock(&d->lock);
    if (error) return error;
    if (slot < 0 || pend < 0) { note("no free input", (long)slot); return kErrMalfunction; }
    uint8_t *dst = d->input[slot];
    size_t at = 0;
    if (d->need_params && d->params_len <= d->info.input_bytes) { memcpy(dst, d->params, d->params_len); at = d->params_len; }
    const size_t body = d->nal_len ? annex_b(d, d->scratch, n, dst + at, d->info.input_bytes - at)
                                   : (n <= d->info.input_bytes - at ? (memcpy(dst + at, d->scratch, n), n) : 0);
    kern_return_t kr = KERN_FAILURE;
    if (body) {
        const uint64_t args[4] = {(uint64_t)slot, at + body, token, CoachzVideoInputEndOfFrame};
        kr = IOConnectCallScalarMethod(d->conn, CoachzVideoQueueInput, args, 4, NULL, NULL);
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
    const uint32_t before = d->drained;
    pthread_mutex_unlock(&d->lock);
    if (IOConnectCallScalarMethod(d->conn, CoachzVideoDrain, NULL, 0, NULL, NULL) == KERN_SUCCESS) wait_count(d, &d->drained, before);
    return d->error;
}
static Boolean can_accept(VTVideoDecoderRef obj, CMFormatDescriptionRef format) {
    Decoder *d = storage(obj);
    return d->format && CMFormatDescriptionEqual(d->format, format);
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
static OSStatus set_property(CFTypeRef obj, CFStringRef key, CFTypeRef value) { return kErrPropertyNotFound; }
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
