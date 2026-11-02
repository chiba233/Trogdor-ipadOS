// SPDX-License-Identifier: BSD-2-Clause-Patent
// Written for the integrator; deliberately NOT run or wired into tests/build.
// A host model checks arithmetic and ownership, not hardware support.
#include "CoachzMicCore.hpp"
#include "CoachzMicHal.h"
#include <cassert>
#include <map>
#include <utility>
#include <vector>
using namespace CoachzMic;

struct Model {
    std::map<uint64_t, uint32_t> regs;
    std::vector<std::pair<uint64_t, uint32_t>> writes;
    uint32_t dma[COACHZ_MIC_DMA_BYTES / 4] = {};
    int16_t mono[65536] = {};
    CoachzMicShared shared = {};
    Engine engine;
    uint64_t us = 0;
    int votes = 0;
    bool rejectSupply = false;
    static constexpr uint64_t Pa = 0x82000000; // MODEL ONLY, not a platform allocation
    Model() {
        regs[Pll] = 0x80000001;
        regs[Pll + 4] = 0x20;
        regs[Pll + 12] = 5u << 12;
        regs[Rcg + 0x18] = 1u << 31;
        for (unsigned pin : {53u,54u,55u,56u,57u,83u}) regs[TlmmWest + pin * 4096] = 0x200;
        const Ops ops = {this, read, write, barrier, delay, now, enter, leave, supply};
        const Region region = {Pa, sizeof(dma), dma, true, true};
        assert(engine.init(ops, region, &shared, mono, 65536));
    }
    static uint32_t read(void *ctx, uint64_t pa) {return static_cast<Model *>(ctx)->regs[pa];}
    static void write(void *ctx, uint64_t pa, uint32_t v) {
        auto &m = *static_cast<Model *>(ctx);
        m.writes.emplace_back(pa, v);
        if (pa == Rcg) v &= ~1u; // model UPDATE acknowledgement
        if (pa == Rcg + 0x18) v = (v & 1u) ? v & ~(1u << 31) : v | (1u << 31);
        if (pa == Dma + 4) m.regs[Dma + 12] = v;
        if (pa == Irq + 12) m.regs[Irq + 4] &= ~v;
        m.regs[pa] = v;
    }
    static void barrier(void *) {}
    static void delay(void *, uint32_t) {}
    static uint64_t now(void *ctx) {return static_cast<Model *>(ctx)->us;}
    static uintptr_t enter(void *) {return 0;}
    static void leave(void *, uintptr_t) {}
    static int supply(void *ctx, bool on) {
        auto &m = *static_cast<Model *>(ctx);
        if (on && m.rejectSupply) return -1;
        m.votes += on ? 1 : -1;
        return 0;
    }
    int step(uint64_t micros, uint64_t anchor = 0) {
        us = micros;
        regs[Dma + 12] = Pa + ((micros * 48 / 1000) % COACHZ_MIC_DMA_FRAMES) * 8;
        return engine.poll(micros * 24, anchor, 500); // model 24 MHz, 48 kHz
    }
    void warm() {
        assert(engine.start(0) == 0);
        for (uint64_t t = 10000; t <= 80000; t += 10000) assert(step(t) == 0);
        assert(shared.state == CoachzMicRunning);
        assert(shared.written == 3840 && shared.first == 3840);
    }
};

int main() {
    assert(Engine::s16(0x7fff0000) == 32767);
    assert(Engine::s16(0x80000000) == -32768);
    assert(Engine::s16(0xffff0000) == -1);
    {
        Model m;
        m.regs[Irq] = 7u << 3; // unrelated DMA channel stays enabled
        m.warm();
        assert(m.regs[Dma] == 0x22100f);
        assert(m.regs[Dma + 8] == COACHZ_MIC_DMA_BYTES / 4 - 1 && m.regs[Dma + 16] == COACHZ_MIC_PERIOD_BYTES / 4 - 1);
        assert(m.regs[Rcg + 8] == 1 && m.regs[Rcg + 12] == 0xfffc && m.regs[Rcg + 16] == 0xfffb);
        assert((m.regs[I2s] & I2sOwned) == 0x212);
        for (unsigned i = 0; i < COACHZ_MIC_DMA_FRAMES; ++i) {
            m.dma[2*i] = 0x10000000; m.dma[2*i+1] = 0xe0000000;
        }
        assert(m.step(90000) == 0 && m.shared.frames_copied == 480);
        float samples[4];
        assert(coachz_mic_read(&m.shared, m.mono, 3840, 4, samples));
        assert(samples[0] == 0.125f);
        assert(coachz_mic_read(&m.shared, m.mono, 0, 4, samples) && samples[0] == 0);
        assert(m.shared.peak[0] == 4096 && m.shared.peak[1] == 8192);
        // Cross the non-power-of-two physical ring boundary.
        for (uint64_t t = 100000; t <= 140000; t += 10000) assert(m.step(t) == 0);
        assert(m.shared.frames_copied == 2880);
        // Rebase discards previous capture interval.
        assert(m.step(150000, 24000) == 0);
        assert(m.shared.first == m.shared.written && m.shared.generation == 2);
        assert(m.engine.stop() == 0 && m.votes == 0);
        assert((m.regs[Irq] & (7u << 3)) == (7u << 3));
        assert(m.regs[TlmmWest + 83*4096] == 0x200); // OE retained/restored
        assert(coachz_mic_read(&m.shared, m.mono, 3840, 4, samples) && samples[0] == 0);
        for (auto w : m.writes) {
            assert((w.first & 3) == 0);
            assert(w.first < 0x62f02000 || w.first >= 0x62f03000); // no SECONDARY writes
            assert(w.first < Pll || w.first >= Pll + 4096); // no shared PLL writes
        }
    }
    {
        Model m; m.regs[Dma] = 1;
        assert(m.engine.start(0) == CoachzMicBusy && m.writes.empty() && m.votes == 0);
    }
    {
        Model m; m.rejectSupply = true;
        assert(m.engine.start(0) == CoachzMicSupply && m.writes.empty());
    }
    {
        Model m; m.warm();
        assert(m.step(500000) == CoachzMicOverrun); // missed worker deadline
        assert(m.shared.state == CoachzMicFault && m.votes == 0);
    }
    {
        Model m; m.warm();
        m.regs[Irq + 4] = IrqPeriod | IrqXrun | (1u << 3);
        m.engine.interrupt();
        assert(m.regs[Irq + 4] == (1u << 3));
        assert(m.step(90000) == CoachzMicDmaFault && m.votes == 0);
    }
    {
        Model m; m.warm();
        m.regs[Dma + 12] = Model::Pa - 4;
        m.us = 90000;
        assert(m.engine.poll(90000*24, 0, 500) == CoachzMicPosition);
    }
}
