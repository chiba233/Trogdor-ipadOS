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
#include "NativeAdrenoRing.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoTimer.h"
#include "AdrenoPM4.h"

extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *, const NativeAdrenoRingClock *,
    const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);
extern "C" volatile uint32_t native_adreno_pool_mib;
extern "C" volatile uint32_t native_adreno_wc_enabled;
extern "C" void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
extern "C" IOService *native_adreno_accelerator_allocate(void);
extern "C" unsigned native_adreno_accelerator_instance_size(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" void native_adreno_metal_report(void);
extern "C" volatile uint64_t native_gop_gpu_offers, native_gop_gpu_completions, native_gop_gpu_failures, native_gop_gpu_abandoned;
extern "C" void *memprobe_cpu_to_processor(int) __asm__("_cpu_to_processor");
extern "C" void *memprobe_thread_bind(void *) __asm__("_thread_bind");
extern "C" int memprobe_thread_block(void *) __asm__("_thread_block");
extern kmod_info_t g_kernel_kmod_info;

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

uint32_t coreRead(uint32_t reg) {
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(gCore + size_t(reg) * 4);
    __asm__ volatile("dsb sy" ::: "memory");
    return value;
}
void coreWrite(uint32_t reg, uint32_t value) {
    __asm__ volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(gCore + size_t(reg) * 4) = value;
    __asm__ volatile("dsb sy" ::: "memory");
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
int ringIdle(void *) {IODelay(20); return 0;}
int timerRead(void *, uint32_t reg, uint32_t *value) {*value = coreRead(reg); return 0;}

void cpState(const char *when) {
    printf("COACHZ gpu9: %s rbbm 0x%x int 0x%x hw fault 0x%x protect 0x%x bank0 fsr 0x%x gfsr 0x%x\n", when, coreRead(RbbmStatus), coreRead(RbbmInt0Status),
        coreRead(CpHwFault), coreRead(CpProtectStatus), *reinterpret_cast<volatile uint32_t *>(gSmmu + 0x8000 + 0x58),
        *reinterpret_cast<volatile uint32_t *>(gSmmu + SmmuSgfsr));
}

// Every ten seconds, what reached the driver: whether the plug-in opened it, what it reports, what the CP has run and what reached the screen.
void reportThread(void *, wait_result_t) {
    for (;;) {
        IOSleep(10000);
        native_adreno_metal_report();
        printf("COACHZ gpu9: ring submissions %u fence %u/%u, frames offered %llu shown %llu failed %llu abandoned %llu\n", gRing.submissions(), gRing.completedFence(),
            gRing.lastSubmitted(), static_cast<unsigned long long>(native_gop_gpu_offers), static_cast<unsigned long long>(native_gop_gpu_completions),
            static_cast<unsigned long long>(native_gop_gpu_failures), static_cast<unsigned long long>(native_gop_gpu_abandoned));
        cpState("now");
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

void rateThread(void *, wait_result_t) {
    uint64_t cycles = coreCounter(0x400), busy = gmuBusy(), always = coreCounter(0x980), last[ProfileCount], lastGbif[4];
    profileSetup();
    for (unsigned i = 0; i < ProfileCount; i++) last[i] = coreCounter(kProfile[i].counter);
    for (unsigned i = 0; i < 4; i++) lastGbif[i] = gbifCounter(i);
    for (;;) {
        IOSleep(1000);
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
        printf("COACHZ gpu rate: busy %llu%%, %llu MHz over the second, %llu MHz while busy\n", static_cast<unsigned long long>(dBusy * 100 / dAlways),
            static_cast<unsigned long long>(dCycles * 192 / dAlways / 10), static_cast<unsigned long long>(dBusy ? dCycles * 192 / dBusy / 10 : 0));
        printf("COACHZ gpu prof: sp %llu alu %llu st tp %llu uche %llu rb %llu ne %llu | tp %llu st %llu l1 %lluk miss %llu%% | uche %llu arb %llu lat %llu"
            " | rb %llu starve sp %llu ccu %llu c r %lluk w %lluk | gbif r %lluk %lluk w %lluk %lluk\n",
            pct(d[0]), pct(d[1]), pct(d[2]), pct(d[3]), pct(d[4]), pct(d[5]), pct(d[6]), pct(d[7]), static_cast<unsigned long long>(d[8] / 1000),
            static_cast<unsigned long long>(d[8] ? d[9] * 100 / d[8] : 0), pct(d[10]), pct(d[11]), static_cast<unsigned long long>(d[13] ? d[12] / d[13] : 0),
            pct(d[14]), pct(d[15]), pct(d[16]), static_cast<unsigned long long>(d[17] / 1000), static_cast<unsigned long long>(d[18] / 1000),
            static_cast<unsigned long long>(g[0] / 1000), static_cast<unsigned long long>(g[1] / 1000), static_cast<unsigned long long>(g[2] / 1000),
            static_cast<unsigned long long>(g[3] / 1000));
    }
}

// Whether the system cache serves the GPU, read only: mainline activates sc7180's GPU and GPUHTW LLCC slices (12 and 11; CPUSS 1, MDM 8) through
// the broadcast ACT_CTRL and STATUS registers (llcc-qcom.c, n * 4 KiB and 4 + n * 4 KiB; ATTR0/1 at 0x21000 + 8n), then writes the slice IDs to
// CX_MISC SYSTEM_CACHE_CNTL_1 (a6xx_llc_activate). Bank 0 holds the same registers.
extern "C" vm_offset_t ml_io_map(vm_offset_t physical, vm_size_t size);
void cacheProbe() {
    const vm_offset_t llcc = ml_io_map(0x09200000, 0x22000), misc = ml_io_map(0x0509e000, 0x1000);
    auto read = [](vm_offset_t at) {return *reinterpret_cast<volatile uint32_t *>(at);};
    if (!llcc || !misc) {
        printf("COACHZ llcc: map failed\n");
        return;
    }
    static const unsigned slices[] = {1, 8, 11, 12};
    for (unsigned id : slices)
        printf("COACHZ llcc: slice %u act 0x%x status 0x%x attr0 0x%x attr1 0x%x\n", id, read(llcc + id * 0x1000), read(llcc + 4 + id * 0x1000),
            read(llcc + 0x21000 + 8 * id), read(llcc + 0x21004 + 8 * id));
    printf("COACHZ llcc: gpu system cache cntl0 0x%x cntl1 0x%x\n", read(misc + 4), read(misc + 8));
}

// Memory bandwidth and latency on an A55 and an A76, with loops the same as a Debian baseline's on this hardware: no XNU memory path votes for bandwidth,
// so this says how much the DRAM gives here compared with Linux, which votes.
void memoryProbeThread(void *, wait_result_t) {
    enum : uint32_t {Bytes = 32u << 20, Line = 64, Passes = 8, Hops = 2000000};
    IOSleep(40000);
    IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | kIOMemoryPhysicallyContiguous, Bytes, 4096);
    if (!memory) {
        printf("COACHZ mem: no buffer\n");
        return;
    }
    volatile uint64_t *b = static_cast<volatile uint64_t *>(memory->getBytesNoCopy());
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
            printf("COACHZ mem: cpu %d read %llu MB/s write %llu MB/s chase %llu ns (%u %llu)\n", cpus[c],
                static_cast<unsigned long long>(uint64_t(Bytes) * Passes / (read ? read : 1)), static_cast<unsigned long long>(uint64_t(Bytes) * Passes / (write ? write : 1)),
                static_cast<unsigned long long>(chase * 1000 / Hops), at, static_cast<unsigned long long>(sum & 1));
        }
    }
    memprobe_thread_bind(nullptr);
    memprobe_thread_block(nullptr);
    memory->release();
}

// The SQE firmware, the kit's GPU context and the CP's first packets.
bool startCp(volatile uint8_t *smmu, size_t smmuBytes, const uint32_t *sqe, uint32_t sqeBytes) {
    unsigned pool = 512;
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

// Called once from coachz_gpu_start with the windows it mapped; the seq left the GMU up. Nonzero when the driver is serving.
extern "C" int coachz_gpu_driver_start(volatile uint8_t *core, volatile uint8_t *gmu, volatile uint8_t *smmu, size_t smmuBytes, const uint32_t *sqe, uint32_t sqeBytes) {
    gCore = core;
    gGmu = gmu;
    gSmmu = smmu;
    if (!core || !smmu || !sqe || !startCp(smmu, smmuBytes, sqe, sqeBytes)) {
        printf("COACHZ gpu9: CP not started; rendering stays on the CPU\n");
        return 0;
    }
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
    IOReturn status = vm ? gSubmit.attach(vm, &gRing) : kIOReturnNotReady;
    const NativeAdrenoTimeIo timer{nullptr, timerRead};
    const IOReturn timed = gTimer.attach(&timer, NativeAdrenoTimeCp);
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    if (!status) status = native_adreno_publish(vm, &gSubmit, &gRing, &clock, timed ? nullptr : &gTimer, &gDeviceInfo, 32, gReport.output_bits);
    printf("COACHZ gpu9: published 0x%x, timer 0x%x\n", status, timed);
    if (status) return 0;
    const bool accelerator = publishAccelerator();
    printf("COACHZ gpu9: accelerator %d\n", accelerator);
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(reportThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    if (kernel_thread_start(memoryProbeThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    cacheProbe();
    if (kernel_thread_start(rateThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    return accelerator;
}
