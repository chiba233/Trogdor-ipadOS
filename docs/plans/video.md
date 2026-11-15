# Hardware video decode (Venus on SC7180)

Every hardware step follows mainline Linux's latest stable release (checked against 7.3.3; check kernel.org, do not pin to an
older version), `drivers/media/platform/qcom/venus/` and `arch/arm64/boot/dts/qcom/sc7180.dtsi`. Each step in the code names the
mainline function it follows. Runtime data from Linux on this machine is not a reference.

## Hardware, from mainline

- `venus: video-codec@aa00000`, `reg 0xff000`, `interrupts <GIC_SPI 174 IRQ_TYPE_LEVEL_HIGH>`.
- Power domains `venus`, `vcodec0` (videocc GDSCs) and `cx` (rpmhpd). Clocks `core`, `iface`, `bus`, `vcodec0_core`,
  `vcodec0_bus`.
- OPP table: 150, 270, 340, 434, 500 MHz with CX at low_svs, svs, svs_l1, nom, turbo.
- Interconnects `video-mem` (MASTER_VIDEO_P0 to SLAVE_EBI1) and `cpu-cfg` (MASTER_APPSS_PROC to SLAVE_VENUS_CFG).
- `iommus <&apps_smmu 0x0c00 0x60>`; the `video-firmware` child `<&apps_smmu 0x0c42 0x0>`; `memory-region venus_mem` (5 MiB).
- `sc7180_res`: HFI 4xx, AR50, `dma_mask 0xe0000000 - 1`, firmware `qcom/venus-5.4/venus.mbn`, no `min_fw`.
- Decode capabilities: `hfi_platform_v4.c` (H.264, HEVC, VP8, VP9; frames 96x96 to 4096x4096).

## Status

| Part | State |
| --- | --- |
| Power, clocks, GDSCs, bus vote | Runs on hardware (`pm_helpers.c` v4 order) |
| apps SMMU bank, firmware load, ARM9 release (`venus_boot_no_tz`, `venus_reset_cpu`) | Runs on hardware |
| HFI queues, SYS_INIT, image version | Runs on hardware: SYS_INIT_DONE, `VIDEO.VE.5.4-00059-PROD-1` |
| vcodec0 power-on | Runs on hardware |
| Decoder engine: `vdec_start_output`, `vdec_event_change`, `vdec_start_capture`, `vdec_decoder_cmd`, flush, `vdec_session_release` | Runs on hardware: the boot self-test decodes its I_PCM picture exactly; a resolution change mid-stream and flush are not exercised |
| Per-buffer memory: contiguous, write-combined, mapped into the bank at a first-fit IOVA below the shared queues, unmapped on free | Runs on hardware (up to 66 ranges, 31 table pages with two sessions) |
| Interrupts (SPI 174, `venus_isr` / `venus_isr_thread`, 10 ms poll fallback) | Runs on hardware |
| Clock scaling (`load_scale_v4`, the five OPPs with their CX corners, Fabia rate change and the RCG divider) and the bandwidth table | Bandwidth votes run on hardware; no session has needed more than 150 MHz yet, so the PLL rate change is not exercised |
| Boot self-test through the engine (one I_PCM IDR picture, compared sample by sample) | Runs on hardware: 0 of 16384 luma and 0 of 8192 chroma samples differ |
| IOKit service `CoachzVenus` and user client, ABI `CoachzVideoAbi.h` | Runs on hardware |
| VideoToolbox decoder `AVD.videodecoder` (`avc1`, `hvc1`, `hev1`, `vp09`) and its delivery (bindfs union, trust cache, kobject map grant) | Runs on hardware: HEVC 640x360 and H.264 playback produce frames; the horizontal green lines are gone since k213 (`coachz-prelink-video-cache.patch`); VP9 and larger sizes are unverified |
| Idle power collapse (`venus_suspend_3xx`/4xx) and retained resume | Compiled, opt-in with `--venus-runtime-pm`; not exercised on hardware; whole-system sleep is not integrated |
| `cpu-cfg` vote and system-error recovery (`venus_sys_error_handler`) | Compiled: CN0 aggregation with IPA; fail sessions, quiesce DMA, reload firmware/HFI; not exercised on hardware |
| 10-bit output (P010) | Compiled for HEVC/VP9, negotiated with the client; TP10 UBWC DPB, linear P010 output and x420 pool; not exercised on hardware |

## VideoToolbox

The OS image has no decoder bundles on disk (`/System/Library/VideoDecoders` holds only `AV1SW.metallib`); VideoToolbox in the shared
cache names `/System/Library/VideoDecoders/AVD.videodecoder`, `H264H8.videodecoder`, `HEVCH8.videodecoder`, `AV1SW.videodecoder` and
others, whose code lives in the cache. UniXNU's EVB10 record (upstream unixnu-gpu docs/devices/evb10.md) found that iPadOS decodes
H.264 and HEVC through the kernel service behind AppleAVDUserClient (from the entitlements of videodecodeservice and videocodecd); that
interface is Apple's own decoder hardware's command protocol, so the route here is a VideoToolbox decoder of our own over the Venus user
client, not an imitation of AppleAVD. Read from the 21H461 cache (ipsw 3.1.743):

- `vtInitializeVideoDecoderRegistry` registers the built-in decoders with `vtRegisterVideoDecoderWithInfoKeysAndValues`, then calls
  `VTLoadVideoDecoders`, then `VTAvoidHardwareDecoders`.
- `VTLoadVideoDecoders` is a fixed table, not a directory scan: `VTLoadVideoDecoder(path, register symbol)` for each entry, which is
  `dlopen(path)` then `dlsym(register)` and a call. Entries include `/System/Library/VideoDecoders/H264H8.videodecoder` (`H264H8Register`),
  `HEVCH8.videodecoder` (`HEVCH8Register`), `AVD.videodecoder` (`AVDRegister`), `AV1SW.videodecoder` (`AV1RegisterDecoder`).
- The cache has no image for `AVD.videodecoder` (this IPSW is J120's), nor for `HEVCH8`, `H264H2/4/6`, `ExampleIPB` and others, so
  `dlopen` of those paths reads the disk. A dylib named `AVD.videodecoder` exporting `_AVDRegister`, placed in
  `/System/Library/VideoDecoders` by a bindfs union as the audio plug-in is and covered by the root trust cache, is loaded by every
  process that builds the decoder registry.
- `H264H8Register` checks its platform and calls `VTRegisterVideoDecoder(codec type, create-instance function)` once per codec type.
  The decoder object's class (the CMBaseObject method table VideoToolbox calls: start session, decode frame, properties, finish delayed
  frames) is read from H264H8's create function and VideoToolbox's call sites; the macOS SDK has no header for it.

## Rules for this work

- No image is written for a step that has no mainline source behind it.
- Host tests cover what can run off the device (SMMU ranges through the kit, HFI packet layouts, the test bitstream) before an image.

## Decoder object (read from H264H8 in the cache)

`AppleD5500WrapperH264DecoderCreateInstance` calls `VTVideoDecoderGetClassID` and `CMDerivedObjectCreate` with `_kH264VideoDecoderVTable`
= `{0, &baseClass, &decoderClass, 0}`:

- base class, version 1: derived storage 0x2230 bytes, then `equal` (null), `invalidate`, `finalize`, `copyDebugDescription`,
  `copyProperty`, `setProperty`;
- decoder class, version 3: `startSession` (+0x08), `decodeFrame` (+0x10), `copySupportedPropertyDictionary` (+0x18), then four
  slots H264H8 leaves null (+0x20 to +0x38, the order of the old macOS `VTVideoDecoder.h`: `setProperties`, `copySerializableProperties`,
  `canAcceptFormatDescription`, `finishDelayedFrames`), `startTileSession` (+0x40), `decodeTile` (+0x48).

H264H8 registers `avc1` and `drmi` with its H.264 create function and `hvc1` and two more HEVC types with its HEVC one.

## Design (as written)

- Kernel (`CoachzVenus.cpp`): the bring-up above, then a message thread woken by SPI 174 that reads the message and debug queues and
  dispatches each packet to its session (session ids `0x56` + serial + slot). A session per user client. Client threads send the
  commands and wait for their replies as `hfi.c` does (1 s); the message thread records buffer-done packets and events and wakes them.
  The sequence change sets `reconfig`; the client's next WaitEvent runs `vdec_start_capture` (output configuration for the stream's size,
  counts, scratch buffers again, DPB, CONTINUE, DPB and capture buffers queued) and returns an Outputs event. A resolution change flushes
  the output first (`vdec_stop_capture`'s DRC branch). A drain before the first sequence change is held until the capture side streams.
- DPB: UBWC NV12 on OUTPUT (`hold_count` buffers of `get_framesize_raw_nv12_ubwc`), kernel-only. Capture: linear NV12 on OUTPUT2,
  `fw_min_cnt` (or the OUTPUT2 hold count) plus four, `get_framesize_raw_nv12` each; the client maps them write-combined. A negotiated
  10-bit HEVC/VP9 sequence selects mainline's TP10 UBWC DPB and P010 capture sizes instead.
- Input: at least eight buffers of `venus_helper_get_framesz` for the compressed size (or the firmware's), the token as the HFI
  timestamp, which the firmware carries to the picture it decodes.
- ABI (`CoachzVideoAbi.h`): methods Create, QueueInput, ReturnOutput, Drain, Flush, WaitEvent, Destroy, SetFrameRate; memory types 0x100 + i (input)
  and 0x200 + i (output of the current generation); events InputDone, Outputs, Frame, Drained, Flushed, Error.
- Plug-in (`CoachzVideoDecoder.c`): `AVDRegister` registers `avc1`, `hvc1`, `hev1`, `vp09`. StartSession reads the size and parameter
  sets (`CMVideoFormatDescriptionGet{H264,HEVC}ParameterSetAtIndex`), opens the service and sets 420v pool attributes. DecodeFrame
  converts length-prefixed NAL units to Annex B (parameter sets before the first sample), queues it and returns asynchronously. An event
  thread copies each picture's crop rectangle into a pool pixel buffer with the format's colour attachments, returns the capture buffer
  and emits the frame outside its lock; Drained, Flushed and errors emit the rest as dropped. FinishDelayedFrames drains and waits.
  On a negotiated P010 Outputs event it updates the pool to x420, checks the returned format and row bounds, and copies 16-bit samples.
  The Info/Event layouts remain 16/80 bytes. Original three-scalar clients remain NV12-only; the new plug-in retries that Create on
  an old service. `SetFrameRate` uses the sample's media duration, rounded up, with 30 fps as the initial fallback.
- Delivery: `coachz-prelink-video-decoder.patch` writes the dylib to `/private/var/coachz/video` and mounts it read-only as a bindfs union
  over `/System/Library/VideoDecoders` before launchd; `build-coachz-venus.py` signs it and writes its trust cache module (passed with
  `--root-trust-cache`); `coachz-prelink-gpu-map-grant.patch` lets `CoachzVenusUserClient` mappings past the sandbox's kobject filter, and
  the service grants every process (`NativeSandboxGrantProcess` `*`).

## Historical green-line evidence in HEVC and H.264 playback

Seen on hardware with Safari's 640x360 HEVC video. The firmware logs, per session: `DMAConfigDec(245): Error on LCU size: 3` right
after the sequence change, and `h265Dec_Continue(241): Downscale is not supported: Invalid frame dimensions for output2!` (answered as
the non-fatal 0x1008) after CONTINUE. OUTPUT2's frame size is the capture height aligned to 32 (640x384), as `vdec_try_fmt_common` and
`vdec_output_conf` set it, against a 640x360 sequence. Not yet separated: whether the firmware then leaves some OPBs unconverted (zero
NV12 shows green), whether the LCU size error leaves part of the picture undecoded, or whether the decoder's copy is at fault. Decode
latency is 30-100 ms a frame with 400-800 ms spikes.

The coded-size experiment subsequently ran on hardware: the OUTPUT2 dimension error disappeared, but both 640x360 HEVC and H.264
still displayed many horizontal green lines with varying lengths and row counts. HEVC completed 630 input/output frames and H.264
1612, with no short payload, corrupt flag, firmware error, stale tag or address/bounds error in the FBD summaries. The LCU message
remained specific to HEVC and does not explain the common symptom. H.264's reported FBD dimensions were inconsistent despite the
valid sequence/crop; those diagnostic fields are not used to size the copy.

All 21 paired kernel/plug-in source samples matched. Before frame emission, three plug-in destination samples differed from their
sources: the first two HEVC samples differed in Y and UV, and one H.264 sample differed in Y. The eight-row probes locate a data
consistency failure in the copy boundary; they do not inspect every pixel or establish a unique cache/ordering mechanism.

The k212 decoder revision replaces only the pixel-plane libc copies with aligned temporal volatile accesses and byte tails, with
an acquire barrier before consuming DMA output and a completion barrier before publishing the pixel buffer. The pinned original
`libsystem_platform` uses `LDNP` in its non-overlapping row-copy path; that observation is not proof that the instruction caused the
failure. Sampled pictures now also compare every visible byte and report mismatched rows/bytes, first row/byte, zero destination
bytes and source hashes before/after the transfer. No pixel values are logged, and no retry or frame drop hides a failed comparison.
The revision was linked, packaged and written to the kernel partition with a complete
480 MiB readback match. Playback on k212 still had green lines. Of twelve selected pictures, three had destination differences;
all differing destination bytes were zero, in totals divisible by 64 bytes. The Y/UV difference totals were 1792/128, 384/0 and 576/64
bytes. These observations do not prove a cache-line eviction mechanism: the earlier source stability check covered only eight rows,
and the full comparison did not retain an independent pre-copy reference.

### k213 cache transition and complete boundary evidence

These changes were included in k213. The later hardware observation records the picture improvement and the open frame-rate regression.

The cache candidate in `coachz-prelink-video-cache.patch` cleans and invalidates the cacheable aperture before the BCM2837 batch
transition to non-cacheable memory. The original batch path cleans only RT pages; its individual-page counterpart has a separate
WIMG synchronization path. The existing Venus allocation already clears and flushes its own buffers before changing their attributes;
that operation is retained. Actual playback in k213 no longer showed the lines. The ordering and the expanded observations changed
together, so the unique mechanism is not isolated by that result.

Selected pictures now carry the following diagnostic chain. The existing sampling budget is retained.

- Kernel FBD and the decoder's independent cached snapshot hash every visible Y/UV byte, with zero-byte and zero-row counts. Input
  summaries associate the queued compressed bytes, kernel ownership observations, presentation timestamp and returned picture token.
  Input history is bounded; an overwritten or absent association is reported as missing, rather than treated as matching.
- The snapshot is captured before the destination copy and retained until emission. Exact full-plane comparisons check source and
  destination before the first comparison log, after logging, and after the mapping query. Layout, first differing row/byte, total
  differing bytes/rows, zero destination bytes and observation times are recorded without pixel values.
- Unlock, ReturnOutput, requeue, event dequeue and VideoToolbox emission are timed separately. A read-only relock compares the
  destination against the retained snapshot before emission. It never rereads a returned DMA buffer. Relocking and the probes
  themselves can affect synchronization and performance; their results are diagnostic observations, not production frame timings.
- Optional method 8 reads existing page translations and managed-page cache-attribute metadata for both source and destination.
  It reports absent, unmanaged or changed translations explicitly. It does not fault in pages, wire memory, flush caches, return
  addresses, or describe every alias PTE. The Info/Event ABI remains 16/80 bytes; the mapping result is 24 bytes.
- Snapshot allocation has a 128 MiB aggregate budget and reports allocation or layout failures. No comparison failure repairs pixels,
  retries the copy or suppresses a frame to make the symptom disappear.

The current kernel and decoder sources pass syntax checks against the target headers. Those checks do not establish runtime
correctness or produce a new decoder or kernel artifact.

For the next authorized hardware batch, compare the same token through input digest, FBD, snapshot, copied/locked, post-log,
post-query and pre-emit observations, then correlate actual playback. Full digest equality still cannot prove that memory never
changed between observations. Keep seek, resolution changes, flush and 10-bit acceptance separate from a green-line result.

`tools/coachz_video_audit.py --kernel KERNEL_LOG --decoder DECODER_LOG --output REPORT_JSON` correlates logs already restricted to
one boot. It retains source/destination difference counts and observation timelines, flags absent stages, input-history gaps,
unsupported mappings, event loss, conflicting token records and inconsistent times. Identical repeated records are deduplicated;
conflicting repetitions remain explicit. A merged historical logarchive must first be restricted to the selected boot.

The reader reproduces all three k212 copy-difference observations from the retained kernel and decoder logs, and marks their missing
new probe stages as incomplete. It never labels matching sparse probes or complete metadata coverage as a repaired picture or
successful playback. The decoder's layout log uses the
process's actual VM page size for page offsets, independently of the SMMU's page granule.

### Diagnostics and OUTPUT2 comparison

Linux v7.3.3 still aligns capture height to 32 in `vdec_try_fmt_common` and passes that height to OUTPUT2 in `vdec_output_conf`.
The earlier [output-resolution reconfiguration change](https://github.com/torvalds/linux/commit/ef15219c3e25e1c4427ffebd2de9ab5f73b29b45)
also selects the capture dimensions. No matching fix for the two firmware messages was found in the history/mail searches. The
alignment is therefore a comparison to test, not an established cause or fix.

- `session ... FBD:` at release records nonempty filled-length min/max, zero returns, payloads shorter than the visible NV12/P010 span,
  offsets, flags, picture-type counts, corrupt/error/drop/flush counts, stale tags, allocation bounds, address and dimension mismatches,
  and lost client events. Flush returns are included and counted explicitly; `short` compares metadata with the last byte the crop
  copy needs, and does not prove unwritten pixels.
- `COACHZ venus: sample` and `AVD coachz: sample` share the session id, generation, token and output index. Compare kernel Y/UV hashes
  with the plug-in's source hashes, then its source with destination hashes. Each hashes eight evenly spaced complete rows per plane
  and counts all-zero sampled rows. The first four pictures per generation, up to eight periodic samples (one every 120 pictures),
  and up to four further anomalous pictures per session are sampled. Equal hashes cover only those rows. The probe reads while the
  output is owned; the client returns it after the copy and probe. No pixel bytes are logged.
- `tools/xnu_screen.py --venus-output2-coded` writes the opt-in `/chosen/coachz-venus-output2-coded` property without extending the
  boot command line. After the sequence event it sends the unaligned coded width
  and height only in OUTPUT2's FRAME_SIZE property. Allocations, DPB, stride and scanlines keep their original aligned calculations;
  the default remains the mainline path. `OUTPUT2 frame size` logs which path is selected. Compare the same clip's visible bars and
  the firmware's `Invalid frame dimensions for output2` message. A successful comparison still needs hardware evidence.

The MM1 display reservation is calculated from mainline's
[Coachz panel selection](https://github.com/gregkh/linux/blob/v7.3.3/arch/arm64/boot/dts/qcom/sc7180-trogdor-coachz.dtsi),
[panel mode](https://github.com/gregkh/linux/blob/v7.3.3/drivers/gpu/drm/panel/panel-edp.c),
[DPU plane bandwidth calculation](https://github.com/gregkh/linux/blob/v7.3.3/drivers/gpu/drm/msm/disp/dpu1/dpu_plane.c) and
[SC7180 performance table](https://github.com/gregkh/linux/blob/v7.3.3/drivers/gpu/drm/msm/disp/dpu1/catalog/dpu_6_2_sc7180.h).
For the current full-panel 32-bit scanout at 60 Hz, `2160 * 1480 * 60 * 4` bytes/s exceeds the 24/37 prefill term. Multiplying by
120/100 and converting to kB/s gives 920678; the catalog's peak floor is 1600000. There is one mdp0-mem path. These calculated values
preserve the existing vote; the provenance is mainline source.

## Additional changes awaiting hardware

The following changes were included in earlier unified kernel builds. The k211/k212 playback observations above are the current
green-line baseline; individual lifecycle, recovery, power and 10-bit behavior still require their own hardware results.

- DRC queues its Outputs event before any FTB can produce a frame for that generation. An unacknowledged DRC flush or internal-buffer
  release stops reconfiguration and retains registrations for abort/recovery. A client flush reports success only after its ACK and
  clears pending drain state. Corrupt FBD flags reach VideoToolbox as bad-data status. The plug-in logs start/close, output generations,
  decode flags, DoNotOutputFrame counts, FinishDelayedFrames, format-accept calls and property calls. It still has no invented VT flush
  callback; the actual seek lifecycle and the existing 48-token late-frame heuristic need playback evidence.
- Clock demand uses the media frame rate and the most recent payload of each input slot, rather than a lifetime payload maximum.
  Scaling happens before the sampled ETB submission; failed PLL/RCG programming is returned as an error and leaves the rate unknown,
  requiring a top CX vote before another transition. At the default rate, the VPP term for 1920x1080 at 60 fps is 102816000 Hz (150 MHz
  OPP); 3840x2160 at 30 fps is 204120000 Hz (270 MHz OPP), and 4096x2160 at 30 fps is 217728000 Hz (270 MHz OPP). Bitstream work can
  raise demand further. These are calculations, not exercised clock transitions.
- The 1000 kB/s cpu-cfg vote is aggregated with IPA's CN0 request before the BCM command is quantized: sum average, maximum peak.
  A serialized vote/send updates the saved consumer request only on success. Dropping Venus's request preserves IPA's request and
  [SC7180's CN0 keepalive](https://github.com/gregkh/linux/blob/v7.3.3/drivers/interconnect/qcom/sc7180.c): when both consumers are zero,
  [bcm_aggregate](https://github.com/gregkh/linux/blob/v7.3.3/drivers/interconnect/qcom/bcm-voter.c) keeps the active x/y vote at one.
- `--venus-runtime-pm` sets a separate opt-in ADT property. With no session of the current core epoch, the worker waits for firmware
  WFI/idle, sends PC_PREP and checks PC_READY, gates IRQ/message reads, asserts ARM9 reset and waits for AXI halt before disabling
  clocks/GDSCs/PLL and dropping votes. SC7180's HW_CTRL_TRIGGER GDSC returns to software mode before collapse. A new session restores
  votes/power, releases reset and runs retained firmware/queues. This covers idle core collapse only, not system sleep or active-stream
  suspend. The default leaves idle collapse disabled.
- SYS_ERROR/watchdog recovery fails clients, excludes command writers and the queue reader, and cold-restarts firmware/HFI only after
  a successful AXI halt and power cycle. Old clients stay failed and must recreate. Close keeps unquiesced buffers quarantined until
  reset; a failed halt retains their memory and clocks. Partial collapse/resume failures enter the same recovery path. None of these
  error paths has a fault-injection or device result yet.
- P010 requires the optional Create capability and a 10-bit HEVC/VP9 sequence. Format sizes and HFI constants follow `helpers.c`,
  `hfi_helper.h` and `hfi_platform_v4.c`; output stride is bytes and crop coordinates remain pixels. The plug-in changes the pool to
  `kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange` and validates the returned format, plane count and row capacity before copying.
  This does not establish Main10 playback, HDR metadata correctness or display acceptance.

Earlier unified kernel revisions compiled and linked. The new decoder, recovery and power paths still require their own hardware
results.


## k213 hardware observation (2026-11-10)

Actual playback no longer showed horizontal green lines, while the displayed frame rate became much worse.
The returned kernel ring contains no panic. This is a picture-integrity milestone with an open performance regression, not complete
video acceptance. The image included both the cached-to-uncached page-transition drain and the expanded boundary observations;
this run does not isolate the drain from the additional observation and synchronization effects.

Of eighteen selected playback pictures, seventeen have complete FBD, private pre-copy snapshot, three locked source/destination
comparisons, page-metadata queries, return and pre-emit observations. All captured differences are zero, compressed-input hashes
agree, and the four plane mappings report cache metadata 0x6 without missing or changed pages. The eighteenth picture's persistent
log ends before its final query/unlock/emission records. The separate fixed-frame self-test is not included in the playback count.
Unobserved frames and intervals remain outside these comparisons.

The packed ADT supplies the 19.2 MHz timebase used to interpret the recorded ticks. On those seventeen pictures, the kernel full-plane
digest takes a median 10.23 ms; the private snapshot takes 1.47 ms; the actual copy interval, including the sparse pre-copy probe,
takes 1.37 ms (maximum 11.28 ms). Snapshot through the final locked comparison takes a median 12.49 ms. FBD-to-return is a median
96.88 ms, including scheduling, queueing, pool work and observations; it is not a measured hardware decode time.

During the stable playback window the native decoder statistics record 189, 178 and 181 decoded pictures in roughly six-second
windows, with zero decoder drops. The display queue records 111 and 109 displayed pictures and 67 and 68 dropped pictures in two
roughly six-second windows. Thus steady decoded throughput is close to 30 fps while displayed throughput is about 18 fps. Startup
also contains much longer queued decode latencies; those are not explained by the steady-state counts. GPU/presentation traces
are retained for the display-side investigation; decoder startup, copy and observation cost also remain open. Preserve the accepted
picture behavior while investigating both boundaries.

## Pending decoder performance revision

This is source work after k213, without a new kernel, decoder artifact or device write. Video frame-rate investigation remains part
of the decoder work; steady output count alone does not establish timely delivery.

Two decoder changes address demonstrated or source-visible unnecessary work:

- Quantized duration is not a per-picture stream-rate change. k213 has 451 native 30/31 fps updates across the three AVC sessions
  (11, 87 and 353); k212 already had 789 such updates in its longer session. The observed 533/16000 and 534/16000 sample durations
  straddle 30 when individually rounded upward. The revision starts with the first valid timing, then rounds the average over at most
  sixteen samples or half a media second. CoreMedia time arithmetic retains timescale semantics; a missing duration retains the last
  valid rate. This removes a concrete source of control calls, without claiming that an older issue uniquely caused the k213 regression.
- Completed inputs in a returned batch are released before its potentially expensive frame copies, comparisons and callbacks.
  `hfi_session_etb_done` calls `vdec_buf_done` for INPUT independently of capture consumption; the previous plug-in instead delayed
  later InputDone events until earlier frame work finished. Look-ahead stops at Outputs, Drain, Flush and Error, so completions never
  cross a control boundary and picture/control ordering stays intact. The firmware has already returned input ownership before this
  release; output ownership, copies and full-plane integrity observations are unchanged. The latency benefit remains unaccepted.

Lightweight counters now cover all input submissions and picture events, separately from selected-picture integrity sampling:
rate selection, block copy, input-slot reservation, Annex B conversion, input hash, queue call, event wait, ETB-to-EBD/FBD message
intervals, message-to-batch delivery, each event's wait behind earlier events of that batch, input release, pixel-pool allocation,
pixel lock, private snapshot, temporal copy, locked comparisons/query/logging, unlock, attachments, output return, pending matching,
pre-emit comparison, emission callback, frame-event body and reporting itself. Event-wait includes normal idle waits; firmware-message
intervals include HFI transport, queueing and reordering. None is labeled hardware-only decode time. Overlapping stages must not be
summed as disjoint CPU time.

The optional timing flag reuses otherwise unused Outputs fields in Frame/InputDone events, leaving the Info/Event ABI at 16/80 bytes.
Older clients ignore the flag; newer clients count missing or invalid timing rather than inventing values. Each session records its
actual mach timebase. Cumulative counts/ticks/maxima are reported every 120 frame events and at close; concurrent fields are explicitly
marked as live observations. Slow inputs and frames retain the token, timing interval and stage costs, bounded to the first eight
and subsequent powers of two. Slow-frame logging happens after all original comparisons and emission. Pixel values are never logged.

Batch cache-transition logging additionally records the whole invocation's elapsed ticks and their cumulative sum, with two time
reads per invocation and the existing bounded log schedule. This includes mapping work, preemption and potentially overlapping
invocations; it is not isolated cache-clean time. The accepted clean condition, clean/invalidate operation and attribute-update
ordering remain intact.

Target-header syntax checks pass for the decoder, kernel service and pmap timing source. The pmap check retains existing header and
include-directory warnings. The actual k213 log reader still returns the same picture records and integrity summary; there are no
new performance records in that older deployment. No model, fixture, unit test, component binary, kernel or image was produced.

### Observed diagnostic deadline interference and forward optimization

Native CoreMedia layer timing supplies a deadline reference beyond steady decoded-frame counts. In the k213 AVC steady-state
samples, three selected pictures are emitted roughly 174, 250 and 91 ms ahead of their layer times, while token 475 is emitted
about 13 ms late against the most recent native layer anchor. For that picture the FBD-to-emission interval is 99.25 ms: 13.37 ms
is the kernel full digest, 18.99 ms covers FBD through client batch dequeue, and the copy-to-final-locked-comparison interval is
66.43 ms. These intervals overlap and must not be added as disjoint costs. This establishes diagnostic interference for that
picture, not a unique explanation of every missing displayed frame. The retained native image-queue enqueue snapshots also contain
frames arriving ahead of playback; a late sample is not evidence that all decoded frames were late.

The pending digest and exact comparison implementations now issue four independent aligned 64-bit volatile reads from a view before
folding hashes or branching on equality. The FNV byte order, zero counts, every visible byte, all comparison phases, and mismatch
reporting remain present; padding and bytes beyond a row are not read. Exact differences are counted from the already-read words
in ascending byte order. No pixel-copy function, barrier, cache-clean condition or attribute-update order is changed. Reordering
observations changes their timing, so throughput and deadline benefit still require device evidence.

Slow-picture records additionally compare elapsed time with the current thread's CPU time. Cumulative scheduling observations report
read status, requested QoS and base/current priority without changing scheduling. CPU-time sampling has its own failure count; the
sampling interval and reporting overhead are not treated as free or as hardware decode time.

The actual k212 and k213 decoder binaries have also been extracted from their respective deployment payloads for comparison. They
are 77,200 and 94,992 bytes respectively; the earlier pre-rebuild decoder proof described k212 and is superseded by the deployed
k213 proof. Both extracted event-thread implementations contain no non-temporal load/store pair. This observation is separate from
checking the current, unbuilt source candidate.
