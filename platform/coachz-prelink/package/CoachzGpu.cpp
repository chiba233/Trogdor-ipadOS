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
extern "C" volatile uint32_t native_adreno_pool_mib;
extern "C" volatile uint32_t native_adreno_wc_enabled;
extern "C" volatile uint32_t coachz_gpu_level_cap;   // coachz_thermal.c: 7, or 6 while the temperature clamp holds
extern "C" void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
extern "C" IOService *native_adreno_accelerator_allocate(void);
extern "C" unsigned native_adreno_accelerator_instance_size(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" void native_adreno_metal_report(void);
extern "C" void native_adreno_publisher_lock(int locked);
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
extern "C" volatile uint32_t native_adreno_deferred[4];   // NativeAdrenoMemory.cpp: BOs freed only once no process maps them
extern "C" volatile uint32_t native_adreno_system_bos[3];   // NativeAdrenoMemory.cpp: BOs of system memory, mapped by their VM object
extern "C" void native_adreno_deferred_poll(void);
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
// The client's addresses are the driver's window of the one 2 GiB space, as NativeAdrenoPublish expects.
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
    clock_sec_t sec = 0;
    clock_usec_t usec = 0;
    clock_get_system_microtime(&sec, &usec);
    return uint64_t(sec) * 1000000 + usec;
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
        if (status & IrqCpHw) {
            gIrqCpHw = gIrqCpHw + 1;
            gCpHwStatus = *reinterpret_cast<volatile uint32_t *>(gCore + size_t(CpInterruptStatus) * 4);
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

extern "C" {
extern volatile uint32_t native_adreno_stamp_seqno[64];
extern volatile uint64_t native_adreno_stamp_cpu[64][2];
}

// The last 64 framed submissions as the ring stamps them (NativeAdrenoRing.cpp, a6xx_submit's CP_ALWAYS_ON_COUNTER reads): queue is
// submission to the CP starting it, gpu the CP start to the GPU idle after its buffers, seen that idle to a fence poll noticing it. The
// GPU counter and mach_absolute_time both count 19.2 MHz; their offset is read here. Only submissions completed since the last line.
void stampReport() {
    static uint32_t reported;
    if (!gMaps[2]) return;
    const volatile uint32_t *page = reinterpret_cast<const volatile uint32_t *>(gMaps[2]->getVirtualAddress()) + 0x800 / 4;
    const uint64_t before = mach_absolute_time();
    uint32_t lo = coreRead(0x980), hi = coreRead(0x981);
    const uint32_t again = coreRead(0x980);
    if (again < lo) {lo = again; hi = coreRead(0x981);}
    const uint64_t after = mach_absolute_time();
    const int64_t offset = static_cast<int64_t>((before + after) / 2) - static_cast<int64_t>(static_cast<uint64_t>(hi) << 32 | lo);
    uint64_t queue = 0, gpu = 0, seen = 0, queueMax = 0, gpuMax = 0, seenMax = 0;
    unsigned n = 0, bins[7] = {};
    uint32_t newest = reported;
    for (unsigned slot = 0; slot < 64; ++slot) {
        const uint32_t seq = native_adreno_stamp_seqno[slot];
        const uint64_t submitted = native_adreno_stamp_cpu[slot][0], noticed = native_adreno_stamp_cpu[slot][1];
        if (!seq || seq <= reported || !noticed) continue;
        const uint64_t start = page[slot * 4] | static_cast<uint64_t>(page[slot * 4 + 1]) << 32;
        const uint64_t end = page[slot * 4 + 2] | static_cast<uint64_t>(page[slot * 4 + 3]) << 32;
        if (!start || end < start) continue;
        const int64_t startCpu = static_cast<int64_t>(start) + offset, endCpu = static_cast<int64_t>(end) + offset;
        const uint64_t q = startCpu > static_cast<int64_t>(submitted) ? (static_cast<uint64_t>(startCpu) - submitted) * 10 / 192 : 0;
        const uint64_t g = (end - start) * 10 / 192;
        const uint64_t d = static_cast<int64_t>(noticed) > endCpu ? (noticed - static_cast<uint64_t>(endCpu)) * 10 / 192 : 0;
        queue += q; gpu += g; seen += d; ++n;
        if (q > queueMax) queueMax = q;
        if (g > gpuMax) gpuMax = g;
        if (d > seenMax) seenMax = d;
        unsigned bin = 0;
        while (bin < 6 && g >= (1000ull << bin)) ++bin;
        ++bins[bin];
        if (seq > newest) newest = seq;
    }
    reported = newest;
    if (!n) return;
    printf("COACHZ gpu stamps: %u submissions, queue mean %llu max %llu us, gpu mean %llu max %llu us, seen mean %llu max %llu us; "
        "gpu <1 %u <2 %u <4 %u <8 %u <16 %u <32 %u more %u ms\n", n, queue / n, queueMax, gpu / n, gpuMax, seen / n, seenMax,
        bins[0], bins[1], bins[2], bins[3], bins[4], bins[5], bins[6]);
}

// Every ten seconds, what reached the driver: whether the plug-in opened it, what it reports, what the CP has run and what reached the screen.
void reportThread(void *, wait_result_t) {
    for (unsigned round = 1;; round++) {
        IOSleep(10000);
        native_adreno_metal_report();
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
        // Imported BOs the submit path left alone under IO coherence: [2] before submissions, [3] after completions.
        printf("COACHZ gpu9: coherent %u, maintenance skipped before submit %u after completion %u\n", native_adreno_io_coherent,
            native_adreno_coherent_skips[2], native_adreno_coherent_skips[3]);
        // UniXNU porting/gpu#frame-path, submission to GPU start: other processes' jobs ahead of backboardd's in the one ring.
        printf("COACHZ gpu9: system BOs %u (%u spans, %u refused); deferred BO frees %u, freed later %u, waiting %u, kept %u\n",
            native_adreno_system_bos[0], native_adreno_system_bos[1], native_adreno_system_bos[2], native_adreno_deferred[0],
            native_adreno_deferred[1], native_adreno_deferred[2], native_adreno_deferred[3]);
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
uint64_t gbifCounter(unsigned index) {
    uint32_t high, low;
    do {
        high = coreRead(GbifHigh + index);
        low = coreRead(GbifLow + index);
    } while (high != coreRead(GbifHigh + index));
    return uint64_t(high) << 32 | low;
}
void profileSetup() {
    for (const ProfileCounter &c : kProfile) coreWrite(c.select, c.countable);
    coreWrite(GbifClear, 0xf);
    coreWrite(GbifClear, 0);
    coreWrite(GbifSel, 34u | 35u << 8 | 46u << 16 | 47u << 24);
    coreWrite(GbifEnable, coreRead(GbifEnable) | 0xf);
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
    if (__atomic_load_n(&gPmAsleep, __ATOMIC_SEQ_CST)) return 0;   // no firmware to ask: the level is set again on resume
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
        const uint64_t cycles = coreCounter(0x400), always = coreCounter(0x980);
        IOSleep(500);
        const uint64_t dCycles = coreCounter(0x400) - cycles, dAlways = coreCounter(0x980) - always;
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
    __atomic_store_n(&gPmAsleep, 0u, __ATOMIC_SEQ_CST);
    coreWrite(CpSqeInstrBase, uint32_t(gBuffers[0].iova));
    coreWrite(CpSqeInstrBase + 1, uint32_t(gBuffers[0].iova >> 32));
    const IOReturn ring = gRing.restart();
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
    uint64_t cycles = coreCounter(0x400), busy = gmuBusy(), always = coreCounter(0x980), last[ProfileCount], lastGbif[4];
    unsigned governed = 0, level = 7, changes = 0, lower = 0;
    PE_parse_boot_argn("coachz-gpu-dcvs", &governed, sizeof(governed));
    profileSetup();
    for (unsigned i = 0; i < ProfileCount; i++) last[i] = coreCounter(kProfile[i].counter);
    for (unsigned i = 0; i < 4; i++) lastGbif[i] = gbifCounter(i);
    uint64_t windowBusy = busy, windowAlways = always, boostUntil = 0, ticks = 0;
    uint32_t touches = coachz_touch_reports;
    bool idle = true;
    for (unsigned seconds = 0;; seconds++) {
        for (unsigned tick = 0; tick < 10; tick++, ticks++) {
            IOSleep(100);
            if (!governed || seconds <= 20) continue;
            const uint64_t nowBusy = gmuBusy(), nowAlways = coreCounter(0x980);
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
        native_adreno_deferred_poll();
        const uint64_t newCycles = coreCounter(0x400), newBusy = gmuBusy(), newAlways = coreCounter(0x980);
        const uint64_t dCycles = newCycles - cycles, dBusy = newBusy - busy, dAlways = newAlways - always;
        uint64_t d[ProfileCount], g[4];
        for (unsigned i = 0; i < ProfileCount; i++) {
            const uint64_t now = coreCounter(kProfile[i].counter);
            d[i] = now - last[i];
            last[i] = now;
        }
        for (unsigned i = 0; i < 4; i++) {
            const uint64_t now = gbifCounter(i);
            g[i] = now - lastGbif[i];
            lastGbif[i] = now;
        }
        cycles = newCycles;
        busy = newBusy;
        always = newAlways;
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
    // 1536 MiB, UniXNU's default (sheng-gpu-pool): k89's pool peaked at 472 of 512 MiB, and a BO outside the pool is the kind a crashed
    // process's close freed under its mapping.
    unsigned pool = 1536;
    PE_parse_boot_argn("coachz-gpu-pool", &pool, sizeof pool);
    native_adreno_pool_mib = pool;
    native_adreno_wc_enabled = 1;
    *reinterpret_cast<volatile uint32_t *>(smmu + SmmuScr0) = *reinterpret_cast<volatile uint32_t *>(smmu + SmmuScr0) | SmmuKitGlobal;
    // The groups nothing uses fault, as Linux's arm_smmu_device_reset leaves them; their reset value would translate into bank 0.
    for (unsigned group = 0; group < SmmuGroups; ++group) {
        if (group != SmmuGmuGroup) *reinterpret_cast<volatile uint32_t *>(smmu + SmmuS2cr + 4 * group) = SmmuS2crFault;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    // Bank 0 and stream 0, as Linux's Adreno SMMU; group 1 and bank 1 are the GMU's. The kit routes two streams: sc7180.dtsi names only stream 0 for
    // the GPU, so the second is an unused stream on a free group.
    const IOReturn context = NativeAdrenoGpuContextBringup(smmu, smmuBytes, 0, 2, 0, 0, 2, 1, nullptr, &gReport);
    printf("COACHZ gpu9: context status 0x%x step %u program %u/%u route %u/%u verified %u output bits %u spans %u pool status %u\n", context, gReport.step,
        gReport.program_status, gReport.program_enabled, gReport.route_status, gReport.route_phase, gReport.routes_verified, gReport.output_bits, gReport.spans,
        native_adreno_pool_mib);
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
    // The two keys the discovery reads, no Apple GPU identity; and who may open it in place of the sandbox verdict.
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    accelerator->setProperty("NativeSandboxGrantProcess", "backboardd,iconservicesagent,WidgetRenderer_Default,PosterBoard");
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
    if (!status) coherenceProbe(vm);
    if (!status) gpuInterruptsOn();
    if (!status) status = native_adreno_publish(vm, &gSubmit, &gRing, &clock, timed ? nullptr : &gTimer, &gDeviceInfo, 32, gReport.output_bits);
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
