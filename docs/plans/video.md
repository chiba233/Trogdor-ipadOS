# Hardware video decode (Venus on SC7180)

Every hardware step follows mainline Linux's latest stable release (7.3.1 when this was written; check kernel.org, do not pin to an
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
| Session init, buffer requirements, load resources, start, sequence-change event | Runs on hardware (k206, the earlier fixed-region self-test) |
| Decoder engine: `vdec_start_output`, `vdec_event_change` (incl. resolution change), `vdec_start_capture`, `vdec_decoder_cmd`, flush, `vdec_session_release` | Written, not run on hardware |
| Per-buffer memory: contiguous, write-combined, mapped into the bank at a first-fit IOVA below the shared queues, unmapped on free | Written, not run on hardware |
| Interrupts (SPI 174, `venus_isr` / `venus_isr_thread`, 10 ms poll fallback) | Written, not run on hardware |
| Clock scaling (`load_scale_v4`, the five OPPs with their CX corners, Fabia rate change and the RCG divider) and the bandwidth table | Written, not run on hardware |
| Boot self-test through the engine (one I_PCM IDR picture, compared sample by sample) | Written, not run on hardware |
| IOKit service `CoachzVenus` and user client, ABI `CoachzVideoAbi.h` | Written, not run on hardware |
| VideoToolbox decoder `AVD.videodecoder` (`avc1`, `hvc1`, `hev1`, `vp09`) and its delivery (bindfs union, trust cache, kobject map grant) | Written, not run on hardware |
| Power collapse (`venus_suspend_3xx`/4xx), `cpu-cfg` vote, system-error recovery (`venus_sys_error_handler`) | Not written: the core stays powered at its OPP; a SYS_ERROR or watchdog fails every session until reboot |
| 10-bit output (P010) | Not written: a 10-bit sequence fails its session |

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
  `fw_min_cnt` (or the OUTPUT2 hold count) plus four, `get_framesize_raw_nv12` each; the client maps them write-combined.
- Input: at least eight buffers of `venus_helper_get_framesz` for the compressed size (or the firmware's), the token as the HFI
  timestamp, which the firmware carries to the picture it decodes.
- ABI (`CoachzVideoAbi.h`): methods Create, QueueInput, ReturnOutput, Drain, Flush, WaitEvent, Destroy; memory types 0x100 + i (input)
  and 0x200 + i (output of the current generation); events InputDone, Outputs, Frame, Drained, Flushed, Error.
- Plug-in (`CoachzVideoDecoder.c`): `AVDRegister` registers `avc1`, `hvc1`, `hev1`, `vp09`. StartSession reads the size and parameter
  sets (`CMVideoFormatDescriptionGet{H264,HEVC}ParameterSetAtIndex`), opens the service and sets 420v pool attributes. DecodeFrame
  converts length-prefixed NAL units to Annex B (parameter sets before the first sample), queues it and returns asynchronously. An event
  thread copies each picture's crop rectangle into a pool pixel buffer with the format's colour attachments, returns the capture buffer
  and emits the frame outside its lock; Drained, Flushed and errors emit the rest as dropped. FinishDelayedFrames drains and waits.
- Delivery: `coachz-prelink-video-decoder.patch` writes the dylib to `/private/var/coachz/video` and mounts it read-only as a bindfs union
  over `/System/Library/VideoDecoders` before launchd; `build-coachz-venus.py` signs it and writes its trust cache module (passed with
  `--root-trust-cache`); `coachz-prelink-gpu-map-grant.patch` lets `CoachzVenusUserClient` mappings past the sandbox's kobject filter, and
  the service grants every process (`NativeSandboxGrantProcess` `*`).
