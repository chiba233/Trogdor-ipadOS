# Camera

## Route

The drivers follow the sources directly; there is no identification run.

- Rear camera: OV8856. libcamera's sensor helper states the OV8856 ships on sc7180-trogdor-coachz, and the rear camera is the 8 MP
  one. Mainline's OV8856 binding examples and the Snapdragon 845 mezzanine overlay put it at I2C address 0x10 with four data lanes;
  mainline's driver runs it from a 19.2 MHz clock, a camcc-sc7180.c MCLK rate. The ChromeOS node asks for 24 MHz for its own HAL;
  the mainline register tables are for 19.2 MHz, so 19.2 MHz is used.
- Front camera: no source names its part; it follows once a source does.
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
   protocol. The plug-in interface still has to be read from the original bundle.

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
