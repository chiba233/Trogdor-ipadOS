// SPDX-License-Identifier: BSD-2-Clause-Patent
// Independently expressed SC7180/ADAU7002 capture engine. Hardware facts and
// sequencing: pinned Linux files and annotated excerpts in SOURCES.md.
// No allocations, XNU symbols, automatic startup, or build registration here.
#ifndef COACHZ_MIC_CORE_HPP
#define COACHZ_MIC_CORE_HPP
#include "CoachzMicShared.h"
#include <stddef.h>

namespace CoachzMic {
enum : uint64_t {
    I2s = 0x62f01000, Dma = 0x62f18000, Irq = 0x62f09000,
    Rcg = 0x62d10000, Pll = 0x62d01000, TlmmWest = 0x03500000
};
enum : uint32_t {
    MicEnable = 1u << 9, SpeakerEnable = 1u << 16,
    I2sOwned = (1u << 17) | 0x3ffu,
    I2sFormat = (1u << 4) | 2u, // SD0, stereo, internal WS, 32-bit
    DmaFormat = (1u << 21) | (1u << 17) | (1u << 12) | (7u << 1),
    IrqPeriod = 1u << 15, IrqXrun = 1u << 16, IrqError = 1u << 17,
    IrqOwned = IrqPeriod | IrqXrun | IrqError,
    PinMask = 0x1ffu, // mux[5:2], pull[1:0], drive[8:6]; NOT OE[9]
    RcgFields = 0x1fu | (7u << 8) | (3u << 12) | (1u << 20)
};
static_assert(DmaFormat == 0x0022100e, "WRDMA fields differ from RDMA");
static_assert(COACHZ_MIC_DMA_FRAMES == 32768, "256 KiB stereo S32 ring");
// Half the ring is 341 ms: the worker must have copied before the producer can overwrite the oldest unread sample.
enum : uint32_t {DeadlineUs = 320000u, CursorSlackFrames = 1024u};
static_assert(sizeof(CoachzMicShared) + COACHZ_MIC_CONTROL_OFFSET <= 2048,
    "microphone status must not overlap existing diagnostics");

struct Ops {
    void *context;
    uint32_t (*read32)(void *, uint64_t);
    void (*write32)(void *, uint64_t, uint32_t);
    void (*barrier)(void *); // dsb sy on this non-cacheable DMA mapping
    void (*delayUs)(void *, uint32_t);
    uint64_t (*nowUs)(void *); // monotonic, same domain for every call
    // Serialize IRQ callback with short start/stop register transitions.
    // XNU: IOSimpleLockLockDisableInterrupt/UnlockEnableInterrupt. Never held
    // across supply votes, waits, copies, or clock configuration.
    uintptr_t (*irqEnter)(void *);
    void (*irqLeave)(void *, uintptr_t);
    // Shared RPMh provider; a separately allocated consumer, ldoa15, 1800 mV,
    // HPM. 0 success. On failed enable the provider must roll back its vote.
    int (*supply)(void *, bool);
};
struct Region {
    uint64_t physical;
    size_t bytes;
    volatile uint32_t *words; // Normal non-cacheable CPU mapping, not Device
    // These are loader/platform contracts, NOT a way to reserve RAM or set an
    // SMMU mapping. No default PA: the caller must actually establish both.
    bool exclusivelyReserved, lpassIdentityMapped;
};

class Engine {
public:
    // Must live for the whole kernel, including the disabled IRQ handler.
    // start/stop/poll are serialized by the owner; interrupt() never sleeps.
    bool init(const Ops &ops, const Region &region, CoachzMicShared *shared,
        int16_t *monoRing, uint32_t monoFrames) {
        if (initialized_ || !ops.read32 || !ops.write32 || !ops.barrier ||
            !ops.delayUs || !ops.nowUs || !ops.irqEnter || !ops.irqLeave ||
            !ops.supply || !shared || !monoRing ||
            (reinterpret_cast<uintptr_t>(shared) & 7u) ||
            (reinterpret_cast<uintptr_t>(monoRing) & 1u) ||
            monoFrames < COACHZ_MIC_DMA_FRAMES || !region.words ||
            !region.exclusivelyReserved || !region.lpassIdentityMapped ||
            !region.physical || (region.physical & 4095u) ||
            (reinterpret_cast<uintptr_t>(region.words) & 3u) ||
            region.bytes < COACHZ_MIC_DMA_BYTES ||
            region.physical > 0x100000000ULL - COACHZ_MIC_DMA_BYTES ||
            (region.physical < 0x81240000ULL && region.physical + COACHZ_MIC_DMA_BYTES > 0x81200000ULL))
            return false;
        ops_ = ops; region_ = region; shared_ = shared; mono_ = monoRing;
        capacity_ = monoFrames;
        *shared_ = {};
        shared_->magic = COACHZ_MIC_MAGIC; shared_->version = COACHZ_MIC_VERSION;
        shared_->rate = COACHZ_MIC_RATE; shared_->ring_frames = capacity_;
        initialized_ = true;
        return true;
    }

    int start(unsigned channel) {
        if (!initialized_ || channel > 2) return CoachzMicArgument;
        if (quarantined_) return shared_->error;
        if (owned_) return shared_->state == CoachzMicFault ? shared_->error : CoachzMicBusy;
        // The currently working speaker uses SECONDARY/RDMA1. Never retune
        // its PLL, reset LPASS, or take PRIMARY from a different active user.
        if ((read(Dma) & 1u) || (read(I2s) & (MicEnable | SpeakerEnable)) ||
            (read(Rcg + 0x18) & 1u) || (read(Rcg) & 1u) || (read(Irq) & IrqOwned))
            return setError(CoachzMicBusy);
        if ((read(Pll) & 0x80000001u) != 0x80000001u || read(Pll + 4) != 0x20u ||
            (read(Pll + 0x40) & 0xffffu) || ((read(Pll + 0xc) >> 12) & 15u) != 5u)
            return setError(CoachzMicParentClock);
        save(); owned_ = true;
        if (ops_.supply(ops_.context, true)) {
            owned_ = false;
            return setError(CoachzMicSupply);
        }
        supplyOn_ = true;
        for (unsigned i = 0; i < 6; ++i)
            update(pin(i), PinMask, i == 5 ? (3u << 6) | 3u : (1u << 2) | 1u);
        // Linux clk-rcg2 helper: F(3072000, odd PLL, 10, 1, 4).
        // HID=2*10-1, M=1, N=~(4-1), D=~4; all M/N/D 16 bits.
        write(Rcg + 8, 1); write(Rcg + 12, 0xfffc); write(Rcg + 16, 0xfffb);
        update(Rcg + 4, RcgFields, 19u | (5u << 8) | (2u << 12));
        if (!commitClock()) return fail(CoachzMicClockUpdate);
        update(Rcg + 0x18, 1u, 1u);
        if (!branchWait(true)) return fail(CoachzMicBranch);
        // All MMIO operations above/below are aligned 32-bit operations.
        update(I2s, I2sOwned, I2sFormat);
        for (unsigned i = 0; i < COACHZ_MIC_DMA_BYTES / 4; ++i) region_.words[i] = 0;
        ops_.barrier(ops_.context);
        write(Dma, 0);
        write(Dma + 4, static_cast<uint32_t>(region_.physical));
        write(Dma + 8, COACHZ_MIC_DMA_BYTES / 4 - 1);
        write(Dma + 16, COACHZ_MIC_PERIOD_BYTES / 4 - 1);
        write(Dma, DmaFormat);
        const uintptr_t irqState = ops_.irqEnter(ops_.context);
        __atomic_store_n(&periods_, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&faults_, 0u, __ATOMIC_RELAXED);
        write(Irq + 12, IrqOwned);
        update(Irq, IrqOwned, IrqOwned);
        write(Dma, DmaFormat | 1u);
        update(I2s, MicEnable, MicEnable);
        ops_.irqLeave(ops_.context, irqState);
        startedUs_ = lastUs_ = ops_.nowUs(ops_.context);
        lastIndex_ = 0; stalledUs_ = startedUs_; source_ = channel; filterPrimed_ = false;
        begin();
        ++shared_->generation;
        state(CoachzMicWarming); shared_->error = CoachzMicOK;
        range(0, 0); shared_->anchor_host = 0;
        shared_->frames_copied = 0; shared_->source_channel = channel; shared_->cursor_jumps = 0;
        shared_->peak[0] = shared_->peak[1] = 0;
        shared_->nonzero[0] = shared_->nonzero[1] = 0;
        end();
        return CoachzMicOK;
    }

    // Install on LPAIF host port 0, SPI160/INTID192. Only our channel's
    // bits are acknowledged. Caller holds the IRQ spinlock, never the worker
    // mutex. No RPMh, allocations, sleeping, or PCM copies here.
    void interrupt() {
        const uint32_t flags = read(Irq + 4) & IrqOwned;
        if (!flags) return;
        write(Irq + 12, flags);
        if (flags & IrqPeriod) __atomic_fetch_add(&periods_, 1u, __ATOMIC_RELAXED);
        if (flags & (IrqXrun | IrqError)) {
            __atomic_fetch_or(&faults_, flags, __ATOMIC_RELAXED);
            // Faults can continuously assert: stop this channel immediately.
            // start/stop mask the source before changing DMA ownership.
            update(Irq, IrqOwned, 0);
            update(Dma, 1u, 0);
        }
    }

    // Existing audio worker calls every 10 ms. host and anchor are in the
    // existing playback timeline, ticksPerFrame in its real hardware ticks.
    // No timer-generated PCM: only completed DMA frames advance written.
    int poll(uint64_t host, uint64_t anchor, uint64_t ticksPerFrame) {
        if (!owned_) return initialized_ ? static_cast<int>(shared_->error) : CoachzMicArgument;
        const uint64_t now = ops_.nowUs(ops_.context);
        if (__atomic_load_n(&faults_, __ATOMIC_RELAXED)) return fail(CoachzMicDmaFault);
        uint32_t index;
        if (!position(index)) return fail(CoachzMicPosition);
        // 256 KiB is 683 ms; a half-ring deadline leaves time to copy before
        // the producer can overwrite our oldest unread sample. Lost/coalesced
        // IRQs cannot manufacture another wrap. Ask the owner to restart.
        if (now < lastUs_ || now - lastUs_ >= DeadlineUs) return fail(CoachzMicOverrun);
        const uint32_t count = (index + COACHZ_MIC_DMA_FRAMES - lastIndex_) % COACHZ_MIC_DMA_FRAMES;
        // CURRENT reads back behind or ahead of the DMA now and then: on hardware
        // a jump tripped the half-ring test with ticks only 11 ms apart, and each
        // restart left a gap of zeros in the recording. The time since the last
        // accepted read bounds how far the DMA can have moved, so a larger step
        // is a bad read: skip it. A real overrun still ends at DeadlineUs.
        if (count > (now - lastUs_) * 48u / 1000u + CursorSlackFrames) {
            ++shared_->cursor_jumps;
            return CoachzMicOK;
        }
        if (count >= COACHZ_MIC_DMA_FRAMES / 2) return fail(CoachzMicOverrun);
        if (count) stalledUs_ = now;
        else if (now - stalledUs_ >= DeadlineUs) return fail(CoachzMicStoppedDma);
        lastUs_ = now;
        if (now - startedUs_ < 80000u || !ticksPerFrame || host < anchor) {
            lastIndex_ = index;
            return CoachzMicOK; // discard codec's 80 ms wake-up samples
        }
        if (shared_->state == CoachzMicWarming || shared_->anchor_host != anchor) {
            // Rebase invalidates history. No old PCM from a preceding capture
            // or playback-clock adjustment is exposed in the new epoch.
            begin();
            shared_->anchor_host = anchor;
            filterPrimed_ = false;
            const uint64_t epoch = (host - anchor) / ticksPerFrame;
            range(epoch, epoch);
            if (shared_->state != CoachzMicWarming) ++shared_->generation;
            state(CoachzMicRunning);
            snapshotIrq(); end();
            lastIndex_ = index;
            return CoachzMicOK;
        }
        if (!count) return CoachzMicOK;
        const uint64_t copyStart = ops_.nowUs(ops_.context);
        ops_.barrier(ops_.context); // DMA writes before CPU sample loads
        begin();
        uint64_t out = shared_->written;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t f = (lastIndex_ + i) % COACHZ_MIC_DMA_FRAMES;
            const int16_t left = s16(region_.words[f * 2]);
            const int16_t right = s16(region_.words[f * 2 + 1]);
            __atomic_store_n(&mono_[out++ % capacity_], condition(left, right), __ATOMIC_RELAXED);
            measure(0, left); measure(1, right);
        }
        ops_.barrier(ops_.context);
        const uint64_t copyEnd = ops_.nowUs(ops_.context);
        // The source began at most a half-ring behind. Never publish a copy
        // that might have been overtaken, even when the final cursor wraps.
        const bool valid = copyEnd >= copyStart && copyEnd - copyStart < DeadlineUs &&
            !__atomic_load_n(&faults_, __ATOMIC_RELAXED);
        if (valid) {
            range(out - shared_->first > capacity_ ? out - capacity_ : shared_->first, out);
            shared_->frames_copied += count;
            shared_->dma_current = static_cast<uint32_t>(region_.physical + index * 8u);
            snapshotIrq();
        } else {
            // Invalidate while seq is still odd: samples may have changed.
            range(shared_->written, shared_->written);
            state(CoachzMicFault);
        }
        end(); lastIndex_ = index;
        if (!valid) return fail(CoachzMicOverrun);
        return CoachzMicOK;
    }

    int stop() {
        if (!initialized_) return CoachzMicArgument;
        if (!owned_) return CoachzMicOK;
        begin(); state(CoachzMicOff); range(shared_->written, shared_->written); end();
        const uintptr_t irqState = ops_.irqEnter(ops_.context);
        update(Irq, IrqOwned, 0);
        update(Dma, 1u, 0);
        update(I2s, MicEnable, 0);
        write(Irq + 12, IrqOwned);
        ops_.irqLeave(ops_.context, irqState);
        if (!wait(Dma, 1u, 0)) {
            // Keep mappings, pins, supply and reserved memory: do not recycle
            // anything when the engine might still own it. No retry in boot.
            quarantined_ = true;
            return setError(CoachzMicStoppedDma);
        }
        update(Rcg + 0x18, 1u, 0);
        if (!branchWait(false)) {
            quarantined_ = true;
            return setError(CoachzMicBranch);
        }
        update(I2s, I2sOwned, savedI2s_ & I2sOwned);
        write(Rcg + 8, savedClock_[1]); write(Rcg + 12, savedClock_[2]); write(Rcg + 16, savedClock_[3]);
        write(Rcg + 4, savedClock_[0]);
        if (!commitClock()) {
            quarantined_ = true;
            return setError(CoachzMicClockUpdate);
        }
        for (unsigned i = 0; i < 6; ++i) update(pin(i), PinMask, savedPins_[i] & PinMask);
        if (supplyOn_ && ops_.supply(ops_.context, false)) {
            quarantined_ = true;
            return setError(CoachzMicSupply);
        }
        supplyOn_ = false; owned_ = false;
        return CoachzMicOK;
    }
    bool initialized() const {return initialized_;}
    // The ADAU7002 output carries a large DC offset (about +730 of 32768 on this
    // machine), so any discontinuity in the stream is a loud click. Remove it per
    // channel with a one-pole high-pass at about 38 Hz, then mix the two
    // microphones (source 2) and lift the average 12 dB: they peak near -26 dBFS
    // at speaking level with no gain anywhere else in the path.
    int32_t highPass(unsigned channel, int32_t sample) {
        const int64_t value = (static_cast<int64_t>(sample - filterIn_[channel]) << 15) +
            ((filterOut_[channel] * 32604) >> 15);
        filterIn_[channel] = sample; filterOut_[channel] = value;
        return static_cast<int32_t>(value >> 15);
    }
    int16_t condition(int16_t left, int16_t right) {
        if (!filterPrimed_) {
            filterIn_[0] = left; filterIn_[1] = right;
            filterOut_[0] = filterOut_[1] = 0;
            filterPrimed_ = true;
        }
        const int32_t l = highPass(0, left), r = highPass(1, right);
        const int32_t value = source_ == 2 ? (l + r) * 2 : (source_ ? r : l) * 4;
        return static_cast<int16_t>(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
    }
    static int16_t s16(uint32_t word) {return static_cast<int16_t>(static_cast<uint16_t>(word >> 16));}

private:
    Ops ops_ = {};
    Region region_ = {};
    CoachzMicShared *shared_ = nullptr;
    int16_t *mono_ = nullptr;
    uint32_t capacity_ = 0, source_ = 0, lastIndex_ = 0;
    uint32_t savedI2s_ = 0, savedPins_[6] = {}, savedClock_[4] = {};
    uint32_t periods_ = 0, faults_ = 0;
    uint64_t startedUs_ = 0, lastUs_ = 0, stalledUs_ = 0;
    int64_t filterOut_[2] = {};
    int32_t filterIn_[2] = {};
    bool filterPrimed_ = false;
    bool initialized_ = false, owned_ = false, supplyOn_ = false, quarantined_ = false;
    uint32_t read(uint64_t pa) {return ops_.read32(ops_.context, pa);}
    void write(uint64_t pa, uint32_t v) {ops_.write32(ops_.context, pa, v);}
    void update(uint64_t pa, uint32_t mask, uint32_t v) {write(pa, (read(pa) & ~mask) | (v & mask));}
    static uint64_t pin(unsigned i) {return TlmmWest + (i == 5 ? 83u : 53u + i) * 0x1000u;}
    bool wait(uint64_t pa, uint32_t mask, uint32_t value) {
        for (unsigned i = 0; i < 500; ++i) {
            if ((read(pa) & mask) == value) return true;
            ops_.delayUs(ops_.context, 1);
        }
        return false;
    }
    bool commitClock() {update(Rcg, 1u, 1u); return wait(Rcg, 1u, 0);}
    bool branchWait(bool enabling) {
        // clk_branch_wait/clk_branch2_check_halt: HWCG skips polling;
        // otherwise enable accepts CLK_OFF clear OR NOC FSM_STATUS_ON=2.
        // Disable requires CLK_OFF set. This is not the RCG UPDATE test.
        if (read(Rcg + 0x18) & 2u) return true;
        for (unsigned i = 0; i < 200; ++i) {
            const uint32_t value = read(Rcg + 0x18);
            if (enabling ? (!(value & (1u << 31)) || ((value >> 28) & 7u) == 2u)
                         : (value & (1u << 31))) return true;
            ops_.delayUs(ops_.context, 1);
        }
        return false;
    }
    bool position(uint32_t &index) {
        const uint32_t pa = read(Dma + 12);
        if ((pa & 3u) || pa < region_.physical || uint64_t(pa) - region_.physical >= COACHZ_MIC_DMA_BYTES) return false;
        index = static_cast<uint32_t>((uint64_t(pa) - region_.physical) / 8u);
        return true; // floor the in-progress stereo frame, including odd word
    }
    void save() {
        savedI2s_ = read(I2s);
        for (unsigned i = 0; i < 6; ++i) savedPins_[i] = read(pin(i));
        for (unsigned i = 0; i < 4; ++i) savedClock_[i] = read(Rcg + 4u * (i + 1u));
    }
    void begin() {
        __atomic_fetch_add(&shared_->seq, 1u, __ATOMIC_ACQ_REL);
        __atomic_thread_fence(__ATOMIC_RELEASE);
    }
    void end() {__atomic_fetch_add(&shared_->seq, 1u, __ATOMIC_RELEASE);}
    void state(uint32_t value) {__atomic_store_n(&shared_->state, value, __ATOMIC_RELAXED);}
    void range(uint64_t first, uint64_t written) {
        __atomic_store_n(&shared_->first, first, __ATOMIC_RELAXED);
        __atomic_store_n(&shared_->written, written, __ATOMIC_RELAXED);
    }
    void snapshotIrq() {
        shared_->irq_periods = __atomic_load_n(&periods_, __ATOMIC_RELAXED);
        shared_->irq_faults = __atomic_load_n(&faults_, __ATOMIC_RELAXED);
    }
    void measure(unsigned ch, int16_t sample) {
        const uint32_t magnitude = sample < 0 ? static_cast<uint32_t>(-static_cast<int32_t>(sample)) : static_cast<uint32_t>(sample);
        if (magnitude > shared_->peak[ch]) shared_->peak[ch] = magnitude;
        if (sample) ++shared_->nonzero[ch];
    }
    int setError(unsigned error) {
        begin(); shared_->error = error; state(CoachzMicFault);
        range(shared_->written, shared_->written); snapshotIrq(); end();
        return static_cast<int>(error);
    }
    int fail(unsigned error) {
        const int stopped = stop();
        return setError(stopped ? static_cast<unsigned>(stopped) : error);
    }
};
}
#endif
