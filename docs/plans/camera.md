# Camera

## Route

The drivers follow the sources directly; there is no identification run.

- Rear camera: OV8856. libcamera's sensor helper states the OV8856 ships on sc7180-trogdor-coachz, and the rear camera is the 8 MP
  one. Mainline's OV8856 binding examples and the Snapdragon 845 mezzanine overlay put it at I2C address 0x10 with four data lanes;
  mainline's driver runs it from a 19.2 MHz clock, a camcc-sc7180.c MCLK rate. The ChromeOS node asks for 24 MHz for its own HAL;
  the mainline register tables are for 19.2 MHz, so 19.2 MHz is used.
- Front camera: the read-only probe on k224b found an OmniVision OV5675 at 0x36 (chip ID 0x5675 at 0x300a), so mainline's
  drivers/media/i2c/ov5675.c is its source.
- Kernel: `platform/coachz-prelink/drivers/RamBlock/camera/coachz_camera.c`, built into `libCoachzUsb.a` and started from
  `coachz_thread.c` (`coachz-prelink-camera.patch`) when `/chosen/coachz-camera` is set (`tools/xnu_screen.py --camera`). The order
  and values follow ChromeOS's camx driver (CSIPHY 1.2.2, PPI 170, CSID 170, VFE 170 top, RDI and bus ver2), mainline's
  camcc-sc7180.c, clk-alpha-pll.c, i2c-qcom-cci.c and ov8856.c. The register tables of ov8856.c (v6.12) and of camx's
  `cam_csiphy_1_2_2_hwreg.h` are GPL data: `tools/camera_tables.py` fetches the files pinned in `upstream.lock.json` (`linux-camera`,
  `chromiumos-camx`) into the ignored `upstream/camera`, checks their git blob hashes and generates `coachz_camera_tables.h` in the
  build copy only.
- iPadOS: UniXNU has no camera module. The viewer app (step 2) shows the camera; iPadOS's own capture stack is step 3, and the
  external camera (UVC) path is not available in this IPSW (see Next steps).

The earlier draft under `platform/coachz-prelink/pending/camera` ports GPL code and tables and is not used or committed.

## Board facts

Same-board ChromeOS kernel `3f8e1538e0851f3484cc16a1d5071803a6f32472` (Google open source), `sc7180-trogdor.dtsi`,
`sc7180-trogdor-coachz.dtsi`, `sc7180-trogdor-mipi-camera.dtsi`:

| | Front (UF) | Rear (WF) |
| --- | --- | --- |
| CCI | CCI0 master 0, GPIO17 SDA / GPIO18 SCL | CCI0 master 1, GPIO19 SDA / GPIO20 SCL |
| Sensor clock | CAM_CC_MCLK0, GPIO13, 24 MHz requested | CAM_CC_MCLK1, GPIO14, 24 MHz requested |
| Reset | GPIO30 (CAM_RESET0) | GPIO29 (CAM_RESET1) |
| Rails | GPIO6 high enables pp1800_uf_cam (1.8 V from always-on pp1800_ldo, 1 ms ramp) and pp2800_uf_cam (2.85 V from always-on pp3300_a) | GPIO7 the same for pp1800_wf_cam and pp2800_wf_cam (the rear VCM supply is pp2800_wf_cam) |
| CSIPHY | 0 | 1 |
| EEPROM (not a sensor address, not read) | 0x50 | 0x58 |

GPIO6 and GPIO7 are plain GPIOs, drive strength 2, no bias (external pull-down). The sensor nodes are generic `qcom,cam-sensor` and
name no part, address or lane count. A libcamera change states that the OV8856 ships on sc7180-trogdor-coachz, without the slot.

Mainline Linux has CAMCC for SC7180 and enables it for Coachz, but has no SC7180 CCI or CAMSS node.

## Camera hardware (ChromeOS `sc7180-camera.dtsi`, same commit)

Mainline Linux has no SC7180 CCI or CAMSS, and the Linux on this machine has no camera driver; ChromeOS ships this board's camera
with Qualcomm's camera kernel drivers, so their definitions are the source here.

| Block | Address | Interrupt | Clocks and rates |
| --- | --- | --- | --- |
| CCI0 (`qcom,cci`) | 0x0ac4a000, 0x1000 | SPI 468, rising edge | camnoc_axi, soc_ahb, slow_ahb_src, cpas_ahb, cci_0; cci_0_src 37.5 MHz (lowsvs) |
| CCI1 | 0x0ac4b000 | SPI 462 | the same with cci_1 |
| CSIPHY0/1/2/3 (`qcom,csiphy-v1.2.2`) | 0x0ac65000 / 0x0ac66000 / 0x0ac67000 / 0x0ac68000, 0x1000 | SPI 477 / 478 / 479 / 461 | cphy_rx_src 270 MHz (svs) or 360 MHz, csiN_phytimer_src 300 MHz |
| CSID0 (`qcom,csid170`) | 0x0acb3000, 0x1000 | SPI 464 | IFE_0 GDSC; ife_0_csid_src 270/360/480 MHz, ife_0_src 360/432/600 MHz, ife_0_cphy_rx, ife_0_axi |
| IFE0 (`qcom,vfe170`) | 0x0acaf000, 0x4000; CAMNOC 0x0ac42000, 0x5000 | SPI 465 | IFE_0 GDSC; ife_0_src 360/432/600 MHz, ife_0_axi |
| CPAS (`qcom,cam-cpas`, hw 0x150110) | 0x0ac40000, 0x1000; CAMNOC 0x0ac42000 | SPI 459 | TITAN_TOP GDSC; slow_ahb_src 80 MHz; CAMNOC AXI at least 3 GB/s; buses cam_hf_1, cam_hf_2, cam_sf_1 |
| Camera SMMU, IFE bank | stream IDs 0x820, 0x840, 0x860 | | IOVA 0x7400000, length 0xd8c00000 |

CCI0 timing at 37.5 MHz from ChromeOS (thigh, tlow, tsu_sto, tsu_sta, thd_dat, thd_sta, tbuf, stretch, trdhld, tsp):
100 kHz 201, 174, 204, 231, 22, 162, 227, 0, 6, 3; 400 kHz 38, 56, 40, 40, 22, 35, 62, 0, 6, 3; 1 MHz 16, 22, 17, 18, 16, 15, 24, 0, 3, 3.

The sensor address and part come from ChromeOS's camera HAL modules, which are not public. The rear part is the OV8856 (libcamera).

Other sources checked for the sensor address and lanes: the public trogdor board overlays (no camera HAL or sensor modules; those are
in a private overlay), the 2024 SC7180 CAMSS series on linux-media (written for an SM7125 phone, untested, no sensor nodes), the
libcamera OV8856 change (tested on an SM8150 phone, no Coachz address), and postmarketOS, now Nura (its wiki and GitLab sit behind a
bot challenge that automated tools may not pass). None gives Coachz's address or lane count, so the driver uses the closest source
values: 0x10 and four lanes, as in mainline's OV8856 examples. Nura's Coachz page (read in a browser) lists the camera as Broken and
names no part, address or lane count. The OV8856 answers at 0x10 or 0x36 depending on its SID pin, so the driver's probe checks the
chip identifier at 0x10 and then at 0x36, as any sensor driver checks its identifier before use.

## CAMCC (mainline `drivers/clk/qcom/camcc-sc7180.c`)

- PLL0, Fabia, offset 0x0: L 0x1f, alpha 0x4000 (600 MHz); CCI uses its even output.
- PLL2, Agera, offset 0x2000: L 0x64 (1920 MHz), config_ctl 0x20000800, config_ctl_hi 0x400003d2, test_ctl 0x04000400,
  test_ctl_hi 0x00004000, user_ctl 0x0000030f; the aux output divides by 4 (post-div field shift 8, width 2, value 3).
- MCLK0 RCG 0x4004, MCLK1 RCG 0x4024 (MND width 8, HID width 5); parent 1 is PLL2 aux. 24 MHz = PLL2 aux, pre-divider 10, M 1, N 2;
  19.2 MHz = XO.
- CCI0 RCG 0xb0d8; parent 6 is PLL0 even. 37.5 MHz = pre-divider 16.
- Branches (enable bit 0, halt-checked): MCLK0 0x401c, MCLK1 0x403c, CCI0 0xb0f0, CAMNOC AXI 0xb124, SOC AHB 0xb140, CPAS AHB 0xb11c.
- TITAN top GDSC 0xb134; no flags.

## Step 1: colour bar into memory

Written and checked on the host (kernel flags with `-Werror`, `tests/test_camera.py`); not run on hardware.

- Votes: cx at svs as consumer 6 of the shared aggregator; MM0 (qns_mem_noc_hf, the mmss hop of CPAS's cam_hf_1 path) with the
  display's share, 76 MB/s average and ChromeOS's 3 GB/s CAMNOC floor as peak (`coachz_camera_bus_vote`).
- Clocks: GCC camera AHB, XO and HF AXI; camcc PLL0 (600 MHz) and PLL3 (1080 MHz) with camcc-sc7180.c's configuration; slow AHB
  80 MHz, CCI 37.5 MHz, cphy_rx and CSID 270 MHz, PHY timer 300 MHz, IFE 360 MHz (ChromeOS's svs rates), MCLK1 19.2 MHz from XO;
  TITAN top and IFE 0 GDSCs.
- CCI0 master 1 with the v2 standard-mode timing at 37.5 MHz (mainline cci_v2_data, the same values as ChromeOS).
- OV8856: mainline's power order (clock, reset held 1-2 ms, GPIO7 rails with their 1 ms ramp, reset released, 1.8 ms), the chip
  identifier at 0x10 and then 0x36, the 360 Mbps PLL list, mode 1640x1232 four lanes (link 180 MHz, hts 3820, vts 1256), SGRBG10,
  mainline's control defaults and test pattern 1 ("Standard Color Bar").
- Receive side, started before the sensor streams as the ISP manager does: CSIPHY1 (lane enable 0xd5, settle count 24 from
  mainline's csiphy_settle_cnt_calc with the 300 MHz timer), PPI1 (DL0 and DL1), CSID0 (phy 1, lane_cfg 0x3210, RDI0 RAW10 payload
  only), VFE0 write master 0 in frame mode (width and stride 0xff01) into 0x84000000 and 0x84400000, the next address queued on each
  RDI0 start of frame with an RDI0 register update. The apps SMMU leaves the IFE stream IDs in bypass (sCR0.USFCFG clear), so the
  buffers are physical addresses; 0x84000000 to 0x86000000 is free low DRAM after boot.
- For 4 s the CSID, IFE and bus status registers are polled (no interrupts): CSID SOF and EOF, IFE SOF, write-master-0 done, RDI
  overflow, CSI2 errors and the PHY status are printed as `COACHZ camera` lines, then row 2 and the middle row of both buffers are
  compared (the bars are vertical), with the byte at the frame's end and the one after it. Everything is then powered down in reverse
  order and the pins are restored.

Not done in step 1: CAMNOC QoS (cpastop_v150_110.h), the camera SMMU bank, interrupts, the front sensor.

## Step 2: real images in an app

Written and checked on the host (kernel flags, `-Werror` for the app, `tests/test_camera.py`); not run on hardware.

- `/chosen/coachz-camera = 2` (`tools/xnu_screen.py --camera-service`): after the step-1 run everything is powered down, the GPIO7
  rails included. A client's start powers the whole path up again (votes, clocks, the sensor's power order, CCI, identification,
  CSIPHY, PPI, CSID, VFE) and streams without the colour bar into a ring of four 4 MiB buffers from 0x84000000 (ending at
  0x85000000, below mpss_mem); its stop or close powers everything down. The ring follows mainline camss-vfe-17x.c: two addresses
  are queued before the stream (each with an RDI0 register update), each write-master done completes the older one and queues the
  next (address and frame increment only). The earlier scheme queued at each start of frame and published the buffer queued last,
  which was the one the next frame was being written into: k222's tearing. Exposure lines and analog gain requested by the client
  are written between frames, within ov8856.c's ranges.
- `/chosen/coachz-camera` bit 0x100 (`--camera-front-probe`): before the rear run, a read-only chip ID probe of the front sensor on
  CCI0 master 0 (GPIO6 rails, MCLK0 on GPIO13 at 19.2 MHz from XO instead of the node's 24 MHz, which needs the Agera PLL2, reset
  GPIO30), at 0x36, 0x10 and 0x3c (OmniVision 0x300a), 0x20 (Hynix 0x0f16) and 0x37 (GalaxyCore 0xf0/0xf1, 8-bit registers),
  each read with the register width of the candidates at that address, then powered off with the pins restored.
- `/chosen/coachz-camera` bit 0x200 (`--camera-vcm-probe`, off unless asked for): one plain read at 0x0c on master 1 during the rear
  run, no register address written, to see whether the rear VCM answers there.
- `CoachzCamera.cpp`: the IOService CoachzCamera with NativeSandboxGrantProcess "*" and CoachzCameraUserClient (start, stop,
  status, exposure and gain, newest frame into a structure output). Frames are copied from their physical pages with
  `IOMemoryDescriptor::readBytes`, so neither a permanent kernel mapping nor a memory-mapping message is needed.
- `apps/camera/CoachzCamera.m`: the viewer. It copies the newest frame about fifteen times a second and develops it at full size:
  black level subtracted (estimated from the darkest green cells, shown on screen), grey-world white balance, bilinear SGRBG
  demosaic, sRGB gamma; exposure first and gain after move the linear mean towards 18 % grey. The sensor does no white balance
  (ov8856.c writes its MWB gains only as one digital gain), which is why k222's raw picture was green. A copy the ring overtook
  (three more frames complete during it) is taken again, and counted.
  `tools/coachz_apps.py` builds and signs it, delivers it in the addon with its CodeDirectory in the apps trust cache, the installer
  copies it into `/var/jb/Applications` and registers it, and `coachz-prelink-apps.patch` adds its path to the applications whose
  process tree skips the sandbox.

## Gaps

- The rear VCM: sc7180-trogdor-mipi-camera.dtsi has actuator_rear on CCI0 master 1 supplied by pp2800_wf_cam (GPIO7), and
  ChromeOS's model.yaml says the rear camera supports autofocus, but no public source gives its model or address (camx reads them
  from the module's binary configuration). Kukui's OV8856 module has a dongwoon,dw9768 at 0x0c (mt8183-kukui-krane.dtsi), another
  board. Until a 0x0c read (bit 0x200, only when approved) and a source agree, the lens is not driven and stays where its spring holds it.
- OV8856's black level has no value in mainline ov8856.c or libcamera's sensor helpers; the viewer estimates it.
- The camera LED: no node in the ChromeOS or mainline device trees, so it is wired to a rail; the boot log prints both enables as the
  firmware left them.

## Front camera (OV5675), planned

Board side from the table above: CCI0 master 0, CSIPHY0, MCLK0 on GPIO13, reset GPIO30, rails on GPIO6. Mainline ov5675.c (v6.12):
2 data lanes, 450 MHz link (900 Mbit/s a lane), 19.2 MHz XVCLK only (what the probe already gives MCLK0 from XO), SBGGR10 by the driver (GRBG in the buffer on hardware, below),
modes 2592x1944 and 1296x972 at 30 fps (HTS 1500, VTS 0x7e4). Power on: XVCLK, reset asserted, rails, at least 2 ms, reset
released, 1.5 ms before the first CCI transfer. The common register list, the 900 Mbit/s list and each mode's list follow; the
same CSID0/VFE0 path as the rear, with CSIPHY0 and 2 lanes instead of CSIPHY1 and 4. Written (2026-11-18), host-built only:
`tools/xnu_screen.py --camera-front` (`/chosen/coachz-camera` bit 0x800, with --camera or --camera-service) makes the whole run
drive the OV5675; its register lists come from the pinned ov5675.c through tools/camera_tables.py like the OV8856's. The status
selector returns two more values when asked for 12 (Bayer order, front), and the viewer and the capture plug-in move other Bayer orders onto
their GRBG development by starting one row later; the capture formats' crops scale from 1640x1232 to the sensor's size.

Implementation plan (coachz_camera.c is rear-only today: CSIPHY1, PPI1, CSID PHY select 1, MCLK1/GPIO14, GPIO7/GPIO29, CCI master 1,
OV8856 tables and 1640x1232 geometry are constants):
1. A sensor description chosen once per boot: CCI master and address, chip ID, pins (rail, reset, MCLK, SDA, SCL), MCLK RCG/CBCR,
   CSIPHY index (PA 0x0ac65000 + 0x1000 * n; CAMCC csiphy CBCR 0x5020 / 0x5044, phytimer RCG 0x5004 / 0x5028 and CBCR 0x501c /
   0x5040, checked against camcc-sc7180.c v6.12), PPI offset (0x200 * n), CSID PHY select n, lane count, link rate, width,
   height, VTS, Bayer order and the stream-on sequence. Rear: OV8856 as today. Front: OV5675 1296x972 (1.5 MiB a frame fits the
   4 MiB ring slots; 2592x1944 does not), GRBG as k225's colour bar shows.
2. `/chosen/coachz-camera` bit 0x800 selects the front sensor for the whole run (self-test, service, viewer and capture), so the
   first hardware test needs no switching between sensors at run time.
3. Geometry already crosses to the service and clients through coachz_camera_geometry(); add the Bayer order there and make
   CoachzCamera.cpp, the viewer and CoachzCapture.c take width, height and Bayer order from it instead of 1632x1224 / SGRBG.
4. Hardware points: chip ID 0x5675 and CSIPHY0 lane enable for 2 lanes; frames written with CSI2 errors 0; the viewer and the
   system camera show the front image.

## Next steps

3. iPadOS's own camera stack (AVCapture). The external camera (UVC) path is not in this IPSW: the OS volume's
   `/System/Library/DriverExtensions` holds fifteen DriverKit extensions and none for UVC, and neither the OS nor the SystemOS volume
   has a UVC component by name. The IPSW is the A10X iPad Pro's (Lightning), and iPadOS 17's external camera support is limited to the
   USB-C iPads. AVCapture here therefore needs a capture device of its own, behind the camera services the OS volume does start
   (`mediaserverd`, `dietapplecamerad`, `continuitycaptured`); until then the viewer app is how the camera is seen.
   This IPSW's built-in camera path is the A10X ISP: the kernelcache carries `com.apple.driver.AppleH9CameraInterface` (7.8.0), whose
   user client and ISP firmware protocol the capture stack above it speaks. UniXNU and the other ports record no camera work (umi:
   camera not implemented), so either that interface is answered for this sensor or a capture source is added where the capture stack
   enumerates devices. The 21H461 shared cache settles the shape: CMCapture names the kernel services `AppleH6CamIn`,
   `AppleH9CamIn`, `AppleH10CamIn`, `AppleH13CamIn` and `AppleH16CamIn` next to the frameworks `H9ISPServices` to `H16ISPServices`, and
   the A10X device's capture plugin is `/System/Library/MediaCapture/H9ISP.mediacapture`. The smaller route is the one the video decoder
   took: a kernel service that matches as `AppleH9CamIn`, and our own `H9ISP.mediacapture` plug-in, delivered the way
   `AVD.videodecoder` is, that implements the plug-in's interface over the CoachzCamera user client instead of the ISP firmware
   protocol.

   The original plug-in's interface (21H461 shared cache): CMCapture's `FigCaptureGetCaptureDeviceCreateFunction` dlopens the bundle of
   the `AppleH*CamIn` service present and looks up `H9ISPCaptureDeviceCreate(allocator, ?, &device)`, which makes the device with
   `CMDerivedObjectCreate` and `FigCaptureDeviceGetClassID()`. Its class tables:

   | Object | Table | Entries |
   | --- | --- | --- |
   | FigCaptureDevice | base, version 1, storage 0xf0 | Invalidate, Finalize, CopyProperty, SetProperty |
   | FigCaptureDevice | class, version 1 | ProcessingSessionCreate |
   | FigCaptureStream | base, version 1, storage 0x580 | Finalize, CopyProperty, SetProperty |
   | FigCaptureStream | class, version 2 | Start, Stop, AddAttachmentsToSampleBuffer |

   The stream's object table (`CMDerivedObjectCreate` with `FigCaptureStreamGetClassID()`) is `{0, base class, stream class}`; a
   `CMBaseClass` is `{version, derivedStorageSize, equal, invalidate, finalize, copyDebugDescription, copyProperty, setProperty,
   notificationBarrier, protocolTable}`; Start and Stop take only the stream.

   Which plug-in: CMCapture's table is `{service, dylib, symbol}` for `AppleH16CamIn`, `AppleH13CamIn`, `AppleH10CamIn`, `AppleH9CamIn`,
   `AppleH6CamIn` in that order, and the first service `IOServiceGetMatchingService(IOServiceMatching(name))` finds selects
   `dlopen("/System/Library/MediaCapture/H<n>ISP.mediacapture")` and `dlsym("H<n>ISPCaptureDeviceCreate")`. The bundle path is a flat
   dylib. The shared cache holds only `H9ISP.mediacapture`, and dyld takes a cached image over a file at the same path, so our plug-in is
   `H10ISP.mediacapture` (not in the cache, read from disk) behind a kernel nub of class `AppleH10CamIn`, which CMCapture tries before
   `AppleH9CamIn`. `/System/Library/MediaCapture` is on neither volume. The capture host is `mediaserverd`.

   Properties the original answers (its static tables, filled by `__GLOBAL__sub_I_H9ISPCaptureDevice.cpp`, entries
   `{key, type, setter, copier, flags}`): the device answers `SupportedPropertiesDictionary`, `Clock`, `StreamArray`,
   `FirmwareVersion`, `MemoryPoolID` (set) and `SupportedSynchronizedStreamsGroups`; the stream 143 keys, of which the shape-giving ones
   are `BufferQueue`, `StillImageBufferQueue`, `SupportedFormatsArray`, `FormatDescription`, `FormatIndex`, `Minimum`/`MaximumFrameRate`,
   `PortType`, `ModuleInfo` (`SensorID`, `ModuleLensID`, `ModuleIntegratorID`, `ModuleActuatorID`, `ModuleFocusID`), `FocalLength`,
   `PixelSize`, `LensFNumber`, `StillImageCaptureNow`, `TorchLevel` and `SupportedPropertiesDictionary`; the rest are 3A, metadata and
   ISP-specific controls. Unknown keys return -12784. `SupportedFormatsArray` is a CFArray of `CMVideoFormatDescription('420v', w, h)`
   (and `'yuvs'`) with no extensions.

   Frames: each stream owns two `CMBufferQueue`s (`CMBufferQueueGetCallbacksForUnsortedSampleBuffers`, no capacity limit), handed out by
   `BufferQueue` and `StillImageBufferQueue`. The frame callback wraps the ISP's `CVPixelBuffer` with
   `CMVideoFormatDescriptionCreateForImageBuffer` and `CMSampleBufferCreateForImageBuffer` and calls `CMBufferQueueEnqueue`; no
   frame notification is posted (CMCapture installs its own trigger on the queue). The only frame-path notification is
   `FrameDroppedFromQueue` with the presentation time.

   CMCapture's side (`BWFigVideoCaptureStream initWithCaptureStream:`) reads `ModuleInfo`, `FocalLength`, `PixelSize`, `LensFNumber`,
   `SensorDimensions`, `CameraViewMatrix` and calibration radii, and looks the sensor up in `CMCapture.framework/J120/CameraSetup.plist`
   (`DefaultSensorIDs`: back `0x0303`, front `0x0314`). Which of those it requires is decided on hardware: the plug-in reports every key
   it is asked for and what it returned through the kernel log.

   Implementation (built on the host; not run on hardware):
   - `package/CoachzCapture.c` is `H10ISP.mediacapture`: one device with one rear stream, the original's object layout, CoreMedia
     and CMCapture symbols resolved with `dlsym` in the capture host. Formats are 420v at 1632x1224 (the sensor less a 4-pixel border),
     1280x720 (16:9 from the middle) and 640x480. Start opens the CoachzCamera user client (waiting while the service is not yet
     published), and a thread copies each new RAW10 frame, develops it as the viewer does, fills an IOSurface-backed pixel buffer from
     a pool of the selected format, wraps it in a sample buffer stamped with host time and enqueues it (at most six waiting);
     `StillImageCaptureNow` also puts the next frame on the still queue. Sets of keys it does not implement succeed and change nothing.
     Each (object, operation, key) is reported once as a `COACHZ capture:` line through user client selector 5, and every 300th frame
     with exposure, gain, black level and white balance.
   - `drivers/RamBlock/camera/CoachzCamera.cpp` adds selector 5 and the empty `AppleH10CamIn` nub.
   - `coachz-prelink-capture.patch` (only with `/chosen/coachz-camera` bit 0x400, `tools/xnu_screen.py --camera-capture`, which needs
     `--camera-service`) writes the plug-in to `/private/var/coachz/capture/system/MediaCapture`, the platform files to
     `/private/var/coachz/capture/cmcapture/J120` and `J121`, mounts both read-only as bindfs unions over `/System/Library` and
     `CMCapture.framework`, checks each new file through the union and one of the covered directory's own files
     (`CoreServices/SystemVersion.plist`, `CMCapture.framework/Info.plist`), unmounts a union that fails either, and publishes the nub
     last. It prints one line with the stage and status. The kernel parts pass a syntax check with the kernel's own compile record.
   - `scripts/build-coachz-capture.py` signs the plug-in, writes its trust cache module (`build/coachz-capture/trustcache.bin`, passed with
     `--root-trust-cache`) and derives `AVCaptureSession.plist` from the OS volume (`COACHZ_OS_ROOT`): the rear camera's
     `FigCaptureStreamFormats` become the plug-in's three (`Photo`, `Video-BES-30`, `Video-4by3-Low-Resolution`), its device formats
     `12MP`, `720p60` and `640x480` keep their names at the plug-in's sizes with HDR, stabilization, Live Photo and night mode removed,
     Medium and Low move to 640x480, High to 1280x720, and presets on dropped formats go. J120 and J121 hold the same original.

## On hardware

- The CCI has to be initialised after the sensor is powered, as camx does (`cam_sensor_power_up`: `cam_sensor_core_power_up`, then
  `camera_io_init` and `cam_cci_init`). Reset and pins set up before the rails came up left the bus in a state where the first transfer
  timed out. Reads use camx's single-queue form: SET_PARAM with 3 retries, LOCK, WRITE_DISABLE_P with the register address, READ,
  UNLOCK.
- The OV8856 answers at 0x10 with chip id 0x00885a. The self-test then counts 120 start-of-frames and 119 completed writes in 4 s
  (30 fps), with no RDI overflow, and the service publishes CoachzCamera.
- CSI2 RX interrupt bits 0-10 are information (EOT/SOT captured per lane, packets captured), not errors (camx
  `cam_ife_csid_core.h`); the error mask is bits 11-13, 15-24 and 26.
- The viewer installs on the home screen, but AMFI rejects its ad hoc signature ("unsuitable CT policy 0") although its cdhash is in
  the image's trust cache, so it exits at launch.

## System camera on hardware (k225, k226)

- k225: mediaserverd threw "-[__NSCFType objectForKeyedSubscript:]" at launch (csu_resolveFormatIndexForNamedStreamFormat reads each
  SupportedFormatsArray entry as a dictionary). k226 sends dictionaries (FormatDescription, SensorWidth/Height, frame rates, field of
  view, VideoIsBinned): mediaserverd stays up and Magnifier and Measure list the rear camera ("后置相机").
- k226: "Fig assert: pixelFormatIsSupported" right after "Resolving formats for Back Camera", then "CaptureSource Back Camera is missing
  formats (720p60, 640x480, 12MP)", and Magnifier cannot add the input ("does not support AVCaptureSessionPresetHigh"). The plug-in
  never received a stream start.
- What is missing: CMCapture drives an ISP stream by outputs. A format names its outputs (kFigCaptureStreamVideoFormatKey_SupportedOutputs:
  kFigCaptureStreamVideoOutputID_PrimaryScaler, StillImage, Raw, ...) and each output's kFigCaptureStreamVideoOutputKey_SupportedPixelFormats;
  the session then sends output configurations (kFigCaptureStreamVideoOutputConfigurationKey_Width/Height/PixelFormat/BufferPool...) and
  receives frames through output handlers (kFigCaptureStreamVideoOutputHandlerKey_PixelBufferHandler ...), not through a buffer queue.
  The plug-in implements none of that.
- The viewer: AMFI refused it while nine trust cache modules were loaded (k224b, k225); with the audio, Venus and capture caches merged
  into one (eight in all, k226) it runs. The front image was grey because the buffer is GRBG, not ov5675.c's SBGGR10 (k225's colour
  bar); the fix is not yet on hardware.

