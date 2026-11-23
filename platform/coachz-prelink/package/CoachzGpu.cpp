// SPDX-License-Identifier: MIT
// The Adreno 618 board code (coachz-gpu=9): coachz_gpu_seq.h has left the GMU running and GX up with a618's hw_init registers
// written; this file builds the GPU's SMMU context with the Adreno740 kit, starts the CP on its ring and hands the live objects to
// NativeAdrenoPublish, which serves Turnip, then publishes the accelerator the original Metal discovery looks for. Order adapted
// from UniXNU-sheng package/GpuPowerProbe.cpp (MIT); the CP start itself is Linux a6xx_cp_init, which the A740's differs from.
// There is no power collapse and no interrupt: GPU_SET stays up, and the ring's waiters poll.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/OSKextLib.h>
#include <string.h>
#include "NativeAdrenoABI.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoMemory.h"
#include "NativeAdrenoVm.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoSubmit.h"
#include "CoachzGpuHang.h"
#include "NativeAdrenoTimer.h"
#include "AdrenoPM4.h"
#include "ArmSmmu4KLive.h"
#include <pexpert/device_tree.h>

extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *, const NativeAdrenoRingClock *,
    const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);
extern "C" volatile uint32_t coachz_gpu_level_cap;   // coachz_thermal.c: 7, or 6 while the temperature clamp holds
extern "C" void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
extern "C" IOService *native_adreno_accelerator_allocate(void);
extern "C" unsigned native_adreno_accelerator_instance_size(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" void native_adreno_metal_report(void);
extern "C" void native_adreno_publisher_lock(int locked);
extern "C" void native_adreno_set_restart(NativeAdrenoRestart restart, void *cookie);
extern "C" volatile uint32_t native_adreno_recovery[8];
extern "C" volatile uint32_t native_adreno_unstick[15];
// NativeLegacyDisplay: swaps retired when the scanout client took the frame (sheng-early-retire), and those whose copy then failed.
extern "C" volatile uint64_t native_legacy_early_retires, native_legacy_early_failures;
extern "C" {
extern volatile uint32_t native_adreno_ring_log[4][4 + 112];
extern volatile uint32_t native_adreno_ring_log_count;
extern volatile uint32_t native_adreno_ib_log[8][4 + 208];
extern volatile uint32_t native_adreno_ib_log_count;
extern volatile uint64_t native_adreno_submit_ticks[64], native_adreno_submit_cost[3], native_adreno_wait_outer[5], native_gop_wait_stats[7];
extern volatile uint64_t native_adreno_submit_phase[7];
extern volatile uint32_t native_adreno_ib_probe_request, native_adreno_ib_probe_fence, native_adreno_ib_probe_count, native_adreno_ib_probe_pid;
extern volatile uint32_t native_adreno_ib_probe_dwords[36];
extern volatile uint32_t native_adreno_ib_probe_small[16], native_adreno_ib_probe_small_at, native_adreno_ib_probe_small_count;
extern volatile uint32_t native_adreno_lock_holder[2], native_adreno_lock_hold_max[32];
extern volatile uint64_t native_adreno_view_lock[4];
extern volatile uint32_t native_adreno_method_stats[20][4];
extern volatile uint64_t native_adreno_maint_phase[8][2], native_adreno_publish_kinds[4][2];
extern volatile uint64_t native_adreno_vm_phase_us[2][8], native_adreno_vm_kind_us[5][3];
}
extern "C" unsigned NativeAdrenoGpuContextInputBits();
// The framework's two weak board hooks. [1] of the first is the GPU's current level, which it notes per job. The second names a process still mapping a
// page when a BO goes back; only a diagnostic, and no BO's pages are handed back under a mapping now that views map the VM object.
extern "C" {volatile uint32_t native_gpu_dcvs[15];}
extern "C" unsigned int native_pmap_user_mappings(unsigned int, void **, uint64_t *, char *) { return 0; }
extern "C" void (*volatile native_adreno_power_get)(void);
extern "C" int coachz_gpu_pm_suspend(void);
extern "C" int coachz_gpu_pm_resume(void);
extern "C" int coachz_gpu_pm_force_off(int collapse);
extern "C" void coachz_gpu_pm_arm(void);
// NativeAdrenoPublish.cpp's per-process table (first eight process names that opened the plug-in type) and its latest mount failure:
// [0] 1 no free slot, 2 the core's open, 3 the window scope; [1] IOReturn; [2] slot; [3] failures so far.
extern "C" struct NativeAdrenoClientRecord {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal;} native_adreno_client_log[8];
extern "C" volatile uint32_t native_adreno_mount_fail[4];
extern "C" volatile uint64_t native_gop_gpu_offers, native_gop_gpu_completions, native_gop_gpu_failures, native_gop_gpu_abandoned;
extern kmod_info_t g_kernel_kmod_info;
extern "C" volatile uint32_t native_adreno_io_coherent;
extern "C" volatile uint32_t native_adreno_coherent_skips[4];
extern "C" volatile uint32_t coachz_gpu_ahead[5];   // NativeAdrenoPublish.cpp (patches/gpu/Adreno740.patch)
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern "C" void ml_io_unmap(vm_offset_t, vm_size_t);

// NativeAdrenoPublish's sink for the plug-in's cache export to the boot log disk, which this board does not have.
extern "C" int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) {return 0;}

namespace {
enum : uint32_t {
    CpSqeCntl = 0x0808, CpSqeInstrBase = 0x0830, RbbmSecvidTrustCntl = 0xf400, RbbmStatus = 0x0210, RbbmInt0Status = 0x0201, CpHwFault = 0x0821,
    CpProtectStatus = 0x0824, Pm4MeInit = 0x48, Pm4WaitRegMem = 0x3c, CpScratch7 = 0x088a, SmmuScr0 = 0, SmmuSgfsr = 0x48,
    // What the kit requires of the global configuration before it routes a stream: global fault interrupt and reports on, an unmatched stream faults.
    SmmuKitGlobal = (1u << 2) | (1u << 10), SmmuS2cr = 0xc00, SmmuS2crFault = 2u << 16, SmmuGroups = 6, SmmuGmuGroup = 1
};
// The original IOAcceleratorES as audited (UniXNU scripts/data/ioaccelerator-base-abi.json): the bindings in build-coachz-gpu.py use this address.
const uint64_t kOriginalAcceleratorMetaclass = 0xfffffff006aeb688ull;

volatile uint8_t *gCore, *gGmu, *gSmmu;
NativeAdrenoRing gRing;
NativeAdrenoSubmit gSubmit;
NativeAdrenoTimer gTimer;
NativeAdrenoGmuContextReport gReport;
NativeAdrenoGpuBuffer gBuffers[GpuBufferCount];
IOMemoryMap *gMaps[GpuBufferCount];
// Linux: chip 0x06010800, 512 KiB of GMEM at 1 MiB, UBWC 2.0 with highest bank bit 14 and swizzle 6 (a6xx_catalog.c, a6xx_calc_ubwc_config).
// The client's addresses come from its own address space above 4 GiB (NativeAdrenoPublish's window answers the query); these two are only the fallback.
const NativeAdrenoAbiDeviceInfo gDeviceInfo = {0x06010800u, 0x80000u, 0x100000ull, 0x1fffffffff000ull, 0x20000000ull, 0x60000000ull,
    14u, 6u, 0u, 2u, NativeAdrenoDeviceClientIova, 0u};

// Runtime power: with GX off an access to a core register stalls the bus and stops the machine, so every one passes this gate. The sleeping
// side sets gPmAsleep and then waits for the users to leave.
volatile uint32_t gPmAsleep, gPmUsers, gPmDropped;
inline bool pmEnter() {
    __atomic_fetch_add(&gPmUsers, 1, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&gPmAsleep, __ATOMIC_SEQ_CST)) return true;
    __atomic_fetch_sub(&gPmUsers, 1, __ATOMIC_SEQ_CST);
    gPmDropped = gPmDropped + 1;
    return false;
}
inline void pmExit() {__atomic_fetch_sub(&gPmUsers, 1, __ATOMIC_SEQ_CST);}

uint32_t coreRead(uint32_t reg) {
    if (!pmEnter()) return 0;
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(gCore + size_t(reg) * 4);
    __asm__ volatile("dsb sy" ::: "memory");
    pmExit();
    return value;
}
void coreWrite(uint32_t reg, uint32_t value) {
    if (!pmEnter()) return;
    __asm__ volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(gCore + size_t(reg) * 4) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    pmExit();
}
int ringWrite(void *, uint32_t reg, uint32_t value) {coreWrite(reg, value); return 0;}
int ringRead(void *, uint32_t reg, uint32_t *value) {*value = coreRead(reg); return 0;}
int ringBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
// The ring, memptrs and lists are Normal non-cacheable on both sides (GPU PTE attribute 0, CPU write-combine): only ordering.
int ringPublish(void *, const volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
int ringAcquire(void *, volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
uint64_t ringNow(void *) {
    // ARM rtclock's microtime helper truncates 19.2 ticks/us to 19. Its
    // fractional second can exceed one million us and run backwards at rollover.
    uint64_t ns=0;
    absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
    return ns/1000;
}
// The GPU's interrupts, as a6xx_irq, a6xx_gmu_irq and a6xx_hfi_irq take them (sc7180.dtsi: SPI 300, 305 and 304, level high). CACHE_FLUSH_TS ends
// every submission, so it wakes the fence waiters. The errors are counted for the report, and masked if they storm; a hang is recovered by the
// framework's hangcheck through gpuRestart below.
extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);
extern "C" int coachz_gpu_assert_wait_deadline(void *, int, uint64_t) __asm__("_assert_wait_deadline");
extern "C" int coachz_gpu_thread_wakeup(void *, int, int) __asm__("_thread_wakeup_prim");
extern "C" int coachz_gpu_thread_block(void *) __asm__("_thread_block");
enum : uint32_t {
    RbbmIntClearCmd = 0x0037, RbbmInt0Mask = 0x0038, CpInterruptStatus = 0x0823,
    IrqAhb = 1u << 1, IrqAtbAsync = 1u << 6, IrqCpHw = 1u << 9, IrqFlushTs = 1u << 20, IrqAtbBus = 1u << 22, IrqHang = 1u << 23, IrqUcheOob = 1u << 24,
    IrqErrors = IrqAhb | IrqAtbAsync | IrqCpHw | IrqAtbBus | IrqHang | IrqUcheOob,
    // A6XX_INT_MASK: the errors, the fence, the CP's IB1, IB2 and RB ends and the UCHE trap.
    IrqMask = IrqErrors | IrqFlushTs | 1u << 13 | 1u << 14 | 1u << 15 | 1u << 25,
    // GMU window byte offsets: (a6xx_gmu.xml dword offset * 4) - 0x6a000.
    GmuHfiClr = 0x14644, GmuHfiInfo = 0x14648, GmuHfiMask = 0x1464c, GmuAoClr = 0x24c10, GmuAoStatus = 0x24c14, GmuAoMask = 0x24c18,
    GmuHfiFault = 1u << 23, GmuAoErrors = 1u << 0 | 1u << 3 | 1u << 5,   // CM3 fault; watchdog bite, fence error, AHB bus error
};
volatile bool gIrqOn;
// The first errors as the interrupt finds them: status, CP interrupt status, CP_SQE_STAT word 1 (a6xx_cp_hw_err_irq's possible opcode), CP_HW_FAULT,
// CP_PROTECT_STATUS, RBBM_STATUS, RBBM_STATUS3, IB1 base and remainder, IB2 base, read pointer, bank 0 FSR.
volatile uint32_t gErrCount, gErrLog[4][13];
volatile uint32_t gFlushTs, gFlushSeen, gIrqAll, gIrqAhb, gIrqAtb, gIrqCpHw, gIrqHang, gIrqOob, gCpHwStatus, gGmuAoIrqs, gGmuAoStatus, gHfiIrqs, gHfiInfo;
volatile uint32_t gIdleBlocks, gIdleTimeouts;

void gpuIrq(void *) {
    const uint32_t status = *reinterpret_cast<volatile uint32_t *>(gCore + size_t(RbbmInt0Status) * 4);
    *reinterpret_cast<volatile uint32_t *>(gCore + size_t(RbbmIntClearCmd) * 4) = status;
    gIrqAll = gIrqAll + 1;
    if (status & IrqErrors) {
        if (status & IrqAhb) gIrqAhb = gIrqAhb + 1;
        if (status & (IrqAtbAsync | IrqAtbBus)) gIrqAtb = gIrqAtb + 1;
        if (status & IrqHang) gIrqHang = gIrqHang + 1;
        if (status & IrqUcheOob) gIrqOob = gIrqOob + 1;
        const auto core = [](uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(gCore + size_t(reg) * 4);};
        uint32_t cp = 0, opcode = 0;
        if (status & IrqCpHw) {
            gIrqCpHw = gIrqCpHw + 1;
            gCpHwStatus = cp = core(CpInterruptStatus);
            *reinterpret_cast<volatile uint32_t *>(gCore + size_t(0x0908) * 4) = 1;
            opcode = core(0x0909);
        }
        const uint32_t n = gErrCount;
        if (n < 4) {
            const uint32_t row[13] = {status, cp, opcode, core(CpHwFault), core(0x0824), core(RbbmStatus), core(0x0213), core(0x0928), core(0x0929), core(0x092a), core(0x092b), core(0x0806),
                *reinterpret_cast<volatile uint32_t *>(gSmmu + 0x8000 + 0x58)};
            for (unsigned i = 0; i < 13; ++i) gErrLog[n][i] = row[i];
            gErrCount = n + 1;
        }
        // priv->disable_err_irq's reason: an error that does not stop must not hold a core.
        if (gIrqAhb + gIrqAtb + gIrqHang + gIrqOob + gIrqCpHw > 10000)
            *reinterpret_cast<volatile uint32_t *>(gCore + size_t(RbbmInt0Mask) * 4) = IrqFlushTs;
    }
    if (status & IrqFlushTs) {
        gFlushTs = gFlushTs + 1;
        coachz_gpu_thread_wakeup(const_cast<uint32_t *>(&gFlushTs), 0, 0);   // every waiter, THREAD_AWAKENED
    }
}
void gmuAoIrq(void *) {
    const uint32_t status = *reinterpret_cast<volatile uint32_t *>(gGmu + GmuAoStatus);
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuAoClr) = status;
    gGmuAoIrqs = gGmuAoIrqs + 1;
    gGmuAoStatus = gGmuAoStatus | status;
}
void hfiIrq(void *) {
    const uint32_t info = *reinterpret_cast<volatile uint32_t *>(gGmu + GmuHfiInfo);
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuHfiClr) = info;
    gHfiIrqs = gHfiIrqs + 1;
    gHfiInfo = gHfiInfo | info;
}
// After hw_init, as a6xx_hw_init and a6xx_gmu_resume do it: anything pending cleared, handlers in place, then unmasked.
void gpuInterruptsOn() {
    uint32_t readback = 0;
    uint64_t router = 0;
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuAoClr) = ~0u;
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuHfiClr) = ~0u;
    const int ao = armcpu_spi_attach(32 + 305, false, gmuAoIrq, nullptr, &readback, &router);
    const int hfi = armcpu_spi_attach(32 + 304, false, hfiIrq, nullptr, &readback, &router);
    coreWrite(RbbmIntClearCmd, ~0u);
    const int rbbm = armcpu_spi_attach(32 + 300, false, gpuIrq, nullptr, &readback, &router);
    if (!ao) *reinterpret_cast<volatile uint32_t *>(gGmu + GmuAoMask) = ~uint32_t(GmuAoErrors);
    if (!hfi) *reinterpret_cast<volatile uint32_t *>(gGmu + GmuHfiMask) = ~uint32_t(GmuHfiFault);
    if (!rbbm) {
        coreWrite(RbbmInt0Mask, IrqMask);
        __asm__ volatile("dsb sy" ::: "memory");
        gIrqOn = true;
    }
    printf("COACHZ gpu irq: rbbm attach 0x%x mask 0x%x readback 0x%x, gmu 0x%x hfi 0x%x\n", rbbm, coreRead(RbbmInt0Mask), readback, ao, hfi);
}
// Between two fence polls, which take the publisher's lock (not held here). With the interrupt: sleep until a fence completes, at most 1 ms, as UniXNU porting/power's fence waits (a wake lost
// between the caller's poll and this wait); a completion since the last call means polling again at once. Before it: 100 us, as upstream's board pauses.
int ringIdle(void *) {
    if (!gIrqOn) {IODelay(100); return 0;}
    const uint32_t flushes = gFlushTs;
    if (flushes != gFlushSeen) {gFlushSeen = flushes; return 0;}
    gIdleBlocks = gIdleBlocks + 1;
    (void)coachz_gpu_assert_wait_deadline(const_cast<uint32_t *>(&gFlushTs), 0, mach_absolute_time() + 19200);   // 1 ms of the 19.2 MHz clock, THREAD_UNINT
    if (gFlushTs != flushes) coachz_gpu_thread_wakeup(const_cast<uint32_t *>(&gFlushTs), 0, 0);
    if (coachz_gpu_thread_block(nullptr) == 1) gIdleTimeouts = gIdleTimeouts + 1;   // THREAD_TIMED_OUT
    return 0;
}
int timerRead(void *, uint32_t reg, uint32_t *value) {*value = coreRead(reg); return 0;}

void cpState(const char *when) {
    printf("COACHZ gpu9: %s rbbm 0x%x int 0x%x hw fault 0x%x protect 0x%x bank0 fsr 0x%x gfsr 0x%x\n", when, coreRead(RbbmStatus), coreRead(RbbmInt0Status),
        coreRead(CpHwFault), coreRead(CpProtectStatus), *reinterpret_cast<volatile uint32_t *>(gSmmu + 0x8000 + 0x58),
        *reinterpret_cast<volatile uint32_t *>(gSmmu + SmmuSgfsr));
}

// The last framed submissions as the ring stamps them (NativeAdrenoRing's timeline, a6xx_submit's CP_ALWAYS_ON_COUNTER reads at 19.2 MHz): the CP's
// start to the GPU idle after its buffers, for the ones completed since the last line.
// The framework's views of DRAM and of the caches, adapted from UniXNU-sheng package/GpuPowerProbe.cpp: without them it cannot audit a write-combined BO
// (a client view that still caches its pages, the first IB read as zeros by the GPU, int-147) and falls back on nothing.
extern "C" {
extern int (*native_adreno_dram_read32)(uint64_t, uint32_t *);
extern int (*native_adreno_cached_read32)(uint64_t, uint32_t *);
extern int (*native_adreno_line_audit)(uint64_t, size_t, uint32_t *, uint32_t *);
extern volatile uint32_t native_adreno_wc_audit[6];
extern volatile uint32_t native_adreno_wc_distrusted;
}
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern "C" void ml_io_unmap(vm_offset_t addr, vm_size_t size);
bool physRead64(uint64_t pa, uint64_t *out) {
    const vm_offset_t va = ml_io_map_unmappable(vm_offset_t(pa & ~uint64_t(0xfff)), 0x1000, 0x7 /* VM_WIMG_IO */);
    if (!va) return false;
    *out = *reinterpret_cast<volatile uint64_t *>(va + (pa & 0xff8));
    ml_io_unmap(va, 0x1000);
    return true;
}
// NativeAdrenoVm::sampleWord's cached reader: one aligned word through the kernel's cacheable physical aperture, so a line a client's
// cacheable view left dirty is seen whatever the cache mode of the BO's own mappings.
extern "C" vm_offset_t phystokv(uint64_t pa);
extern "C" uint64_t gPhysBase, gPhysSize;
int cachedRead32(uint64_t pa, uint32_t *value) {
    if ((pa & 3) || pa < gPhysBase || pa - gPhysBase >= gPhysSize) return 1;
    const vm_offset_t va = phystokv(pa);
    if (!va) return 1;
    *value = *reinterpret_cast<volatile uint32_t *>(va);
    // Drop the line this read brought in: a stale clean copy would later differ from DRAM and look like a dirty one.
    __asm__ volatile("dsb sy\n\tdc civac, %0\n\tdsb sy" :: "r"(va) : "memory");
    return 0;
}
// NativeAdrenoVm::auditRange's line comparer (int-100): whole lines of one page as the cacheable aperture and DRAM (a fresh uncached
// mapping) read them. A differing line is cleaned and invalidated, then DRAM is read again: holding the cached copy now, the line was
// dirty; unchanged, it was a stale clean copy (now dropped). int-099's single-word check wrote its line back before reading DRAM.
// The time spent and the attribute XNU keeps for the first page that held a dirty line go to the banner (GPUW).
extern "C" unsigned int pmap_cache_attributes(unsigned int pn);
extern "C" {volatile uint64_t native_adreno_audit_ticks; volatile uint32_t native_adreno_audit_attr;}
int lineAudit(uint64_t pa, size_t bytes, uint32_t *dirty, uint32_t *stale) {
    if (!bytes || ((pa | bytes) & 63) || (pa & 0xfff) + bytes > 0x1000 || pa < gPhysBase || pa - gPhysBase >= gPhysSize) return 1;
    const uint64_t began = mach_absolute_time();
    const vm_offset_t page = ml_io_map_unmappable(vm_offset_t(pa & ~uint64_t(0xfff)), 0x1000, 0x7 /* VM_WIMG_IO */);
    const vm_offset_t cached = phystokv(pa);
    if (!page || !cached) {if (page) ml_io_unmap(page, 0x1000); return 1;}
    const vm_offset_t dram = page + vm_offset_t(pa & 0xfff);
    for (size_t at = 0; at < bytes; at += 64) {
        const volatile uint64_t *c = reinterpret_cast<const volatile uint64_t *>(cached + at);
        const volatile uint64_t *d = reinterpret_cast<const volatile uint64_t *>(dram + at);
        uint64_t copy[8];
        bool differs = false;
        for (unsigned i = 0; i < 8; ++i) {copy[i] = c[i]; if (copy[i] != d[i]) differs = true;}
        if (!differs) continue;
        __asm__ volatile("dsb sy\n\tdc civac, %0\n\tdsb sy" :: "r"(cached + at) : "memory");
        bool written = true;
        for (unsigned i = 0; i < 8; ++i) if (d[i] != copy[i]) written = false;
        if (written) {
            if (!*dirty && !native_adreno_audit_attr) native_adreno_audit_attr = 0x100 | (pmap_cache_attributes(unsigned(pa >> 12)) & 0xff);
            ++*dirty;
        } else {
            ++*stale;
        }
    }
    ml_io_unmap(page, 0x1000);
    native_adreno_audit_ticks += mach_absolute_time() - began;
    return 0;
}
// NativeAdrenoVm::sampleWord's DRAM reader (int-070): one aligned word through a fresh uncached mapping.
int dramRead32(uint64_t pa, uint32_t *value) {
    uint64_t w = 0;
    if (!physRead64(pa & ~uint64_t(7), &w)) return 1;
    *value = uint32_t(w >> ((pa & 4) * 8));
    return 0;
}

// A fence that has not moved between two reports: the CP's position and the words it was given, a few times a boot.
// A stopped CP as a6xx_recover and a6xx_gpu_state record it (a6xx_registers' CP ranges, the CP_SQE_STAT and CP_ROQ_DBG banks), with the bank's own view of the
// addresses it was using: its registers, an address translation (arm_smmu_iova_to_phys_hard) and the table walked in DRAM.
extern "C" {
extern volatile uint32_t native_adreno_table_switches[8];
extern volatile uint32_t native_adreno_recover_trace[4];
extern volatile uint32_t native_adreno_cp_aperture[4];
}
uint64_t bankTranslate(uint64_t va) {
    volatile uint8_t *bank = gSmmu + 0x8000;
    __asm__ volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile uint64_t *>(bank + 0x800) = va & ~uint64_t(0xfff);
    __asm__ volatile("dsb sy\n\tisb" ::: "memory");
    for (unsigned i = 0; i < 1000 && (*reinterpret_cast<volatile uint32_t *>(bank + 0x8f0) & 1); ++i) IODelay(1);
    return *reinterpret_cast<volatile uint64_t *>(bank + 0x50);
}
void tableWalk(const char *name, uint64_t va) {
    uint64_t table = *reinterpret_cast<volatile uint64_t *>(gSmmu + 0x8000 + 0x20) & 0xfffffffff000ull, entry[4] = {};
    for (unsigned level = 0; level < 4; ++level) {
        if (!physRead64(table + (((va >> (39 - 9 * level)) & 0x1ff) << 3), &entry[level]) || (entry[level] & 3) != 3) break;
        table = entry[level] & 0xfffffffff000ull;
    }
    const uint64_t par = bankTranslate(va);
    printf("COACHZ gpu dump: %s va 0x%llx par 0x%llx walk %llx %llx %llx %llx\n", name, static_cast<unsigned long long>(va), static_cast<unsigned long long>(par),
        static_cast<unsigned long long>(entry[0]), static_cast<unsigned long long>(entry[1]), static_cast<unsigned long long>(entry[2]), static_cast<unsigned long long>(entry[3]));
}
void wordsPrint(const char *name, const uint32_t *words, unsigned count) {
    unsigned lines = 0;
    for (unsigned i = 0; i < count && lines < 128; i += 16) {
        uint32_t any = 0;
        for (unsigned j = i; j < i + 16 && j < count; ++j) any |= words[j];
        if (!any) continue;
        printf("COACHZ gpu dump: %s +%u:", name, i);
        for (unsigned j = i; j < i + 16 && j < count; ++j) printf(" %08x", words[j]);
        printf("\n");
        ++lines;
        IOSleep(20);
    }
}
void cpDump(const char *why) {
    static uint32_t words[2048];
    const uint32_t done = gRing.completedFence(), issued = gRing.lastSubmitted();
    printf("COACHZ gpu dump: %s, fence %u of %u, %u errors\n", why, done, issued, gErrCount);
    for (unsigned i = 0; i < gErrCount && i < 4; ++i) {
        const volatile uint32_t *e = gErrLog[i];
        printf("COACHZ gpu dump: error %u status 0x%x cp 0x%x opcode 0x%x hw fault 0x%x protect 0x%x rbbm 0x%x rbbm3 0x%x ib1 0x%x%08x rem 0x%x ib2 0x%x rptr 0x%x fsr 0x%x\n", i, e[0], e[1], e[2], e[3], e[4], e[5], e[6],
            e[8], e[7], e[9], e[10], e[11], e[12]);
    }
    static const uint16_t ranges[][2] = {{0x0210, 0x0213}, {0x0800, 0x0808}, {0x0810, 0x0813}, {0x0820, 0x0821}, {0x0823, 0x0824}, {0x0826, 0x0827}, {0x0830, 0x0833}, {0x0840, 0x0845}, {0x0880, 0x088a},
        {0x08a0, 0x08ab}, {0x08c0, 0x08c4}, {0x08d0, 0x08dd}, {0x08f0, 0x08f3}, {0x0900, 0x0903}, {0x0928, 0x093e}, {0x0942, 0x094d}};
    for (const auto &range : ranges) {
        printf("COACHZ gpu dump: regs 0x%04x:", range[0]);
        for (uint32_t reg = range[0]; reg <= range[1]; ++reg) printf(" %08x", coreRead(reg));
        printf("\n");
    }
    IOSleep(20);
    coreWrite(0x0908, 0);
    for (unsigned i = 0; i < 0x33; ++i) words[i] = coreRead(0x0909);
    wordsPrint("sqe", words, 0x33);
    unsigned roq = coreRead(0x08c2) >> 14;   // a6xx_get_cp_roq_size
    if (roq > 2048) roq = 2048;
    coreWrite(0x090c, 0);
    for (unsigned i = 0; i < roq; ++i) words[i] = coreRead(0x090d);
    printf("COACHZ gpu dump: roq %u words\n", roq);
    wordsPrint("roq", words, roq);
    volatile uint8_t *bank = gSmmu + 0x8000;
    const auto bank32 = [&](size_t at) {return *reinterpret_cast<volatile uint32_t *>(bank + at);};
    const auto bank64 = [&](size_t at) {return static_cast<unsigned long long>(*reinterpret_cast<volatile uint64_t *>(bank + at));};
    printf("COACHZ gpu dump: bank sctlr 0x%x tcr2 0x%x ttbr0 0x%llx ttbr1 0x%llx tcr 0x%x mair0 0x%x fsr 0x%x far 0x%llx fsynr0 0x%x gfsr 0x%x\n", bank32(0), bank32(0x10), bank64(0x20), bank64(0x28), bank32(0x30),
        bank32(0x38), bank32(0x58), bank64(0x60), bank32(0x68), *reinterpret_cast<volatile uint32_t *>(gSmmu + SmmuSgfsr));
    printf("COACHZ gpu dump: table switches %u %u %u %u %u %u 0x%x %u, table mode %u, recover %u 0x%x %u %u\n", native_adreno_table_switches[0], native_adreno_table_switches[1], native_adreno_table_switches[2],
        native_adreno_table_switches[3], native_adreno_table_switches[4], native_adreno_table_switches[5], native_adreno_table_switches[6], native_adreno_table_switches[7], native_adreno_cp_aperture[3],
        native_adreno_recover_trace[0], native_adreno_recover_trace[1], native_adreno_recover_trace[2], native_adreno_recover_trace[3]);
    uint64_t start = 0, end = 0;
    const uint32_t seen = gRing.timeline(issued, &start, &end);
    printf("COACHZ gpu dump: stamps of fence %u status %u start 0x%llx end 0x%llx\n", issued, seen, static_cast<unsigned long long>(start), static_cast<unsigned long long>(end));
    const uint32_t logged = native_adreno_ib_log_count < 8 ? native_adreno_ib_log_count : 8;
    uint32_t slot = logged ? logged - 1 : 0;
    for (uint32_t i = 0; i < logged; ++i) if (native_adreno_ib_log[i][0] == done + 1) slot = i;
    const volatile uint32_t *r = native_adreno_ring_log[(native_adreno_ring_log_count - 1) & 3], *b = native_adreno_ib_log[slot];
    tableWalk("ib", uint64_t(b[2]) << 32 | b[1]);
    tableWalk("ring", GpuRingIova);
    tableWalk("memptrs", GpuMemptrsIova);
    tableWalk("sqe", GpuSqeIova);
    printf("COACHZ gpu dump: ring fence %u start %u end %u words %u:", r[0], r[1], r[2], r[3]);
    for (unsigned i = 0; i < 40 && i < r[3]; ++i) printf(" %08x", r[4 + i]);
    printf("\nCOACHZ gpu dump: ib fence %u at 0x%x%08x bytes %u\n", b[0], b[2], b[1], b[3]);
    IOSleep(20);
    for (unsigned i = 0; i < 208 && i < b[3] / 4; ++i) words[i] = b[4 + i];
    wordsPrint("ib", words, b[3] / 4 < 208 ? b[3] / 4 : 208);
}
// msm_gpu.c::recover_worker captures the offending submit before reset and BO
// release. Save bounded data here; reportThread prints it after the publisher
// lock is released, so diagnostic output cannot delay recovery fence progress.
struct HangSnapshot {
    uint32_t state, sequence, fence, owner, asleep, errors, error[4][13];
    uint64_t table, atUs, elapsedUs, far;
    uint32_t observed, issued, context, recover[4], pmDropped[2];
    uint32_t bank[8], gfsr, regs[160], regCount, sqe[0x33], roq[2048], roqCount, roqTotal;
    uint32_t packets[NativeAdrenoSubmitMaxBuffers * 4], packetTotal, packetCount;
    uint32_t ring[8192], ringTotal, ringCount, memptrs[1024], memptrTotal, memptrCount;
    struct Ib {
        uint64_t address;
        uint32_t kind, total, requested, offset, flags;
        CoachzGpuHangRead read;
    } ib[130];
    uint32_t ibCount, packetError, packetOffset, captured, words[16384];
};
HangSnapshot gHangSnapshot;
const uint16_t kHangRegisters[] = {
    RbbmInt0Status, 0x0210, 0x0213, 0x0800, 0x0801, 0x0802, 0x0803, 0x0804, 0x0806, 0x0807, 0x0808,
    0x0820, 0x0821, 0x0823, 0x0824, 0x0826, 0x0827, 0x0830, 0x0831, 0x0832, 0x0833,
    0x0840, 0x0841, 0x0842, 0x0843, 0x0844, 0x0845,
    0x0880, 0x0881, 0x0882, 0x0883, 0x0884, 0x0885, 0x0886, 0x0887, 0x0888, 0x0889, 0x088a,
    0x08a0, 0x08a1, 0x08a2, 0x08a3, 0x08a4, 0x08a5, 0x08a6, 0x08a7, 0x08a8, 0x08a9, 0x08aa, 0x08ab,
    0x08c0, 0x08c1, 0x08c2, 0x08c3, 0x08c4,
    0x08d0, 0x08d1, 0x08d2, 0x08d3, 0x08d4, 0x08d5, 0x08d6, 0x08d7, 0x08d8, 0x08d9, 0x08da, 0x08db, 0x08dc, 0x08dd,
    0x0928, 0x0929, 0x092a, 0x092b, 0x092c, 0x092d, 0x093e, 0x0942, 0x0943, 0x0949, 0x094a
};
static_assert(sizeof(kHangRegisters) / sizeof(kHangRegisters[0]) <= 160, "hang register capacity");
void gpuHangSnapshot(NativeAdrenoVm *vm, const uint32_t *packets, size_t count, int32_t owner, uint32_t fence) {
    static unsigned captures;
    uint32_t empty = 0;
    if (captures >= 2 || !__atomic_compare_exchange_n(&gHangSnapshot.state, &empty, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    HangSnapshot &out = gHangSnapshot;
    out.atUs = ringNow(nullptr);
    out.sequence = ++captures; out.fence = fence; out.owner = uint32_t(owner);
    out.table = vm ? vm->ttbr0() : 0;
    out.observed = gRing.completedFence(); out.issued = gRing.lastSubmitted(); out.context = gRing.context();
    for (unsigned i = 0; i < 4; ++i) out.recover[i] = native_adreno_recover_trace[i];
    out.pmDropped[0] = gPmDropped;
    out.asleep = __atomic_load_n(&gPmAsleep, __ATOMIC_ACQUIRE);
    out.errors = gErrCount < 4 ? gErrCount : 4;
    for (unsigned i = 0; i < out.errors; ++i) for (unsigned j = 0; j < 13; ++j) out.error[i][j] = gErrLog[i][j];
    out.regCount = out.roqCount = out.ibCount = out.captured = out.packetError = 0;
    out.packetOffset = ~0u;
    out.packetTotal = uint32_t(count);
    out.packetCount = packets ? uint32_t(count < NativeAdrenoSubmitMaxBuffers * 4 ? count : NativeAdrenoSubmitMaxBuffers * 4) : 0;
    for (unsigned i = 0; i < out.packetCount; ++i) out.packets[i] = packets[i];
    // These permanent, CPU-visible ring/memptrs mappings survive a missing job
    // record. Copy them before restart overwrites the ring and fence page.
    const auto copyBuffer = [&](unsigned index, uint32_t *words, unsigned limit, uint32_t &total, uint32_t &saved) {
        total = uint32_t(gBuffers[index].bytes / 4); saved = 0;
        if (!gMaps[index]) return;
        const auto source = reinterpret_cast<const volatile uint32_t *>(gMaps[index]->getVirtualAddress());
        if (!source) return;
        saved = total < limit ? total : limit;
        for (unsigned i = 0; i < saved; ++i) words[i] = source[i];
    };
    copyBuffer(1, out.ring, 8192, out.ringTotal, out.ringCount);
    copyBuffer(2, out.memptrs, 1024, out.memptrTotal, out.memptrCount);
    out.roqTotal = 0;
    if (!out.asleep) {
        for (uint16_t reg : kHangRegisters) out.regs[out.regCount++] = coreRead(reg);
        volatile uint8_t *bank = gSmmu + 0x8000;
        const uint16_t offsets[] = {0, 0x10, 0x20, 0x24, 0x30, 0x38, 0x58, 0x68};
        for (unsigned i = 0; i < 8; ++i) out.bank[i] = *reinterpret_cast<volatile uint32_t *>(bank + offsets[i]);
        out.far = *reinterpret_cast<volatile uint64_t *>(bank + 0x60);
        out.gfsr = *reinterpret_cast<volatile uint32_t *>(gSmmu + SmmuSgfsr);
        // Same CP debug banks as the existing a6xx_gpu_state dump; no SP selector reads.
        coreWrite(0x0908, 0);
        for (unsigned i = 0; i < 0x33; ++i) out.sqe[i] = coreRead(0x0909);
        out.roqTotal = out.roqCount = coreRead(0x08c2) >> 14;
        if (out.roqCount > 2048) out.roqCount = 2048;
        coreWrite(0x090c, 0);
        for (unsigned i = 0; i < out.roqCount; ++i) out.roq[i] = coreRead(0x090d);
    }
    const auto captureIb = [&](uint32_t kind, uint64_t address, uint32_t total, uint32_t limit) {
        if (out.ibCount >= 130) return;
        HangSnapshot::Ib &ib = out.ib[out.ibCount++];
        ib.kind = kind; ib.address = address; ib.total = total; ib.offset = out.captured;
        ib.flags = total > limit ? 1 : 0; // per-window limit
        ib.requested = total < limit ? total : limit;
        if (ib.requested > 16384 - out.captured) {ib.requested = 16384 - out.captured; ib.flags |= 2;}
        ib.read = coachzGpuHangRead(vm, address, out.words + ib.offset, ib.requested);
        if (!ib.requested && total) ib.read.status = kIOReturnNoResources;
        if (!ib.read.status && ib.read.count < ib.requested) ib.flags |= 4; // mapping end
        out.captured += uint32_t(ib.read.count);
    };
    // Preserve the CP's nested IB windows first, then the actual submitted IBs.
    // This also covers a child IB that was not itself an execbuf entry.
    if (!out.asleep) {
        const auto savedReg = [&](uint16_t wanted) {
            for (unsigned i = 0; i < out.regCount; ++i) if (kHangRegisters[i] == wanted) return out.regs[i];
            return uint32_t(0);
        };
        captureIb(2, (uint64_t(savedReg(0x0929)) << 32) | savedReg(0x0928), savedReg(0x092a), 2048);
        captureIb(3, (uint64_t(savedReg(0x092c)) << 32) | savedReg(0x092b), savedReg(0x092d), 2048);
    }
    if (packets) for (size_t at = 0; at + 3 < out.packetCount && out.ibCount < 130; at += 4) {
        if (((packets[at] >> 16) & 0x7f) != 0x3f || (packets[at] & 0x3fff) != 3) {
            out.packetError = packets[at]; out.packetOffset = uint32_t(at); break;
        }
        captureIb(1, (uint64_t(packets[at + 2]) << 32) | packets[at + 1], packets[at + 3] & 0xfffff, 16384);
    }
    out.pmDropped[1] = gPmDropped;
    out.elapsedUs = ringNow(nullptr) - out.atUs;
    __atomic_store_n(&out.state, 2u, __ATOMIC_RELEASE);
}
extern "C" void (*native_adreno_capture_hang)(NativeAdrenoVm *, const uint32_t *, size_t, int32_t, uint32_t);
// Preserve zeros and every captured word; the older console dumper has a
// 128-line limit. Pace output outside the publisher lock so the persistent
// console receiver can flush its buffer while this larger record is printed.
void hangWordsPrint(const char *name, const uint32_t *words, unsigned count) {
    for (unsigned at = 0; at < count; at += 16) {
        printf("COACHZ gpu hang %s +%u:", name, at);
        for (unsigned i = at; i < at + 16 && i < count; ++i) printf(" %08x", words[i]);
        printf("\n");
        IOSleep(20);
    }
}
void hangSnapshotReport() {
    uint32_t ready = 2;
    if (!__atomic_compare_exchange_n(&gHangSnapshot.state, &ready, 3, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    const HangSnapshot &s = gHangSnapshot;
    printf("COACHZ gpu hang snapshot: n %u fence %u owner %d job %u asleep %u table 0x%llx at %llu us capture %llu us\n",
        s.sequence, s.fence, int32_t(s.owner), s.packetCount != 0, s.asleep, s.table, s.atUs, s.elapsedUs);
    printf("COACHZ gpu hang timeline: observed %u issued %u context %u recover %u %x %u %u gate-dropped %u/%u\n",
        s.observed, s.issued, s.context, s.recover[0], s.recover[1], s.recover[2], s.recover[3], s.pmDropped[0], s.pmDropped[1]);
    printf("COACHZ gpu hang limits: snapshots 2 IB-words 16384 CP-window-words 2048; packets %u/%u ring %u/%u memptrs %u/%u\n",
        s.packetCount, s.packetTotal, s.ringCount, s.ringTotal, s.memptrCount, s.memptrTotal);
    for (unsigned i = 0; i < s.errors; ++i) {
        printf("COACHZ gpu hang irq %u:", i);
        for (uint32_t value : s.error[i]) printf(" %08x", value);
        printf("\n");
    }
    for (unsigned i = 0; i < s.regCount; ++i) printf("COACHZ gpu hang reg %04x %08x\n", kHangRegisters[i], s.regs[i]);
    if (!s.asleep) {
        printf("COACHZ gpu hang bank:");
        for (uint32_t value : s.bank) printf(" %08x", value);
        printf(" far %llx gfsr %x\n", s.far, s.gfsr);
        hangWordsPrint("sqe", s.sqe, 0x33);
        printf("COACHZ gpu hang ROQ: saved %u/%u words\n", s.roqCount, s.roqTotal);
        hangWordsPrint("roq", s.roq, s.roqCount);
    }
    if (s.packetOffset != ~0u) printf("COACHZ gpu hang packet: offset %u header %08x unrecognized\n", s.packetOffset, s.packetError);
    for (unsigned i = 0; i < s.ibCount; ++i) {
        const HangSnapshot::Ib &ib = s.ib[i];
        printf("COACHZ gpu hang IB: n %u kind %u address %llx words %u requested %u captured %lu flags %x status 0x%x handle %llx base %llx bytes %lu perms %x\n",
            i + 1, ib.kind, ib.address, ib.total, ib.requested, (unsigned long)ib.read.count, ib.flags,
            uint32_t(ib.read.status), ib.read.handle, ib.read.base, (unsigned long)ib.read.bytes, ib.read.permissions);
        if (ib.read.status) continue;
        for (unsigned at = 0; at < ib.read.count; at += 16) {
            printf("COACHZ gpu hang IB%u +%u:", i + 1, at);
            for (unsigned j = at; j < at + 16 && j < ib.read.count; ++j) printf(" %08x", s.words[ib.offset + j]);
            printf("\n");
            IOSleep(20);
        }
    }
    hangWordsPrint("packets", s.packets, s.packetCount);
    hangWordsPrint("ring", s.ring, s.ringCount);
    hangWordsPrint("memptrs", s.memptrs, s.memptrCount);
    printf("COACHZ gpu hang snapshot: end n %u IBs %u captured %u words\n", s.sequence, s.ibCount, s.captured);
    __atomic_store_n(&gHangSnapshot.state, 0u, __ATOMIC_RELEASE);
}

void stuckReport() {
    static uint32_t lastFence, printed;
    const uint32_t done = gRing.completedFence(), issued = gRing.lastSubmitted();
    printf("COACHZ gpu wc: audits %u lines %u dirty %u stale %u status 0x%x re-asserted %u, distrusted at %u, attribute 0x%x, audit %llu ms\n", native_adreno_wc_audit[0], native_adreno_wc_audit[1],
        native_adreno_wc_audit[2], native_adreno_wc_audit[3], native_adreno_wc_audit[4], native_adreno_wc_audit[5], native_adreno_wc_distrusted, native_adreno_audit_attr,
        (unsigned long long)(native_adreno_audit_ticks / 19200));
    if (done == issued || done != lastFence) {lastFence = done; return;}
    if (printed >= 3 || __atomic_load_n(&gPmAsleep, __ATOMIC_SEQ_CST)) return;
    ++printed;
    printf("COACHZ gpu stuck: fence %u of %u, rptr 0x%x wptr 0x%x, ib1 0x%x%08x rem 0x%x, ib2 0x%x%08x rem 0x%x, rbbm 0x%x, sqe stat 0x%x, ring ttbr0 0x%x%08x\n", done, issued, coreRead(0x0806), coreRead(0x0807),
        coreRead(0x0929), coreRead(0x0928), coreRead(0x092a), coreRead(0x092c), coreRead(0x092b), coreRead(0x092d), coreRead(0x0210), coreRead(0x0831),
        *reinterpret_cast<volatile uint32_t *>(gSmmu + 0x8000 + 0x24), *reinterpret_cast<volatile uint32_t *>(gSmmu + 0x8000 + 0x20));
    if (printed == 1) cpDump("stuck");
}

uint64_t coreCounter(uint32_t reg);
void stampReport() {
    static uint32_t reported;
    const uint32_t done = gRing.completedFence();
    uint64_t gpu = 0, gpuMax = 0, queue = 0, queueMax = 0;
    unsigned n = 0, bins[7] = {};
    // The stamps count the always-on counter, the kernel its own clock, both at 19.2 MHz: the offset is read here. A power cycle restarts the counter; such a stamp shows as no queue.
    const int64_t offset = static_cast<int64_t>(mach_absolute_time()) - static_cast<int64_t>(coreCounter(0x980));
    for (uint32_t fence = done - reported > 64 ? done - 63 : reported + 1; fence && fence <= done; ++fence) {
        uint64_t start = 0, end = 0;
        if (gRing.timeline(fence, &start, &end) != 31 || !start || end < start) continue;
        const uint64_t g = (end - start) * 10 / 192;
        gpu += g; ++n;
        if (g > gpuMax) gpuMax = g;
        const int64_t began = static_cast<int64_t>(start) + offset - static_cast<int64_t>(native_adreno_submit_ticks[fence & 63]);
        const uint64_t q = began > 0 && began < 19200000 ? static_cast<uint64_t>(began) * 10 / 192 : 0;
        queue += q;
        if (q > queueMax) queueMax = q;
        unsigned bin = 0;
        while (bin < 6 && g >= (1000ull << bin)) ++bin;
        ++bins[bin];
    }
    reported = done;
    if (!n) return;
    printf("COACHZ gpu path: kernel submit mean %llu max %llu us over %llu; fence waits %llu mean %llu max %llu us; gop waits %llu timeouts %llu timeout mean %llu max %llu us req %llu ms\n",
        native_adreno_submit_cost[0] ? native_adreno_submit_cost[1] * 10 / 192 / native_adreno_submit_cost[0] : 0, native_adreno_submit_cost[2] * 10 / 192, native_adreno_submit_cost[0],
        native_adreno_wait_outer[0], native_adreno_wait_outer[0] ? native_adreno_wait_outer[1] / native_adreno_wait_outer[0] : 0, native_adreno_wait_outer[3],
        native_gop_wait_stats[0], native_gop_wait_stats[1], native_gop_wait_stats[1] ? native_gop_wait_stats[6] / native_gop_wait_stats[1] / 1000 : 0, native_gop_wait_stats[5] / 1000, native_gop_wait_stats[2]);
    printf("COACHZ gpu stamps: %u submissions, queue mean %llu max %llu us, gpu mean %llu max %llu us, gpu <1 %u <2 %u <4 %u <8 %u <16 %u <32 %u more %u ms\n", n, queue / n, queueMax, gpu / n, gpuMax,
        bins[0], bins[1], bins[2], bins[3], bins[4], bins[5], bins[6]);
    // The submission path by phase (NativeAdrenoSubmit::submit), mean per submission over the boot.
    if (const uint64_t s = native_adreno_submit_cost[0]) {
        const volatile uint64_t *p = native_adreno_submit_phase;
        printf("COACHZ gpu submit phases: check %llu retire %llu hold %llu wc %llu publish %llu choose %llu ring %llu us over %llu\n", p[0] * 10 / 192 / s, p[1] * 10 / 192 / s,
            p[2] * 10 / 192 / s, p[3] * 10 / 192 / s, p[4] * 10 / 192 / s, p[5] * 10 / 192 / s, p[6] * 10 / 192 / s, s);
    }
    // The publisher lock (NativeAdrenoPublish.cpp): BO view lookups that found it held, their wait, the longest and who held it then
    // (method + 1, pid), and the longest hold of each method that held it (k220: a command stream chunk's mapping sat 20 to 90 ms).
    {
        const volatile uint64_t *v = native_adreno_view_lock;
        char line[320];
        size_t at = static_cast<size_t>(snprintf(line, sizeof(line), "COACHZ gpu lock: view waits %llu mean %llu max %llu us holder m%llu pid %llu; hold max",
            v[0], v[0] ? v[1] / v[0] : 0, v[2], (v[3] & 0xffff) ? (v[3] & 0xffff) - 1 : 0, v[3] >> 16));
        for (unsigned m = 0; m < 32 && at < sizeof(line); ++m)
            if (const uint32_t held = native_adreno_lock_hold_max[m])
                at += static_cast<size_t>(snprintf(line + at, sizeof(line) - at, " m%u %u", m, held));
        printf("%s us\n", line);
        // Each ABI method's calls, failures and mean time inside the core (NativeAdrenoUserClient::dispatch's tally, under the lock).
        at = static_cast<size_t>(snprintf(line, sizeof(line), "COACHZ gpu methods:"));
        for (unsigned m = 0; m < 20 && at < sizeof(line); ++m)
            if (const uint32_t calls = native_adreno_method_stats[m][0])
                at += static_cast<size_t>(snprintf(line + at, sizeof(line) - at, " m%u %u/%u %uus", m, calls, native_adreno_method_stats[m][1],
                    native_adreno_method_stats[m][3] / calls));
        printf("%s\n", line);
        // Cache maintenance by phase (NativeAdrenoNotePhase: allocate, map, submit, unmap, complete, table, diagnostic), calls and KiB, and the
        // submission phase's by kind of memory (imports, write-combined BOs maintained because write combine is distrusted, system memory,
        // BOs of a kernel mapping). k221: the walk before each submission still took 854 us.
        // Formatted here and printed as one string: the kernel's printf encodes its arguments for os_log, and 25 of them overran the
        // encoder (k222: log_encode.c "log_fits_cmd" panic right after this row).
        char wide[512];
        const volatile uint64_t (*m)[2] = native_adreno_maint_phase;
        const volatile uint64_t (*k)[2] = native_adreno_publish_kinds;
        snprintf(wide, sizeof(wide), "COACHZ gpu maintenance: alloc %llu/%llu map %llu/%llu submit %llu/%llu unmap %llu/%llu complete %llu/%llu "
                 "table %llu/%llu KiB; submit import %llu/%llu wc %llu/%llu system %llu/%llu mapped %llu/%llu KiB",
            m[1][0], m[1][1], m[2][0], m[2][1], m[3][0], m[3][1], m[4][0], m[4][1], m[5][0], m[5][1], m[6][0], m[6][1],
            k[0][0], k[0][1], k[1][0], k[1][1], k[2][0], k[2][1], k[3][0], k[3][1]);
        printf("%s\n", wide);
        // The VM's table transactions (NativeAdrenoVm::map and unmap), us in total per phase: map preflight, observe, publish, tables, insert;
        // unmap observe, acquire, tables, erase, retire; the slowest call and the MiB mapped; then by kind {calls, us} (own BOs, imports without
        // and with the alias pass, unmaps by page TLBIs and by ASID). The method tally's clock times them (NativeAdrenoUserClient), and nothing
        // printed them before. k221: BoMap 517 us and BoFree 421 us on average.
        // Two rows, each formatted here and printed as one string (see the maintenance row).
        const volatile uint64_t (*vp)[8] = native_adreno_vm_phase_us;
        const volatile uint64_t (*kv)[3] = native_adreno_vm_kind_us;
        snprintf(wide, sizeof(wide), "COACHZ gpu vm map: %llu pre %llu obs %llu pub %llu tab %llu ins %llu max %llu us %llu MiB; own %llu/%llu "
                 "import %llu/%llu alias %llu/%llu us",
            vp[0][0], vp[0][5], vp[0][1], vp[0][2], vp[0][3], vp[0][4], vp[0][6], vp[0][7] >> 20, kv[0][0], kv[0][1], kv[1][0], kv[1][1],
            kv[2][0], kv[2][1]);
        printf("%s\n", wide);
        snprintf(wide, sizeof(wide), "COACHZ gpu vm unmap: %llu obs %llu acq %llu tab %llu erase %llu ret %llu max %llu us; pages %llu/%llu "
                 "asid %llu/%llu us",
            vp[1][0], vp[1][1], vp[1][2], vp[1][3], vp[1][4], vp[1][5], vp[1][6], kv[3][0], kv[3][1], kv[4][0], kv[4][1]);
        printf("%s\n", wide);
    }
}

// sheng-diag=1: one render server submission of 2 to 36 IBs at a time carries the CP's and the RB's always-on counter before each IB and after the last
// (NativeAdrenoSubmit, fence page from byte 2048). Printed as the first stamps and the ticks (19.2 MHz) between boundaries, to set beside the command
// buffers' own stamps in the work trace (same counter, same fence). k217b: 0.1-1.3 ms between command buffers that no command buffer's stamps cover.
void ibProbeReport() {
    static int diagnostic = -1;
    if (diagnostic < 0) {unsigned on = 0; diagnostic = PE_parse_boot_argn("sheng-diag", &on, sizeof(on)) && on ? 1 : 0;}
    if (!diagnostic) return;
    if (const uint32_t fence = native_adreno_ib_probe_fence) {
        if (static_cast<int32_t>(gRing.completedFence() - fence) < 0) return;   // still on the GPU
        const uint32_t n = native_adreno_ib_probe_count;
        uint32_t words[4 * 37] = {};
        if (n >= 2 && n <= 36 && gRing.readFenceWords(2048, words, 4 * (n + 1)) == kIOReturnSuccess) {
            auto cp = [&](uint32_t i) {return static_cast<uint64_t>(words[4 * i + 1]) << 32 | words[4 * i];};
            auto rb = [&](uint32_t i) {return static_cast<uint64_t>(words[4 * i + 3]) << 32 | words[4 * i + 2];};
            char line[420]; size_t at = 0;
            at += static_cast<size_t>(snprintf(line, sizeof(line), "COACHZ gpu ibs: fence %u pid %u n %u cp0 0x%llx rb0 0x%llx dwords", fence, native_adreno_ib_probe_pid, n,
                static_cast<unsigned long long>(cp(0)), static_cast<unsigned long long>(rb(0))));
            for (uint32_t i = 0; i < n && at < sizeof(line); ++i) at += static_cast<size_t>(snprintf(line + at, sizeof(line) - at, " %u", native_adreno_ib_probe_dwords[i]));
            printf("%s\n", line);
            for (unsigned which = 0; which < 2; ++which) {
                at = static_cast<size_t>(snprintf(line, sizeof(line), "COACHZ gpu ibs %s:", which ? "rb" : "cp"));
                for (uint32_t i = 1; i <= n && at < sizeof(line); ++i) {
                    const uint64_t a = which ? rb(i - 1) : cp(i - 1), b = which ? rb(i) : cp(i);
                    at += static_cast<size_t>(snprintf(line + at, sizeof(line) - at, " %lld", static_cast<long long>(b - a)));
                }
                printf("%s\n", line);
            }
            // The first IB of at most 16 dwords, word by word (k219: a 9-dword IB after each command buffer held the CP about 64 us).
            if (const uint32_t smallWords = native_adreno_ib_probe_small_count) {
                at = static_cast<size_t>(snprintf(line, sizeof(line), "COACHZ gpu ibs small: ib %u dwords %u:", native_adreno_ib_probe_small_at, smallWords));
                for (uint32_t i = 0; i < smallWords && i < 16 && at < sizeof(line); ++i)
                    at += static_cast<size_t>(snprintf(line + at, sizeof(line) - at, " %08x", native_adreno_ib_probe_small[i]));
                printf("%s\n", line);
            }
        }
        native_adreno_ib_probe_fence = 0;
    }
    native_adreno_ib_probe_request = 1;
}

// The firmware framebuffer as the panel scans it, 4096 pixels scattered over the frame: how many values each channel holds, which of its bits are ever set and green's most common
// values. Bit depth lost anywhere before the panel shows here (a channel holding only multiples of 8, say); a smooth white scene with neighbouring values does not lose any.
extern "C" volatile uint64_t native_gop_virtual;
void frameSample() {
    const volatile uint32_t *frame = reinterpret_cast<const volatile uint32_t *>(native_gop_virtual);
    if (!frame) return;
    uint32_t counts[3][256] = {}, bits = 0;
    for (uint32_t i = 0; i < 4096; ++i) {
        const uint32_t pixel = frame[(uint64_t(i) * 2654435761u) % (2160u * 1440u)];
        bits |= pixel;
        for (unsigned c = 0; c < 3; ++c) ++counts[c][(pixel >> (c * 8)) & 255];
    }
    unsigned distinct[3] = {};
    for (unsigned c = 0; c < 3; ++c) for (unsigned v = 0; v < 256; ++v) if (counts[c][v]) ++distinct[c];
    printf("COACHZ frame sample: distinct b %u g %u r %u, bits set 0x%06x, green top", distinct[0], distinct[1], distinct[2], bits & 0xffffffu);
    for (unsigned n = 0; n < 12; ++n) {
        unsigned best = 0;
        for (unsigned v = 1; v < 256; ++v) if (counts[1][v] > counts[1][best]) best = v;
        if (!counts[1][best]) break;
        printf(" %u:%u", best, counts[1][best]);
        counts[1][best] = 0;
    }
    printf("\n");
}

// Every ten seconds, what reached the driver: whether the plug-in opened it, what it reports, what the CP has run and what reached the screen.
void reportThread(void *, wait_result_t) {
    for (unsigned round = 1;; round++) {
        IOSleep(10000);
        native_adreno_metal_report();
        hangSnapshotReport();
        stuckReport();
        // Once a minute, which processes have a GPU connection slot (four in this framework) and which were refused.
        if (!(round % 6)) {
            for (const NativeAdrenoClientRecord &r : native_adreno_client_log)
                if (r.name[0])
                    printf("COACHZ gpu clients: %.19s opens %u refused %u closes %u slot %d peak %u MiB last refusal 0x%x\n", r.name, r.opens,
                        r.refused, r.closes, r.slot == 0xff ? -1 : int(r.slot), r.peak_mib, r.last_refusal);
            printf("COACHZ gpu clients: mount failures %u, latest reason %u status 0x%x slot %u\n", native_adreno_mount_fail[3],
                native_adreno_mount_fail[0], native_adreno_mount_fail[1], native_adreno_mount_fail[2]);
        }
        printf("COACHZ gpu9: ring submissions %u fence %u/%u, frames offered %llu shown %llu failed %llu abandoned %llu\n", gRing.submissions(), gRing.completedFence(),
            gRing.lastSubmitted(), static_cast<unsigned long long>(native_gop_gpu_offers), static_cast<unsigned long long>(native_gop_gpu_completions),
            static_cast<unsigned long long>(native_gop_gpu_failures), static_cast<unsigned long long>(native_gop_gpu_abandoned));
        cpState("now");
        printf("COACHZ gpu recover: hangs %u restarts %u failed %u, last hung fence %u status 0x%x, replayed %u, guilty %u kernel %u; trace %u 0x%x %u %u\n",
            native_adreno_recovery[0], native_adreno_recovery[1], native_adreno_recovery[2], native_adreno_recovery[3], native_adreno_recovery[4],
            native_adreno_recovery[5], native_adreno_recovery[6], native_adreno_recovery[7], native_adreno_recover_trace[0], native_adreno_recover_trace[1],
            native_adreno_recover_trace[2], native_adreno_recover_trace[3]);
        // The framework's stall handler (NativeAdrenoSubmit::noteStall): [0] write pointer given again, [4] bus drains, [9]/[11] stalls that
        // ended after those stages, [14] long jobs it found still moving and left alone.
        printf("COACHZ gpu stall: kicks %u (last fence %u), bus drains %u (last fence %u), ended after kick %u after drain %u, longest %u waits, left alone %u\n",
            native_adreno_unstick[0], native_adreno_unstick[1], native_adreno_unstick[4], native_adreno_unstick[7], native_adreno_unstick[9],
            native_adreno_unstick[11], native_adreno_unstick[12], native_adreno_unstick[14]);
        // Whether the render server got its swap back when the scanout client took each frame, or only when the copy finished: k216
        // showed it recording the next frame 0.3 ms after the client took one, and committing it only once that frame's copy completed.
        printf("COACHZ gpu swap: early retires %llu, early copies failed %llu, of %llu frames offered\n",
            (unsigned long long)native_legacy_early_retires, (unsigned long long)native_legacy_early_failures,
            (unsigned long long)native_gop_gpu_offers);
        printf("COACHZ gpu irq: %u interrupts, %u fences; waits slept %u, timed out %u; errors ahb %u atb %u cp hw %u (0x%x) hang %u uche %u; gmu %u (0x%x) hfi %u (0x%x)\n",
            gIrqAll, gFlushTs, gIdleBlocks, gIdleTimeouts, gIrqAhb, gIrqAtb, gIrqCpHw, gCpHwStatus, gIrqHang, gIrqOob, gGmuAoIrqs, gGmuAoStatus, gHfiIrqs, gHfiInfo);
        stampReport();
        ibProbeReport();
        frameSample();
        // Imported BOs the submit path left alone under IO coherence: [2] before submissions, [3] after completions.
        printf("COACHZ gpu9: coherent %u, maintenance skipped before submit %u after completion %u\n", native_adreno_io_coherent,
            native_adreno_coherent_skips[2], native_adreno_coherent_skips[3]);
        // UniXNU porting/gpu#frame-path, submission to GPU start: other processes' jobs ahead of backboardd's in the one ring.
        printf("COACHZ gpu9: ahead of backboardd: %u of %u submissions behind others, %u jobs in all, at most %u; others submitted %u\n",
            coachz_gpu_ahead[1], coachz_gpu_ahead[0], coachz_gpu_ahead[2], coachz_gpu_ahead[3], coachz_gpu_ahead[4]);
    }
}

// The GPU's load and clock each second it had work, from the counters hw_init starts as Linux does: the CP's always-count in core cycles (RBBM_PERFCTR_CP(0)),
// the GMU's busy counter (a6xx_gpu_busy) and the always-on counter, both at 19.2 MHz. A clock below the requested one would show here.
uint64_t coreCounter(uint32_t reg) {
    uint32_t high, low;
    do {
        high = coreRead(reg + 1);
        low = coreRead(reg);
    } while (high != coreRead(reg + 1));
    return uint64_t(high) << 32 | low;
}
uint64_t gmuBusy() {
    volatile uint32_t *low = reinterpret_cast<volatile uint32_t *>(gGmu + 0x14110), *top = reinterpret_cast<volatile uint32_t *>(gGmu + 0x14114);
    uint32_t high, value;
    do {
        high = *top;
        value = *low;
    } while (high != *top);
    return uint64_t(high) << 32 | value;
}
// CX-side 19.2 MHz timestamp, as a6xx_gmu_get_timestamp. The governor does
// not need a GX CP register just to measure elapsed time.
uint64_t gmuAlways() {
    volatile uint32_t *low=reinterpret_cast<volatile uint32_t *>(gGmu+0x14220), *top=reinterpret_cast<volatile uint32_t *>(gGmu+0x14224);
    uint32_t high,value;
    do {high=*top;value=*low;} while(high!=*top);
    return uint64_t(high)<<32|value;
}
// Where a busy second goes, by the A6xx performance counters (a6xx.xml countables; the selects are written once): SP busy, ALU, its stalls on
// TP, UCHE and RB and its non-execution cycles; TP busy, its UCHE stall and L1 requests and misses; UCHE busy, arbiter stalls and the bus latency;
// RB busy, its SP and CCU starvation and colour reads and writes; and the GBIF's AXI0/AXI1 read and write beats (kgsl's GBIF countables 34, 35,
// 46 and 47). Percentages are of the core cycles of that second.
struct ProfileCounter {uint32_t select, countable, counter;};
const ProfileCounter kProfile[] = {
    {0xae10, 0, 0x4a6}, {0xae11, 1, 0x4a8}, {0xae12, 4, 0x4aa}, {0xae13, 5, 0x4ac}, {0xae14, 6, 0x4ae}, {0xae15, 7, 0x4b0},
    {0xb610, 0, 0x48e}, {0xb611, 1, 0x490}, {0xb612, 6, 0x492}, {0xb613, 7, 0x494},
    {0x0e1c, 0, 0x476}, {0x0e1d, 1, 0x478}, {0x0e1e, 2, 0x47a}, {0x0e1f, 3, 0x47c},
    {0x8e10, 0, 0x4d6}, {0x8e11, 5, 0x4d8}, {0x8e12, 7, 0x4da}, {0x8e13, 14, 0x4dc}, {0x8e14, 15, 0x4de},
};
enum : unsigned {ProfileCount = sizeof(kProfile) / sizeof(kProfile[0]), GbifSel = 0x3cc2, GbifEnable = 0x3cc0, GbifClear = 0x3cc1, GbifLow = 0x3cc4, GbifHigh = 0x3cc8};
// Protected by the publisher lock. A GX restart loses the counter selectors too.
uint32_t gProfileEpoch;
constexpr uint32_t ProfileGbifSelection = 34u | 35u << 8 | 46u << 16 | 47u << 24;
uint64_t gbifCounter(unsigned index) {
    uint32_t high, low;
    do {
        high = coreRead(GbifHigh + index);
        low = coreRead(GbifLow + index);
    } while (high != coreRead(GbifHigh + index));
    return uint64_t(high) << 32 | low;
}
// Mainline a6xx_gmu_sysprof_setup: a persistent PERFCOUNTER OOB vote blocks
// IFPC while profiling. Publisher lock serializes this vote with explicit PM;
// the software asleep flag alone says nothing about GX/SP hardware availability.
uint32_t gmuRead(uint32_t at);
void gmuWrite(uint32_t at,uint32_t value);
bool gProfileVote;
uint32_t gProfilePowerSkips,gProfileOobFailures,gProfilePowerStatus;
enum : uint32_t {ProfilePowerStatus=0x14340, ProfileHostSet=0x14650,
    ProfileIntrInfo=0x14648, ProfileIntrClear=0x14644,
    ProfileRequest=1u<<17, ProfileAck=1u<<25};
void profilePowerPut() {
    if(!gProfileVote)return;
    // Legacy a6xx_gmu_clear_oob(PERFCOUNTER_SET): clear bit is also 25.
    gmuWrite(ProfileHostSet,ProfileAck);
    gProfileVote=false;
}
bool profilePowerGet() {
    if(!gGmu || __atomic_load_n(&gPmAsleep,__ATOMIC_SEQ_CST))return false;
    const bool acquiring=!gProfileVote;
    if(acquiring) {
        gmuWrite(ProfileHostSet,ProfileRequest);
        bool acknowledged=false;
        for(unsigned i=0;i<100;i++) {
            if(gmuRead(ProfileIntrInfo)&ProfileAck){acknowledged=true;break;}
            IODelay(100);
        }
        gmuWrite(ProfileIntrClear,ProfileAck);
        if(!acknowledged) {
            // Cancel the request even when firmware did not acknowledge it.
            gmuWrite(ProfileHostSet,ProfileAck);
            ++gProfileOobFailures;
            printf("COACHZ gpu prof power: OOB timeout %u; GX untouched\n",gProfileOobFailures);
            return false;
        }
        gProfileVote=true;
    }
    // a6xx_gmu_gx_is_on + a6xx_gmu_sptprac_is_on, A6XX (not A7XX)
    // bit positions. Require the positive SP power-on bit too, not an all-zero
    // status from an uninitialized block. Read only the CX/GMU window here.
    constexpr uint32_t off=(1u<<0)|(1u<<1)|(1u<<2)|(1u<<4)|(1u<<6)|(1u<<7);
    for(unsigned i=0;i<100;i++) {
        const uint32_t status=gmuRead(ProfilePowerStatus);
        gProfilePowerStatus=status;
        if(!(status&off) && (status&(1u<<3))) {
            if(acquiring)printf("COACHZ gpu prof power: vote on status 0x%x\n",status);
            return true;
        }
        IODelay(100);
    }
    const uint32_t skipped=++gProfilePowerSkips;
    if(skipped<=4 || !(skipped&(skipped-1)))
        printf("COACHZ gpu prof power: status 0x%x skip %u; GX untouched\n",gProfilePowerStatus,skipped);
    profilePowerPut();
    return false;
}
void profileSetup() {
    // All callers own the publisher lock; no GX MMIO precedes this guard.
    if(!profilePowerGet())return;

    for (const ProfileCounter &c : kProfile) coreWrite(c.select, c.countable);
    coreWrite(GbifClear, 0xf);
    coreWrite(GbifClear, 0);
    coreWrite(GbifSel, ProfileGbifSelection);
    coreWrite(GbifEnable, coreRead(GbifEnable) | 0xf);
    unsigned bad = 0;
    if (coreRead(GbifSel) != ProfileGbifSelection || (coreRead(GbifEnable) & 0xf) != 0xf) bad++;
    printf("COACHZ gpu prof config: epoch %u selector mismatches %u\n", ++gProfileEpoch, bad);
}
struct ProfileSample {
    uint64_t cycles, busy, always, counters[ProfileCount], gbif[4];
    uint32_t epoch;
};
bool profileSample(ProfileSample &sample) {
    // Keep all reads in one powered epoch; individual coreRead guards alone allow a
    // suspend/resume between two counters and produce an invalid subtraction.
    native_adreno_publisher_lock(1);
    if (!profilePowerGet()) {
        native_adreno_publisher_lock(0);
        return false;
    }
    unsigned bad = 0;
    uint32_t firstReg = 0, observed = 0, wanted = 0;
    auto check = [&](uint32_t reg, uint32_t value, uint32_t mask) {
        const uint32_t read = coreRead(reg) & mask;
        if (read == value) return;
        if (!bad) {firstReg = reg; observed = read; wanted = value;}
        bad++;
    };
    // Never read the core selectors back: SP_PERFCTR_SP_SEL_0 (0xae10) took a synchronous external abort on hardware with the GMU vote held and the
    // power status good.
    check(GbifSel, ProfileGbifSelection, ~0u);
    check(GbifEnable, 0xf, 0xf);
    const uint32_t epoch = gProfileEpoch;
    if (!bad) {
        sample.epoch = epoch;
        sample.cycles = coreCounter(0x400);
        sample.busy = gmuBusy();
        sample.always = coreCounter(0x980);
        for (unsigned i = 0; i < ProfileCount; i++) sample.counters[i] = coreCounter(kProfile[i].counter);
        for (unsigned i = 0; i < 4; i++) sample.gbif[i] = gbifCounter(i);
    }
    native_adreno_publisher_lock(0);
    // A bad selection is not a bandwidth or stall measurement. Report once per epoch.
    static uint32_t lastBadEpoch = ~0u;
    if (bad && lastBadEpoch != epoch) {
        lastBadEpoch = epoch;
        printf("COACHZ gpu prof invalid: epoch %u mismatches %u first 0x%x read 0x%x want 0x%x\n", epoch, bad, firstReg, observed, wanted);
    }
    return !bad;
}

// Dynamic GPU frequency, tried once 20 s after the driver starts: the DCVS request the bring-up sends (a6xx_gmu_set_freq for a legacy
// GMU: DCVS_PERF_SETTING, then OOB DCVS_SET on bit 23, acknowledged on bit 31 and cleared through bit 31), at perf levels 3, 1 and 7
// (355, 180, 800 MHz), each held half a second while the CP cycle counter measures the clock that results. It ends at 800 MHz, the run level.
enum : uint32_t {GmuDcvsAck = 0x8ff0, GmuDcvsPerf = 0x8ff4, GmuDcvsBw = 0x8ff8, GmuDcvsReturn = 0x8ffc, GmuHostSet = 0x14650,
    GmuInfo = 0x14648, GmuClearIrq = 0x14644};
uint32_t gmuRead(uint32_t at) {return *reinterpret_cast<volatile uint32_t *>(gGmu + at);}
void gmuWrite(uint32_t at, uint32_t value) {
    __asm__ volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(gGmu + at) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
unsigned dcvsSet(unsigned level, uint32_t *result) {
    native_adreno_publisher_lock(1);
    native_gpu_dcvs[1] = level;
    if (__atomic_load_n(&gPmAsleep, __ATOMIC_SEQ_CST)) {native_adreno_publisher_lock(0);return 0;}   // no firmware to ask
    gmuWrite(GmuDcvsAck, 0);
    gmuWrite(GmuDcvsPerf, 3u << 28 | level);
    gmuWrite(GmuDcvsBw, 0xff);
    gmuWrite(GmuHostSet, 1u << 23);
    unsigned polls = 0;
    while (polls < 1000 && !(gmuRead(GmuInfo) & (1u << 31))) {
        IODelay(10);
        polls++;
    }
    gmuWrite(GmuClearIrq, 1u << 31);
    gmuWrite(GmuHostSet, 1u << 31);
    *result = gmuRead(GmuDcvsReturn);
    native_adreno_publisher_lock(0);
    return polls < 1000 ? polls + 1 : 0;
}
void dcvsTest() {
    static const unsigned levels[] = {3, 1, 7};
    for (unsigned level : levels) {
        uint32_t result = 0;
        const uint64_t began = mach_absolute_time();
        const unsigned polls = dcvsSet(level, &result);
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
        ProfileSample first{},last{};
        if(!profileSample(first))continue;
        IOSleep(500);
        if(!profileSample(last) || first.epoch!=last.epoch)continue;
        const uint64_t dCycles=last.cycles-first.cycles, dAlways=last.always-first.always;
        printf("COACHZ gpu dcvs: level %u acknowledged after %u polls in %llu us, return %u, clock %llu MHz\n", level, polls,
            static_cast<unsigned long long>(ns / 1000), result, static_cast<unsigned long long>(dAlways ? dCycles * 192 / dAlways / 10 : 0));
    }
}

// The governor (coachz-gpu-dcvs=1), sheng's rules from UniXNU porting/power: every 100 ms the GMU's XO busy count picks the lowest level that
// keeps the GPU about half busy, never below level 3; within 1 s of a touch report the top level; work resuming from an idle window at least
// level 5 (565 MHz; sheng doubles to at least 578 MHz); up at once, down after four windows that all ask for less; within the thermal cap.
// Level clocks as the DCVS test measured them on hardware: 3 355, 4 430, 5 565, 6 650, 7 800 MHz.
extern "C" volatile uint32_t coachz_touch_reports;   // bsd/dev/coachz_touch.c
const unsigned kLevelMHz[8] = {0, 180, 267, 355, 430, 565, 650, 800};
unsigned governorTarget(unsigned level, uint64_t dBusy, uint64_t dAlways, bool touched, bool resumed) {
    if (touched) return 7;
    const uint64_t demand = dAlways ? dBusy * kLevelMHz[level] * 2 / dAlways : 0;   // the clock at which it would be half busy
    unsigned target = 3;
    while (target < 7 && kLevelMHz[target] < demand) target++;
    if (resumed && target < 5) target = 5;
    return target;
}

// Runtime power for A618 (no IFPC): 66 ms after the last work retires the GMU slumbers, and the next submission
// brings it back through the whole start. Armed by the power button (coachz_gpu_pm_arm), so a failure can only follow that press.
volatile uint32_t gPmArmed;
// CX_MISC (sc7180.dtsi's cx_mem), mapped by cacheSetup: it lies in the CX domain, so a restart that collapses CX writes its system cache
// control back (a6xx_pm_resume's a6xx_llc_activate).
vm_offset_t gCxMisc;
volatile uint64_t gPmLastSubmit, gPmLastBusy;
uint32_t gPmRetireHolds, gPmBusyHolds;
uint32_t gPmSleeps, gPmWakes;
void pmSleep() {
    const uint64_t began = mach_absolute_time();
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuAoMask) = ~0u;   // a6xx_gmu_irq_disable
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuHfiMask) = ~0u;
    coreWrite(RbbmInt0Mask, 0);
    __atomic_store_n(&gPmAsleep, 1u, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&gPmUsers, __ATOMIC_SEQ_CST)) IODelay(1);
    profilePowerPut();
    const int status = coachz_gpu_pm_suspend();
    gPmSleeps = gPmSleeps + 1;
    printf("COACHZ gpu pm: slumber %u status %d in %llu us\n", gPmSleeps, status, (mach_absolute_time() - began) / 19);
}
// a6xx_recover's power-off of a hung GPU (coachz_gpu_pm_force_off, a6xx_gmu_force_off and with collapse the CX collapse), entered as pmSleep
// enters the slumber: the GMU's and the core's interrupts masked, the core's users waited out.
void pmForceOff(int collapse) {
    const uint64_t began = mach_absolute_time();
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuAoMask) = ~0u;   // a6xx_gmu_irq_disable
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuHfiMask) = ~0u;
    coreWrite(RbbmInt0Mask, 0);
    __atomic_store_n(&gPmAsleep, 1u, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&gPmUsers, __ATOMIC_SEQ_CST)) IODelay(1);
    profilePowerPut();
    const int status = coachz_gpu_pm_force_off(collapse);
    printf("COACHZ gpu recover: force off status %d collapse %d in %llu us\n", status, collapse, (mach_absolute_time() - began) / 19);
}
// Under the publisher's lock, before a submission: the firmware and hw_init again, then what startCp does after them.
void pmGet() {
    const uint64_t now = mach_absolute_time();
    __atomic_store_n(&gPmLastSubmit, now, __ATOMIC_RELEASE);
    __atomic_store_n(&gPmLastBusy, now, __ATOMIC_RELEASE);
    if (!__atomic_load_n(&gPmAsleep, __ATOMIC_SEQ_CST)) return;
    const uint64_t began = mach_absolute_time();
    const int status = coachz_gpu_pm_resume();
    if(status) {
        printf("COACHZ gpu pm: resume failed %d; GX access remains blocked\n",status);
        return;
    }
    __atomic_store_n(&gPmAsleep, 0u, __ATOMIC_SEQ_CST);
    coreWrite(CpSqeInstrBase, uint32_t(gBuffers[0].iova));
    coreWrite(CpSqeInstrBase + 1, uint32_t(gBuffers[0].iova >> 32));
    const IOReturn ring = gRing.restart(gRing.lastSubmitted());
    coreWrite(CpSqeCntl, 1);
    uint32_t init[9] = {0, 0x2fu, 3u, 0x20000000u, 0, 0, 0, 0, 0};
    uint32_t fence = 0, observed = 0;
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    const IOReturn sent = AdrenoPM4Type7(Pm4MeInit, 8, &init[0]) == AdrenoPM4Success ? gRing.submit(init, 9, &fence) : kIOReturnError;
    const NativeAdrenoRingFence waited = sent ? NativeAdrenoRingFenceArgument : gRing.wait(fence, &clock, 200000, &observed);
    coreWrite(RbbmSecvidTrustCntl, 0);
    coreWrite(RbbmIntClearCmd, ~0u);
    coreWrite(RbbmInt0Mask, IrqMask);
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuAoClr) = ~0u;
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuHfiClr) = ~0u;
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuAoMask) = ~uint32_t(GmuAoErrors);
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuHfiMask) = ~uint32_t(GmuHfiFault);
    if (!status) profileSetup();
    gPmWakes = gPmWakes + 1;
    printf("COACHZ gpu pm: resume %u status %d ring 0x%x ME_INIT 0x%x wait %d in %llu us, %u core accesses dropped asleep\n", gPmWakes, status, ring, sent,
        int(waited), (mach_absolute_time() - began) / 19, gPmDropped);
}
// The board's GPU restart for the framework's hangcheck (NativeAdrenoPublish.cpp, msm's hangcheck timer and recover_worker), called under
// the publisher lock when the ring's fence stood still for a period and the CP made no progress. As a6xx_recover: the SQE halted first
// (CP_SQE_CNTL 3), then the power-off a runtime suspend does (a6xx_bus_clear_pending_transactions, the software reset, the GMU slumber,
// GX collapsed), then the cold start and hw_init of a resume (msm_gpu_hw_init). Between the reset and the CP init the ring restarts at the
// hung fence (NativeAdrenoRing::restart), and the CP init goes without a fence and is waited for with a6xx_idle, so the submissions the
// framework replays keep their numbers. Without this hook a hang was only counted and every client waited on the stuck fence forever.
// The power-off is a6xx_recover's for a hung GPU (gRecoverMode 1, coachz-gpu-recover): a6xx_gmu_force_off, then GX and CX collapsed, which
// resets the GBIF and the GPU SMMU; the SMMU's registers and CX_MISC's system cache control are written back before the cold start.
// k222 took the runtime slumber instead (coachz-gpu-recover=0): its GBIF halt waited 100 ms in vain, and the start after it stopped where
// the HFI queues start. 2 forces off with CX left on.
uint32_t gRestarts, gRestartFailures;
unsigned gRecoverMode = 1;
IOReturn gpuRestart(void *, NativeAdrenoRing *ring, uint32_t completed) {
    const uint64_t began = mach_absolute_time();
    const bool awake = !__atomic_load_n(&gPmAsleep, __ATOMIC_SEQ_CST);
    const uint32_t rbbm = awake ? coreRead(0x0210) : 0;
    uint32_t misc[2] = {};
    if (awake && gCxMisc) {
        misc[0] = *reinterpret_cast<volatile uint32_t *>(gCxMisc + 4);
        misc[1] = *reinterpret_cast<volatile uint32_t *>(gCxMisc + 8);
    }
    if (awake) {
        coreWrite(CpSqeCntl, 3);
        if (gRecoverMode == 1 || gRecoverMode == 2) pmForceOff(gRecoverMode == 1);
        else pmSleep();
    }
    const int status = coachz_gpu_pm_resume();
    if (status) {
        gRestartFailures = gRestartFailures + 1;
        printf("COACHZ gpu recover: restart %u at fence %u failed: resume %d; GX access remains blocked\n", gRestarts + gRestartFailures,
            completed, status);
        return kIOReturnNotResponding;
    }
    __atomic_store_n(&gPmAsleep, 0u, __ATOMIC_SEQ_CST);
    if (awake && gCxMisc && (gRecoverMode == 1 || gRecoverMode == 2)) {
        // a6xx_llc_activate's order: SYSTEM_CACHE_CNTL_1, then CNTL_0.
        *reinterpret_cast<volatile uint32_t *>(gCxMisc + 8) = misc[1];
        __asm__ volatile("dsb sy" ::: "memory");
        *reinterpret_cast<volatile uint32_t *>(gCxMisc + 4) = misc[0];
        __asm__ volatile("dsb sy" ::: "memory");
    }
    coreWrite(CpSqeInstrBase, uint32_t(gBuffers[0].iova));
    coreWrite(CpSqeInstrBase + 1, uint32_t(gBuffers[0].iova >> 32));
    const IOReturn restarted = ring->restart(completed);
    coreWrite(CpSqeCntl, 1);
    uint32_t init[9] = {0, 0x2fu, 3u, 0x20000000u, 0, 0, 0, 0, 0};
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    const IOReturn sent = restarted ? restarted :
        AdrenoPM4Type7(Pm4MeInit, 8, &init[0]) == AdrenoPM4Success ? ring->submitUnfenced(init, 9) : kIOReturnError;
    const IOReturn idle = sent ? sent : ring->idle(&clock, 200000);
    coreWrite(RbbmSecvidTrustCntl, 0);
    coreWrite(RbbmIntClearCmd, ~0u);
    coreWrite(RbbmInt0Mask, IrqMask);
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuAoClr) = ~0u;
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuHfiClr) = ~0u;
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuAoMask) = ~uint32_t(GmuAoErrors);
    *reinterpret_cast<volatile uint32_t *>(gGmu + GmuHfiMask) = ~uint32_t(GmuHfiFault);
    profileSetup();
    const uint64_t now = mach_absolute_time();
    __atomic_store_n(&gPmLastSubmit, now, __ATOMIC_RELEASE);
    __atomic_store_n(&gPmLastBusy, now, __ATOMIC_RELEASE);
    if (idle) gRestartFailures = gRestartFailures + 1;
    else gRestarts = gRestarts + 1;
    printf("COACHZ gpu recover: restart %u at fence %u, rbbm before 0x%x, ring 0x%x ME_INIT 0x%x idle 0x%x in %llu us\n",
        gRestarts + gRestartFailures, completed, rbbm, restarted, sent, idle, (now - began) / 19);
    return idle;
}
void pmThread(void *, wait_result_t) {
    uint32_t retired = 0;
    for (;;) {
        IOSleep(20);
        if (!gPmArmed || __atomic_load_n(&gPmAsleep, __ATOMIC_ACQUIRE)) continue;
        native_adreno_publisher_lock(1);
        const uint64_t now = mach_absolute_time();
        const uint32_t completed = gRing.completedFence(); // shared fence memory, not GX MMIO
        const bool pending = completed != gRing.lastSubmitted();
        // Linux msm_gpu.c::retire_submit marks last busy before the final
        // autosuspend put. Submission age alone can expire while work runs.
        if (pending || completed != retired) {
            if (pending) ++gPmBusyHolds;
            if (completed != retired) ++gPmRetireHolds;
            retired = completed;
            __atomic_store_n(&gPmLastBusy, now, __ATOMIC_RELEASE);
        } else if (now - __atomic_load_n(&gPmLastBusy, __ATOMIC_ACQUIRE) >= 19200 * 66) {
            const uint32_t n = gPmSleeps + 1;
            if (n <= 4 || !(n & (n - 1)))
                printf("COACHZ gpu pm: idle %u submit %llu ms retire %llu ms held busy %u retire %u\n", n,
                    (now - __atomic_load_n(&gPmLastSubmit, __ATOMIC_ACQUIRE)) / 19200,
                    (now - __atomic_load_n(&gPmLastBusy, __ATOMIC_ACQUIRE)) / 19200, gPmBusyHolds, gPmRetireHolds);
            pmSleep();
        }
        native_adreno_publisher_lock(0);
    }
}

void rateThread(void *, wait_result_t) {
    uint64_t busy=0,always=0;
    unsigned governed = 0, level = 7, changes = 0, lower = 0;
    PE_parse_boot_argn("coachz-gpu-dcvs", &governed, sizeof(governed));
    native_adreno_publisher_lock(1);
    if (!__atomic_load_n(&gPmAsleep, __ATOMIC_SEQ_CST)) profileSetup();
    native_adreno_publisher_lock(0);
    ProfileSample previous{};
    bool havePrevious = false;
    uint64_t windowBusy = busy, windowAlways = always, boostUntil = 0, ticks = 0;
    bool haveGovernorWindow=false;
    uint32_t touches = coachz_touch_reports;
    bool idle = true;
    for (unsigned seconds = 0;; seconds++) {
        for (unsigned tick = 0; tick < 10; tick++, ticks++) {
            IOSleep(100);
            if (!governed || seconds <= 20) continue;
            native_adreno_publisher_lock(1);
            const bool active=!__atomic_load_n(&gPmAsleep,__ATOMIC_SEQ_CST);
            const uint64_t nowBusy=active?gmuBusy():0,nowAlways=active?gmuAlways():0;
            native_adreno_publisher_lock(0);
            if(!active){haveGovernorWindow=false;continue;}
            if(!haveGovernorWindow || nowBusy<windowBusy || nowAlways<windowAlways) {
                windowBusy=nowBusy;windowAlways=nowAlways;haveGovernorWindow=true;continue;
            }
            const uint64_t wBusy = nowBusy - windowBusy, wAlways = nowAlways - windowAlways;
            windowBusy = nowBusy;
            windowAlways = nowAlways;
            const uint32_t reports = coachz_touch_reports;
            if (reports != touches) boostUntil = ticks + 10;
            touches = reports;
            const bool working = wAlways && wBusy * 50 >= wAlways;   // at least 2 % busy
            unsigned target = governorTarget(level, wBusy, wAlways, ticks < boostUntil, working && idle);
            idle = !working;
            if (target > coachz_gpu_level_cap) target = coachz_gpu_level_cap;
            lower = target < level ? lower + 1 : 0;
            uint32_t result = 0;
            if ((target > level || (target < level && (lower >= 4 || level > coachz_gpu_level_cap))) && dcvsSet(target, &result)) {
                level = target;
                changes++;
                lower = 0;
            }
        }
        if (seconds == 20) dcvsTest();
        ProfileSample current{};
        if (!profileSample(current)) {havePrevious = false; continue;}
        if (!havePrevious || current.epoch != previous.epoch) {
            previous = current;
            havePrevious = true;
            continue;
        }
        const uint64_t dCycles = current.cycles - previous.cycles, dBusy = current.busy - previous.busy, dAlways = current.always - previous.always;
        uint64_t d[ProfileCount], g[4];
        for (unsigned i = 0; i < ProfileCount; i++) d[i] = current.counters[i] - previous.counters[i];
        for (unsigned i = 0; i < 4; i++) g[i] = current.gbif[i] - previous.gbif[i];
        previous = current;
        if (!dAlways || dBusy * 20 < dAlways || !dCycles) continue;
        auto pct = [dCycles](uint64_t value) {return static_cast<unsigned long long>(value * 100 / dCycles);};
        // The CP counter counts every core cycle, idle ones too, so the second number is the clock itself.
        printf("COACHZ gpu rate: busy %llu%%, %llu MHz over the second, level %u (%u changes)\n",
            static_cast<unsigned long long>(dBusy * 100 / dAlways), static_cast<unsigned long long>(dCycles * 192 / dAlways / 10), level, changes);
        printf("COACHZ gpu prof: sp %llu alu %llu st tp %llu uche %llu rb %llu ne %llu | tp %llu st %llu l1 %lluk miss %llu%% | uche %llu arb %llu lat %llu"
            " | rb %llu starve sp %llu ccu %llu c r %lluk w %lluk | gbif r %lluk %lluk w %lluk %lluk\n",
            pct(d[0]), pct(d[1]), pct(d[2]), pct(d[3]), pct(d[4]), pct(d[5]), pct(d[6]), pct(d[7]), static_cast<unsigned long long>(d[8] / 1000),
            static_cast<unsigned long long>(d[8] ? d[9] * 100 / d[8] : 0), pct(d[10]), pct(d[11]), static_cast<unsigned long long>(d[13] ? d[12] / d[13] : 0),
            pct(d[14]), pct(d[15]), pct(d[16]), static_cast<unsigned long long>(d[17] / 1000), static_cast<unsigned long long>(d[18] / 1000),
            static_cast<unsigned long long>(g[0] / 1000), static_cast<unsigned long long>(g[1] / 1000), static_cast<unsigned long long>(g[2] / 1000),
            static_cast<unsigned long long>(g[3] / 1000));
    }
}

// The system cache as mainline configures it (llcc-qcom.c; sc7180_data with llcc_v1_reg_offset, so the version 1 path): read the
// version and bank count from llcc0, write each slice's ATTR1 (priority, max capacity in cache lines per bank) and ATTR0 (bonus
// ways) through the broadcast region, clear its capacity-allocation disable bit and set its retain-on-power-collapse bit, then
// activate CPUSS (activate_on_init). The GPU then activates its GPU and GPUHTW slices and programs CX_MISC SYSTEM_CACHE_CNTL_1 and
// CNTL_0 as a6xx_llc_activate does for a GPU without MMU-500. Each slice's state is printed before and after (coachz-llcc=1).
extern "C" vm_offset_t ml_io_map(vm_offset_t physical, vm_size_t size);
struct LlccSlice {uint32_t id, maxCapKiB, priority, bonusWays; bool retain, activateOnInit;};
const LlccSlice kSc7180Slices[] = {{1, 256, 1, 0xf, true, true}, {8, 128, 1, 0xf, true, false}, {11, 128, 1, 0xf, true, false},
    {12, 128, 1, 0xf, true, false}};
enum : uint32_t {LlccActCtrl = 0x1000, LlccStatus = 4, LlccAttr0 = 0x21000, LlccAttr1 = 0x21004, LlccDisCapAlloc = 0x21f00,
    LlccPcbAct = 0x21f04, LlccHwInfo = 0x30000, LlccStatus0 = 0x3000c, LlccDeactivate = 2};

bool llccActivate(vm_offset_t broadcast, uint32_t id) {
    auto reg = [broadcast](uint32_t at) {return reinterpret_cast<volatile uint32_t *>(broadcast + at);};
    const uint32_t value = 1u << 1;   // ACT_CTRL_OPCODE_ACTIVATE << ACT_CTRL_OPCODE_SHIFT
    *reg(id * LlccActCtrl) = value | 1u;   // with ACT_CTRL_ACT_TRIG
    __asm__ volatile("dsb sy" ::: "memory");
    *reg(id * LlccActCtrl) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned polls = 0; polls < 100; polls++) {
        if (!(*reg(LlccStatus + id * LlccActCtrl) & LlccDeactivate)) return true;
        IODelay(1);
    }
    return false;
}

// The GPU speed bin, read only: sc7180.dtsi's gpu_speed_bin cell (qfprom corrected region at 0x00784000, reg <0x1d2 0x2>, bits <5 8>),
// which a6xx_catalog.c's A618 entry maps to a bin (0 -> 0, 169 -> 1, 174 -> 2) and the GPU OPP table's opp-supported-hw to 825 MHz on
// bin 2 alone (0x04). The USB PHY setup reads the same region (coachz_usb_soc.c, qusb2 trim).
void speedBinReport() {
    const vm_offset_t page = ml_io_map_unmappable(0x00784000, 0x1000, 0x7 /* VM_WIMG_IO */);
    if (!page) {printf("COACHZ gpu speed bin: qfprom map failed\n"); return;}
    const uint32_t word = *reinterpret_cast<volatile uint32_t *>(page + 0x1d0);
    ml_io_unmap(page, 0x1000);
    const uint32_t fuse = ((word >> 16) >> 5) & 0xff;
    const unsigned bin = fuse == 174 ? 2 : fuse == 169 ? 1 : 0;
    printf("COACHZ gpu speed bin: qfprom 0x1d0 0x%08x fuse %u bin %u (%s)\n", word, fuse, bin, bin == 2 ? "825 MHz allowed" : "800 MHz top");
}

void cacheSetup() {
    const vm_offset_t llcc = ml_io_map(0x09200000, 0x31000), broadcast = ml_io_map(0x09600000, 0x22000), misc = ml_io_map(0x0509e000, 0x1000);
    gCxMisc = misc;
    auto read = [](vm_offset_t at) {return *reinterpret_cast<volatile uint32_t *>(at);};
    auto write = [](vm_offset_t at, uint32_t value) {
        *reinterpret_cast<volatile uint32_t *>(at) = value;
        __asm__ volatile("dsb sy" ::: "memory");
    };
    if (!llcc || !broadcast || !misc) {
        printf("COACHZ llcc: map failed\n");
        return;
    }
    auto report = [&](const char *when) {
        for (const LlccSlice &s : kSc7180Slices)
            printf("COACHZ llcc: %s slice %u act 0x%x status 0x%x attr0 0x%x attr1 0x%x\n", when, s.id, read(llcc + s.id * LlccActCtrl),
                read(llcc + LlccStatus + s.id * LlccActCtrl), read(llcc + LlccAttr0 + 8 * s.id), read(llcc + LlccAttr1 + 8 * s.id));
        printf("COACHZ llcc: %s pcb 0x%x dis-cap 0x%x gpu system cache cntl0 0x%x cntl1 0x%x\n", when, read(llcc + LlccPcbAct),
            read(llcc + LlccDisCapAlloc), read(misc + 4), read(misc + 8));
    };
    const uint32_t version = read(llcc + LlccHwInfo), banks = read(llcc + LlccStatus0) >> 28;
    printf("COACHZ llcc: version 0x%x banks %u\n", version, banks);
    report("before");
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("coachz-llcc", &enabled, sizeof(enabled)) || enabled != 1) return;
    if (version >= 0x02000000u || !banks) {
        printf("COACHZ llcc: not the version 1 layout sc7180_data uses; left alone\n");
        return;
    }
    for (const LlccSlice &s : kSc7180Slices) {
        const uint32_t lines = (s.maxCapKiB * 1024 / banks) >> 6;
        write(broadcast + LlccAttr1 + 8 * s.id, s.priority << 4 | lines << 16);
        write(broadcast + LlccAttr0 + 8 * s.id, s.bonusWays << 16);
        write(broadcast + LlccDisCapAlloc, read(broadcast + LlccDisCapAlloc) & ~(1u << s.id));
        if (s.retain) write(broadcast + LlccPcbAct, read(broadcast + LlccPcbAct) | 1u << s.id);
    }
    bool cpu = llccActivate(broadcast, 1), gpu = llccActivate(broadcast, 12), htw = llccActivate(broadcast, 11);
    printf("COACHZ llcc: activated cpu %d gpu %d gpuhtw %d\n", cpu, gpu, htw);
    if (gpu && htw) {
        const uint32_t scid = 12;
        write(misc + 8, scid | scid << 5 | scid << 10 | scid << 15 | scid << 20 | 11u << 25);
        write(misc + 4, (read(misc + 4) & ~0xfu) | 0x3u);   // no allocation on a write miss
    }
    report("after");
}

extern "C" void coachz_gpu_trace_start(void);   // bsd/dev/coachz_trace.c, coachz-gpu-trace=1
// The SQE firmware, the kit's GPU context and the CP's first packets.
bool startCp(volatile uint8_t *smmu, size_t smmuBytes, const uint32_t *sqe, uint32_t sqeBytes) {
    *reinterpret_cast<volatile uint32_t *>(smmu + SmmuScr0) = *reinterpret_cast<volatile uint32_t *>(smmu + SmmuScr0) | SmmuKitGlobal;
    // The groups nothing uses fault, as Linux's arm_smmu_device_reset leaves them; their reset value would translate into bank 0.
    for (unsigned group = 0; group < SmmuGroups; ++group) {
        if (group != SmmuGmuGroup) *reinterpret_cast<volatile uint32_t *>(smmu + SmmuS2cr + 4 * group) = SmmuS2crFault;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    // Bank 0 and stream 0, as Linux's Adreno SMMU; group 1 and bank 1 are the GMU's. The kit routes two streams: sc7180.dtsi names only stream 0 for
    // the GPU, so the second is an unused stream on a free group.
    const IOReturn context = NativeAdrenoGpuContextBringup(smmu, smmuBytes, 0, 2, 0, 0, 2, 1, nullptr, &gReport);
    printf("COACHZ gpu9: context status 0x%x step %u program %u/%u route %u/%u verified %u output bits %u spans %u\n", context, gReport.step,
        gReport.program_status, gReport.program_enabled, gReport.route_status, gReport.route_phase, gReport.routes_verified, gReport.output_bits, gReport.spans);
    if (context) return false;
    if (NativeAdrenoGpuContextBuffers(gBuffers)) {
        printf("COACHZ gpu9: context buffers refused\n");
        return false;
    }
    volatile uint8_t *cpu[GpuBufferCount] = {};
    for (unsigned i = 0; i < GpuBufferCount; ++i) {
        IODeviceMemory *memory = IODeviceMemory::withRange(gBuffers[i].physical, gBuffers[i].bytes);
        gMaps[i] = memory ? memory->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
        if (memory) memory->release();
        if (!gMaps[i]) {
            printf("COACHZ gpu9: buffer %u did not map\n", i);
            return false;
        }
        cpu[i] = reinterpret_cast<volatile uint8_t *>(gMaps[i]->getVirtualAddress());
    }
    // The file without its first dword, as adreno_fw_create_bo copies it.
    const uint32_t words = sqeBytes / 4 - 1;
    if (words * 4 > gBuffers[0].bytes) {
        printf("COACHZ gpu9: SQE firmware of %u bytes does not fit\n", sqeBytes);
        return false;
    }
    volatile uint32_t *image = reinterpret_cast<volatile uint32_t *>(cpu[0]);
    for (uint32_t i = 0; i < words; ++i) image[i] = sqe[i + 1];
    __asm__ volatile("dsb sy" ::: "memory");
    coreWrite(CpSqeInstrBase, uint32_t(gBuffers[0].iova));
    coreWrite(CpSqeInstrBase + 1, uint32_t(gBuffers[0].iova >> 32));
    const NativeAdrenoRingIo io{nullptr, ringWrite, ringRead, ringBarrier, nullptr, nullptr};
    const NativeAdrenoRingCache cache{nullptr, ringPublish, ringAcquire};
    IOReturn status = gRing.attach(&io, &cache, const_cast<uint8_t *>(cpu[1]), gBuffers[1].bytes, gBuffers[1].iova, const_cast<uint8_t *>(cpu[2]),
        gBuffers[2].bytes, gBuffers[2].iova);
    if (status) {
        printf("COACHZ gpu9: ring attach 0x%x\n", status);
        return false;
    }
    coreWrite(CpSqeCntl, 1);
    // a6xx_cp_init: multiple hardware contexts, error detection, no header dump, no workarounds.
    uint32_t init[9] = {0, 0x2fu, 3u, 0x20000000u, 0, 0, 0, 0, 0};
    if (AdrenoPM4Type7(Pm4MeInit, 8, &init[0]) != AdrenoPM4Success) return false;
    uint32_t fence = 0, observed = 0;
    status = gRing.submit(init, 9, &fence);
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    const NativeAdrenoRingFence waited = status ? NativeAdrenoRingFenceArgument : gRing.wait(fence, &clock, 200000, &observed);
    printf("COACHZ gpu9: ME_INIT submit 0x%x fence %u wait %d observed %u\n", status, fence, int(waited), observed);
    cpState("after ME_INIT");
    if (waited != NativeAdrenoRingFenceComplete) return false;
    coreWrite(RbbmSecvidTrustCntl, 0);   // no zap shader on this board: Linux leaves secure mode this way
    return true;
}

// IO coherence, before any client (sheng-gpu-coherent, default 1), as UniXNU-sheng package/GpuPowerProbe.cpp coherenceProbe (MIT) proves
// it: mainline marks A618 ADRENO_QUIRK_HAS_CACHED_COHERENT (a6xx_catalog.c), like sheng's A740. Without native_adreno_io_coherent the
// driver cleans and invalidates every imported page around every submission (asteroids T40c: 18 s for 3000 submissions; sheng int-143:
// about 12 ms each). A page of kernel memory is imported and mapped write-back inner-shareable, and both directions are proved against
// DRAM: the GPU copies a page whose new words only the CPU's caches hold (at least 48 of 64 lines seen dirty first), and the CPU reads,
// with plain cached loads, a page the GPU wrote over clean lines it holds. Only both proofs turn the flag on. The copies are CP_MEMCPY and
// CP_WAIT_MEM_WRITES, which Turnip itself emits on A6xx.
bool dramPage(uint64_t pa, uint32_t *out) {
    const vm_offset_t page = ml_io_map_unmappable(vm_offset_t(pa), 0x1000, 0x7 /* VM_WIMG_IO */);
    if (!page) return false;
    for (unsigned i = 0; i < 1024; ++i) out[i] = reinterpret_cast<const volatile uint32_t *>(page)[i];
    ml_io_unmap(page, 0x1000);
    return true;
}
bool gpuCopyPage(uint64_t src, uint64_t dst) {
    uint32_t words[7] = {};
    if (AdrenoPM4Type7(0x75, 5, &words[0]) != AdrenoPM4Success || AdrenoPM4Type7(0x12, 0, &words[6]) != AdrenoPM4Success) return false;
    words[1] = 1024; words[2] = uint32_t(src); words[3] = uint32_t(src >> 32); words[4] = uint32_t(dst); words[5] = uint32_t(dst >> 32);
    uint32_t fence = 0, observed = 0;
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    return !gRing.submit(words, 7, &fence) && gRing.wait(fence, &clock, 200000, &observed) == NativeAdrenoRingFenceComplete;
}
void coherenceProbe(NativeAdrenoVm *vm) {
    uint32_t k[8] = {};
    unsigned wanted = 1, attempts = 0;
    PE_parse_boot_argn("sheng-gpu-coherent", &wanted, sizeof wanted);
    if (!wanted || !vm) {
        printf("COACHZ gpu9: coherence H skip (arg %u vm %d)\n", wanted, vm != nullptr);
        return;
    }
    constexpr size_t kBytes = 4 * 4096;
    uint64_t iova = 0;
    uint8_t *buf = static_cast<uint8_t *>(IOMallocAligned(kBytes, 4096));
    NativeAdrenoMemory mem;
    uint64_t handle = 0;
    IOReturn st = buf ? mem.importTask(kernel_task, uint64_t(uintptr_t(buf)), kBytes, gReport.output_bits, kBytes) : kIOReturnNoMemory;
    k[0] = 1;
    if (!st) st = vm->findAddress(0, kBytes, 4096, &iova);
    if (!st) {k[0] = 2; st = vm->map(&mem, 0, iova, 3, 1, 3, &handle);}
    uint64_t phys[4] = {};
    for (size_t j = 0; !st && j < mem.spanCount(); ++j)
        for (size_t at = 0; at < mem.spans()[j].bytes; at += 4096) phys[(mem.spans()[j].offset + at) / 4096] = mem.spans()[j].physical + at;
    static uint32_t words[1024];
    bool pass = false;
    if (!st && phys[0] && phys[1] && phys[2] && phys[3]) {
        k[0] = 3;
        volatile uint32_t *S = reinterpret_cast<volatile uint32_t *>(buf), *D = S + 1024, *R = S + 2048, *T = S + 3072;
        bool dirty = false;
        for (unsigned attempt = 0; attempt < 8 && !st && !dirty; ++attempt) {
            attempts = attempt + 1;
            for (unsigned i = 0; i < 1024; ++i) {S[i] = 0x11110000u | i; D[i] = 0; R[i] = 0x33330000u | i; T[i] = 0x44440000u | i;}
            st = NativeAdrenoMaintainOwnedPages(buf, kBytes, NativeAdrenoCleanInvalidate);
            for (unsigned i = 0; i < 1024; ++i) S[i] = 0x22220000u | i;
            __asm__ volatile("dsb sy" ::: "memory");
            k[1] = 0;
            if (!st && dramPage(phys[0], words)) for (unsigned i = 0; i < 1024; i += 16) k[1] += words[i] == (0x11110000u | i);
            dirty = k[1] >= 48;
        }
        if (!st && dirty) {
            k[0] = 4;
            if (gpuCopyPage(iova, iova + 4096) && dramPage(phys[1], words)) {
                k[0] = 5;
                for (unsigned i = 0; i < 1024; ++i) {k[2] += words[i] == (0x22220000u | i); k[3] += words[i] == (0x11110000u | i);}
            }
        }
        if (!st && k[0] == 5) st = NativeAdrenoMaintainOwnedPages(const_cast<uint32_t *>(T), 4096, NativeAdrenoClean);
        uint32_t touch = 0;
        for (unsigned i = 0; i < 1024; ++i) touch += T[i];
        __asm__ volatile("dsb sy" ::: "memory");
        if (!st && k[0] == 5 && gpuCopyPage(iova + 8192, iova + 12288)) {
            k[0] = 6;
            for (unsigned i = 0; i < 1024; ++i) k[4] += T[i] == (0x33330000u | i);
            if (dramPage(phys[3], words)) for (unsigned i = 0; i < 1024; ++i) k[5] += words[i] == (0x33330000u | i);
        }
        (void)touch;
        pass = k[0] == 6 && k[1] >= 48 && k[2] == 1024 && k[4] == 1024 && k[5] == 1024;
    }
    k[6] = uint32_t(st);
    if (handle) (void)vm->unmap(handle);
    if (buf) (void)NativeAdrenoMaintainOwnedPages(buf, kBytes, NativeAdrenoCleanInvalidate);
    (void)mem.release();
    if (buf) IOFreeAligned(buf, kBytes);
    native_adreno_io_coherent = pass ? 1 : 0;
    printf("COACHZ gpu9: coherence H d%u/64 a%u r%u/%u w%u/%u s%x k%u at 0x%llx -> %s\n", k[1], attempts, k[2], k[3], k[4], k[5], k[6], k[0],
        static_cast<unsigned long long>(iova), pass ? "on" : "off");
}

// The accelerator the original _MTLRegisterDevices finds: its class derives from the original IOAcceleratorES, which exists once IOGPUFamily loaded.
bool publishAccelerator() {
    const kern_return_t kext = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    printf("COACHZ gpu9: IOGPUFamily load 0x%x, IOAcceleratorES %p size %u\n", kext, parent, parent ? parent->getClassSize() : 0);
    if (!parent || parent->getClassSize() != 136 || uint64_t(VM_KERNEL_UNSLIDE(parent)) != kOriginalAcceleratorMetaclass) {
        printf("COACHZ gpu9: the accelerator base differs from the audited one (unslid 0x%llx)\n", static_cast<unsigned long long>(VM_KERNEL_UNSLIDE(parent)));
        return false;
    }
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return false;
    native_adreno_accelerator_static_init();
    const OSReturn loaded = OSMetaClass::postModLoad(handle);
    if (loaded || native_adreno_accelerator_instance_size() != 136) {
        printf("COACHZ gpu9: accelerator class registration 0x%x\n", loaded);
        return false;
    }
    IOService *accelerator = native_adreno_accelerator_allocate();
    if (!accelerator || !accelerator->init()) {
        if (accelerator) accelerator->release();
        return false;
    }
    // The two keys the discovery reads, no Apple GPU identity; and who may open it in place of the sandbox verdict: every process.
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    accelerator->setProperty("NativeSandboxGrantProcess", "*");
    if (!accelerator->attach(IOService::getPlatform())) {
        accelerator->release();
        return false;
    }
    accelerator->registerService();
    return true;
}
}  // namespace

// The apps SMMU (MMU-500 at 0x15000000) as found, read only: the first step toward direct scanout (UniXNU porting/display: the display
// pipe reads the compositor's surface through a window of its own). k85 read sCR0 0x200001, CLIENTPD set, so every master bypasses it
// today, USB included; a window for the display's stream (0x800 mask 0x2 in sc7180.dtsi) needs to know which stream matches and context
// banks the firmware left behind. Prints the ID registers, every valid SMR with its S2CR, and how many CBARs are in use.
void appsSmmuProbe() {
    const vm_offset_t base = ml_io_map(0x15000000, 0x20000);
    if (!base) {
        printf("COACHZ smmu apps: no window\n");
        return;
    }
    const volatile uint32_t *gr0 = reinterpret_cast<const volatile uint32_t *>(base);
    const uint32_t idr0 = gr0[0x20 / 4], idr1 = gr0[0x24 / 4], idr2 = gr0[0x28 / 4];
    const unsigned smrs = idr0 & 0xff, cbs = idr1 & 0xff, page = (idr1 >> 31) ? 0x10000 : 0x1000;
    printf("COACHZ smmu apps: sCR0 0x%x sACR 0x%x IDR0 0x%x IDR1 0x%x IDR2 0x%x IDR7 0x%x, %u SMRs %u CBs page 0x%x\n", gr0[0], gr0[0x10 / 4],
        idr0, idr1, idr2, gr0[0x3c / 4], smrs, cbs, page);
    unsigned valid = 0, nondefault = 0;
    for (unsigned i = 0; i < smrs && i < 128; ++i) {
        const uint32_t smr = gr0[(0x800 + 4 * i) / 4], s2cr = gr0[(0xc00 + 4 * i) / 4];
        if (s2cr != 0x20000) ++nondefault;
        if (!(smr >> 31)) continue;
        ++valid;
        printf("COACHZ smmu apps: SMR %u id 0x%x mask 0x%x -> S2CR 0x%x (type %u cb %u)\n", i, smr & 0xffff, (smr >> 16) & 0x7fff, s2cr,
            (s2cr >> 16) & 3, s2cr & 0xff);
    }
    unsigned cbars = 0;
    if (page == 0x1000) {
        const volatile uint32_t *gr1 = reinterpret_cast<const volatile uint32_t *>(base + page);
        for (unsigned i = 0; i < cbs && i < 128; ++i) cbars += gr1[i] != 0;
    }
    printf("COACHZ smmu apps: %u valid SMRs, %u S2CRs not at reset, %u of %u CBARs set%s\n", valid, nondefault, cbars, cbs,
        page == 0x1000 ? "" : " (64 KiB pages: CBARs not read)");
}

// ---- Direct scanout (/chosen coachz-dpu-scanout = 1) ----
// The display pipe fetches the render server's frame where the GPU rendered it and its scaler fits it to the panel, instead of the
// presenter's scaled GPU copy into the flip buffers (k221: presentation about 10.5 % of GPU time). Mainline does the same for a plane
// (dpu_plane.c over dpu_hw_sspp.c; the pipe itself in bsd/dev/coachz_bridge.c, coachz_scanout_direct). The plug-in names the frame by its
// BO handle and Turnip's layout of it (tu_darwin_image_scanout); the framework holds the BO's pages (native_adreno_scanout_hold); the pipe
// reads them through a context of the apps SMMU of its own, as arm-smmu.c attaches mdss_mdp (sc7180.dtsi: iommus = <&apps_smmu 0x800 0x2>)
// and as CoachzVenus.cpp attaches the codec: the BOs lie above 4 GiB and the pipe's addresses have 32 bits. Until the stream is routed
// the pipe scans physical addresses, so the firmware framebuffer and the flip buffers are mapped at IOVA = PA for good, before the SMR is
// written: the buffer flips and the panic path's return work unchanged. A frame stays held until a later flip has latched.
namespace {
// NativeGOPWire.h NGPDirect, the record COMPLETE_SWAP carries for a direct frame.
struct DirectRequest {
    uint64_t sequence;
    uint32_t status, reserved;
    uint64_t handle;
    uint32_t color_offset, meta_offset, meta_bytes, color_pitch, meta_pitch, width, height, ubwc, order, pad;
};
static_assert(sizeof(DirectRequest) == 64, "NGPDirect");
}  // namespace
extern "C" {
// bsd/dev/coachz_bridge.c, which declares the same record.
struct coachz_scanout_frame {uint32_t color, meta, color_pitch, meta_pitch, width, height, ubwc, order, tag;};
int coachz_scanout_direct(const struct coachz_scanout_frame *frame);
uint32_t coachz_scanout_busy(void);
void coachz_scanout_flip_wait(void);
void coachz_scanout_home(void);
uint64_t coachz_scanout_display_window(uint64_t *bytes);
IOReturn native_adreno_scanout_hold(task_t task, uint64_t handle, uint64_t offset, uint64_t length, NativeAdrenoPhysicalSpan *spans, size_t capacity,
    size_t *count, uint64_t *bytes, void **connection);
void native_adreno_scanout_release(void *connection, uint64_t handle);
extern void (*native_adreno_scanout_closing)(void *connection);
vm_offset_t ml_io_map(vm_offset_t physical, vm_size_t size);
void dcache_incoherent_io_flush64(addr64_t pa, unsigned int count, unsigned int remaining, unsigned int *res);
// [0] direct frames shown, [1] slot mappings made, [2] reused, [3] refused (any reason), [4] the last refusal's code, [5] SMMU attach
// state (0 not tried, 1 up, 2 failed), [6] releases, [7] closing connections let go, [8] mapping time total (us), [9] the longest (us),
// [10] spans of the last mapping, [11] latch time total (us), [12] the longest latch (us), [13] bank faults seen (FSR nonzero).
volatile uint64_t coachz_gpu_direct[14];
}
namespace {
constexpr uint64_t kAppsSmmu = 0x15000000, kAppsSmmuBytes = 0x100000;
constexpr uint32_t kMdpStream = 0x800, kMdpStreamMask = 0x2;
constexpr unsigned kDirectSlots = 4, kDirectAsid = 3, kDirectPoll = 1000000;
constexpr uint64_t kDirectSlotIova = 0x10000000, kDirectSlotBytes = 32ull << 20, kDirectArenaBytes = 0xfff00000;
constexpr size_t kDirectSpans = 8192, kDirectTablePages = 192, kTablePage = 4096;
unsigned gDirectEnabled;
IOLock *gDirectLock;
// The SMMU as three mapped pages, as CoachzVenus.cpp has it: GR0, GR1 and the bank's own page.
struct DirectWindow {volatile uint8_t *gr0, *gr1, *cb; uint64_t cb_offset;};
DirectWindow gDirectSmmu;
volatile uint8_t *directAt(void *c, size_t at, size_t width) {
    const DirectWindow *w = static_cast<DirectWindow *>(c);
    if (at & (width - 1)) return nullptr;
    if (at + width <= 0x1000) return w->gr0 ? w->gr0 + at : nullptr;
    if (at >= 0x1000 && at + width <= 0x2000) return w->gr1 ? w->gr1 + (at - 0x1000) : nullptr;
    if (w->cb && at >= w->cb_offset && at + width <= w->cb_offset + 0x1000) return w->cb + (at - w->cb_offset);
    return nullptr;
}
int directRead32(void *c, size_t at, uint32_t *v) {volatile uint8_t *p = directAt(c, at, 4); if (!p) return 1; *v = *reinterpret_cast<volatile uint32_t *>(p); return 0;}
int directRead64(void *c, size_t at, uint64_t *v) {volatile uint8_t *p = directAt(c, at, 8); if (!p) return 1; *v = *reinterpret_cast<volatile uint64_t *>(p); return 0;}
int directWrite32(void *c, size_t at, uint32_t v) {volatile uint8_t *p = directAt(c, at, 4); if (!p) return 1; *reinterpret_cast<volatile uint32_t *>(p) = v; return 0;}
int directWrite64(void *c, size_t at, uint64_t v) {volatile uint8_t *p = directAt(c, at, 8); if (!p) return 1; *reinterpret_cast<volatile uint64_t *>(p) = v; return 0;}
int directBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
const ArmSmmu4KIo gDirectIo = {&gDirectSmmu, kAppsSmmuBytes, directRead32, directRead64, directWrite32, directWrite64, directBarrier};
uint32_t directGr0(size_t at) {uint32_t v = 0; directRead32(&gDirectSmmu, at, &v); return v;}
void directGr0Write(size_t at, uint32_t v) {directWrite32(&gDirectSmmu, at, v); directBarrier(nullptr);}
// The table pool: contiguous cacheable pages, cleaned to memory for the SMMU's walks (coherent_walk 0).
struct DirectPool {IOBufferMemoryDescriptor *md; uint8_t *va; uint64_t pa; size_t used;};
DirectPool gDirectPool;
void directClean(uint64_t physical, uint64_t bytes) {unsigned int result = 0; dcache_incoherent_io_flush64(physical, static_cast<unsigned>(bytes), 0, &result);}
int directAllocate(void *, uint64_t *physical, volatile uint64_t **mapping) {
    if (gDirectPool.used >= kDirectTablePages) return 1;
    uint8_t *page = gDirectPool.va + gDirectPool.used * kTablePage;
    bzero(page, kTablePage);
    directClean(gDirectPool.pa + gDirectPool.used * kTablePage, kTablePage);
    *physical = gDirectPool.pa + gDirectPool.used * kTablePage;
    *mapping = reinterpret_cast<volatile uint64_t *>(page);
    ++gDirectPool.used;
    return 0;
}
volatile uint64_t *directFind(void *, uint64_t physical) {
    if (physical < gDirectPool.pa || physical >= gDirectPool.pa + gDirectPool.used * kTablePage || (physical & (kTablePage - 1))) return nullptr;
    return reinterpret_cast<volatile uint64_t *>(gDirectPool.va + (physical - gDirectPool.pa));
}
int directOverlaps(void *, uint64_t physical, uint64_t bytes) {
    return physical < gDirectPool.pa + kDirectTablePages * kTablePage && gDirectPool.pa < physical + bytes;
}
int directPublish(void *, volatile uint64_t *page, size_t bytes) {
    const uint8_t *at = reinterpret_cast<const uint8_t *>(const_cast<uint64_t *>(page));
    if (at < gDirectPool.va || at + bytes > gDirectPool.va + kDirectTablePages * kTablePage) return 1;
    directClean(gDirectPool.pa + static_cast<uint64_t>(at - gDirectPool.va), bytes);
    directBarrier(nullptr);
    return 0;
}
const ArmSmmu4KTablePages gDirectPages = {nullptr, directAllocate, directFind, directOverlaps};
const ArmSmmu4KLiveVisibility gDirectVisibility = {nullptr, directPublish};
ArmSmmu4KLiveArena gDirectArena;
ArmSmmu4KLiveState gDirectState;
ArmSmmu4KContextConfig gDirectContext;
unsigned gDirectBank;
uint64_t gDirectCbOffset;
// The mapped ranges, sorted by IOVA as the kit wants them: each slot's at its own window, then the two fixed windows (flip buffers,
// firmware framebuffer) at IOVA = PA, which lie above every slot.
struct DirectSlot {
    void *connection;              // non-null while held: the pipe may fetch it
    task_t task;
    uint64_t handle, start, bytes;  // the object and the window of it (an image inside a larger allocation) the slot maps
    ArmSmmu4KRange *ranges;        // what the slot's window maps now (kept after a release, for reuse)
    size_t count;
    uint64_t used;
};
DirectSlot gSlots[kDirectSlots];
ArmSmmu4KRange gFixed[2];
size_t gFixedCount;
ArmSmmu4KRange *gAllRanges;        // the composed list handed to the kit
NativeAdrenoPhysicalSpan *gHeldSpans;
uint64_t gDirectClock;

size_t directCompose() {
    size_t n = 0;
    for (const DirectSlot &slot : gSlots) for (size_t i = 0; i < slot.count; ++i) gAllRanges[n++] = slot.ranges[i];
    for (size_t i = 0; i < gFixedCount; ++i) gAllRanges[n++] = gFixed[i];
    return n;
}
bool directChange(const ArmSmmu4KRange *changes, size_t count, unsigned map) {
    const ArmSmmu4KLiveRegion lower = {&gDirectArena, &gDirectState, gAllRanges, directCompose()};
    ArmSmmu4KLiveResult done = {};
    const ArmSmmu4KStatus status = ArmSmmu4KLiveUpdateContextBatch(&gDirectIo, gDirectBank, &gDirectContext, &lower, nullptr, 0, &gDirectVisibility,
        changes, count, map, kDirectPoll, &done);
    if (status != ArmSmmu4KSuccess) {
        printf("COACHZ scanout: SMMU %s of %zu ranges failed: %d (complete %u), %zu table pages\n", map ? "map" : "unmap", count, status,
            done.complete, gDirectPool.used);
        return false;
    }
    return true;
}
// Attaches the display's streams to a bank of their own (arm_smmu_attach_dev, then arm_smmu_write_sme: S2CR before SMR). Needs the apps
// SMMU enabled (the USB host does it, CoachzUsbHost.cpp; with CLIENTPD every master bypasses it and the BOs above 4 GiB are out of reach).
bool directAttach() {
    gDirectSmmu.gr0 = reinterpret_cast<volatile uint8_t *>(ml_io_map(kAppsSmmu, 0x1000));
    gDirectSmmu.gr1 = reinterpret_cast<volatile uint8_t *>(ml_io_map(kAppsSmmu + 0x1000, 0x1000));
    if (!gDirectSmmu.gr0 || !gDirectSmmu.gr1) {printf("COACHZ scanout: no SMMU window\n"); return false;}
    const uint32_t scr0 = directGr0(0x0), idr0 = directGr0(0x20), idr1 = directGr0(0x24), idr2 = directGr0(0x28);
    const unsigned smrs = idr0 & 0xff, banks = idr1 & 0xff, pagesize = (idr1 >> 31) ? 0x10000 : 0x1000;
    const unsigned numpage = 1u << (((idr1 >> 28) & 7) + 1);
    if (scr0 & 1u) {printf("COACHZ scanout: the apps SMMU is not enabled (sCR0 0x%x)\n", scr0); return false;}
    if (pagesize != 0x1000 || !smrs || banks <= 2) {printf("COACHZ scanout: unexpected SMMU layout\n"); return false;}
    // The display's streams must still bypass (no SMR of theirs), and a stream match must be free.
    unsigned freeSmr = smrs;
    for (unsigned i = 0; i < smrs; ++i) {
        const uint32_t smr = directGr0(0x800 + 4 * i);
        const uint32_t id = smr & 0xffff, mask = (smr >> 16) & 0x7fff;
        if ((smr >> 31) && !((kMdpStream ^ id) & ~(mask | kMdpStreamMask))) {printf("COACHZ scanout: SMR %u 0x%x already matches the display\n", i, smr); return false;}
        if (!(smr >> 31) && freeSmr == smrs) freeSmr = i;
    }
    if (freeSmr == smrs) {printf("COACHZ scanout: no free stream match\n"); return false;}
    // Bank 2: the USB host has bank 0 and the codec bank 1 (CoachzVenus.cpp). It must not be routed to or enabled.
    gDirectBank = 2;
    bool routed = false;
    for (unsigned i = 0; i < smrs && !routed; ++i) routed = (directGr0(0x800 + 4 * i) >> 31) && (directGr0(0xc00 + 4 * i) & 0xff) == gDirectBank;
    gDirectCbOffset = static_cast<uint64_t>(numpage) * pagesize + static_cast<uint64_t>(gDirectBank) * pagesize;
    if (gDirectCbOffset + 0x1000 > kAppsSmmuBytes) {printf("COACHZ scanout: bank page outside the SMMU\n"); return false;}
    gDirectSmmu.cb = reinterpret_cast<volatile uint8_t *>(ml_io_map(kAppsSmmu + gDirectCbOffset, 0x1000));
    gDirectSmmu.cb_offset = gDirectCbOffset;
    uint32_t sctlr = 0;
    if (!gDirectSmmu.cb || directRead32(&gDirectSmmu, gDirectCbOffset, &sctlr) || routed || (sctlr & 1)) {
        printf("COACHZ scanout: context bank %u not free (routed %d SCTLR 0x%x)\n", gDirectBank, routed, sctlr);
        return false;
    }
    uint64_t oneBuffer = 0;
    const uint64_t home = coachz_scanout_display_window(&oneBuffer);
    if (!home || !oneBuffer) {printf("COACHZ scanout: the flip buffers are not prepared\n"); return false;}
    gDirectPool.md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), kDirectTablePages * kTablePage,
        0x0000000ffffff000ULL);
    if (!gDirectPool.md || gDirectPool.md->prepare() != kIOReturnSuccess) {printf("COACHZ scanout: no table pool\n"); return false;}
    gDirectPool.va = static_cast<uint8_t *>(gDirectPool.md->getBytesNoCopy());
    gDirectPool.pa = gDirectPool.md->getPhysicalAddress();
    gAllRanges = static_cast<ArmSmmu4KRange *>(IOMallocData((kDirectSlots * kDirectSpans + 2) * sizeof(ArmSmmu4KRange)));
    gHeldSpans = static_cast<NativeAdrenoPhysicalSpan *>(IOMallocData(kDirectSpans * sizeof(NativeAdrenoPhysicalSpan)));
    for (DirectSlot &slot : gSlots) slot.ranges = static_cast<ArmSmmu4KRange *>(IOMallocData(kDirectSpans * sizeof(ArmSmmu4KRange)));
    for (const DirectSlot &slot : gSlots) if (!slot.ranges) return false;
    if (!gAllRanges || !gHeldSpans) return false;
    uint64_t root = 0;
    volatile uint64_t *rootMapping = nullptr;
    if (directAllocate(nullptr, &root, &rootMapping)) return false;
    gDirectArena.config = {32, 36, root, static_cast<size_t>(kDirectArenaBytes / kTablePage)};
    gDirectArena.iova = 0;
    gDirectArena.bytes = kDirectArenaBytes;
    gDirectArena.upper_region = 0;
    gDirectArena.pages = &gDirectPages;
    ArmSmmu4KStatus status = ArmSmmu4KLivePrepare(&gDirectArena, &gDirectState);
    if (status != ArmSmmu4KSuccess) {printf("COACHZ scanout: arena not prepared: %d\n", status); return false;}
    directPublish(nullptr, rootMapping, kTablePage);
    gDirectContext.lower = &gDirectArena.config;
    gDirectContext.lower_tables = &gDirectState.tables;
    gDirectContext.upper = nullptr;
    gDirectContext.upper_tables = nullptr;
    gDirectContext.id0 = idr0;
    gDirectContext.id2 = idr2;
    gDirectContext.asid = kDirectAsid;
    gDirectContext.coherent_walk = 0;
    gDirectContext.stall_on_fault = 0;
    gDirectContext.continue_after_fault = 0;
    gDirectContext.ttbr0_switched = 0;
    ArmSmmu4KProgramResult programmed = {};
    status = ArmSmmu4KProgramDisabledContext(&gDirectIo, gDirectBank, &gDirectContext, kDirectPoll, &programmed);
    if (status != ArmSmmu4KSuccess) {printf("COACHZ scanout: context bank %u not programmed: %d (phase %u)\n", gDirectBank, status, programmed.phase); return false;}
    // What the pipe scans before any direct frame, at IOVA = PA: the flip buffers (low memory) and the firmware framebuffer. Read only,
    // normal non-cacheable, outer shareable, as io-pgtable-arm maps for a master that does not snoop.
    const uint64_t flipBytes = (2 * oneBuffer + kTablePage - 1) & ~(kTablePage - 1), homeBytes = (oneBuffer + kTablePage - 1) & ~(kTablePage - 1);
    const uint64_t flipBase = 0x82400000ull;
    if (flipBase + flipBytes > home && home + homeBytes > flipBase) {printf("COACHZ scanout: fixed windows overlap\n"); return false;}
    const ArmSmmu4KRange low = {flipBase, flipBase, flipBytes, ArmSmmu4KRead, 0, 2}, high = {home, home, homeBytes, ArmSmmu4KRead, 0, 2};
    gFixed[0] = flipBase < home ? low : high;
    gFixed[1] = flipBase < home ? high : low;
    if (gFixed[0].iova < kDirectSlotIova + kDirectSlots * kDirectSlotBytes || gFixed[1].iova + gFixed[1].bytes > kDirectArenaBytes) {
        printf("COACHZ scanout: fixed windows at 0x%llx and 0x%llx outside the arena's room\n", gFixed[0].iova, gFixed[1].iova);
        return false;
    }
    if (!directChange(gFixed, 2, 1)) return false;
    gFixedCount = 2;
    directGr0Write(0xc00 + 4 * freeSmr, gDirectBank);
    directGr0Write(0x800 + 4 * freeSmr, (1u << 31) | kMdpStreamMask << 16 | kMdpStream);
    directGr0Write(0x68, 0);   // TLBIALLNSNH
    directGr0Write(0x70, 0);   // TLBGSYNC
    unsigned polls = 0;
    while ((directGr0(0x74) & 1) && ++polls < kDirectPoll) {}
    printf("COACHZ scanout: apps SMMU bank %u, SMR %u 0x%x, windows 0x%llx+0x%llx and 0x%llx+0x%llx at IOVA = PA, %zu table pages\n", gDirectBank,
        freeSmr, directGr0(0x800 + 4 * freeSmr), gFixed[0].iova, gFixed[0].bytes, gFixed[1].iova, gFixed[1].bytes, gDirectPool.used);
    return true;
}
void directRelease(DirectSlot &slot) {
    if (!slot.connection) return;
    native_adreno_scanout_release(slot.connection, slot.handle);
    slot.connection = nullptr;
    coachz_gpu_direct[6] = coachz_gpu_direct[6] + 1;
}
// Lets go of every held frame the pipe no longer fetches (coachz_scanout_busy: bit n for slot n - 1). Under gDirectLock.
void directReleaseIdle() {
    const uint32_t busy = coachz_scanout_busy();
    for (unsigned k = 0; k < kDirectSlots; ++k) if (!(busy & (1u << (k + 1)))) directRelease(gSlots[k]);
}
uint64_t directNowUs() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000;}
// The slot for this window of an object: the one already holding it, or a free one (not fetched, not held), the least recently used
// unless another still maps the same pages; its old mapping is replaced. The object is held on return. The slot maps the window at its
// own start: IOVA = slot + (offset - start).
bool directSameMapping(const DirectSlot &slot, uint64_t base, task_t task, uint64_t handle, uint64_t start, uint64_t bytes, size_t count) {
    if (slot.task != task || slot.handle != handle || slot.start != start || slot.bytes != bytes || slot.count != count) return false;
    for (size_t i = 0; i < count; ++i)
        if (slot.ranges[i].physical != gHeldSpans[i].physical || slot.ranges[i].bytes != gHeldSpans[i].bytes ||
            slot.ranges[i].iova != base + (gHeldSpans[i].offset - start))
            return false;
    return true;
}
int directSlotFor(task_t task, uint64_t handle, uint64_t start, uint64_t bytes) {
    ++gDirectClock;
    for (unsigned k = 0; k < kDirectSlots; ++k) {
        DirectSlot &slot = gSlots[k];
        if (slot.connection && slot.task == task && slot.handle == handle && slot.start == start && slot.bytes == bytes) {slot.used = gDirectClock; return int(k);}
    }
    size_t count = 0;
    uint64_t objectBytes = 0;
    void *connection = nullptr;
    const IOReturn held = native_adreno_scanout_hold(task, handle, start, bytes, gHeldSpans, kDirectSpans, &count, &objectBytes, &connection);
    if (held) {coachz_gpu_direct[4] = uint32_t(held); return -1;}
    const uint32_t busy = coachz_scanout_busy();
    int pick = -1;
    for (unsigned k = 0; k < kDirectSlots; ++k) {
        const DirectSlot &slot = gSlots[k];
        if (slot.connection || (busy & (1u << (k + 1)))) continue;
        if (directSameMapping(slot, kDirectSlotIova + k * kDirectSlotBytes, task, handle, start, bytes, count)) {pick = int(k); break;}
        if (pick < 0 || slot.used < gSlots[pick].used) pick = int(k);
    }
    if (pick < 0) {native_adreno_scanout_release(connection, handle); coachz_gpu_direct[4] = 0x11; return -1;}
    DirectSlot &slot = gSlots[pick];
    const uint64_t base = kDirectSlotIova + uint64_t(pick) * kDirectSlotBytes;
    if (directSameMapping(slot, base, task, handle, start, bytes, count)) {
        coachz_gpu_direct[2] = coachz_gpu_direct[2] + 1;
    } else {
        const uint64_t began = directNowUs();
        if (slot.count) {
            // The kit checks the unmap against the composed list, which still holds the slot's ranges.
            if (!directChange(slot.ranges, slot.count, 0)) {native_adreno_scanout_release(connection, handle); coachz_gpu_direct[4] = 0x12; return -1;}
            slot.count = 0;
        }
        slot.task = nullptr; slot.handle = 0; slot.start = 0; slot.bytes = 0;
        bool ordered = true;
        for (size_t i = 0; i < count; ++i) {
            const NativeAdrenoPhysicalSpan &span = gHeldSpans[i];
            ordered = ordered && span.bytes && !(span.bytes & (kTablePage - 1)) && !(span.physical & (kTablePage - 1)) &&
                      !(span.offset & (kTablePage - 1)) && span.offset >= start && span.offset + span.bytes <= start + bytes &&
                      (!i || span.offset >= gHeldSpans[i - 1].offset + gHeldSpans[i - 1].bytes);
            slot.ranges[i] = {base + (span.offset - start), span.physical, span.bytes, ArmSmmu4KRead, 0, 2};
        }
        if (!ordered || !directChange(slot.ranges, count, 1)) {native_adreno_scanout_release(connection, handle); coachz_gpu_direct[4] = 0x13; return -1;}
        slot.count = count;
        slot.task = task; slot.handle = handle; slot.start = start; slot.bytes = bytes;
        const uint64_t us = directNowUs() - began;
        coachz_gpu_direct[1] = coachz_gpu_direct[1] + 1;
        coachz_gpu_direct[8] = coachz_gpu_direct[8] + us;
        if (us > coachz_gpu_direct[9]) coachz_gpu_direct[9] = us;
        coachz_gpu_direct[10] = count;
    }
    slot.connection = connection;
    slot.used = gDirectClock;
    return pick;
}
// A connection closing with a held frame: the pipe returns to the firmware framebuffer and, once that latched, every frame of the
// connection is let go (the teardown would find it held). Called by the framework without its lock.
void directClosing(void *connection) {
    if (!gDirectLock) return;
    IOLockLock(gDirectLock);
    bool mine = false;
    for (const DirectSlot &slot : gSlots) mine = mine || slot.connection == connection;
    if (mine) {
        coachz_scanout_home();
        coachz_scanout_flip_wait();
        for (DirectSlot &slot : gSlots) if (slot.connection == connection) directRelease(slot);
        coachz_gpu_direct[7] = coachz_gpu_direct[7] + 1;
    }
    IOLockUnlock(gDirectLock);
}
}  // namespace

// The presenter's COMPLETE_SWAP with a direct frame (NativeGOPPresenter.cpp): shows it and returns once the pipe latched it, the earlier
// frames let go. Any refusal leaves the pipe as it was; the plug-in then completes the swap with its copy.
extern "C" IOReturn coachz_gpu_scanout_direct(task_t task, const void *request, size_t bytes) {
    if (!gDirectEnabled || !gDirectLock) return kIOReturnUnsupported;
    DirectRequest q = {};
    if (!task || !request || bytes != sizeof q) return kIOReturnBadArgument;
    memcpy(&q, request, sizeof q);
    const uint64_t rows = q.ubwc ? (uint64_t(q.height) + 15) & ~15ull : q.height;   // a UBWC plane holds whole 16-row tiles
    const uint64_t flagRows = (uint64_t(q.height) + 3) / 4;                          // a flag byte covers 16x4 pixels
    const bool valid = q.width && q.height && q.width <= 4096 && q.height <= 4096 && (q.order == 1 || q.order == 2) && q.ubwc <= 1 &&
        q.color_pitch >= q.width * 4 && !(q.color_offset & 0xfff) &&
        (!q.ubwc || (q.order == 1 && q.meta_pitch >= (q.width + 15) / 16 && q.meta_bytes >= q.meta_pitch * flagRows && !(q.meta_offset & 0xfff)));
    if (!valid) {coachz_gpu_direct[3] = coachz_gpu_direct[3] + 1; coachz_gpu_direct[4] = 0x20; return kIOReturnBadArgument;}
    // The window of the object the pipe reads: both planes, in whole pages.
    const uint64_t colorEnd = q.ubwc ? q.color_offset + q.color_pitch * rows : q.color_offset + q.color_pitch * (rows - 1) + uint64_t(q.width) * 4;
    const uint64_t metaEnd = uint64_t(q.meta_offset) + q.meta_bytes;
    const uint64_t start = (q.ubwc && q.meta_offset < q.color_offset ? q.meta_offset : q.color_offset) & ~0xfffull;
    const uint64_t end = ((q.ubwc && metaEnd > colorEnd ? metaEnd : colorEnd) + 0xfff) & ~0xfffull;
    if (end - start > kDirectSlotBytes) {coachz_gpu_direct[3] = coachz_gpu_direct[3] + 1; coachz_gpu_direct[4] = 0x22; return kIOReturnNoSpace;}
    IOLockLock(gDirectLock);
    if (coachz_gpu_direct[5] == 0) coachz_gpu_direct[5] = directAttach() ? 1 : 2;
    if (coachz_gpu_direct[5] != 1) {
        IOLockUnlock(gDirectLock);
        coachz_gpu_direct[3] = coachz_gpu_direct[3] + 1;
        coachz_gpu_direct[4] = 0x21;
        return kIOReturnUnsupported;
    }
    const int k = directSlotFor(task, q.handle, start, end - start);
    IOReturn status = k < 0 ? kIOReturnNoResources : kIOReturnSuccess;
    if (!status) {
        const uint64_t window = kDirectSlotIova + uint64_t(k) * kDirectSlotBytes;   // maps the object from start on
        const coachz_scanout_frame frame = {uint32_t(window + (q.color_offset - start)), q.ubwc ? uint32_t(window + (q.meta_offset - start)) : 0u,
                                            q.color_pitch, q.ubwc ? q.meta_pitch : 0u, q.width, q.height, q.ubwc, q.order, uint32_t(k) + 1};
        const uint64_t began = directNowUs();
        const int shown = coachz_scanout_direct(&frame);
        if (shown) {
            coachz_gpu_direct[4] = 0x100 | uint32_t(shown);
            status = kIOReturnError;
        } else {
            coachz_scanout_flip_wait();
            const uint64_t us = directNowUs() - began;
            coachz_gpu_direct[0] = coachz_gpu_direct[0] + 1;
            coachz_gpu_direct[11] = coachz_gpu_direct[11] + us;
            if (us > coachz_gpu_direct[12]) coachz_gpu_direct[12] = us;
            // Only the fault bits count (arm-smmu.h ARM_SMMU_CB_FSR_FAULT: MULTI, SS, UUT, ASF, TLBLKF, TLBMCF, EF, PF, AFF, TF): FSR.FORMAT
            // [10:9] reads 0b10 on this bank at all times (k224b: FSR 0x400 with a stale FAR was counted as a fault).
            constexpr uint32_t kCbFsrFault = 0xc00001feu;
            uint32_t fsr = 0;
            if (!directRead32(&gDirectSmmu, gDirectCbOffset + 0x58, &fsr) && (fsr & kCbFsrFault)) {
                coachz_gpu_direct[13] = coachz_gpu_direct[13] + 1;
                uint64_t far = 0;
                directRead64(&gDirectSmmu, gDirectCbOffset + 0x60, &far);
                if (coachz_gpu_direct[13] <= 4) printf("COACHZ scanout: bank %u fault FSR 0x%x FAR 0x%llx\n", gDirectBank, fsr, far);
                directWrite32(&gDirectSmmu, gDirectCbOffset + 0x58, fsr & kCbFsrFault);
            }
        }
    }
    directReleaseIdle();
    IOLockUnlock(gDirectLock);
    if (status) coachz_gpu_direct[3] = coachz_gpu_direct[3] + 1;
    const uint64_t shown = coachz_gpu_direct[0];
    if (!status && (shown == 1 || shown % 600 == 0)) {
        char row[256];
        snprintf(row, sizeof row, "COACHZ scanout: %llu direct, %llu mapped (mean %llu us, max %llu, %llu spans), %llu reused, %llu refused (last 0x%llx), "
            "latch mean %llu max %llu us, %llu faults",
            shown, coachz_gpu_direct[1], coachz_gpu_direct[1] ? coachz_gpu_direct[8] / coachz_gpu_direct[1] : 0, coachz_gpu_direct[9],
            coachz_gpu_direct[10], coachz_gpu_direct[2], coachz_gpu_direct[3], coachz_gpu_direct[4], coachz_gpu_direct[11] / shown,
            coachz_gpu_direct[12], coachz_gpu_direct[13]);
        printf("%s\n", row);
    }
    return status;
}
// Before the presenter draws into a flip buffer (waitSwap, after the latch): frames the pipe no longer fetches are let go.
extern "C" void coachz_gpu_scanout_idle(void) {
    if (!gDirectEnabled || !gDirectLock || coachz_gpu_direct[5] != 1) return;
    IOLockLock(gDirectLock);
    directReleaseIdle();
    IOLockUnlock(gDirectLock);
}
// The plug-in went away (abandonSwap): back to the firmware framebuffer, then every frame let go once that latched.
extern "C" void coachz_gpu_scanout_abandon(void) {
    if (!gDirectEnabled || !gDirectLock || coachz_gpu_direct[5] != 1) {coachz_scanout_home(); return;}
    IOLockLock(gDirectLock);
    coachz_scanout_home();
    coachz_scanout_flip_wait();
    for (DirectSlot &slot : gSlots) directRelease(slot);
    IOLockUnlock(gDirectLock);
}
// Whether the plug-in may send direct frames (the presenter's mode reply).
extern "C" int coachz_gpu_scanout_enabled(void) {return gDirectEnabled != 0;}

// A 32-bit switch from /chosen (tools/xnu_screen.py writes it there: the boot command line is full).
static bool chosenWord(const char *name, uint32_t *out) {
    DTEntry chosen;
    const void *value = nullptr;
    unsigned bytes = 0;
    if (SecureDTLookupEntry(nullptr, "/chosen", &chosen) != kSuccess || SecureDTGetProperty(chosen, name, &value, &bytes) != kSuccess ||
        bytes != sizeof *out)
        return false;
    memcpy(out, value, sizeof *out);
    return true;
}

// Called once from coachz_gpu_start with the windows it mapped; the seq left the GMU up. Nonzero when the driver is serving.
// /chosen coachz-gpu-hang-test = N (or the boot-arg of that name): N seconds after the driver starts, one kernel submission that never ends, a CP_WAIT_REG_MEM on CP_SCRATCH_REG(7),
// which nothing else writes, for a value it never holds. The framework's hangcheck then restarts the GPU once through gpuRestart on a GPU
// that is otherwise sound: a test of the restart. NativeAdrenoSubmit::recover finds no job for that fence and replays the jobs after it.
unsigned gHangTest;
void hangTestThread(void *, wait_result_t) {
    IOSleep(gHangTest * 1000);
    native_adreno_publisher_lock(1);
    pmGet();
    IOReturn sent = kIOReturnNotReady;
    uint32_t fence = 0;
    if (!__atomic_load_n(&gPmAsleep, __ATOMIC_SEQ_CST)) {
        coreWrite(CpScratch7, 0);
        // CP_WAIT_REG_MEM: WRITE_EQ, POLL_REGISTER; the register; the value; every bit; 16 cycles between reads.
        uint32_t words[7] = {0, 3u, CpScratch7, 0, 0x600dfeedu, ~0u, 16u};
        sent = AdrenoPM4Type7(Pm4WaitRegMem, 6, &words[0]) == AdrenoPM4Success ? gRing.submit(words, 7, &fence) : kIOReturnError;
    }
    native_adreno_publisher_lock(0);
    printf("COACHZ gpu hang test: submitted 0x%x at fence %u, recover mode %u\n", sent, fence, gRecoverMode);
}

extern "C" int coachz_gpu_driver_start(volatile uint8_t *core, volatile uint8_t *gmu, volatile uint8_t *smmu, size_t smmuBytes, const uint32_t *sqe, uint32_t sqeBytes) {
    gCore = core;
    gGmu = gmu;
    gSmmu = smmu;
    if (!core || !smmu || !sqe || !startCp(smmu, smmuBytes, sqe, sqeBytes)) {
        printf("COACHZ gpu9: CP not started; rendering stays on the CPU\n");
        return 0;
    }
    appsSmmuProbe();
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
    IOReturn status = vm ? gSubmit.attach(vm, &gRing) : kIOReturnNotReady;
    const NativeAdrenoTimeIo timer{nullptr, timerRead};
    const IOReturn timed = gTimer.attach(&timer, NativeAdrenoTimeCp);
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    gRing.setTimeline(true);
    native_adreno_dram_read32 = dramRead32;
    native_adreno_cached_read32 = cachedRead32;
    native_adreno_line_audit = lineAudit;
    if (!status) coherenceProbe(vm);
    if (!status) gpuInterruptsOn();
    if (!status) status = native_adreno_publish(vm, &gSubmit, &gRing, &clock, timed ? nullptr : &gTimer, &gDeviceInfo, NativeAdrenoGpuContextInputBits(), gReport.output_bits);
    printf("COACHZ gpu9: published 0x%x, timer 0x%x\n", status, timed);
    if (status) return 0;
    const bool accelerator = publishAccelerator();
    printf("COACHZ gpu9: accelerator %d\n", accelerator);
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(reportThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    speedBinReport();
    cacheSetup();
    if (kernel_thread_start(rateThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    native_adreno_power_get = pmGet;
    native_adreno_set_restart(gpuRestart, nullptr);
    native_adreno_capture_hang = gpuHangSnapshot;
    printf("COACHZ gpu hang capture: armed, before reset and BO release; 2 snapshots, IB 64 KiB, ring 32 KiB, memptrs 4 KiB\n");
    if (kernel_thread_start(pmThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    PE_parse_boot_argn("coachz-gpu-recover", &gRecoverMode, sizeof(gRecoverMode));
    uint32_t hang = 0;
    if (chosenWord("coachz-gpu-hang-test", &hang) || PE_parse_boot_argn("coachz-gpu-hang-test", &hang, sizeof(hang))) gHangTest = hang;
    if (gHangTest && kernel_thread_start(hangTestThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    uint32_t direct = 0;
    if (chosenWord("coachz-dpu-scanout", &direct) && direct == 1 && (gDirectLock = IOLockAlloc())) {
        gDirectEnabled = 1;
        native_adreno_scanout_closing = directClosing;
    }
    printf("COACHZ scanout: direct %s\n", gDirectEnabled ? "on (/chosen coachz-dpu-scanout)" : "off");
    coachz_gpu_trace_start();
    return accelerator;
}

extern "C" void coachz_gpu_pm_arm(void) {
    if (!gPmArmed) printf("COACHZ gpu pm: armed by the power button\n");
    gPmArmed = 1;
}
