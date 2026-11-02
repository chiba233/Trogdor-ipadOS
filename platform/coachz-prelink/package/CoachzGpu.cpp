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
#include "NativeAdrenoTimer.h"
#include "AdrenoPM4.h"

extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *, const NativeAdrenoRingClock *,
    const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);
extern "C" volatile uint32_t coachz_gpu_level_cap;   // coachz_thermal.c: 7, or 6 while the temperature clamp holds
extern "C" void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
extern "C" IOService *native_adreno_accelerator_allocate(void);
extern "C" unsigned native_adreno_accelerator_instance_size(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" void native_adreno_metal_report(void);
extern "C" void native_adreno_publisher_lock(int locked);
extern "C" {
extern volatile uint32_t native_adreno_ring_log[4][4 + 112];
extern volatile uint32_t native_adreno_ring_log_count;
extern volatile uint32_t native_adreno_ib_log[8][4 + 208];
extern volatile uint32_t native_adreno_ib_log_count;
extern volatile uint64_t native_adreno_submit_ticks[64], native_adreno_submit_cost[3], native_adreno_wait_outer[5], native_gop_wait_stats[7];
}
extern "C" unsigned NativeAdrenoGpuContextInputBits();
// The framework's two weak board hooks. [1] of the first is the GPU's current level, which it notes per job. The second names a process still mapping a
// page when a BO goes back; only a diagnostic, and no BO's pages are handed back under a mapping now that views map the VM object.
extern "C" {volatile uint32_t native_gpu_dcvs[15];}
extern "C" unsigned int native_pmap_user_mappings(unsigned int, void **, uint64_t *, char *) { return 0; }
extern "C" void (*volatile native_adreno_power_get)(void);
extern "C" int coachz_gpu_pm_suspend(void);
extern "C" int coachz_gpu_pm_resume(void);
extern "C" void coachz_gpu_pm_arm(void);
// NativeAdrenoPublish.cpp's per-process table (first eight process names that opened the plug-in type) and its latest mount failure:
// [0] 1 no free slot, 2 the core's open, 3 the window scope; [1] IOReturn; [2] slot; [3] failures so far.
extern "C" struct NativeAdrenoClientRecord {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal;} native_adreno_client_log[8];
extern "C" volatile uint32_t native_adreno_mount_fail[4];
extern "C" volatile uint64_t native_gop_gpu_offers, native_gop_gpu_completions, native_gop_gpu_failures, native_gop_gpu_abandoned;
extern "C" void *memprobe_cpu_to_processor(int) __asm__("_cpu_to_processor");
extern "C" void *memprobe_thread_bind(void *) __asm__("_thread_bind");
extern "C" int memprobe_thread_block(void *) __asm__("_thread_block");
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
    CpProtectStatus = 0x0824, Pm4MeInit = 0x48, SmmuScr0 = 0, SmmuSgfsr = 0x48,
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
// every submission, so it wakes the fence waiters. There is no hang recovery yet: the errors are counted for the report, and masked if they storm.
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
    printf("COACHZ gpu path: kernel submit mean %llu max %llu us over %llu; fence waits %llu mean %llu max %llu us; gop waits %llu done %llu mean %llu max %llu us\n",
        native_adreno_submit_cost[0] ? native_adreno_submit_cost[1] * 10 / 192 / native_adreno_submit_cost[0] : 0, native_adreno_submit_cost[2] * 10 / 192, native_adreno_submit_cost[0],
        native_adreno_wait_outer[0], native_adreno_wait_outer[0] ? native_adreno_wait_outer[1] / native_adreno_wait_outer[0] : 0, native_adreno_wait_outer[3],
        native_gop_wait_stats[0], native_gop_wait_stats[1], native_gop_wait_stats[1] ? native_gop_wait_stats[6] / native_gop_wait_stats[1] / 1000 : 0, native_gop_wait_stats[5] / 1000);
    printf("COACHZ gpu stamps: %u submissions, queue mean %llu max %llu us, gpu mean %llu max %llu us, gpu <1 %u <2 %u <4 %u <8 %u <16 %u <32 %u more %u ms\n", n, queue / n, queueMax, gpu / n, gpuMax,
        bins[0], bins[1], bins[2], bins[3], bins[4], bins[5], bins[6]);
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
        printf("COACHZ gpu irq: %u interrupts, %u fences; waits slept %u, timed out %u; errors ahb %u atb %u cp hw %u (0x%x) hang %u uche %u; gmu %u (0x%x) hfi %u (0x%x)\n",
            gIrqAll, gFlushTs, gIdleBlocks, gIdleTimeouts, gIrqAhb, gIrqAtb, gIrqCpHw, gCpHwStatus, gIrqHang, gIrqOob, gGmuAoIrqs, gGmuAoStatus, gHfiIrqs, gHfiInfo);
        stampReport();
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
    for (const ProfileCounter &c : kProfile) if (coreRead(c.select) != c.countable) bad++;
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
    for (const ProfileCounter &c : kProfile) check(c.select, c.countable, ~0u);
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

// Runtime power as Linux's msm driver has it for A618 (no IFPC): 66 ms after the last submission the GMU slumbers, and the next submission
// brings it back through the whole start. Armed by the power button (coachz_gpu_pm_arm), so a failure can only follow that press.
volatile uint32_t gPmArmed;
volatile uint64_t gPmLastSubmit;
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
// Under the publisher's lock, before a submission: the firmware and hw_init again, then what startCp does after them.
void pmGet() {
    gPmLastSubmit = mach_absolute_time();
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
void pmThread(void *, wait_result_t) {
    for (;;) {
        IOSleep(20);
        if (!gPmArmed || gPmAsleep || mach_absolute_time() - gPmLastSubmit < 19200 * 66) continue;
        native_adreno_publisher_lock(1);
        if (!gRing.outstanding() && mach_absolute_time() - gPmLastSubmit >= 19200 * 66) pmSleep();
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

void cacheSetup() {
    const vm_offset_t llcc = ml_io_map(0x09200000, 0x31000), broadcast = ml_io_map(0x09600000, 0x22000), misc = ml_io_map(0x0509e000, 0x1000);
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

// Memory bandwidth and latency on an A55 and an A76, with loops the same as a Debian baseline's on this hardware (Debian: chase 132-136 ns,
// A76 read 11-13 GB/s; here without a vote 195-199 ns and 8 GB/s, after coachz_wifi.c's DDR vote 134-142 ns and 11-12 GB/s on hardware).
// Run 40 s after the driver starts, then again once the vote (coachz-ddr-vote=1 at start, 2 at 150 s) has been sent, labelled with its result.
extern "C" volatile uint32_t coachz_ddr_voted;
extern "C" void coachz_gpu_trace_start(void);   // bsd/dev/coachz_trace.c, coachz-gpu-trace=1
void memoryProbeRun(volatile uint64_t *b, const char *label) {
    enum : uint32_t {Bytes = 32u << 20, Line = 64, Passes = 8, Hops = 2000000};
    static const int cpus[2] = {0, 6};
    for (int c = 0; c < 2; ++c) {
        memprobe_thread_bind(memprobe_cpu_to_processor(cpus[c]));
        memprobe_thread_block(nullptr);
        for (int round = 0; round < 3; ++round) {
            uint64_t sum = 0;
            uint64_t start = ringNow(nullptr);
            for (uint32_t p = 0; p < Passes; ++p) for (uint32_t i = 0; i < Bytes / 8; i += 4) sum += b[i] + b[i + 1] + b[i + 2] + b[i + 3];
            const uint64_t read = ringNow(nullptr) - start;
            start = ringNow(nullptr);
            for (uint32_t p = 0; p < Passes; ++p) for (uint32_t i = 0; i < Bytes / 8; i += 4) {b[i] = p; b[i + 1] = p; b[i + 2] = p; b[i + 3] = p;}
            const uint64_t write = ringNow(nullptr) - start;
            const uint32_t lines = Bytes / Line, step = (40503u % lines) | 1u;
            volatile uint32_t *q = reinterpret_cast<volatile uint32_t *>(b);
            for (uint32_t i = 0; i < lines; ++i) q[i * Line / 4] = ((i + step) % lines) * Line / 4;
            start = ringNow(nullptr);
            uint32_t at = 0;
            for (uint32_t i = 0; i < Hops; ++i) at = q[at];
            const uint64_t chase = ringNow(nullptr) - start;
            printf("COACHZ mem%s: cpu %d read %llu MB/s write %llu MB/s chase %llu ns (%u %llu)\n", label, cpus[c],
                static_cast<unsigned long long>(uint64_t(Bytes) * Passes / (read ? read : 1)), static_cast<unsigned long long>(uint64_t(Bytes) * Passes / (write ? write : 1)),
                static_cast<unsigned long long>(chase * 1000 / Hops), at, static_cast<unsigned long long>(sum & 1));
        }
    }
    memprobe_thread_bind(nullptr);
    memprobe_thread_block(nullptr);
}

void memoryProbeThread(void *, wait_result_t) {
    IOSleep(40000);
    IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | kIOMemoryPhysicallyContiguous, 32u << 20, 4096);
    if (!memory) {
        printf("COACHZ mem: no buffer\n");
        return;
    }
    volatile uint64_t *b = static_cast<volatile uint64_t *>(memory->getBytesNoCopy());
    memoryProbeRun(b, "");
    unsigned vote = 0;
    if (PE_parse_boot_argn("coachz-ddr-vote", &vote, sizeof(vote)) && (vote == 1 || vote == 2)) {
        for (unsigned waited = 0; waited < 600 && !coachz_ddr_voted; ++waited) IOSleep(1000);
        IOSleep(2000);
        memoryProbeRun(b, coachz_ddr_voted == 1 ? " voted" : coachz_ddr_voted == 2 ? " vote-failed" : " no-vote");
    }
    memory->release();
}

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

// Called once from coachz_gpu_start with the windows it mapped; the seq left the GMU up. Nonzero when the driver is serving.
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
    if (kernel_thread_start(memoryProbeThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    cacheSetup();
    if (kernel_thread_start(rateThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    native_adreno_power_get = pmGet;
    if (kernel_thread_start(pmThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    coachz_gpu_trace_start();
    return accelerator;
}

extern "C" void coachz_gpu_pm_arm(void) {
    if (!gPmArmed) printf("COACHZ gpu pm: armed by the power button\n");
    gPmArmed = 1;
}
