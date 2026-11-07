// SPDX-License-Identifier: BSD-2-Clause-Patent
// The video codec (Venus "AR50", HFI 4xx) on SC7180: power, firmware and the host firmware interface, brought up by the kernel itself.
//
// Order and values are mainline Linux's (facts only, the code is written here): drivers/media/platform/qcom/venus (core.c sc7180_res,
// venus_probe and venus_runtime_resume; pm_helpers.c core_power_v4, poweron_coreid and vcodec_control_v4; firmware.c venus_load_fw,
// venus_boot_no_tz and venus_reset_cpu; hfi_venus.c venus_interface_queues_init, venus_run, venus_boot_core, venus_core_init and
// its queue reads and writes; hfi_cmds.c the SYS packets), drivers/clk/qcom/videocc-sc7180.c (video_pll0, video_cc_venus_clk_src,
// the branches and both GDSCs), clk-alpha-pll.c (Fabia configure and enable), gdsc.c, gcc-sc7180.c (the always-on video AHB and XO
// branches), drivers/soc/qcom/mdt_loader.c, and the device tree Debian runs on this machine (sc7180.dtsi: video-codec@aa00000, its
// video-firmware child, memory@8f600000, iommus <&apps_smmu 0xc00 0x60> and <0xc42 0>).
//
// Trogdor's TF-A offers no PAS calls, which is why its device tree has the video-firmware child: Linux then boots the firmware
// without TrustZone (venus_boot_no_tz), mapping the 5 MiB firmware region at device address 0 in a domain of its own and releasing
// the ARM9 from reset. Here both of the codec's stream groups go to one context bank of the apps SMMU, next to the USB controller's
// (CoachzUsbHost.cpp enables the SMMU and keeps every unmatched stream bypassing): the firmware at IOVA 0, privileged and executable,
// the shared queues higher up. The codec does not snoop (no dma-coherent): the CPU uses write-combined views of both regions.
//
// It starts only when tools/linux_log.py put venus.mbn into the firmware blob (coachz_wifi.c keeps it): no boot argument.
//
// Above the bring-up the kernel runs mainline's stateful decoder itself (vdec.c vdec_start_output, vdec_event_change,
// vdec_start_capture, vdec_decoder_cmd, vdec_stop_capture/_output, vdec_session_release, vdec_buf_done; helpers.c intbufs, DPB and
// framesz; hfi_msgs.c event_seq_changed and the buffer-done parsers; hfi_venus.c venus_isr and venus_isr_thread; pm_helpers.c
// load_scale_v4 with sc7180's OPP and bandwidth tables, videocc-sc7180.c's venus_clk_src table and clk-alpha-pll.c's Fabia rate
// change), one session per user client of the CoachzVenus service (CoachzVideoAbi.h), which the VideoToolbox decoder
// (CoachzVideoDecoder.c) opens. Every buffer is its own contiguous allocation, mapped into the bank at an IOVA below dma_mask.
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <string.h>
#include "ArmSmmu4KLive.h"
#include "CoachzVideoAbi.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t physical, vm_size_t size);
extern "C" unsigned int pmap_cache_attributes(ppnum_t pn);
extern "C" void pmap_set_cache_attributes(ppnum_t pn, unsigned int cacheattr);
extern "C" void dcache_incoherent_io_flush64(addr64_t pa, unsigned int count, unsigned int remaining, unsigned int *res);
extern "C" int coachz_rpmh_cx_request(unsigned consumer, unsigned level);          // coachz_wifi.c: cx.lvl aggregated over consumers
extern "C" int coachz_venus_bus_vote(uint64_t avg_kbps, uint64_t peak_kbps);       // coachz_wifi.c: MM1 and MM2
extern "C" uint8_t *volatile coachz_venus_fw;                                       // coachz_wifi.c: venus.mbn, SHA-256 checked
extern "C" size_t coachz_venus_fw_bytes;
extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);   // ArmCpu.cpp, IOReturn
extern "C" int coachz_venus_assert_wait_deadline(void *, int, uint64_t) __asm__("_assert_wait_deadline");
extern "C" int coachz_venus_thread_wakeup(void *, int, int) __asm__("_thread_wakeup_prim");
extern "C" int coachz_venus_thread_block(void *) __asm__("_thread_block");
extern "C" void coachz_venus_static_init(void) __asm__("__GLOBAL__sub_I_CoachzVenus.cpp");
extern kmod_info_t g_kernel_kmod_info;

namespace {

void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log(const char *fmt, ...) {
  char line[224]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
  printf("COACHZ venus: %s\n", line);
}
void barrier() { __asm__ volatile("dsb sy" ::: "memory"); }
// Every bring-up step is announced (k206 ran all of them on hardware).
void step(const char *what) { printf("COACHZ venus: step: %s\n", what); }

// ---- addresses (sc7180.dtsi) ----
constexpr uint64_t kVenusBase = 0x0aa00000, kVenusBytes = 0xff000;     // video-codec@aa00000
constexpr uint64_t kVideoCcBase = 0x0ab00000, kVideoCcBytes = 0x10000;  // clock-controller@ab00000 (sc7180-videocc)
// ARM's io_submap is 32 MiB (coachz-prelink-io-map.patch) and the other drivers' permanent windows already hold about 24 MiB of it,
// with the Wi-Fi thread's 8 MiB map among them: k200 mapped 12 MiB here (both memory regions, all of GCC, the whole SMMU) and that
// later map panicked in io_map. So only the pages used are mapped (about 1.1 MiB), and the two memory regions are kernel memory.
constexpr uint64_t kGccPage = 0x0010b000;                               // GCC's video branches, one page
constexpr uint64_t kSmmuBase = 0x15000000, kSmmuBytes = 0x100000;       // apps_smmu, qcom,sc7180-smmu-500
// The firmware region: the size of memory@8f600000 (venus_load_fw maps it whole at VENUS_FW_START_ADDR 0, mapped_mem_size its size).
// Without TrustZone nothing ties it to that address; the codec sees only IOVA 0, so it is contiguous kernel memory. kFwRegionBase
// is the device tree's base, where a non-relocatable image would be linked.
constexpr uint64_t kFwRegionBase = 0x8f600000, kFwBytes = 0x500000;
constexpr uint64_t kSharedBytes = 0x300000;   // the shared queues, SHARED_QSIZE
constexpr uint64_t kSharedIova = 0xc0000000;   // below sc7180_res.dma_mask (0xe0000000 - 1)

// ---- GCC: the video AHB and XO branches gcc-sc7180.c keeps on (qcom_branch_set_clk_en) ----
constexpr uint32_t kGccVideoAhb = 0x0b004 - 0xb000, kGccVideoXo = 0x0b028 - 0xb000;   // in the mapped page

// ---- videocc-sc7180.c ----
constexpr uint32_t kPll = 0x42c;   // video_pll0, Fabia: MODE +0, L +4, USER_CTL +0xc, USER_CTL_U +0x10, STATUS +0x24, OPMODE +0x2c, FRAC +0x38
constexpr uint32_t kPllMode = kPll, kPllL = kPll + 0x04, kPllUser = kPll + 0x0c, kPllUserU = kPll + 0x10, kPllOpmode = kPll + 0x2c,
                   kPllFrac = kPll + 0x38;
constexpr uint32_t kPllOutctrl = 1u << 0, kPllResetN = 1u << 2, kPllVoteFsm = 1u << 20, kPllUpdateBypass = 1u << 23,
                   kPllLockDet = 1u << 31, kPllOutMask = 0x7;
constexpr uint32_t kVenusRcgCmd = 0x7f0, kVenusRcgCfg = 0x7f4;   // video_cc_venus_clk_src, hid_width 5, parent 1 = video_pll0
constexpr uint32_t kVideoCcXo = 0x984;                           // VIDEO_CC_XO_CLK, kept always on by the probe
constexpr uint32_t kCtlCore = 0x850, kAhb = 0xa4c, kCtlAxi = 0x9cc, kVcodec0Core = 0x890, kVcodec0Axi = 0x9ec;
constexpr uint32_t kVenusGdsc = 0x814, kVcodec0Gdsc = 0x874;     // vcodec0_gdsc has HW_CTRL
constexpr uint32_t kGdscPwrOn = 1u << 31, kGdscHwControl = 1u << 1, kGdscSwCollapse = 1u << 0;

// ---- the codec's own registers (hfi_venus_io.h, HFI 4xx: CPU_CS at 0xd2000, CPU_IC at 0xdf000, the wrapper at 0xe0000) ----
constexpr uint32_t kCpuCs = 0xd2000, kCpuIc = 0xdf000, kWrapper = 0xe0000;
constexpr uint32_t kA2hSoftIntClr = kCpuCs + 0x1c, kCtrlInit = kCpuCs + 0x48, kSciaCmdArg0 = kCpuCs + 0x4c, kSciaCmdArg1 = kCpuCs + 0x50,
                   kSciaCmdArg2 = kCpuCs + 0x54, kSfrAddr = kCpuCs + 0x5c, kUcRegionAddr = kCpuCs + 0x64, kUcRegionSize = kCpuCs + 0x68;
constexpr uint32_t kSoftInt = kCpuIc + 0x18, kSoftIntH2a = 1u << 15;
constexpr uint32_t kHwVersion = kWrapper + 0x00, kIntrStatus = kWrapper + 0x0c, kIntrMask = kWrapper + 0x10, kIntrClear = kWrapper + 0x14,
                   kIntrMaskA2hVcodec = 0x8, kVcodec0PowerStatus = kWrapper + 0x90, kVcodec0PowerControl = kWrapper + 0x94;
constexpr uint32_t kCpaStart = kWrapper + 0x1020, kCpaEnd = kWrapper + 0x1024, kFwStart = kWrapper + 0x1028, kFwEnd = kWrapper + 0x102c,
                   kNonpixStart = kWrapper + 0x1030, kNonpixEnd = kWrapper + 0x1034, kCpuClockConfig = kWrapper + 0x2000,
                   kCpuCgcDis = kWrapper + 0x2010, kCpuStatus = kWrapper + 0x2014, kA9ssSwReset = kWrapper + 0x3000;

// ---- HFI (hfi_venus.c, hfi_cmds.h, hfi_msgs.h, hfi_helper.h) ----
constexpr uint32_t kQueueCount = 3, kCmdQ = 0, kMsgQ = 1, kDbgQ = 2;
constexpr uint32_t kQueueBytes = 1024 * 50 * 16;                  // IFACEQ_QUEUE_SIZE
constexpr uint32_t kTableHeader = 24, kQueueHeader = 56;          // hfi_queue_table_header, hfi_queue_header
constexpr uint32_t kTableBytes = kTableHeader + kQueueHeader * kQueueCount;
constexpr uint32_t kQueuesBytes = (kTableBytes + kQueueBytes * kQueueCount + 0xfff) & ~0xfffu;   // ALIGNED_QUEUE_SIZE
constexpr uint32_t kSfrOffset = kQueuesBytes, kSfrBytes = 0x1000;
constexpr uint32_t kSharedQSize = kSharedBytes;                   // SHARED_QSIZE: ALIGN(SFR + queues + QDSS, 1 MiB)
static_assert(kSfrOffset + kSfrBytes <= kSharedBytes, "the SFR inside the shared region");
constexpr uint32_t kDefaultQhdr = 0x01010000;                     // IFACEQ_DFLT_QHDR
constexpr uint32_t kCmdSysInit = 0x10001, kCmdSysSetProperty = 0x10005, kCmdSysGetProperty = 0x10006, kArchOx = 0x1;
constexpr uint32_t kMsgSysInit = 0x20001, kMsgSysDebug = 0x20004, kMsgSysPropertyInfo = 0x2000a, kMsgEventNotify = 0x21001;
constexpr uint32_t kPropDebugConfig = 0x1, kPropPowerPlane = 0x5, kPropImageVersion = 0x6;
constexpr uint32_t kDebugModeQueue = 0x01, kDebugError = 0x08, kDebugFatal = 0x10;

// ---- windows ----
volatile uint8_t *gVenus, *gVideoCc, *gGcc, *gFw, *gShared;
uint32_t rd(volatile uint8_t *base, uint32_t at) { return *reinterpret_cast<volatile uint32_t *>(base + at); }
void wr(volatile uint8_t *base, uint32_t at, uint32_t v) { barrier(); *reinterpret_cast<volatile uint32_t *>(base + at) = v; barrier(); }
void set_bits(volatile uint8_t *base, uint32_t at, uint32_t mask, uint32_t v) { wr(base, at, (rd(base, at) & ~mask) | (v & mask)); }
// Polls until (value & mask) == want, at most budget microseconds; the number of polls taken, 0 when it never came.
unsigned poll(volatile uint8_t *base, uint32_t at, uint32_t mask, uint32_t want, unsigned budget_us) {
  for (unsigned i = 1; i <= budget_us; ++i) { if ((rd(base, at) & mask) == want) return i; IODelay(1); }
  return 0;
}

// ---- power: GCC branches, PLL, RCG, GDSCs, branch clocks (videocc-sc7180.c, clk-alpha-pll.c, gdsc.c, clk-branch.c) ----
bool branch_on(volatile uint8_t *base, uint32_t cbcr, const char *name) {
  set_bits(base, cbcr, 1u, 1u);
  // clk_branch2: CLK_OFF (bit 31) clears once the branch runs; BRANCH_HALT checks it.
  const unsigned polls = poll(base, cbcr, 1u << 31, 0, 500);
  if (!polls) { log("%s did not start: CBCR 0x%x", name, rd(base, cbcr)); return false; }
  return true;
}
bool gdsc_on(uint32_t gdscr, bool hw_control, const char *name) {
  // gdsc_init: the wait values, then gdsc_toggle_logic: SW_COLLAPSE clear, PWR_ON within TIMEOUT_US.
  uint32_t v = rd(gVideoCc, gdscr);
  v = (v & ~(0xfu << 20 | 0xfu << 16 | 0xfu << 12)) | 0x2u << 20 | 0x8u << 16 | 0x2u << 12;
  wr(gVideoCc, gdscr, v);
  if (!(rd(gVideoCc, gdscr) & kGdscPwrOn)) {
    set_bits(gVideoCc, gdscr, kGdscSwCollapse, 0);
    if (!poll(gVideoCc, gdscr, kGdscPwrOn, kGdscPwrOn, 500)) { log("%s did not power on: GDSCR 0x%x", name, rd(gVideoCc, gdscr)); return false; }
  }
  // gdsc_enable: clocks must not start within 400 ns of the power-on.
  IODelay(1);
  if (hw_control) set_bits(gVideoCc, gdscr, kGdscHwControl, kGdscHwControl);
  log("%s on: GDSCR 0x%x", name, rd(gVideoCc, gdscr));
  return true;
}
bool pll_on() {
  const uint32_t mode = rd(gVideoCc, kPllMode), opmode = rd(gVideoCc, kPllOpmode);
  log("video_pll0 as found: MODE 0x%x OPMODE 0x%x L 0x%x FRAC 0x%x USER 0x%x 0x%x", mode, opmode, rd(gVideoCc, kPllL),
      rd(gVideoCc, kPllFrac), rd(gVideoCc, kPllUser), rd(gVideoCc, kPllUserU));
  if (mode & kPllVoteFsm) { log("video_pll0 is in FSM vote mode; not changed"); return false; }
  if ((opmode & 1) && (mode & kPllOutctrl)) return true;   // alpha_pll_fabia_enable: already running
  // clk_fabia_pll_configure with video_pll0_config: L 0x1f, alpha 0x4000 (19.2 MHz * 31.25 = 600 MHz), USER_CTL 1, USER_CTL_U 0x4805;
  // zero fields are not written. Then UPDATE_BYPASS and RESET_N.
  wr(gVideoCc, kPllL, 0x1f);
  wr(gVideoCc, kPllFrac, 0x4000);
  wr(gVideoCc, kPllUser, 0x00000001);
  wr(gVideoCc, kPllUserU, 0x00004805);
  set_bits(gVideoCc, kPllMode, kPllUpdateBypass, kPllUpdateBypass);
  set_bits(gVideoCc, kPllMode, kPllResetN, kPllResetN);
  // alpha_pll_fabia_enable: output off, STANDBY, RESET_N, RUN, lock, outputs on, OUTCTRL.
  set_bits(gVideoCc, kPllMode, kPllOutctrl, 0);
  wr(gVideoCc, kPllOpmode, 0);
  set_bits(gVideoCc, kPllMode, kPllResetN, kPllResetN);
  wr(gVideoCc, kPllOpmode, 1);
  const unsigned polls = poll(gVideoCc, kPllMode, kPllLockDet, kPllLockDet, 1000);
  if (!polls) { log("video_pll0 did not lock: MODE 0x%x", rd(gVideoCc, kPllMode)); return false; }
  set_bits(gVideoCc, kPllUser, kPllOutMask, kPllOutMask);
  set_bits(gVideoCc, kPllMode, kPllOutctrl, kPllOutctrl);
  log("video_pll0 locked in %u us: MODE 0x%x", polls, rd(gVideoCc, kPllMode));
  return true;
}
// video_cc_venus_clk_src at 150 MHz: ftbl F(150000000, P_VIDEO_PLL0_OUT_MAIN, 4, 0, 0) with the PLL at its configured 600 MHz, so no
// PLL rate change: CFG source 1 (parent map) at bits 10:8, divider 2 * 4 - 1 at bits 4:0, then CMD_RCGR UPDATE until it clears.
// ROOT_OFF (CMD bit 31) stays set until a branch below asks for the clock: clk_rcg2 never waits on it (k200 read CMD 0x80000000 here).
bool rcg_150() {
  wr(gVideoCc, kVenusRcgCfg, (rd(gVideoCc, kVenusRcgCfg) & ~(0x7u << 8 | 0x1fu)) | 1u << 8 | 7u);
  set_bits(gVideoCc, kVenusRcgCmd, 1u, 1u);
  const unsigned polls = poll(gVideoCc, kVenusRcgCmd, 1u, 0, 500);
  log("venus_clk_src: CMD 0x%x CFG 0x%x after %u polls", rd(gVideoCc, kVenusRcgCmd), rd(gVideoCc, kVenusRcgCfg), polls);
  return polls != 0;
}

// RPMH_REGULATOR_LEVEL_LOW_SVS: the venus OPP for 150 MHz (sc7180.dtsi opp-150000000, required-opps rpmhpd_opp_low_svs).
constexpr unsigned kCxConsumer = 5, kCxLowSvs = 64;

bool power_on() {
  // gcc-sc7180.c keeps GCC_VIDEO_AHB_CLK and GCC_VIDEO_XO_CLK on; without the AHB branch no videocc register answers.
  log("GCC video AHB 0x%x XO 0x%x as found", rd(gGcc, kGccVideoAhb), rd(gGcc, kGccVideoXo));
  set_bits(gGcc, kGccVideoAhb, 1u, 1u);
  set_bits(gGcc, kGccVideoXo, 1u, 1u);
  IODelay(10);
  log("videocc as found: venus GDSCR 0x%x vcodec0 GDSCR 0x%x, ctl core 0x%x ahb 0x%x ctl axi 0x%x, RCG 0x%x 0x%x", rd(gVideoCc, kVenusGdsc),
      rd(gVideoCc, kVcodec0Gdsc), rd(gVideoCc, kCtlCore), rd(gVideoCc, kAhb), rd(gVideoCc, kCtlAxi), rd(gVideoCc, kVenusRcgCmd),
      rd(gVideoCc, kVenusRcgCfg));
  set_bits(gVideoCc, kVideoCcXo, 1u, 1u);
  // venus_runtime_resume: icc_set_bw(video-mem, 20000 kB/s), icc_set_bw(cpu-cfg, 1000) (CN0 is kept on); then the cx performance state.
  const int bus = coachz_venus_bus_vote(20000, 20000);
  const int cx = coachz_rpmh_cx_request(kCxConsumer, kCxLowSvs);
  log("bus vote %d, cx low_svs vote %d", bus, cx);
  step("video_pll0");
  if (!pll_on()) return false;
  step("venus_clk_src");
  if (!rcg_150()) return false;
  // core_power_v4: the venus domain, then core, iface and bus (sc7180_res.clks).
  step("venus_gdsc");
  if (!gdsc_on(kVenusGdsc, false, "venus_gdsc")) return false;
  step("venus core, ahb and axi branches");
  return branch_on(gVideoCc, kCtlCore, "venus_ctl_core") && branch_on(gVideoCc, kAhb, "venus_ahb") &&
         branch_on(gVideoCc, kCtlAxi, "venus_ctl_axi");
}
// poweron_coreid(VIDC_CORE_ID_1) for a session: the vcodec0 domain (HW_CTRL), the wrapper's power request on, its clocks, then the
// request handed back (vcodec_control_v4: 0 asks for power and waits for status bit 1, 1 releases it and waits for bit 1 to clear).
bool vcodec0_on() {
  if (!gdsc_on(kVcodec0Gdsc, true, "vcodec0_gdsc")) return false;
  wr(gVenus, kVcodec0PowerControl, 0);
  const unsigned up = poll(gVenus, kVcodec0PowerStatus, 1u << 1, 1u << 1, 100);
  const bool clocks = branch_on(gVideoCc, kVcodec0Core, "vcodec0_core") && branch_on(gVideoCc, kVcodec0Axi, "vcodec0_axi");
  wr(gVenus, kVcodec0PowerControl, 1);
  const unsigned handed = poll(gVenus, kVcodec0PowerStatus, 1u << 1, 0, 100);
  log("vcodec0: power request %u polls, clocks %d, handed back %u polls, status 0x%x", up, clocks, handed, rd(gVenus, kVcodec0PowerStatus));
  return up && clocks && handed;
}

// ---- the context bank on the apps SMMU (arm-smmu.c, as CoachzUsbHost.cpp does for the DWC3) ----
// The table pool: on-demand leaf tables, one per 2 MiB of IOVA a buffer touches (a 4K session's DPB alone spans about 300 MiB).
constexpr size_t kPage = 4096, kTablePages = 1024;
constexpr uint64_t kArenaBytes = 0xe0000000;
constexpr unsigned kAsid = 2, kPoll = 1000000;
enum : size_t { kGr0Scr0 = 0x0, kGr0Idr0 = 0x20, kGr0Idr1 = 0x24, kGr0Idr2 = 0x28, kGr0Sgfsr = 0x48, kGr0TlbiAllNsnh = 0x68,
  kGr0TlbiAllH = 0x6c, kGr0TlbGsync = 0x70, kGr0TlbGstatus = 0x74, kGr0Smr = 0x800, kGr0S2cr = 0xc00, kCbSctlr = 0x0, kCbFsr = 0x58,
  kCbFar = 0x60, kCbFsynr0 = 0x68 };
constexpr uint32_t kSmrValid = 1u << 31, kScr0ClientPd = 1u << 0;
// The SMMU as three mapped pages: GR0 (stream matching, TLB), GR1 (the bank's CBAR/CBA2R) and the bank's own page. Any other
// offset fails, which the kit reports as an error instead of touching an unmapped address.
struct Window { volatile uint8_t *gr0, *gr1, *cb; uint64_t cb_offset; };
Window gSmmu;
volatile uint8_t *smmu_at(void *c, size_t at, size_t width) {
  const Window *w = static_cast<Window *>(c);
  if (at & (width - 1)) return nullptr;
  if (at + width <= 0x1000) return w->gr0 ? w->gr0 + at : nullptr;
  if (at >= 0x1000 && at + width <= 0x2000) return w->gr1 ? w->gr1 + (at - 0x1000) : nullptr;
  if (w->cb && at >= w->cb_offset && at + width <= w->cb_offset + 0x1000) return w->cb + (at - w->cb_offset);
  return nullptr;
}
int smmu_read32(void *c, size_t at, uint32_t *v) {
  volatile uint8_t *p = smmu_at(c, at, 4); if (!p) return 1; *v = *reinterpret_cast<volatile uint32_t *>(p); return 0;
}
int smmu_read64(void *c, size_t at, uint64_t *v) {
  volatile uint8_t *p = smmu_at(c, at, 8); if (!p) return 1; *v = *reinterpret_cast<volatile uint64_t *>(p); return 0;
}
int smmu_write32(void *c, size_t at, uint32_t v) {
  volatile uint8_t *p = smmu_at(c, at, 4); if (!p) return 1; *reinterpret_cast<volatile uint32_t *>(p) = v; return 0;
}
int smmu_write64(void *c, size_t at, uint64_t v) {
  volatile uint8_t *p = smmu_at(c, at, 8); if (!p) return 1; *reinterpret_cast<volatile uint64_t *>(p) = v; return 0;
}
int smmu_barrier(void *) { barrier(); return 0; }
const ArmSmmu4KIo gIo = {&gSmmu, kSmmuBytes, smmu_read32, smmu_read64, smmu_write32, smmu_write64, smmu_barrier};
uint32_t gr0(size_t at) { uint32_t v = 0; smmu_read32(&gSmmu, at, &v); return v; }
void gr0_write(size_t at, uint32_t v) { smmu_write32(&gSmmu, at, v); barrier(); }

void flush(uint64_t physical, uint64_t bytes) {
  unsigned int result = 0; dcache_incoherent_io_flush64(physical, static_cast<unsigned>(bytes), 0, &result);
}
// A region the codec reaches only through the SMMU: contiguous kernel memory, cleaned, then write-combined for its whole life (VM_WIMG_WCOMB,
// as CoachzUsbHost.cpp does for the DWC3's buffers: the codec does not snoop the CPU's caches).
struct Memory { IOBufferMemoryDescriptor *md; volatile uint8_t *va; uint64_t pa; };
Memory gFwMemory, gSharedMemory;
bool memory_get(Memory *m, size_t bytes, const char *name) {
  m->md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
      static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), bytes, 0x0000000ffffff000ULL);
  if (!m->md || m->md->prepare() != kIOReturnSuccess) { log("no %s memory", name); return false; }
  m->va = static_cast<volatile uint8_t *>(m->md->getBytesNoCopy()); m->pa = m->md->getPhysicalAddress();
  bzero(const_cast<uint8_t *>(m->va), bytes);
  for (uint64_t at = 0; at < bytes; at += 0x40000000) {
    unsigned int result = 0; dcache_incoherent_io_flush64(m->pa + at, static_cast<unsigned>(bytes - at > 0x40000000 ? 0x40000000 : bytes - at), 0, &result);
  }
  for (uint64_t page = m->pa / 4096; page < (m->pa + bytes) / 4096; ++page) pmap_set_cache_attributes(static_cast<ppnum_t>(page), 0x6);
  log("%s: %zu KiB at 0x%llx, attributes 0x%x", name, bytes / 1024, m->pa, pmap_cache_attributes(static_cast<ppnum_t>(m->pa / 4096)));
  return true;
}

struct Pool { IOBufferMemoryDescriptor *md; uint8_t *va; uint64_t pa; size_t used; };
Pool gPool;
int pool_allocate(void *, uint64_t *physical, volatile uint64_t **mapping) {
  if (gPool.used >= kTablePages) return 1;
  uint8_t *page = gPool.va + gPool.used * kPage;
  bzero(page, kPage);
  flush(gPool.pa + gPool.used * kPage, kPage);
  *physical = gPool.pa + gPool.used * kPage; *mapping = reinterpret_cast<volatile uint64_t *>(page); ++gPool.used;
  return 0;
}
volatile uint64_t *pool_find(void *, uint64_t physical) {
  if (physical < gPool.pa || physical >= gPool.pa + gPool.used * kPage || physical & (kPage - 1)) return nullptr;
  return reinterpret_cast<volatile uint64_t *>(gPool.va + (physical - gPool.pa));
}
int pool_overlaps(void *, uint64_t physical, uint64_t bytes) {
  return physical < gPool.pa + kTablePages * kPage && gPool.pa < physical + bytes;
}
int publish_table_page(void *, volatile uint64_t *page, size_t bytes) {
  const uint8_t *at = reinterpret_cast<const uint8_t *>(const_cast<uint64_t *>(page));
  if (at < gPool.va || at + bytes > gPool.va + kTablePages * kPage) return 1;
  flush(gPool.pa + static_cast<uint64_t>(at - gPool.va), bytes);
  barrier();
  return 0;
}
const ArmSmmu4KTablePages gPages = {nullptr, pool_allocate, pool_find, pool_overlaps};
const ArmSmmu4KLiveVisibility gVisibility = {nullptr, publish_table_page};
ArmSmmu4KLiveArena gArena;
ArmSmmu4KLiveState gState;
ArmSmmu4KContextConfig gContext;
unsigned gBank;
uint64_t gCbOffset;
constexpr size_t kMaxRanges = 512;
ArmSmmu4KRange gRanges[kMaxRanges];
size_t gRangeCount;

// The firmware's IOVA 0 region (iommu_map with IOMMU_READ | IOMMU_WRITE | IOMMU_PRIV, executable) and the shared queues (the DMA
// API's read and write). Normal non-cacheable, outer shareable: the attributes io-pgtable-arm gives a master that does not snoop.
bool smmu_attach() {
  const uint32_t scr0 = gr0(kGr0Scr0), idr0 = gr0(kGr0Idr0), idr1 = gr0(kGr0Idr1), idr2 = gr0(kGr0Idr2);
  const unsigned smrs = idr0 & 0xff, banks = idr1 & 0xff, pagesize = (idr1 >> 31) ? 0x10000 : 0x1000;
  const unsigned numpage = 1u << (((idr1 >> 28) & 7) + 1);
  if (scr0 & kScr0ClientPd) { log("the apps SMMU is not enabled (sCR0 0x%x); the codec stays off", scr0); return false; }
  if (pagesize != 0x1000 || !smrs || banks < 2) { log("unexpected SMMU layout"); return false; }
  // Free stream matches and a free bank: the USB controller holds SMR 0 and bank 0 (CoachzUsbHost.cpp).
  unsigned free_smr[2], found = 0;
  for (unsigned i = 0; i < smrs && found < 2; ++i) if (!(gr0(kGr0Smr + 4 * i) & kSmrValid)) free_smr[found++] = i;
  if (found < 2) { log("no free stream match"); return false; }
  // Bank 1: the USB controller has bank 0. Only that bank's page is mapped, so only it is checked.
  gBank = 1;
  bool used = false;
  for (unsigned i = 0; i < smrs && !used; ++i) used = (gr0(kGr0Smr + 4 * i) & kSmrValid) && (gr0(kGr0S2cr + 4 * i) & 0xff) == gBank;
  gCbOffset = static_cast<uint64_t>(numpage) * pagesize + static_cast<uint64_t>(gBank) * pagesize;
  if (gCbOffset + 0x1000 > kSmmuBytes) { log("bank page outside the SMMU"); return false; }
  gSmmu.cb = reinterpret_cast<volatile uint8_t *>(ml_io_map(kSmmuBase + gCbOffset, 0x1000));
  gSmmu.cb_offset = gCbOffset;
  uint32_t sctlr = 0;
  if (!gSmmu.cb || smmu_read32(&gSmmu, gCbOffset + kCbSctlr, &sctlr) || used || (sctlr & 1)) {
    log("context bank %u not free (used %d SCTLR 0x%x)", gBank, used, sctlr); return false;
  }

  gPool.md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
      static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), kTablePages * kPage,
      0x0000000ffffff000ULL);
  if (!gPool.md || gPool.md->prepare() != kIOReturnSuccess) { log("no table pool"); return false; }
  gPool.va = static_cast<uint8_t *>(gPool.md->getBytesNoCopy()); gPool.pa = gPool.md->getPhysicalAddress(); gPool.used = 0;
  uint64_t root = 0; volatile uint64_t *rootMapping = nullptr;
  if (pool_allocate(nullptr, &root, &rootMapping)) return false;
  gArena.config = {32, 36, root, static_cast<size_t>(kArenaBytes / kPage)};
  gArena.iova = 0; gArena.bytes = kArenaBytes; gArena.upper_region = 0; gArena.pages = &gPages;
  ArmSmmu4KStatus status = ArmSmmu4KLivePrepare(&gArena, &gState);
  if (status != ArmSmmu4KSuccess) { log("arena not prepared: %d", status); return false; }
  publish_table_page(nullptr, rootMapping, kPage);
  gContext.lower = &gArena.config; gContext.lower_tables = &gState.tables; gContext.upper = nullptr; gContext.upper_tables = nullptr;
  gContext.id0 = idr0; gContext.id2 = idr2; gContext.asid = kAsid; gContext.coherent_walk = 0;
  gContext.stall_on_fault = 0; gContext.continue_after_fault = 0; gContext.ttbr0_switched = 0;
  ArmSmmu4KProgramResult programmed = {};
  status = ArmSmmu4KProgramDisabledContext(&gIo, gBank, &gContext, kPoll, &programmed);
  if (status != ArmSmmu4KSuccess) { log("context bank %u not programmed: %d (phase %u)", gBank, status, programmed.phase); return false; }

  // ArmSmmu4K takes the changes in ascending IOVA order (a later range that starts below the previous end is an overlap). Session
  // buffers are mapped between them later (buffer_new).
  const ArmSmmu4KRange changes[2] = {
    {0, gFwMemory.pa, kFwBytes, ArmSmmu4KRead | ArmSmmu4KWrite | ArmSmmu4KExecute | ArmSmmu4KPrivileged, 0, 2},
    {kSharedIova, gSharedMemory.pa, kSharedBytes, ArmSmmu4KRead | ArmSmmu4KWrite, 0, 2},
  };
  const ArmSmmu4KLiveRegion lower = {&gArena, &gState, gRanges, gRangeCount};
  ArmSmmu4KLiveResult done = {};
  status = ArmSmmu4KLiveUpdateContextBatch(&gIo, gBank, &gContext, &lower, nullptr, 0, &gVisibility, changes, 2, 1, kPoll, &done);
  if (status != ArmSmmu4KSuccess) { log("mapping not made: %d (complete %u)", status, done.complete); return false; }
  memcpy(gRanges, changes, sizeof(changes)); gRangeCount = 2;

  // arm_smmu_write_sme for both stream groups of the device tree: S2CR (translate through the bank) before its SMR.
  gr0_write(kGr0S2cr + 4 * free_smr[0], gBank);
  gr0_write(kGr0Smr + 4 * free_smr[0], kSmrValid | 0x60u << 16 | 0xc00u);   // <&apps_smmu 0xc00 0x60>
  gr0_write(kGr0S2cr + 4 * free_smr[1], gBank);
  gr0_write(kGr0Smr + 4 * free_smr[1], kSmrValid | 0xc42u);                  // video-firmware: <&apps_smmu 0xc42 0x0>
  gr0_write(kGr0TlbiAllNsnh, 0); gr0_write(kGr0TlbGsync, 0);
  unsigned polls = 0;
  while ((gr0(kGr0TlbGstatus) & 1) && ++polls < kPoll) {}
  log("apps SMMU: bank %u, SMR %u 0x%x and SMR %u 0x%x, %zu table pages, sCR0 0x%x", gBank, free_smr[0], gr0(kGr0Smr + 4 * free_smr[0]),
      free_smr[1], gr0(kGr0Smr + 4 * free_smr[1]), gPool.used, gr0(kGr0Scr0));
  return true;
}
void smmu_report(const char *when) {
  uint32_t fsr = 0, fsynr = 0; uint64_t far = 0;
  smmu_read32(&gSmmu, gCbOffset + kCbFsr, &fsr); smmu_read32(&gSmmu, gCbOffset + kCbFsynr0, &fsynr); smmu_read64(&gSmmu, gCbOffset + kCbFar, &far);
  if (fsr & 0xc00001feu) log("%s: bank %u fault FSR 0x%x FAR 0x%llx FSYNR0 0x%x, global 0x%x", when, gBank, fsr, far, fsynr, gr0(kGr0Sgfsr));
}

// ---- the firmware: venus_load_fw with qcom_mdt_load_no_init (mdt_loader.c), from the one-file .mbn ----
struct Elf32Header { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Elf32Phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };
constexpr uint32_t kPtLoad = 1, kMdtTypeMask = 7u << 24, kMdtTypeHash = 2u << 24, kMdtRelocatable = 1u << 27;

bool load_firmware(const uint8_t *image, size_t bytes, size_t *size) {
  if (bytes < sizeof(Elf32Header)) return false;
  Elf32Header eh; memcpy(&eh, image, sizeof eh);
  if (memcmp(eh.ident, "\x7f" "ELF", 4) || eh.ident[4] != 1 || eh.phentsize != sizeof(Elf32Phdr) ||
      static_cast<uint64_t>(eh.phoff) + static_cast<uint64_t>(eh.phnum) * sizeof(Elf32Phdr) > bytes) {
    log("venus.mbn is not a 32-bit ELF"); return false;
  }
  // qcom_mdt_get_size / qcom_mdt_load_no_init: valid segments are loadable, sized and not the hash segment; relocatable images go
  // where the region is, by their offset from the lowest address.
  uint64_t lowest = ~0ull, highest = 0; bool relocate = false;
  for (unsigned i = 0; i < eh.phnum; ++i) {
    Elf32Phdr ph; memcpy(&ph, image + eh.phoff + i * sizeof ph, sizeof ph);
    if (ph.type != kPtLoad || !ph.memsz || (ph.flags & kMdtTypeMask) == kMdtTypeHash) continue;
    relocate |= (ph.flags & kMdtRelocatable) != 0;
    if (ph.paddr < lowest) lowest = ph.paddr;
    if (static_cast<uint64_t>(ph.paddr) + ph.memsz > highest) highest = static_cast<uint64_t>(ph.paddr) + ph.memsz;
  }
  if (lowest == ~0ull || highest - lowest > kFwBytes) { log("firmware span 0x%llx does not fit the region", highest - lowest); return false; }
  const uint64_t reloc = relocate ? lowest : kFwRegionBase;
  bzero(const_cast<uint8_t *>(gFw), kFwBytes);
  unsigned loaded = 0;
  for (unsigned i = 0; i < eh.phnum; ++i) {
    Elf32Phdr ph; memcpy(&ph, image + eh.phoff + i * sizeof ph, sizeof ph);
    if (ph.type != kPtLoad || !ph.memsz || (ph.flags & kMdtTypeMask) == kMdtTypeHash) continue;
    const uint64_t at = ph.paddr - reloc;
    if (at + ph.memsz > kFwBytes || ph.filesz > ph.memsz || static_cast<uint64_t>(ph.offset) + ph.filesz > bytes) {
      log("segment %u (paddr 0x%x, %u bytes) outside the region or the file", i, ph.paddr, ph.memsz); return false;
    }
    memcpy(const_cast<uint8_t *>(gFw) + at, image + ph.offset, ph.filesz);
    ++loaded;
  }
  barrier();
  *size = static_cast<size_t>(highest - lowest);
  log("firmware: %u segments, span 0x%llx, relocatable %d, entry 0x%x", loaded, highest - lowest, relocate, eh.entry);
  return loaded != 0;
}
// venus_reset_cpu: the firmware, code and non-pixel windows over the mapped region, ARM9 clocks on, then out of reset.
void reset_cpu() {
  wr(gVenus, kFwStart, 0); wr(gVenus, kFwEnd, kFwBytes);
  wr(gVenus, kCpaStart, 0); wr(gVenus, kCpaEnd, kFwBytes);
  wr(gVenus, kNonpixStart, kFwBytes); wr(gVenus, kNonpixEnd, kFwBytes);
  wr(gVenus, kCpuCgcDis, 0);
  wr(gVenus, kCpuClockConfig, 0);
  wr(gVenus, kA9ssSwReset, 0);
}

// ---- HFI queues (venus_interface_queues_init, venus_write_queue, venus_read_queue) ----
volatile uint32_t *qhdr(unsigned i) { return reinterpret_cast<volatile uint32_t *>(gShared + kTableHeader + kQueueHeader * i); }
enum { kQStatus, kQStart, kQType, kQSize, kQPktSize, kQDrop, kQRxWm, kQTxWm, kQRxReq, kQTxReq, kQRxIrq, kQTxIrq, kQRead, kQWrite };
volatile uint32_t *qmem(unsigned i) { return reinterpret_cast<volatile uint32_t *>(gShared + kTableBytes + kQueueBytes * i); }

void queues_init() {
  bzero(const_cast<uint8_t *>(gShared), kSharedBytes);
  for (unsigned i = 0; i < kQueueCount; ++i) {
    volatile uint32_t *q = qhdr(i);
    q[kQStatus] = 1; q[kQType] = kDefaultQhdr | i; q[kQSize] = kQueueBytes / 4; q[kQPktSize] = 0; q[kQRxWm] = 1; q[kQTxWm] = 1;
    q[kQRxReq] = i == kDbgQ ? 0 : 1; q[kQTxReq] = 0; q[kQRxIrq] = 0; q[kQTxIrq] = 0; q[kQRead] = 0; q[kQWrite] = 0;
    q[kQStart] = static_cast<uint32_t>(kSharedIova + kTableBytes + kQueueBytes * i);
  }
  volatile uint32_t *table = reinterpret_cast<volatile uint32_t *>(gShared);
  table[0] = 0; table[1] = kTableBytes; table[2] = kTableHeader; table[3] = kQueueHeader; table[4] = kQueueCount; table[5] = kQueueCount;
  *reinterpret_cast<volatile uint32_t *>(gShared + kSfrOffset) = kSfrBytes;   // hfi_sfr.buf_size
  barrier();
}
// Returns 0 when written, and whether the firmware asked for an interrupt (rx_req).
int queue_write(unsigned i, const uint32_t *packet) {
  volatile uint32_t *q = qhdr(i), *mem = qmem(i);
  const uint32_t dwords = packet[0] >> 2, rd_idx = q[kQRead], wr_idx = q[kQWrite], size = q[kQSize];
  if (!dwords) return -1;
  const uint32_t space = wr_idx >= rd_idx ? size - (wr_idx - rd_idx) : rd_idx - wr_idx;
  if (space <= dwords) { q[kQTxReq] = 1; barrier(); return -2; }
  q[kQTxReq] = 0;
  for (uint32_t n = 0; n < dwords; ++n) mem[(wr_idx + n) % size] = packet[n];
  barrier();
  q[kQWrite] = (wr_idx + dwords) % size;
  barrier();
  return q[kQRxReq] ? 1 : 0;
}
// One packet into buf (capacity in dwords): its dwords, 0 when the queue is empty, negative for a bad packet (then dropped).
int queue_read(unsigned i, uint32_t *buf, uint32_t capacity) {
  volatile uint32_t *q = qhdr(i), *mem = qmem(i);
  const uint32_t rd_idx = q[kQRead], wr_idx = q[kQWrite], size = q[kQSize];
  const bool msg = i == kMsgQ;
  if (rd_idx == wr_idx) { q[kQRxReq] = msg ? 1 : 0; barrier(); return 0; }
  const uint32_t dwords = mem[rd_idx % size] >> 2;
  int result = static_cast<int>(dwords);
  if (!dwords || dwords > capacity || rd_idx > size) { q[kQRead] = wr_idx; result = -1; }
  else {
    for (uint32_t n = 0; n < dwords; ++n) buf[n] = mem[(rd_idx + n) % size];
    barrier();
    q[kQRead] = (rd_idx + dwords) % size;
  }
  barrier();
  q[kQRxReq] = q[kQRead] != q[kQWrite] ? 0 : (msg ? 1 : 0);
  barrier();
  return result;
}
void soft_int() { wr(gVenus, kSoftInt, kSoftIntH2a); }
// hfi_venus.c venus_iface_cmdq_write: one writer at a time (the client threads and the message thread all send).
IOLock *gHfiLock;
bool send(const uint32_t *packet, const char *what) {
  if (gHfiLock) IOLockLock(gHfiLock);
  const int written = queue_write(kCmdQ, packet);
  if (written > 0) soft_int();
  if (gHfiLock) IOLockUnlock(gHfiLock);
  if (written < 0) { log("%s not queued: %d", what, written); return false; }
  return true;
}

// venus_run: the shared region's place, then venus_boot_core: interrupts, CTRL_INIT, and the firmware's status in SCIACMDARG0.
bool run() {
  wr(gVenus, kUcRegionAddr, static_cast<uint32_t>(kSharedIova));
  wr(gVenus, kUcRegionSize, kSharedQSize);
  wr(gVenus, kSciaCmdArg2, static_cast<uint32_t>(kSharedIova));
  wr(gVenus, kSciaCmdArg1, 0x01);
  wr(gVenus, kSfrAddr, static_cast<uint32_t>(kSharedIova + kSfrOffset));
  wr(gVenus, kIntrMask, kIntrMaskA2hVcodec);
  wr(gVenus, kCtrlInit, 1);
  uint32_t status = 0; unsigned tries = 0;
  while (!status && tries < 100) {
    status = rd(gVenus, kSciaCmdArg0);
    if ((status & 0xfe) == 4) { log("invalid setting for UC_REGION (SCIACMDARG0 0x%x)", status); return false; }
    IOSleep(1); ++tries;
  }
  const uint32_t ver = rd(gVenus, kHwVersion);
  log("boot core: SCIACMDARG0 0x%x after %u ms, CPU status 0x%x, hw version %u.%u.%u (0x%x)", status, tries, rd(gVenus, kCpuStatus),
      (ver >> 28) & 0xf, (ver >> 16) & 0xfff, ver & 0xffff, ver);
  return status != 0;
}

void print_sfr(const char *when) {
  const char *text = reinterpret_cast<const char *>(const_cast<uint8_t *>(gShared) + kSfrOffset + 4);
  char line[160]; size_t n = 0;
  while (n + 1 < sizeof line && n < kSfrBytes - 4 && text[n]) { line[n] = text[n]; ++n; }
  line[n] = 0;
  if (n) log("%s: SFR \"%s\"", when, line);
}

uint32_t gPacket[1024 * 3];
unsigned gDebugLines, gInitDone, gVersionSeen;
volatile uint32_t gSysError;   // SYS_ERROR or the watchdog: every session fails, nothing restarts the core (no venus_sys_error_handler)

// ---- dynamic buffers: dma_alloc_attrs with DMA_ATTR_WRITE_COMBINE as helpers.c and vb2-dma-contig take them, each buffer its own
// contiguous allocation mapped at an IOVA of its own ----
constexpr uint64_t kDynIova = 0x00800000, kDynEnd = kSharedIova;   // above the firmware's 5 MiB, below the shared queues
IOLock *gSmmuLock;
struct Buffer { IOBufferMemoryDescriptor *md; volatile uint8_t *va; uint64_t pa; uint32_t iova, bytes; };

bool smmu_change(const ArmSmmu4KRange &r, unsigned map) {
  const ArmSmmu4KLiveRegion lower = {&gArena, &gState, gRanges, gRangeCount};
  ArmSmmu4KLiveResult done = {};
  const ArmSmmu4KStatus status = ArmSmmu4KLiveUpdateContextBatch(&gIo, gBank, &gContext, &lower, nullptr, 0, &gVisibility, &r, 1, map,
      kPoll, &done);
  if (status != ArmSmmu4KSuccess) {
    log("%s 0x%llx (%llu KiB) failed: %d (complete %u), %zu ranges, %zu table pages", map ? "map" : "unmap", r.iova, r.bytes / 1024,
        status, done.complete, gRangeCount, gPool.used);
    return false;
  }
  return true;
}
// First fit in ascending IOVA order; gRanges stays sorted, as the kit wants its list of mapped ranges.
bool iova_find(uint64_t bytes, uint64_t align, uint64_t *iova, size_t *slot) {
  uint64_t at = kDynIova;
  for (size_t i = 0; i < gRangeCount; ++i) {
    const ArmSmmu4KRange &r = gRanges[i];
    if (r.iova + r.bytes <= at) continue;
    if (at + bytes <= r.iova) { *iova = at; *slot = i; return at + bytes <= kDynEnd; }
    at = (r.iova + r.bytes + align - 1) & ~(align - 1);
  }
  *iova = at; *slot = gRangeCount;
  return at + bytes <= kDynEnd;
}
void buffer_free(Buffer *b) {
  if (!b->md) return;
  if (b->iova) {
    IOLockLock(gSmmuLock);
    size_t i = 0;
    while (i < gRangeCount && gRanges[i].iova != b->iova) ++i;
    if (i < gRangeCount && smmu_change(gRanges[i], 0)) {
      memmove(&gRanges[i], &gRanges[i + 1], (gRangeCount - i - 1) * sizeof gRanges[0]);
      --gRangeCount;
    } else {
      // The translation may still exist: the memory stays the codec's for good.
      IOLockUnlock(gSmmuLock);
      log("buffer at 0x%x kept: its mapping was not removed", b->iova);
      *b = {};
      return;
    }
    IOLockUnlock(gSmmuLock);
  }
  for (uint64_t page = b->pa / kPage; page < (b->pa + b->bytes) / kPage; ++page) pmap_set_cache_attributes(static_cast<ppnum_t>(page), 0x2);
  b->md->complete();
  b->md->release();
  *b = {};
}
bool buffer_new(Buffer *b, size_t bytes, uint32_t alignment) {
  *b = {};
  bytes = (bytes + kPage - 1) & ~(kPage - 1);
  if (!bytes || bytes > (512u << 20)) return false;
  Memory m = {};
  m.md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
      static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), bytes, 0x0000000ffffff000ULL);
  if (!m.md) { log("no memory for a %zu KiB buffer", bytes / 1024); return false; }
  if (m.md->prepare() != kIOReturnSuccess) { m.md->release(); log("buffer not prepared"); return false; }
  m.va = static_cast<volatile uint8_t *>(m.md->getBytesNoCopy()); m.pa = m.md->getPhysicalAddress();
  bzero(const_cast<uint8_t *>(m.va), bytes);
  flush(m.pa, bytes);
  for (uint64_t page = m.pa / kPage; page < (m.pa + bytes) / kPage; ++page) pmap_set_cache_attributes(static_cast<ppnum_t>(page), 0x6);
  b->md = m.md; b->va = m.va; b->pa = m.pa; b->bytes = static_cast<uint32_t>(bytes);
  const uint64_t align = alignment > (1u << 20) ? alignment : bytes >= (1u << 20) ? (1u << 20) : 0x10000;
  IOLockLock(gSmmuLock);
  uint64_t iova = 0; size_t slot = 0;
  bool ok = gRangeCount < kMaxRanges && iova_find(bytes, align, &iova, &slot);
  const ArmSmmu4KRange r = {iova, m.pa, bytes, ArmSmmu4KRead | ArmSmmu4KWrite, 0, 2};
  ok = ok && smmu_change(r, 1);
  if (ok) {
    memmove(&gRanges[slot + 1], &gRanges[slot], (gRangeCount - slot) * sizeof gRanges[0]);
    gRanges[slot] = r; ++gRangeCount;
    b->iova = static_cast<uint32_t>(iova);
  }
  IOLockUnlock(gSmmuLock);
  if (!ok) { log("no IOVA for a %zu KiB buffer (%zu ranges)", bytes / 1024, gRangeCount); buffer_free(b); return false; }
  return true;
}

// ---- clocks and bandwidth: pm_helpers.c load_scale_v4 over sc7180_freq_table, the OPP table's CX corners, sc7180_bw_table_dec ----
// videocc-sc7180.c ftbl_video_cc_venus_clk_src: every rate is video_pll0 (CLK_SET_RATE_PARENT) divided by 4, 2.5 or 2; the PLL rate is
// alpha_pll_round_rate's L and 16-bit alpha over the 19.2 MHz reference (rounded up), the RCG divider field 2 * div - 1.
struct Opp { uint32_t hz, pll_l, pll_alpha, div_field, cx; };
constexpr Opp kOpps[5] = {
  {150000000, 31, 0x4000, 7, 64},    // PLL 600 MHz / 4, low_svs
  {270000000, 35, 0x2800, 4, 128},   // 675 MHz / 2.5, svs
  {340000000, 35, 0x6aab, 3, 192},   // 680 MHz / 2, svs_l1
  {434000000, 45, 0x3556, 3, 256},   // 868 MHz / 2, nom
  {500000000, 52, 0x1556, 3, 384},   // 1000 MHz / 2, turbo
};
struct Bw { uint32_t mbs, avg, peak; };
constexpr Bw kBwDec[3] = {{1036800, 1386000, 0}, {489600, 865000, 0}, {244800, 530000, 0}};
constexpr uint32_t kPllUpdate = 1u << 22, kPllAckLatch = 1u << 29;
unsigned gOpp;                      // 0: 150 MHz, as power_on left it
uint64_t gBwAvg = 20000, gBwPeak = 20000;
IOLock *gScaleLock;

// alpha_pll_fabia_set_rate: L and alpha, then __clk_alpha_pll_update_latch (UPDATE, the ACK latch with UPDATE_BYPASS set, its clear).
bool pll_rate(const Opp &o) {
  wr(gVideoCc, kPllL, o.pll_l);
  wr(gVideoCc, kPllFrac, o.pll_alpha);
  const uint32_t mode = rd(gVideoCc, kPllMode);
  set_bits(gVideoCc, kPllMode, kPllUpdate, kPllUpdate);
  IODelay(1);
  if (mode & kPllUpdateBypass) {
    if (!poll(gVideoCc, kPllMode, kPllAckLatch, kPllAckLatch, 1000)) { log("video_pll0: no update ack (MODE 0x%x)", rd(gVideoCc, kPllMode)); return false; }
    set_bits(gVideoCc, kPllMode, kPllUpdate, 0);
  } else if (!poll(gVideoCc, kPllMode, kPllUpdate, 0, 1000)) {
    log("video_pll0: update did not clear (MODE 0x%x)", rd(gVideoCc, kPllMode)); return false;
  }
  if (!poll(gVideoCc, kPllMode, kPllAckLatch, 0, 1000)) { log("video_pll0: ack did not clear (MODE 0x%x)", rd(gVideoCc, kPllMode)); return false; }
  IODelay(10);
  return true;
}
// core_clks_set_rate through the OPP: a higher corner is voted before the clock rises, a lower one after it falls; the clock core
// changes the PLL (the parent) before the RCG.
bool set_opp(unsigned index) {
  if (index == gOpp || index >= 5) return true;
  const Opp &o = kOpps[index];
  const bool up = index > gOpp;
  if (up && coachz_rpmh_cx_request(kCxConsumer, o.cx)) { log("cx vote %u refused", o.cx); return false; }
  const bool ok = pll_rate(o);
  if (ok) {
    wr(gVideoCc, kVenusRcgCfg, (rd(gVideoCc, kVenusRcgCfg) & ~(0x7u << 8 | 0x1fu)) | 1u << 8 | o.div_field);
    set_bits(gVideoCc, kVenusRcgCmd, 1u, 1u);
    if (!poll(gVideoCc, kVenusRcgCmd, 1u, 0, 500)) log("venus_clk_src: update did not clear (CMD 0x%x)", rd(gVideoCc, kVenusRcgCmd));
  }
  if (ok && !up) coachz_rpmh_cx_request(kCxConsumer, o.cx);
  log("core clock %u MHz: %s, PLL MODE 0x%x L 0x%x, RCG CFG 0x%x", o.hz / 1000000, ok ? "set" : "FAILED", rd(gVideoCc, kPllMode),
      rd(gVideoCc, kPllL), rd(gVideoCc, kVenusRcgCfg));
  if (ok) gOpp = index;
  return ok;
}

// ---- HFI session numbers and properties (hfi_cmds.h, hfi_msgs.h, hfi_helper.h) ----
constexpr uint32_t kCmdSessionInit = 0x10007, kCmdSessionEnd = 0x10008, kCmdSessionSetProperty = 0x11001, kCmdSessionSetBuffers = 0x11002,
    kCmdSessionAbort = 0x210001, kCmdLoadResources = 0x211001, kCmdStart = 0x211002, kCmdStop = 0x211003, kCmdEmptyBuffer = 0x211004,
    kCmdFillBuffer = 0x211005, kCmdFlush = 0x211008, kCmdGetProperty = 0x211009, kCmdReleaseBuffers = 0x21100b,
    kCmdReleaseResources = 0x21100c, kCmdContinue = 0x21100d;
constexpr uint32_t kMsgSessionInit = 0x20006, kMsgSessionEnd = 0x20007, kMsgSessionAbort = 0x220004, kMsgLoadResources = 0x221001,
    kMsgStart = 0x221002, kMsgStop = 0x221003, kMsgFlush = 0x221006, kMsgEmptyBuffer = 0x221007, kMsgFillBuffer = 0x221008,
    kMsgPropertyInfo = 0x221009, kMsgReleaseResources = 0x22100a, kMsgReleaseBuffers = 0x22100c;
constexpr uint32_t kPropFrameSize = 0x1001, kPropFormatSelect = 0x1003, kPropProfileLevel = 0x1005, kPropWorkMode = 0x1015,
    kPropMultiStream = 0x1003001, kPropConcealColor = 0x1003002, kPropBitDepth = 0x1003007, kPropPicStruct = 0x1003009,
    kPropColourSpace = 0x100300a, kPropSufficientEvent = 0x100300b, kPropCountActual = 0x201001,
    kPropSizeActual = 0x20100c, kPropBufreq = 0x202001, kPropDpbCounts = 0x120300e, kPropEntropy = 0x1204004,
    kIndexInputCrop = 0x0700000e;
constexpr uint32_t kEventSysError = 0x1, kEventSessionError = 0x2, kEventSufficient = 0x1000001, kEventInsufficient = 0x1000002,
    kEventSeqChanged = 0x1000003, kEventReleaseRef = 0x1000006;
constexpr uint32_t kBufInput = 1, kBufOutput = 2, kBufOutput2 = 3, kNv12 = 0x2, kNv12Ubwc = 0x8002;
constexpr uint32_t kFlushOutput = 0x1000002, kFlushAll = 0x1000004;
constexpr uint32_t kFlagEos = 0x1, kFlagCorrupt = 0x8, kFlagTimestampInvalid = 0x100, kFlagDropFrame = 0x20000000;
constexpr uint32_t kPicI = 0x01, kPicIdr = 0x08;
constexpr uint32_t kDomainDecoder = 2, kVb2MaxFrame = 32, kFrameMin = 96, kFrameMax = 4096, kFps = 30;
// HFI_BUFFER_INTERNAL_SCRATCH, _1, _2 (4xx numbering), PERSIST, PERSIST_1: intbuf_types_4xx.
constexpr uint32_t kInternalTypes[5] = {6, 7, 8, 4, 5};
constexpr uint32_t kTimeoutMs = 1000;   // hfi.c wait_session_msg: TIMEOUT 1 s

// helpers.c get_framesize_raw_nv12 and get_framesize_raw_nv12_ubwc.
uint32_t al(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
uint32_t framesz_nv12(uint32_t w, uint32_t h) { return al(al(w, 128) * al(h, 32) + al(w, 128) * al((h + 1) >> 1, 16), 4096); }
uint32_t framesz_ubwc(uint32_t w, uint32_t h) {
  const uint32_t y_meta = al(al(al(w, 32) / 32, 64) * al(al(h, 8) / 8, 16), 4096);
  const uint32_t y = al(al(w, 128) * al(h, 32), 4096);
  const uint32_t uv_meta = al(al(al(w / 2, 16) / 16, 64) * al(al(h / 2, 8) / 8, 16), 4096);
  const uint32_t uv = al(al(w, 128) * al(h / 2, 32), 4096);
  const uint32_t extra = al(w, 128) * 48 > 16384 ? al(w, 128) * 48 : 16384;
  return al(y_meta + y + uv_meta + uv + extra, 4096);
}
// venus_helper_get_framesz for a compressed format.
uint32_t framesz_compressed(uint32_t w, uint32_t h) {
  uint32_t sz = al(h, 32) * al(w, 32) * 3 / 2 / 2;
  if (w < 1280 || h < 720) sz *= 8;
  return al(sz, 4096);
}

// ---- sessions (vdec.c's venus_inst and codec states) ----
enum State { kDeinit, kInit, kCaptureSetup, kStopped, kDecoding, kDrain, kDrc, kSeek };
enum Owner : uint32_t { kDriver, kFirmware, kClient };
struct Bufreq { uint32_t type, size, region_size, hold_count, count_min, count_actual, contiguous, alignment; };
struct Frame { Buffer buf; uint32_t owner; };
constexpr unsigned kMaxSessions = 6, kRing = 256, kMaxInternal = 24;
struct Session {
  uint32_t id; bool used;
  IOLock *lock;   // replies, events and what the message thread changes
  IOLock *op;     // one client operation at a time (the message thread never takes it)
  uint32_t codec, width, height, out_width, out_height;
  State state;
  bool reconfig, drain_active, drain_pending, flushing, failed;
  uint32_t error;
  // the reply a client thread waits for (hfi.c wait_session_msg)
  uint32_t want, got, reply_err;
  Bufreq req[16]; uint32_t nreq;
  Buffer internal[kMaxInternal]; uint32_t internal_type[kMaxInternal], internal_size[kMaxInternal], ninternal;
  Frame input[COACHZ_VIDEO_MAX_INPUTS]; uint32_t ninput, input_bytes, payload_max;
  Frame dpb[kVb2MaxFrame]; uint32_t ndpb, dpb_bytes;
  Frame opb[COACHZ_VIDEO_MAX_OUTPUTS]; uint32_t nopb, opb_bytes, generation;
  // vdec_event_change's hfi_event_data
  bool sufficient; uint32_t seq_w, seq_h, seq_count, bit_depth, pic_struct;
  uint32_t crop_l, crop_t, crop_w, crop_h;
  CoachzVideoEvent ring[kRing]; uint32_t head, tail, lost;
  uint32_t frames, inputs;
};
Session gSessions[kMaxSessions];
IOLock *gSessionsLock;
uint32_t gSerial;

Session *session_by_id(uint32_t id) {
  const uint32_t slot = id & 0xf;
  if ((id & 0xff000000u) != 0x56000000u || slot >= kMaxSessions) return nullptr;
  Session *s = &gSessions[slot];
  return s->used && s->id == id ? s : nullptr;
}
// Under s->lock.
void push_locked(Session *s, const CoachzVideoEvent &e) {
  if (s->head - s->tail >= kRing) { s->lost++; return; }
  s->ring[s->head % kRing] = e;
  s->head++;
  IOLockWakeup(s->lock, &s->head, false);
}
void push(Session *s, const CoachzVideoEvent &e) { IOLockLock(s->lock); push_locked(s, e); IOLockUnlock(s->lock); }
void fail(Session *s, uint32_t error, const char *why) {
  IOLockLock(s->lock);
  if (!s->failed) {
    s->failed = true; s->error = error;
    CoachzVideoEvent e = {}; e.kind = CoachzVideoEventError; e.status = error;
    push_locked(s, e);
    log("session 0x%x failed: %s (0x%x)", s->id, why, error);
  }
  IOLockWakeup(s->lock, &s->got, false);
  IOLockUnlock(s->lock);
}

// A command whose reply a client thread waits for, as hfi.c does with wait_session_msg.
bool call(Session *s, const uint32_t *pkt, uint32_t reply, const char *what, unsigned ms = kTimeoutMs) {
  IOLockLock(s->lock); s->want = reply; s->got = 0; s->reply_err = 0; IOLockUnlock(s->lock);
  if (!send(pkt, what)) return false;
  uint64_t deadline = 0;
  clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
  IOLockLock(s->lock);
  while (!s->got && !gSysError && IOLockSleepDeadline(s->lock, &s->got, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {}
  const bool got = s->got; const uint32_t err = s->reply_err;
  s->want = 0;
  IOLockUnlock(s->lock);
  if (!got) { log("%s: no answer in %u ms (session 0x%x)", what, ms, s->id); smmu_report(what); print_sfr(what); return false; }
  if (err) { log("%s: error 0x%x (session 0x%x)", what, err, s->id); return false; }
  return true;
}
bool cmd(Session *s, uint32_t type, uint32_t reply, const char *what) {
  const uint32_t pkt[3] = {12, type, s->id};
  return reply ? call(s, pkt, reply, what) : send(pkt, what);
}
// hfi_session_set_property: no reply; a refusal comes as a session error event.
bool set_prop(Session *s, uint32_t ptype, const uint32_t *data, unsigned words, const char *what) {
  uint32_t pkt[16] = {20 + 4 * words, kCmdSessionSetProperty, s->id, 1, ptype};
  for (unsigned i = 0; i < words && i < 11; ++i) pkt[5 + i] = data[i];
  return !s->failed && send(pkt, what);
}
bool get_bufreq(Session *s, const char *what) {
  const uint32_t pkt[5] = {20, kCmdGetProperty, s->id, 1, kPropBufreq};
  return call(s, pkt, kMsgPropertyInfo, what) && !s->failed;
}
const Bufreq *bufreq(Session *s, uint32_t type) {
  for (uint32_t i = 0; i < s->nreq; ++i) if (s->req[i].type == type) return &s->req[i];
  return nullptr;
}
bool ftb(Session *s, uint32_t stream, const Frame &f, uint32_t tag) {
  const uint32_t pkt[11] = {44, kCmdFillBuffer, s->id, stream, 0, f.buf.bytes, 0, tag, f.buf.iova, 0, 0};
  return send(pkt, stream ? "FTB OPB" : "FTB DPB");
}
bool etb(Session *s, uint32_t addr, uint32_t alloc, uint32_t filled, uint32_t flags, uint32_t tag, uint64_t ts) {
  const uint32_t pkt[15] = {60, kCmdEmptyBuffer, s->id, static_cast<uint32_t>(ts >> 32), static_cast<uint32_t>(ts), flags, 0, 0, 0, alloc,
                            filled, tag, addr, 0, 0};
  return send(pkt, flags & kFlagEos ? "ETB EOS" : "ETB");
}
// hfi_session_flush, blocking: every buffer the firmware holds comes back before FLUSH_DONE.
bool flush_session(Session *s, uint32_t type) {
  const uint32_t pkt[4] = {16, kCmdFlush, s->id, type};
  IOLockLock(s->lock); s->flushing = true; IOLockUnlock(s->lock);
  const bool ok = call(s, pkt, kMsgFlush, type == kFlushAll ? "flush all" : "flush output");
  IOLockLock(s->lock); s->flushing = false; IOLockUnlock(s->lock);
  return ok;
}

// load_scale_v4 and load_scale_bw over every running session: vpp 200 and vsp 10 cycles a macroblock for every decoder of
// hfi_platform_v4.c codec_freq_data, 21/20 and 10/7 overheads, the frame rate vdec_inst_init assumes; the lowest OPP at or above.
void load_scale() {
  IOLockLock(gScaleLock);
  uint64_t freq = 0, mbs_total = 0;
  for (Session &s : gSessions) {
    if (!s.used || s.state == kDeinit) continue;
    const uint64_t mbs = static_cast<uint64_t>(al(s.width, 16) / 16) * (al(s.height, 16) / 16) * kFps;
    uint64_t vpp = mbs * 200; vpp += vpp / 20;
    const uint64_t vsp = mbs * 10 + static_cast<uint64_t>(kFps) * s.payload_max * 8 * 10 / 7;
    freq += vpp > vsp ? vpp : vsp;
    mbs_total += mbs;
  }
  unsigned index = 4;
  for (unsigned i = 0; i < 5; ++i) if (kOpps[i].hz >= freq) { index = i; break; }
  set_opp(index);
  uint64_t avg = 0, peak = 0;
  if (mbs_total) for (unsigned i = 0; i < 3; ++i) { if (i && mbs_total > kBwDec[i].mbs) break; avg = kBwDec[i].avg; peak = kBwDec[i].peak; }
  if (!avg && !peak) avg = 1000;   // the minimum vote load_scale_bw keeps
  if (avg != gBwAvg || peak != gBwPeak) {
    const int voted = coachz_venus_bus_vote(avg, peak);
    log("bus vote avg %llu peak %llu kB/s: %d", avg, peak, voted);
    gBwAvg = avg; gBwPeak = peak;
  }
  IOLockUnlock(gScaleLock);
}

// ---- the message thread (hfi_venus.c venus_isr_thread, hfi_msgs.c hfi_process_msg_packet) ----
void session_reply(Session *s, uint32_t type, uint32_t error) {
  IOLockLock(s->lock);
  if (s->want == type) { s->got = 1; s->reply_err = error; IOLockWakeup(s->lock, &s->got, false); }
  IOLockUnlock(s->lock);
}
// event_seq_changed: the properties after event_data2, each its type and then its structure.
void seq_changed(Session *s, const uint32_t *p, unsigned dwords) {
  uint32_t w = 0, h = 0, count = 0, depth = s->bit_depth, ps = s->pic_struct, cl = 0, ct = 0, cw = 0, ch = 0;
  unsigned i = 6, n = dwords >= 6 ? p[5] : 0;
  while (n-- && i < dwords) {
    const uint32_t type = p[i++];
    unsigned size = 0;
    switch (type) {
    case kPropFrameSize: if (i + 3 <= dwords) { w = p[i + 1]; h = p[i + 2]; } size = 3; break;
    case kPropProfileLevel: size = 2; break;
    case kPropBitDepth: if (i + 2 <= dwords) depth = p[i + 1]; size = 2; break;
    case kPropPicStruct: if (i + 1 <= dwords) ps = p[i]; size = 1; break;
    case kPropColourSpace: case kPropEntropy: size = 1; break;
    case kPropBufreq: if (i + 8 <= dwords) count = p[i + 3]; size = 8; break;   // hfi_bufreq_get_count_min: hold_count on 4xx
    case kIndexInputCrop: if (i + 7 <= dwords) { cl = p[i + 3]; ct = p[i + 4]; cw = p[i + 5]; ch = p[i + 6]; } size = 7; break;
    case kPropDpbCounts: if (i + 5 <= dwords) count = p[i + 4]; size = 5; break;
    default: size = 0; break;
    }
    i += size;
  }
  IOLockLock(s->lock);
  s->sufficient = p[4] == kEventSufficient;
  s->seq_w = w; s->seq_h = h; s->seq_count = s->codec == CoachzVideoVP9 ? 11 : count;   // "overwriting this to 11 for vp9 due to fw bug"
  s->bit_depth = depth; s->pic_struct = ps;
  if (cw && ch) { s->crop_l = cl; s->crop_t = ct; s->crop_w = cw; s->crop_h = ch; }
  else { s->crop_l = 0; s->crop_t = 0; s->crop_w = w; s->crop_h = h; }
  const bool drc = s->state == kDecoding || s->state == kDrain;
  if (s->state == kInit) s->state = kCaptureSetup;
  else if (drc) s->state = kDrc;
  s->reconfig = true;
  IOLockWakeup(s->lock, &s->head, false);
  IOLockUnlock(s->lock);
  log("session 0x%x: sequence change, %s resources, %ux%u, crop %u,%u %ux%u, %u buffers, bit depth %u%s", s->id,
      p[4] == kEventSufficient ? "sufficient" : "insufficient", w, h, cl, ct, cw, ch, count, depth, drc ? ", resolution change" : "");
  // vdec_event_change in DRC: the output flushed without waiting.
  if (drc) { const uint32_t pkt[4] = {16, kCmdFlush, s->id, kFlushOutput}; send(pkt, "DRC flush"); }
}
void fbd(Session *s, const uint32_t *p, unsigned dwords) {
  if (dwords < 24) { log("short FBD (%u dwords)", dwords); return; }
  const uint32_t stream = p[3], error = p[5], hflags = p[8], filled = p[13], offset = p[14], tag = p[21], pic = p[22];
  const uint64_t ts = static_cast<uint64_t>(p[6]) << 32 | p[7];
  if (stream == 0) {
    // venus_helper_change_dpb_owner: a DPB buffer the firmware gave back.
    for (uint32_t i = 0; i < s->ndpb; ++i) if (tag == kVb2MaxFrame + i) s->dpb[i].owner = kDriver;
    return;
  }
  const uint32_t gen = tag >> 16, index = tag & 0xffff;
  IOLockLock(s->lock);
  if (gen != s->generation || index >= s->nopb || s->opb[index].owner != kFirmware) {
    IOLockUnlock(s->lock);
    return;   // a buffer of an older output generation, freed when that generation went
  }
  Frame &o = s->opb[index];
  o.owner = kDriver;
  const bool last = hflags & kFlagEos;
  CoachzVideoEvent e = {};
  e.kind = CoachzVideoEventFrame; e.generation = gen; e.status = error; e.offset = offset; e.filled = filled;
  e.width = s->out_width; e.height = s->out_height;
  e.crop_left = s->crop_l; e.crop_top = s->crop_t; e.crop_width = s->crop_w; e.crop_height = s->crop_h;
  if (!(hflags & kFlagTimestampInvalid) && filled) e.token = ts; else e.flags |= CoachzVideoFrameNoToken;
  if (pic & (kPicI | kPicIdr)) e.flags |= CoachzVideoFrameKey;
  if (hflags & kFlagCorrupt) e.flags |= CoachzVideoFrameCorrupt;
  const bool deliver = filled && !(hflags & kFlagDropFrame) && !s->flushing && !s->failed;
  bool requeue = false;
  if (deliver) {
    o.owner = kClient; e.index = index; s->frames++;
    push_locked(s, e);
  } else {
    if (filled || (hflags & kFlagDropFrame)) { e.flags |= CoachzVideoFrameDropped; e.index = ~0u; push_locked(s, e); }
    requeue = !s->flushing && !s->failed && !s->reconfig && (s->state == kDecoding || s->state == kDrain || s->state == kStopped);
    if (requeue) o.owner = kFirmware;
  }
  if (last) {
    CoachzVideoEvent d = {}; d.kind = CoachzVideoEventDrained;
    push_locked(s, d);
    if (s->state == kDrain) { s->state = kStopped; s->drain_active = false; }
  }
  IOLockUnlock(s->lock);
  if (requeue) ftb(s, 1, o, tag);
}
void dispatch(unsigned dwords) {
  const uint32_t *p = gPacket, type = p[1];
  if (type == kMsgSysInit && dwords >= 4) { gInitDone = 1; log("SYS_INIT_DONE: error 0x%x, %u properties", p[2], p[3]); return; }
  if (type == kMsgSysPropertyInfo && dwords >= 4 && p[3] == kPropImageVersion) {
    char version[128]; const char *v = reinterpret_cast<const char *>(&p[4]); size_t k = 0;
    while (k + 1 < sizeof version && 16 + k < static_cast<size_t>(dwords) * 4 && v[k]) { version[k] = v[k]; ++k; }
    version[k] = 0; gVersionSeen = 1;
    log("image version \"%s\"", version);
    return;
  }
  if (type == kMsgSysDebug) {
    if (dwords >= 6 && gDebugLines < 400) {
      char text[180]; const char *v = reinterpret_cast<const char *>(&p[6]); size_t k = 0;
      while (k + 1 < sizeof text && 24 + k < static_cast<size_t>(dwords) * 4 && v[k]) { text[k] = (v[k] == '\n' ? ' ' : v[k]); ++k; }
      text[k] = 0; ++gDebugLines;
      log("fw: %s", text);
    }
    return;
  }
  if (dwords < 4) return;
  Session *s = session_by_id(p[2]);
  if (type == kMsgEventNotify) {
    // hfi_msg_event_notify_pkt: session header, event, event_data1, event_data2, then the extended data.
    if (p[3] == kEventSysError) {
      gSysError = 1;
      log("SYS_ERROR (data 0x%x 0x%x)", dwords > 4 ? p[4] : 0, dwords > 5 ? p[5] : 0);
      print_sfr("sys error");
      for (Session &x : gSessions) if (x.used) fail(&x, 0xffffffffu, "system error");
      return;
    }
    if (!s) { log("event 0x%x for unknown session 0x%x", p[3], p[2]); return; }
    if (p[3] == kEventSessionError) {
      // event_session_error: INVALID_SCALE_FACTOR, UNSUPPORT_BUFFERTYPE, UNSUPPORTED_SETTING and UPSCALE_NOT_SUPPORTED are not fatal.
      const uint32_t error = dwords > 4 ? p[4] : 0;
      if (error == 0x1012 || error == 0x1010 || error == 0x1008 || error == 0x1013) { log("session 0x%x: non-fatal error 0x%x", s->id, error); return; }
      fail(s, error, "session error event");
      return;
    }
    if (p[3] == kEventSeqChanged && dwords >= 6 && (p[4] == kEventSufficient || p[4] == kEventInsufficient)) { seq_changed(s, p, dwords); return; }
    if (p[3] == kEventReleaseRef) return;   // only DPB buffers are references here (split DPB/OPB)
    log("session 0x%x: event 0x%x data 0x%x", s->id, p[3], dwords > 4 ? p[4] : 0);
    return;
  }
  if (!s) { log("packet 0x%x for unknown session 0x%x", type, p[2]); return; }
  switch (type) {
  case kMsgSessionInit: case kMsgSessionEnd: case kMsgSessionAbort: case kMsgLoadResources: case kMsgStart: case kMsgStop:
  case kMsgFlush: case kMsgReleaseResources: case kMsgReleaseBuffers:
    session_reply(s, type, p[3]);
    return;
  case kMsgPropertyInfo:
    IOLockLock(s->lock);
    if (dwords >= 5 && p[4] == kPropBufreq) {
      const uint32_t n = (p[0] - 20) / sizeof(Bufreq);
      s->nreq = n < 16 ? n : 16;
      memcpy(s->req, &p[5], s->nreq * sizeof(Bufreq));
    }
    IOLockUnlock(s->lock);
    session_reply(s, type, 0);
    return;
  case kMsgEmptyBuffer: {
    // hfi_msg_session_empty_buffer_done_pkt: error, offset, filled, input_tag.
    const uint32_t tag = dwords > 6 ? p[6] : ~0u;
    if (tag < s->ninput) {
      IOLockLock(s->lock);
      s->input[tag].owner = kDriver;
      CoachzVideoEvent e = {}; e.kind = CoachzVideoEventInputDone; e.index = tag; e.status = p[3];
      push_locked(s, e);
      IOLockUnlock(s->lock);
    }
    return;
  }
  case kMsgFillBuffer: fbd(s, p, dwords); return;
  default: log("session 0x%x: packet 0x%x, %u dwords", s->id, type, dwords); return;
  }
}

volatile uint32_t gIrqPending, gIrqs, gIrqStatus;
int gIrqChan;
// venus_isr: the wrapper's status kept for the thread, the A2H soft interrupt and the status cleared (level-high SPI 174 drops).
void venus_irq(void *) {
  const uint32_t status = rd(gVenus, kIntrStatus);
  if (status & (0x4u | 0x10u | 1u << 30)) __atomic_fetch_or(&gIrqStatus, status, __ATOMIC_RELAXED);
  wr(gVenus, kA2hSoftIntClr, 1);
  wr(gVenus, kIntrClear, status);
  __atomic_fetch_add(&gIrqs, 1u, __ATOMIC_RELAXED);
  __atomic_store_n(&gIrqPending, 1, __ATOMIC_RELEASE);
  coachz_venus_thread_wakeup(&gIrqChan, 0, 0);
}
void read_queues() {
  for (unsigned n = 0; n < 256; ++n) {
    const int dwords = queue_read(kMsgQ, gPacket, sizeof gPacket / 4);
    if (!dwords) break;
    if (dwords < 0) { log("bad message packet dropped"); continue; }
    dispatch(static_cast<unsigned>(dwords));
  }
  // venus_flush_debug_queue
  for (unsigned n = 0; n < 64; ++n) {
    const int dwords = queue_read(kDbgQ, gPacket, sizeof gPacket / 4);
    if (!dwords) break;
    if (dwords > 0) dispatch(static_cast<unsigned>(dwords));
  }
}
bool gIrqAttached;
void message_thread(void *, wait_result_t) {
  uint64_t hz = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
  if (!hz) hz = 19200000;
  for (unsigned tick = 0;; ++tick) {
    // The interrupt wakes it; without one (or a lost one) it reads every 10 ms.
    (void)coachz_venus_assert_wait_deadline(&gIrqChan, 0, mach_absolute_time() + hz / 100);
    if (__atomic_load_n(&gIrqPending, __ATOMIC_ACQUIRE)) coachz_venus_thread_wakeup(&gIrqChan, 0, 0);
    coachz_venus_thread_block(nullptr);
    __atomic_store_n(&gIrqPending, 0, __ATOMIC_RELEASE);
    const uint32_t status = __atomic_exchange_n(&gIrqStatus, 0, __ATOMIC_ACQ_REL);
    if (status & 0x10u) {
      // WRAPPER_INTR_STATUS_A2HWD_MASK: the firmware's watchdog.
      if (!gSysError) { log("watchdog interrupt (status 0x%x)", status); print_sfr("watchdog"); smmu_report("watchdog"); }
      gSysError = 1;
      for (Session &x : gSessions) if (x.used) fail(&x, 0xfffffffeu, "watchdog");
    }
    if (!gIrqAttached) {
      const uint32_t raw = rd(gVenus, kIntrStatus);
      if (raw) { wr(gVenus, kA2hSoftIntClr, 1); wr(gVenus, kIntrClear, raw); }
    }
    read_queues();
    if (tick % 6000 == 5999) smmu_report("running");
  }
}

// ---- the decoder: vdec.c on HFI 4xx ----
void free_capture(Session *s) {
  for (uint32_t i = 0; i < s->ndpb; ++i) buffer_free(&s->dpb[i].buf);
  for (uint32_t i = 0; i < s->nopb; ++i) buffer_free(&s->opb[i].buf);
  IOLockLock(s->lock); s->ndpb = 0; s->nopb = 0; s->generation++; IOLockUnlock(s->lock);
}
void free_internal(Session *s, bool keep_persist) {
  uint32_t kept = 0;
  for (uint32_t i = 0; i < s->ninternal; ++i) {
    const uint32_t type = s->internal_type[i];
    if (keep_persist && (type == 4 || type == 5)) {
      s->internal[kept] = s->internal[i]; s->internal_size[kept] = s->internal_size[i]; s->internal_type[kept++] = type;
      continue;
    }
    // intbufs_unset_buffers: HFI_CMD_SESSION_RELEASE_BUFFERS with a response for each.
    const Buffer &b = s->internal[i];
    const uint32_t pkt[9] = {36, kCmdReleaseBuffers, s->id, type, s->internal_size[i], 0, 1, 1, b.iova};
    if (!s->failed && !gSysError) call(s, pkt, kMsgReleaseBuffers, "release internal buffer");
    buffer_free(&s->internal[i]);
  }
  s->ninternal = kept;
}
// intbufs_set_buffer: count_actual buffers of a type that has a size, one SET_BUFFERS each.
bool intbufs_set(Session *s, uint32_t type) {
  const Bufreq *r = bufreq(s, type);
  if (!r || !r->size) return true;
  for (uint32_t i = 0; i < r->count_actual; ++i) {
    if (s->ninternal >= kMaxInternal) { log("too many internal buffers"); return false; }
    Buffer *b = &s->internal[s->ninternal];
    if (!buffer_new(b, r->size, r->alignment)) return false;
    s->internal_size[s->ninternal] = r->size;   // bd.buffer_size is bufreq.size, the allocation only rounds it
    s->internal_type[s->ninternal++] = type;
    const uint32_t pkt[9] = {36, kCmdSessionSetBuffers, s->id, type, r->size, 0, r->size, 1, b->iova};
    if (!send(pkt, "internal buffer")) return false;
  }
  return true;
}
// vdec_output_conf: work mode, the UBWC DPB on OUTPUT and linear NV12 on OUTPUT2 (v4 makes UBWC mandatory), the sizes
// venus_helper_get_framesz_raw gives, checked against the firmware's requirements, and dynamic buffer mode on both outputs.
bool output_conf(Session *s) {
  const uint32_t w = s->width, h = s->height;
  const uint32_t mbs = (al(h, 16) * al(w, 16)) / 256;
  const uint32_t mode[1] = {s->pic_struct != 1 || mbs <= 3600 ? 1u : 2u};   // venus_helper_get_work_mode (NUM_MBS_720P; progressive is 1)
  const uint32_t opb_fmt[2] = {kBufOutput2, kNv12}, dpb_fmt[2] = {kBufOutput, kNv12Ubwc};
  const uint32_t ms_out[2] = {kBufOutput, 0}, ms_out2[2] = {kBufOutput2, 1}, out2_size[3] = {kBufOutput2, w, h};
  if (!(set_prop(s, kPropWorkMode, mode, 1, "work mode") && set_prop(s, kPropFormatSelect, opb_fmt, 2, "OPB format") &&
        set_prop(s, kPropMultiStream, ms_out, 2, "multistream OUTPUT") && set_prop(s, kPropMultiStream, ms_out2, 2, "multistream OUTPUT2") &&
        set_prop(s, kPropFormatSelect, dpb_fmt, 2, "DPB format") && set_prop(s, kPropFrameSize, out2_size, 3, "OUTPUT2 frame size") &&
        get_bufreq(s, "output requirements"))) return false;
  s->dpb_bytes = framesz_ubwc(w, h);
  s->opb_bytes = framesz_nv12(w, h);
  const Bufreq *rd_ = bufreq(s, kBufOutput), *ro = bufreq(s, kBufOutput2);
  if (!rd_ || !ro || rd_->size > s->dpb_bytes || ro->size > s->opb_bytes) {
    log("session 0x%x: output requirements %u and %u exceed %u and %u", s->id, rd_ ? rd_->size : 0, ro ? ro->size : 0, s->dpb_bytes, s->opb_bytes);
    return false;
  }
  const uint32_t opb_sz[2] = {kBufOutput2, s->opb_bytes}, dpb_sz[2] = {kBufOutput, s->dpb_bytes};
  // venus_helper_set_dyn_bufmode: pkt_session_set_property_4xx returns -ENOTSUPP for HFI_PROPERTY_PARAM_BUFFER_ALLOC_MODE and
  // venus_session_set_property takes that as done, so nothing is sent on 4xx (dynamic mode is the firmware's own there).
  return set_prop(s, kPropSizeActual, opb_sz, 2, "OPB size") && set_prop(s, kPropSizeActual, dpb_sz, 2, "DPB size");
}
// venus_helper_set_num_bufs(inst, num_input_bufs, VB2_MAX_FRAME, VB2_MAX_FRAME).
bool num_bufs(Session *s) {
  const uint32_t in[3] = {kBufInput, s->ninput, s->ninput}, out[3] = {kBufOutput, kVb2MaxFrame, kVb2MaxFrame},
      out2[3] = {kBufOutput2, kVb2MaxFrame, kVb2MaxFrame};
  return set_prop(s, kPropCountActual, in, 3, "input count") && set_prop(s, kPropCountActual, out, 3, "OUTPUT count") &&
      set_prop(s, kPropCountActual, out2, 3, "OUTPUT2 count");
}
void release(Session *s);
// vdec_open's defaults, vdec_session_init, vdec_queue_setup for OUTPUT and vdec_start_output.
bool create(Session *s, uint32_t codec, uint32_t width, uint32_t height) {
  s->codec = codec;
  s->width = width < kFrameMin ? kFrameMin : width > kFrameMax ? kFrameMax : width;
  s->height = height < kFrameMin ? kFrameMin : height > kFrameMax ? kFrameMax : height;
  // vdec_s_fmt: the OUTPUT format's size, and the capture format tried from it (its height aligned to 32).
  s->out_width = s->width; s->out_height = s->height; s->height = al(s->height, 32);
  s->pic_struct = 1; s->bit_depth = 0;   // vdec_inst_init: HFI_INTERLACE_FRAME_PROGRESSIVE, VIDC_BITDEPTH_8
  const uint32_t init[5] = {20, kCmdSessionInit, s->id, kDomainDecoder, codec};
  if (!call(s, init, kMsgSessionInit, "session init")) return false;
  s->state = kDeinit;
  // vdec_session_init: the input resolution at the minimum frame size; the firmware reads the stream's own.
  const uint32_t in_size[3] = {kBufInput, kFrameMin, kFrameMin};
  if (!set_prop(s, kPropFrameSize, in_size, 3, "input frame size") || !get_bufreq(s, "input requirements")) return false;
  const Bufreq *rin = bufreq(s, kBufInput);
  if (!rin) { log("session 0x%x: no input requirement", s->id); return false; }
  // vdec_queue_setup: at least count_min (hold_count on 4xx) input buffers of venus_helper_get_framesz, or the firmware's size.
  uint32_t in_num = rin->hold_count ? rin->hold_count : 1;
  if (in_num < 8) in_num = 8;
  s->ninput = in_num > COACHZ_VIDEO_MAX_INPUTS ? COACHZ_VIDEO_MAX_INPUTS : in_num;
  s->input_bytes = framesz_compressed(s->out_width, s->out_height);
  if (rin->size > s->input_bytes) s->input_bytes = al(rin->size, 4096);
  // vdec_set_properties: the sufficient-resources event (firmware 5.4.51 or newer; this image is 5.4-00059) and the conceal colour,
  // the control's default 0x8000800010 packed as vdec_set_properties and pkt_session_set_property_4xx do.
  const uint64_t conceal_ctrl = 0x8000800010ull;
  uint32_t conceal = static_cast<uint32_t>(conceal_ctrl & 0xffff);
  conceal |= static_cast<uint32_t>((conceal_ctrl >> 16) & 0xffff) << 10;
  conceal |= static_cast<uint32_t>((conceal_ctrl >> 32) & 0xffff) << 20;
  const uint32_t conceal_v4[2] = {(conceal & 0xff) | (((conceal >> 10) & 0xff) << 8) | (((conceal >> 20) & 0xff) << 16), conceal};
  const uint32_t enable[1] = {1};
  if (!set_prop(s, kPropSufficientEvent, enable, 1, "sufficient event") || !set_prop(s, kPropConcealColor, conceal_v4, 2, "conceal colour") ||
      !output_conf(s) || !num_bufs(s)) return false;
  // venus_helper_vb2_start_streaming: the internal buffers, each with the requirements read after the counts were set
  // (intbufs_set_buffer calls venus_helper_get_bufreq), no registration in dynamic mode, load resources, start.
  if (!get_bufreq(s, "internal requirements")) return false;
  for (uint32_t type : kInternalTypes) if (!intbufs_set(s, type)) return false;
  if (!cmd(s, kCmdLoadResources, kMsgLoadResources, "load resources") || !cmd(s, kCmdStart, kMsgStart, "start")) return false;
  for (uint32_t i = 0; i < s->ninput; ++i) {
    if (!buffer_new(&s->input[i].buf, s->input_bytes, 0)) return false;
    s->input[i].owner = kDriver;
  }
  s->state = kInit;
  load_scale();
  log("session 0x%x: codec 0x%x %ux%u started, %u inputs of %u KiB, %u internal buffers", s->id, codec, width, height, s->ninput,
      s->input_bytes / 1024, s->ninternal);
  return !s->failed;
}
// vdec_stop_capture's DRC branch, then vdec_start_capture's reconfiguration: the output configuration for the new sequence, the
// counts, the scratch buffers again (venus_helper_intbufs_realloc), the DPB (venus_helper_alloc_dpb_bufs: the OUTPUT requirement's
// count_min), SESSION_CONTINUE, the DPB and capture buffers queued (venus_helper_queue_dpb_bufs, venus_helper_process_initial_cap_bufs).
bool start_capture(Session *s) {
  if (s->state == kDrc) { flush_session(s, kFlushOutput); s->state = kCaptureSetup; }
  if (s->state != kCaptureSetup) { IOLockLock(s->lock); s->reconfig = false; IOLockUnlock(s->lock); return true; }
  // vdec_event_change switches a 10-bit sequence to P010 / TP10 UBWC; the client here takes 8-bit NV12 only.
  if (s->bit_depth) { log("session 0x%x: bit depth 0x%x is not 8", s->id, s->bit_depth); return false; }
  free_capture(s);
  // vdec_event_change through vdec_try_fmt_common: the width clamped, the capture height also aligned to 32.
  const uint32_t w = s->seq_w < kFrameMin ? kFrameMin : s->seq_w > kFrameMax ? kFrameMax : s->seq_w;
  const uint32_t h = s->seq_h < kFrameMin ? kFrameMin : s->seq_h > kFrameMax ? kFrameMax : s->seq_h;
  s->width = w; s->height = al(h, 32); s->out_width = s->seq_w; s->out_height = s->seq_h;
  if (!output_conf(s) || !num_bufs(s)) return false;
  // venus_helper_intbufs_realloc and venus_helper_alloc_dpb_bufs read the requirements after the counts.
  free_internal(s, true);
  if (!get_bufreq(s, "capture requirements")) return false;
  for (uint32_t i = 0; i < 3; ++i) if (!intbufs_set(s, kInternalTypes[i])) return false;
  load_scale();
  const Bufreq *rdpb = bufreq(s, kBufOutput), *ropb = bufreq(s, kBufOutput2);
  uint32_t ndpb = rdpb ? rdpb->hold_count : 0;
  if (ndpb > kVb2MaxFrame) ndpb = kVb2MaxFrame;
  for (uint32_t i = 0; i < ndpb; ++i) {
    if (!buffer_new(&s->dpb[i].buf, s->dpb_bytes, rdpb->alignment)) { s->ndpb = i; return false; }
    s->dpb[i].owner = kDriver;
  }
  s->ndpb = ndpb;
  // The capture queue: the firmware's minimum for the sequence (V4L2_CID_MIN_BUFFERS_FOR_CAPTURE) or the OUTPUT2 requirement, and four
  // more for the pictures the client holds while it copies them.
  uint32_t nopb = s->seq_count;
  if (ropb && ropb->hold_count > nopb) nopb = ropb->hold_count;
  nopb += 4;
  if (nopb > COACHZ_VIDEO_MAX_OUTPUTS) nopb = COACHZ_VIDEO_MAX_OUTPUTS;
  for (uint32_t i = 0; i < nopb; ++i) {
    if (!buffer_new(&s->opb[i].buf, s->opb_bytes, ropb ? ropb->alignment : 0)) { s->nopb = i; return false; }
    s->opb[i].owner = kDriver;
  }
  IOLockLock(s->lock); s->nopb = nopb; IOLockUnlock(s->lock);
  if (!cmd(s, kCmdContinue, 0, "continue")) return false;
  for (uint32_t i = 0; i < s->ndpb; ++i) { s->dpb[i].owner = kFirmware; if (!ftb(s, 0, s->dpb[i], kVb2MaxFrame + i)) return false; }
  for (uint32_t i = 0; i < s->nopb; ++i) { s->opb[i].owner = kFirmware; if (!ftb(s, 1, s->opb[i], s->generation << 16 | i)) return false; }
  IOLockLock(s->lock);
  const bool eos = s->drain_pending;
  if (eos) { s->drain_pending = false; s->drain_active = true; }
  s->state = s->drain_active ? kDrain : kDecoding;
  s->reconfig = false;
  CoachzVideoEvent e = {};
  e.kind = CoachzVideoEventOutputs; e.generation = s->generation; e.count = s->nopb; e.bytes = s->opb_bytes;
  e.stride = al(s->width, 128); e.scanlines = al(s->height, 32); e.width = s->out_width; e.height = s->out_height;
  e.crop_left = s->crop_l; e.crop_top = s->crop_t; e.crop_width = s->crop_w; e.crop_height = s->crop_h;
  push_locked(s, e);
  IOLockUnlock(s->lock);
  log("session 0x%x: capture %ux%u (crop %ux%u), DPB %u x %u KiB, OPB %u x %u KiB, generation %u, %zu ranges, %zu table pages", s->id,
      s->out_width, s->out_height, s->crop_w, s->crop_h, s->ndpb, s->dpb_bytes / 1024, s->nopb, s->opb_bytes / 1024, s->generation,
      gRangeCount, gPool.used);
  if (eos) etb(s, 0xdeadb000u, 0, 0, kFlagEos, 0xffff, 0);
  return true;
}
// vdec_session_release after vdec_stop_streaming of both queues: flush, stop, unload resources, the internal buffers, deinit, abort on
// any failure, then every buffer.
void release(Session *s) {
  if (s->state != kDeinit && !s->failed && !gSysError) flush_session(s, kFlushAll);
  bool abort = s->failed || gSysError;
  if (!abort && s->state != kDeinit) {
    const bool stopped = cmd(s, kCmdStop, kMsgStop, "stop");
    const bool unloaded = cmd(s, kCmdReleaseResources, kMsgReleaseResources, "release resources");
    abort = !stopped || !unloaded;
  }
  free_internal(s, false);
  if (!gSysError && !cmd(s, kCmdSessionEnd, kMsgSessionEnd, "session end")) abort = true;
  if (abort && !gSysError) cmd(s, kCmdSessionAbort, kMsgSessionAbort, "session abort");
  free_capture(s);
  for (uint32_t i = 0; i < s->ninput; ++i) buffer_free(&s->input[i].buf);
  s->ninput = 0;
  s->state = kDeinit;
  log("session 0x%x released: %u inputs, %u frames%s", s->id, s->inputs, s->frames, abort ? ", aborted" : "");
}

Session *session_open() {
  IOLockLock(gSessionsLock);
  Session *found = nullptr;
  for (unsigned i = 0; i < kMaxSessions && !found; ++i) {
    Session *s = &gSessions[i];
    if (s->used) continue;
    IOLock *lock = s->lock, *op = s->op;
    bzero(s, sizeof *s);
    s->lock = lock; s->op = op;
    s->id = 0x56000000u | (++gSerial & 0xffffu) << 4 | i;
    s->used = true; s->state = kDeinit;
    found = s;
  }
  IOLockUnlock(gSessionsLock);
  return found;
}
void session_close(Session *s) {
  IOLockLock(s->op);
  release(s);
  IOLockUnlock(s->op);
  IOLockLock(gSessionsLock); s->used = false; IOLockUnlock(gSessionsLock);
  load_scale();
}

// The client's operations, each under s->op.
IOReturn queue_input(Session *s, uint32_t index, uint32_t bytes, uint64_t token, uint32_t flags) {
  if (s->failed || gSysError) return kIOReturnNotReady;
  if (index >= s->ninput || !bytes || bytes > s->input_bytes) return kIOReturnBadArgument;
  IOLockLock(s->op);
  IOReturn result = kIOReturnSuccess;
  IOLockLock(s->lock);
  if (s->input[index].owner != kDriver) result = kIOReturnBusy;
  else {
    s->input[index].owner = kFirmware;
    if (s->state == kStopped) s->state = kDecoding;   // V4L2_DEC_CMD_START after a drain
    if (bytes > s->payload_max) s->payload_max = bytes;
    s->inputs++;
  }
  IOLockUnlock(s->lock);
  // session_process_buf: the bitstream at offset 0, the client's token as the timestamp the firmware carries to the picture.
  if (result == kIOReturnSuccess) {
    __asm__ volatile("dsb sy" ::: "memory");
    const bool scale = s->inputs < 4 || (s->inputs & 63) == 0;
    if (!etb(s, s->input[index].buf.iova, s->input[index].buf.bytes, bytes, 0, index, token)) {
      IOLockLock(s->lock); s->input[index].owner = kDriver; IOLockUnlock(s->lock);
      result = kIOReturnError;
    } else if (scale) {
      load_scale();
    }
  }
  IOLockUnlock(s->op);
  return result;
}
IOReturn return_output(Session *s, uint32_t generation, uint32_t index) {
  IOLockLock(s->op);
  IOLockLock(s->lock);
  bool queue = false;
  if (generation == s->generation && index < s->nopb && s->opb[index].owner == kClient) {
    queue = !s->failed && !s->reconfig && (s->state == kDecoding || s->state == kDrain || s->state == kStopped);
    s->opb[index].owner = queue ? kFirmware : kDriver;
  }
  IOLockUnlock(s->lock);
  if (queue) ftb(s, 1, s->opb[index], generation << 16 | index);
  IOLockUnlock(s->op);
  return kIOReturnSuccess;
}
// vdec_decoder_cmd(V4L2_DEC_CMD_STOP): an empty EOS input at 0xdeadb000 once both queues stream; before the first sequence nothing
// is pending.
IOReturn drain(Session *s) {
  if (s->failed || gSysError) return kIOReturnNotReady;
  IOLockLock(s->op);
  IOLockLock(s->lock);
  const bool streaming = s->state == kDecoding || s->state == kDrc;
  if (s->state == kDecoding) { s->state = kDrain; s->drain_active = true; }
  // Input queued before the first sequence change: the EOS follows once the capture queue streams (start_capture).
  else if ((s->state == kInit || s->state == kCaptureSetup) && s->inputs) s->drain_pending = true;
  else if (s->state != kDrain && s->state != kDrc) { CoachzVideoEvent d = {}; d.kind = CoachzVideoEventDrained; push_locked(s, d); }
  IOLockUnlock(s->lock);
  if (streaming) etb(s, 0xdeadb000u, 0, 0, kFlagEos, 0xffff, 0);
  IOLockUnlock(s->op);
  return kIOReturnSuccess;
}
// vdec_stop_output (HFI_FLUSH_ALL, then SEEK) and vdec_start_output from SEEK: the DPB queued again, the capture buffers the firmware
// returned queued again by the driver itself.
IOReturn flush_client(Session *s) {
  if (s->failed || gSysError) return kIOReturnNotReady;
  IOLockLock(s->op);
  const bool capture = s->state == kDecoding || s->state == kDrain || s->state == kStopped;
  if (s->state != kDeinit) flush_session(s, kFlushAll);
  if (capture) {
    IOLockLock(s->lock); s->state = kDecoding; s->drain_active = false; IOLockUnlock(s->lock);
    for (uint32_t i = 0; i < s->ndpb; ++i) if (s->dpb[i].owner == kDriver) { s->dpb[i].owner = kFirmware; ftb(s, 0, s->dpb[i], kVb2MaxFrame + i); }
    for (uint32_t i = 0; i < s->nopb; ++i) {
      IOLockLock(s->lock);
      const bool idle = s->opb[i].owner == kDriver;
      if (idle) s->opb[i].owner = kFirmware;
      IOLockUnlock(s->lock);
      if (idle) ftb(s, 1, s->opb[i], s->generation << 16 | i);
    }
  }
  CoachzVideoEvent done = {}; done.kind = CoachzVideoEventFlushed; done.generation = s->generation;
  push(s, done);
  IOLockUnlock(s->op);
  return kIOReturnSuccess;
}
// Up to max events, waiting at most ms for the first; a pending sequence change is configured first (vdec_start_capture runs when
// the client would stream the capture queue on again).
uint32_t wait_events(Session *s, CoachzVideoEvent *out, uint32_t max, uint32_t ms) {
  uint64_t deadline = 0;
  clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
  for (;;) {
    if (s->reconfig && !s->failed) {
      IOLockLock(s->op);
      if (s->reconfig && !start_capture(s)) fail(s, 0xfffffffdu, "capture configuration");
      IOLockUnlock(s->op);
    }
    IOLockLock(s->lock);
    while (s->head == s->tail && !s->reconfig &&
           IOLockSleepDeadline(s->lock, &s->head, deadline, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED) {}
    uint32_t n = 0;
    while (n < max && s->tail != s->head) out[n++] = s->ring[s->tail++ % kRing];
    const bool again = !n && s->reconfig && !s->failed;
    IOLockUnlock(s->lock);
    if (!again) return n;
  }
}

// ---- the boot self-test through the same decoder: one IDR picture of I_PCM macroblocks (ITU-T H.264 7.3), so the decoded frame
// must equal these samples exactly ----
uint8_t test_y(unsigned x, unsigned y) { return static_cast<uint8_t>(16 + (x * 3 + y * 5) % 200); }
uint8_t test_cb(unsigned x, unsigned y) { return static_cast<uint8_t>(64 + (x * 7 + y) % 100); }
uint8_t test_cr(unsigned x, unsigned y) { return static_cast<uint8_t>(128 + (x + y * 3) % 100); }
constexpr uint32_t kTestWidth = 128, kTestHeight = 128;

struct Bits { uint8_t *buf; size_t cap, bytes; uint32_t acc; int n; bool bad; };
void put(Bits &b, uint32_t v, int bits) {
  for (int i = bits - 1; i >= 0; --i) {
    b.acc = (b.acc << 1) | ((v >> i) & 1);
    if (++b.n == 8) { if (b.bytes < b.cap) b.buf[b.bytes++] = static_cast<uint8_t>(b.acc); else b.bad = true; b.acc = 0; b.n = 0; }
  }
}
void ue(Bits &b, uint32_t v) { const uint32_t x = v + 1; const int len = 32 - __builtin_clz(x); put(b, 0, len - 1); put(b, x, len); }
void se(Bits &b, int v) { ue(b, v <= 0 ? static_cast<uint32_t>(-2 * v) : static_cast<uint32_t>(2 * v - 1)); }
void rbsp_trailing(Bits &b) { put(b, 1, 1); while (b.n) put(b, 0, 1); }
// Annex B: start code, NAL header, payload with emulation prevention (7.4.1).
size_t nal(uint8_t *out, size_t cap, uint8_t header, const uint8_t *rbsp, size_t n) {
  size_t o = 0; unsigned zeros = 0;
  if (cap < 5) return 0;
  out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 1; out[o++] = header;
  for (size_t i = 0; i < n; ++i) {
    if (zeros >= 2 && rbsp[i] <= 3) { if (o >= cap) return 0; out[o++] = 3; zeros = 0; }
    if (o >= cap) return 0;
    out[o++] = rbsp[i]; zeros = rbsp[i] ? 0 : zeros + 1;
  }
  return o;
}
size_t make_stream(uint8_t *out, size_t cap, uint8_t *scratch, size_t scratch_cap) {
  const unsigned mbw = kTestWidth / 16, mbh = kTestHeight / 16;
  size_t at = 0, n;
  Bits b = {scratch, scratch_cap, 0, 0, 0, false};
  // SPS: Baseline (constraint sets 0 and 1), level 1.0, POC type 2, one reference frame, frame MBs only, no cropping, no VUI.
  put(b, 66, 8); put(b, 0xc0 >> 2, 6); put(b, 0, 2); put(b, 10, 8); ue(b, 0); ue(b, 0); ue(b, 2); ue(b, 1); put(b, 0, 1);
  ue(b, mbw - 1); ue(b, mbh - 1); put(b, 1, 1); put(b, 1, 1); put(b, 0, 1); put(b, 0, 1); rbsp_trailing(b);
  if (b.bad || !(n = nal(out + at, cap - at, 0x67, scratch, b.bytes))) return 0;
  at += n; b = {scratch, scratch_cap, 0, 0, 0, false};
  // PPS: CAVLC, one slice group, QP 26, deblocking control present.
  ue(b, 0); ue(b, 0); put(b, 0, 1); put(b, 0, 1); ue(b, 0); ue(b, 0); ue(b, 0); put(b, 0, 1); put(b, 0, 2); se(b, 0); se(b, 0); se(b, 0);
  put(b, 1, 1); put(b, 0, 1); put(b, 0, 1); rbsp_trailing(b);
  if (b.bad || !(n = nal(out + at, cap - at, 0x68, scratch, b.bytes))) return 0;
  at += n; b = {scratch, scratch_cap, 0, 0, 0, false};
  // IDR slice: first MB 0, slice type 7 (I), PPS 0, frame_num 0 (4 bits), idr_pic_id 0, no_output_of_prior_pics 0,
  // long_term_reference 0, slice_qp_delta 0, deblocking disabled; then every macroblock I_PCM (mb_type 25), byte aligned, its 256
  // luma then 64 Cb and 64 Cr samples in raster order.
  ue(b, 0); ue(b, 7); ue(b, 0); put(b, 0, 4); ue(b, 0); put(b, 0, 1); put(b, 0, 1); se(b, 0); ue(b, 1);
  for (unsigned my = 0; my < mbh; ++my)
    for (unsigned mx = 0; mx < mbw; ++mx) {
      ue(b, 25);
      while (b.n) put(b, 0, 1);
      for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 16; ++x) put(b, test_y(mx * 16 + x, my * 16 + y), 8);
      for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) put(b, test_cb(mx * 8 + x, my * 8 + y), 8);
      for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) put(b, test_cr(mx * 8 + x, my * 8 + y), 8);
    }
  rbsp_trailing(b);
  if (b.bad || !(n = nal(out + at, cap - at, 0x65, scratch, b.bytes))) return 0;
  return at + n;
}
void self_test() {
  static uint8_t stream[64 * 1024], scratch[64 * 1024];
  static CoachzVideoEvent events[COACHZ_VIDEO_EVENTS_PER_WAIT];
  const size_t stream_bytes = make_stream(stream, sizeof stream, scratch, sizeof scratch);
  Session *s = session_open();
  if (!s || !stream_bytes) { log("self-test: no session"); return; }
  bool ok = false, drained = false;
  uint32_t frame_index = ~0u, frame_offset = 0, outputs = 0, stride = 0, scanlines = 0;
  uint64_t token = 0;
  IOLockLock(s->op);
  const bool created = create(s, CoachzVideoH264, kTestWidth, kTestHeight);
  IOLockUnlock(s->op);
  if (created && stream_bytes <= s->input_bytes) {
    memcpy(const_cast<uint8_t *>(s->input[0].buf.va), stream, stream_bytes);
    ok = queue_input(s, 0, static_cast<uint32_t>(stream_bytes), 0x5eed, CoachzVideoInputEndOfFrame) == kIOReturnSuccess &&
         drain(s) == kIOReturnSuccess;
    for (unsigned t = 0; ok && t < 30 && !drained; ++t) {
      const uint32_t n = wait_events(s, events, COACHZ_VIDEO_EVENTS_PER_WAIT, 100);
      for (uint32_t i = 0; i < n; ++i) {
        const CoachzVideoEvent &e = events[i];
        if (e.kind == CoachzVideoEventOutputs) { outputs++; stride = e.stride; scanlines = e.scanlines; }
        if (e.kind == CoachzVideoEventFrame && e.index != ~0u && frame_index == ~0u) { frame_index = e.index; frame_offset = e.offset; token = e.token; }
        if (e.kind == CoachzVideoEventDrained) drained = true;
        if (e.kind == CoachzVideoEventError) ok = false;
      }
    }
  }
  if (frame_index != ~0u && frame_index < s->nopb) {
    const volatile uint8_t *frame = s->opb[frame_index].buf.va + frame_offset;
    uint32_t bad_y = 0, bad_c = 0;
    for (unsigned y = 0; y < kTestHeight; ++y)
      for (unsigned x = 0; x < kTestWidth; ++x) bad_y += frame[y * stride + x] != test_y(x, y);
    const volatile uint8_t *uv = frame + stride * scanlines;
    for (unsigned y = 0; y < kTestHeight / 2; ++y)
      for (unsigned x = 0; x < kTestWidth / 2; ++x) bad_c += (uv[y * stride + 2 * x] != test_cb(x, y)) + (uv[y * stride + 2 * x + 1] != test_cr(x, y));
    log("self-test: frame token 0x%llx, %u luma and %u chroma samples differ of %u and %u%s", token, bad_y, bad_c, kTestWidth * kTestHeight,
        kTestWidth * kTestHeight / 2, bad_y || bad_c ? "" : ": the decoded picture is exact");
    return_output(s, s->generation, frame_index);
  } else {
    log("self-test: no picture (created %d, queued %d, output generations %u, drained %d, failed %d)", created, ok, outputs, drained, s->failed);
  }
  smmu_report("self-test");
  session_close(s);
}

}  // namespace

// ---- the IOKit service and its user client (CoachzVideoAbi.h) ----
class CoachzVenus : public IOService {
  OSDeclareDefaultStructors(CoachzVenus)
public:
  bool publish() {
    // Any process may decode (the decoder loads wherever VideoToolbox builds its registry), as with the GPU.
    setProperty("NativeSandboxGrantProcess", "*");
    setProperty("IOUserClientClass", "CoachzVenusUserClient");
    registerService();
    return true;
  }
};
OSDefineMetaClassAndStructors(CoachzVenus, IOService)

class CoachzVenusUserClient : public IOUserClient {
  OSDeclareDefaultStructors(CoachzVenusUserClient)
  Session *session = nullptr;
  void close_session() {
    Session *s = __atomic_exchange_n(&session, static_cast<Session *>(nullptr), __ATOMIC_ACQ_REL);
    if (s) session_close(s);
  }
public:
  bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override {
    session = nullptr;
    return IOUserClient::initWithTask(task, security, type, properties);
  }
  IOReturn clientClose() override {
    close_session();
    terminate();
    return kIOReturnSuccess;
  }
  IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override {
    Session *s = session;
    if (!s) return kIOReturnNotReady;
    const uint32_t kind = type & 0xf00, index = type & 0xff;
    IOMemoryDescriptor *chosen = nullptr;
    IOLockLock(s->op);
    if (kind == CoachzVideoMemoryInput && index < s->ninput) chosen = s->input[index].buf.md;
    if (kind == CoachzVideoMemoryOutput && index < s->nopb) chosen = s->opb[index].buf.md;
    if (chosen) chosen->retain();   // the caller consumes the reference
    IOLockUnlock(s->op);
    if (!chosen) return kIOReturnBadArgument;
    *options = kIOMapWriteCombineCache;
    *memory = chosen;
    return kIOReturnSuccess;
  }
  IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override {
    const uint32_t n = args->scalarInputCount;
    const uint64_t *in = args->scalarInput;
    if (selector == CoachzVideoCreate) {
      if (n != 3 || session || args->structureOutputSize < sizeof(CoachzVideoInfo)) return kIOReturnBadArgument;
      const uint32_t codec = static_cast<uint32_t>(in[0]);
      if (codec != CoachzVideoH264 && codec != CoachzVideoHEVC && codec != CoachzVideoVP8 && codec != CoachzVideoVP9) return kIOReturnUnsupported;
      if (gSysError) return kIOReturnNotReady;
      Session *s = session_open();
      if (!s) return kIOReturnNoResources;
      IOLockLock(s->op);
      const bool ok = create(s, codec, static_cast<uint32_t>(in[1]), static_cast<uint32_t>(in[2]));
      IOLockUnlock(s->op);
      if (!ok) { session_close(s); return kIOReturnError; }
      session = s;
      CoachzVideoInfo info = {COACHZ_VIDEO_VERSION, s->ninput, s->input_bytes, 0};
      memcpy(args->structureOutput, &info, sizeof info);
      args->structureOutputSize = sizeof info;
      return kIOReturnSuccess;
    }
    Session *s = session;
    if (!s) return kIOReturnNotReady;
    switch (selector) {
    case CoachzVideoQueueInput:
      if (n != 4) return kIOReturnBadArgument;
      return queue_input(s, static_cast<uint32_t>(in[0]), static_cast<uint32_t>(in[1]), in[2], static_cast<uint32_t>(in[3]));
    case CoachzVideoReturnOutput:
      if (n != 2) return kIOReturnBadArgument;
      return return_output(s, static_cast<uint32_t>(in[0]), static_cast<uint32_t>(in[1]));
    case CoachzVideoDrain: return drain(s);
    case CoachzVideoFlush: return flush_client(s);
    case CoachzVideoWaitEvent: {
      if (n != 1 || args->structureOutputSize < sizeof(CoachzVideoEvent)) return kIOReturnBadArgument;
      uint32_t max = args->structureOutputSize / sizeof(CoachzVideoEvent);
      if (max > COACHZ_VIDEO_EVENTS_PER_WAIT) max = COACHZ_VIDEO_EVENTS_PER_WAIT;
      const uint32_t ms = in[0] > 5000 ? 5000 : static_cast<uint32_t>(in[0]);
      const uint32_t got = wait_events(s, static_cast<CoachzVideoEvent *>(args->structureOutput), max, ms);
      args->structureOutputSize = got * sizeof(CoachzVideoEvent);
      return got ? kIOReturnSuccess : kIOReturnTimeout;
    }
    case CoachzVideoDestroy: close_session(); return kIOReturnSuccess;
    default: return kIOReturnBadArgument;
    }
  }
};
OSDefineMetaClassAndStructors(CoachzVenusUserClient, IOUserClient)

namespace {
void publish_service() {
  // Registered now rather than at boot, like the other late classes here.
  const OSSymbol *name = OSSymbol::withCString(COACHZ_VIDEO_CLASS);
  const bool absent = name && !OSMetaClass::getMetaClassWithName(name);
  OSSafeReleaseNULL(name);
  void *handle = absent ? OSMetaClass::preModLoad(g_kernel_kmod_info.name) : nullptr;
  if (!handle) { log("service class not registered"); return; }
  coachz_venus_static_init();
  const OSReturn meta = OSMetaClass::postModLoad(handle);
  CoachzVenus *service = meta ? nullptr : OSTypeAlloc(CoachzVenus);
  IOService *provider = IOService::getResourceService();
  const bool attached = service && service->init() && service->attach(provider);
  const bool running = attached && service->start(provider) && service->publish();
  log("service %s (meta 0x%x)", running ? "published" : "NOT published", meta);
  OSSafeReleaseNULL(service);
}

void venus_thread(void *, wait_result_t) {
  // venus.mbn arrives with the modem firmware (coachz_wifi.c reads the blob early); the apps SMMU with the USB host stack.
  for (unsigned s = 0; s < 120 && !__atomic_load_n(&coachz_venus_fw, __ATOMIC_ACQUIRE); ++s) IOSleep(1000);
  if (!__atomic_load_n(&coachz_venus_fw, __ATOMIC_ACQUIRE)) { log("no venus.mbn in the firmware blob; the codec stays off"); return; }
  step("windows and memory");
  gHfiLock = IOLockAlloc(); gSmmuLock = IOLockAlloc(); gScaleLock = IOLockAlloc(); gSessionsLock = IOLockAlloc();
  for (Session &s : gSessions) { s.lock = IOLockAlloc(); s.op = IOLockAlloc(); }
  gSmmu.gr0 = reinterpret_cast<volatile uint8_t *>(ml_io_map(kSmmuBase, 0x1000));
  gSmmu.gr1 = reinterpret_cast<volatile uint8_t *>(ml_io_map(kSmmuBase + 0x1000, 0x1000));
  for (unsigned s = 0; s < 120 && gSmmu.gr0 && (gr0(kGr0Scr0) & kScr0ClientPd); ++s) IOSleep(1000);
  gVenus = reinterpret_cast<volatile uint8_t *>(ml_io_map(kVenusBase, kVenusBytes));
  gVideoCc = reinterpret_cast<volatile uint8_t *>(ml_io_map(kVideoCcBase, kVideoCcBytes));
  gGcc = reinterpret_cast<volatile uint8_t *>(ml_io_map(kGccPage, 0x1000));
  if (!gSmmu.gr0 || !gSmmu.gr1 || !gVenus || !gVideoCc || !gGcc || !gHfiLock || !gSmmuLock || !gScaleLock || !gSessionsLock) {
    log("register window or lock missing"); return;
  }
  for (Session &s : gSessions) if (!s.lock || !s.op) { log("no session locks"); return; }
  if (!memory_get(&gFwMemory, kFwBytes, "firmware region") || !memory_get(&gSharedMemory, kSharedBytes, "shared queues")) return;
  gFw = gFwMemory.va; gShared = gSharedMemory.va;
  log("venus.mbn %zu bytes", coachz_venus_fw_bytes);

  step("power");
  if (!power_on()) { log("power-on failed; the codec stays off"); return; }
  size_t span = 0;
  step("apps SMMU bank");
  if (!smmu_attach()) return;
  step("firmware copy");
  if (!load_firmware(coachz_venus_fw, coachz_venus_fw_bytes, &span)) return;
  queues_init();
  step("ARM9 out of reset");
  reset_cpu();
  IOSleep(10);
  smmu_report("after release");
  step("CTRL_INIT");
  if (!run()) { smmu_report("boot"); print_sfr("boot"); return; }
  step("SYS packets");

  // venus_core_init: SYS_INIT, the image version, then venus_sys_set_default_properties (debug to the queue: error and fatal, as
  // venus_fw_debug; codec power plane control on, as venus_fw_low_power_mode).
  const uint32_t init[3] = {12, kCmdSysInit, kArchOx};
  const uint32_t version[4] = {16, kCmdSysGetProperty, 1, kPropImageVersion};
  const uint32_t debug[6] = {24, kCmdSysSetProperty, 1, kPropDebugConfig, kDebugError | kDebugFatal, kDebugModeQueue};
  const uint32_t power[5] = {20, kCmdSysSetProperty, 1, kPropPowerPlane, 1};
  qhdr(kMsgQ)[kQRxReq] = 1; barrier();
  const bool sent = send(init, "SYS_INIT") && send(version, "image version") && send(debug, "debug config") && send(power, "power control");
  for (unsigned ms = 0; ms < 3000 && !(gInitDone && gVersionSeen); ms += 10) { read_queues(); IOSleep(10); }
  smmu_report("after SYS_INIT");
  print_sfr("after SYS_INIT");
  log("SYS packets sent %d; init %s, version %s, SCIACMDARG0 0x%x, CPU status 0x%x", sent, gInitDone ? "done" : "MISSING",
      gVersionSeen ? "read" : "MISSING", rd(gVenus, kSciaCmdArg0), rd(gVenus, kCpuStatus));
  if (!gInitDone) return;
  if (!vcodec0_on()) { log("vcodec0 did not power on; no sessions"); return; }

  // From here the message thread reads the queues, woken by SPI 174 (venus_isr), polling every 10 ms without it.
  thread_t thread = THREAD_NULL;
  if (kernel_thread_start(message_thread, nullptr, &thread) != KERN_SUCCESS) { log("no message thread"); return; }
  thread_deallocate(thread);
  uint32_t readback = 0; uint64_t router = 0;
  const int attached = armcpu_spi_attach(32 + 174, false, venus_irq, nullptr, &readback, &router);
  gIrqAttached = attached == 0;
  log("SPI 174: %s (readback 0x%x, router 0x%llx)", gIrqAttached ? "attached" : "NOT attached, polled", readback, router);

  step("decode self-test");
  self_test();
  log("interrupts so far %u", gIrqs);
  publish_service();
}

}  // namespace

// From NativeRamBlock::start on the eMMC root, after the USB host stack (which enables the apps SMMU).
extern "C" void coachz_venus_start(void) {
  thread_t thread = THREAD_NULL;
  if (kernel_thread_start(venus_thread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
  else log("no thread");
}
