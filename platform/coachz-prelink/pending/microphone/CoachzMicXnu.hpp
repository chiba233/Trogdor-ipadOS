// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pending XNU binding. Deliberately not included by any active translation
// unit. The owner may later include this from CoachzAudio.cpp; no new object
// or build flag is required by this header. See INTEGRATION.md first.
#ifndef COACHZ_MIC_XNU_HPP
#define COACHZ_MIC_XNU_HPP
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include "CoachzMicCore.hpp"

extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern "C" vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);

namespace CoachzMic {
class XnuBinding {
public:
    // The shared LPASS GDSCs, bus clocks, PLL, OSR/mclk0 and the WRDMA5
    // identity SMMU route must remain held by the audio platform throughout
    // this binding's life. Playback's RDMA mapping alone does NOT prove the
    // capture stream's mapping. See the three Linux IOMMU stream IDs.
    //
    // vote(ctx,on) must use a NEW shared-provider consumer for ldoa15,
    // 1800 mV, HPM, and preserve every other consumer's votes. No raw TCS
    // programming or hardcoded reuse of Wi-Fi/USB consumer IDs here.
    bool prepare(uint64_t dmaPhysical, size_t reservedBytes,
        bool reservedByLoader, bool captureSmmuIdentityMapped,
        CoachzMicShared *status, int16_t *mono, uint32_t monoFrames,
        int (*vote)(void *, bool), void *voteContext) {
        if (attempted_) return false;
        attempted_ = true;
        if (!vote || !status || (reinterpret_cast<uintptr_t>(status) & 7u) ||
            !mono || (reinterpret_cast<uintptr_t>(mono) & 1u) ||
            monoFrames < COACHZ_MIC_DMA_FRAMES || !reservedByLoader ||
            !captureSmmuIdentityMapped || !dmaPhysical || (dmaPhysical & 4095u) ||
            reservedBytes < COACHZ_MIC_DMA_BYTES ||
            dmaPhysical > 0x100000000ULL - COACHZ_MIC_DMA_BYTES ||
            (dmaPhysical < 0x81240000ULL && dmaPhysical + COACHZ_MIC_DMA_BYTES > 0x81200000ULL)) return false;
        worker_ = IOLockAlloc(); irq_ = IOSimpleLockAlloc();
        if (!worker_ || !irq_) return false;
        vote_ = vote; voteContext_ = voteContext;
        const uint64_t mmio[] = {I2s, Dma, Irq, Rcg, Pll,
            TlmmWest + 53 * 0x1000, TlmmWest + 54 * 0x1000,
            TlmmWest + 55 * 0x1000, TlmmWest + 56 * 0x1000,
            TlmmWest + 57 * 0x1000, TlmmWest + 83 * 0x1000};
        for (unsigned i = 0; i < sizeof(mmio) / sizeof(mmio[0]); ++i)
            if (!mapPage(mmio[i])) return false;
        // This binding owns the GIC source, not just channel-5 bits. Refuse
        // an already-used host port BEFORE attaching, so an unrelated host
        // channel's handler cannot be replaced. A future shared dispatcher
        // can remove this restriction only with unified IRQEN locking.
        if (read32(this, Irq) || (read32(this, Dma) & 1u) ||
            (read32(this, I2s) & (MicEnable | SpeakerEnable)) ||
            (read32(this, Rcg + 0x18) & 1u)) return false;
        // DMA memory is outside managed XNU RAM and must have no conflicting
        // cached alias. Keep the mapping/reservation even after stop/error.
        const vm_offset_t dma = ml_io_map_wcomb(static_cast<vm_offset_t>(dmaPhysical), COACHZ_MIC_DMA_BYTES);
        if (!dma) return false;
        const Ops ops = {this, read32, write32, barrier, delay, now,
            irqEnter, irqLeave, supply};
        const Region region = {dmaPhysical, reservedBytes,
            reinterpret_cast<volatile uint32_t *>(dma), true, true};
        if (!engine_.init(ops, region, status, mono, monoFrames)) return false;
        // Attach once. There is no detach API in the current ArmCpu export;
        // this object, its lock and mappings must NEVER be freed or moved.
        uint32_t readback = 0; uint64_t router = 0;
        if (armcpu_spi_attach(32u + 160u, false, irqThunk, this, &readback, &router)) return false;
        ready_ = true;
        return true;
    }
    int start(unsigned channel = 0) {
        if (!ready_) return CoachzMicArgument;
        IOLockLock(worker_);
        const int result = engine_.start(channel);
        IOLockUnlock(worker_);
        return result;
    }
    int stop() { // also call on client close, suspend and shutdown
        if (!ready_) return CoachzMicArgument;
        IOLockLock(worker_);
        const int result = engine_.stop();
        IOLockUnlock(worker_);
        return result;
    }
    int poll(uint64_t host, uint64_t anchor, uint64_t ticksPerFrame) {
        if (!ready_) return CoachzMicArgument;
        IOLockLock(worker_);
        const int result = engine_.poll(host, anchor, ticksPerFrame);
        IOLockUnlock(worker_);
        return result;
    }
    bool ready() const {return ready_;}
    XnuBinding() = default;
    XnuBinding(const XnuBinding &) = delete;
    XnuBinding &operator=(const XnuBinding &) = delete;
private:
    struct Page {uint64_t pa; vm_offset_t va;};
    Page pages_[11] = {};
    unsigned count_ = 0;
    bool attempted_ = false, ready_ = false;
    IOLock *worker_ = nullptr;
    IOSimpleLock *irq_ = nullptr;
    Engine engine_;
    int (*vote_)(void *, bool) = nullptr;
    void *voteContext_ = nullptr;
    bool mapPage(uint64_t pa) {
        pa &= ~0xfffULL;
        for (unsigned i = 0; i < count_; ++i) if (pages_[i].pa == pa) return true;
        if (count_ == sizeof(pages_) / sizeof(pages_[0])) return false;
        const vm_offset_t va = ml_io_map(static_cast<vm_offset_t>(pa), 4096);
        if (!va) return false;
        pages_[count_++] = {pa, va};
        return true;
    }
    volatile uint32_t *reg(uint64_t pa) {
        for (unsigned i = 0; i < count_; ++i)
            if (pages_[i].pa == (pa & ~0xfffULL))
                return reinterpret_cast<volatile uint32_t *>(pages_[i].va + (pa & 0xfffu));
        return nullptr;
    }
    static uint32_t read32(void *ctx, uint64_t pa) {
        volatile uint32_t *p = static_cast<XnuBinding *>(ctx)->reg(pa);
        return p ? *p : 0;
    }
    static void write32(void *ctx, uint64_t pa, uint32_t v) {
        volatile uint32_t *p = static_cast<XnuBinding *>(ctx)->reg(pa);
        if (p) *p = v;
        barrier(ctx);
    }
    static void barrier(void *) {__asm__ volatile("dsb sy" ::: "memory");}
    static void delay(void *, uint32_t us) {IODelay(us);}
    static uint64_t now(void *) {
        clock_sec_t seconds = 0; clock_usec_t micros = 0;
        clock_get_system_microtime(&seconds, &micros);
        return uint64_t(seconds) * 1000000u + micros;
    }
    static uintptr_t irqEnter(void *ctx) {
        return IOSimpleLockLockDisableInterrupt(static_cast<XnuBinding *>(ctx)->irq_);
    }
    static void irqLeave(void *ctx, uintptr_t state) {
        IOSimpleLockUnlockEnableInterrupt(static_cast<XnuBinding *>(ctx)->irq_, static_cast<IOInterruptState>(state));
    }
    static int supply(void *ctx, bool on) {
        XnuBinding *self = static_cast<XnuBinding *>(ctx);
        return self->vote_(self->voteContext_, on);
    }
    static void irqThunk(void *ctx) {
        XnuBinding *self = static_cast<XnuBinding *>(ctx);
        const uintptr_t state = irqEnter(ctx);
        self->engine_.interrupt();
        irqLeave(ctx, state);
    }
};
}
#endif
