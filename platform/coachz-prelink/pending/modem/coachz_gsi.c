/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The IPA v4.2 GSI as mainline gsi.c sets it up after the modem raises ipa-setup-ready (ipa_setup -> gsi_setup). Not in
 * the kernel build. The modem loads the GSI firmware ("qcom,gsi-loader = modem"), so GSI_STATUS must already say ENABLED.
 *
 * Mainline waits for each command on a completion that gsi_isr signals. Here a command is written and the same ISR body
 * (coachz_gsi_interrupt) is polled for up to GSI_CMD_TIMEOUT (50 ms); the "gsi" interrupt (GIC SPI 432, level) may call
 * it too once setup is done. The apps SMMU is in bypass, so ring addresses are physical.
 * Ring memory: one block, write-combined or uncached, zeroed, aligned to 64 KiB (largest ring first keeps each 2^n ring
 * aligned to its size, which the hardware requires). v4.2 needs 40 KiB: four TRE rings (8 + 4 + 4 + 4 KiB) and their
 * four event rings.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include "coachz_ipa_kernel.h"


static inline uint32_t rd(struct coachz_gsi *g, uint32_t o) { return *(volatile uint32_t *)(g->virt + o); }
static inline void wr(struct coachz_gsi *g, uint32_t o, uint32_t v) { *(volatile uint32_t *)(g->virt + o) = v; }
#define CH(reg, n) (CGSI_##reg + (n) * CGSI_##reg##_STRIDE)

static uint64_t ring_addr(struct coachz_gsi *g, int event, uint32_t id)
{
    return g->ring_phys + (event ? g->place.evt_offset[id] : g->place.offset[id]);
}

/* gsi_init's channel part: the table, then the rings (dma_alloc_coherent memory is zeroed) */
int coachz_gsi_init(struct coachz_gsi *gsi, vm_offset_t gsi_virt, void *ring_virt, uint64_t ring_phys, uint32_t ring_bytes)
{
    __builtin_memset(gsi, 0, sizeof *gsi);
    gsi->virt = gsi_virt;
    gsi->t = cgs_channel_table();
    if (gsi->t.bad) { printf("COACHZ gsi: endpoint %u has invalid channel data\n", gsi->t.bad - 1u); return -EINVAL; }
    const uint32_t used = cgs_place_rings(&gsi->t, ring_bytes, &gsi->place);
    if (!used || (ring_phys & 0xffffu) || !ring_virt) { printf("COACHZ gsi: ring block too small or unaligned\n"); return -ENOMEM; }
    gsi->ring_virt = ring_virt;
    gsi->ring_phys = ring_phys;
    __builtin_memset(ring_virt, 0, used);
    return 0;
}

/* ---------------------------------------------------------------- interrupts (gsi_irq_*, gsi_isr) */
static void type_update(struct coachz_gsi *g, uint32_t v) { g->type_enabled = v; wr(g, CGSI_CNTXT_TYPE_IRQ_MSK, v); }

static void glob_err(struct coachz_gsi *g)
{
    const uint32_t v = rd(g, CGSI_ERROR_LOG);
    wr(g, CGSI_ERROR_LOG, 0);
    wr(g, CGSI_ERROR_LOG_CLR, ~0u);
    const uint32_t type = cip_decode(CGSI_ERROR_LOG_ERR_TYPE, v), code = cip_decode(CGSI_ERROR_LOG_ERR_CODE, v);
    const uint32_t which = cip_decode(CGSI_ERROR_LOG_ERR_VIRT_IDX, v), ee = cip_decode(CGSI_ERROR_LOG_ERR_EE, v);
    if ((type == CGS_ERR_TYPE_CHAN || type == CGS_ERR_TYPE_EVT) && code == CGS_OUT_OF_RESOURCES) g->completed = 1;
    printf("COACHZ gsi: global error 0x%08x (type %u, %s %u, ee %u, code %u)\n", v, type,
        type == CGS_ERR_TYPE_EVT ? "event ring" : "channel", which, ee, code);
}

void coachz_gsi_interrupt(struct coachz_gsi *g)
{
    uint32_t pending, count = 0;
    while ((pending = rd(g, CGSI_CNTXT_TYPE_IRQ))) {
        do {
            const uint32_t bit = pending & -pending;
            pending ^= bit;
            if (bit == CGS_IRQ_CH_CTRL) {
                const uint32_t m = rd(g, CGSI_CNTXT_SRC_CH_IRQ);
                wr(g, CGSI_CNTXT_SRC_CH_IRQ_CLR, m);
                if (m) g->completed = 1;
            } else if (bit == CGS_IRQ_EV_CTRL) {
                const uint32_t m = rd(g, CGSI_CNTXT_SRC_EV_CH_IRQ);
                wr(g, CGSI_CNTXT_SRC_EV_CH_IRQ_CLR, m);
                if (m) g->completed = 1;
            } else if (bit == CGS_IRQ_GLOB_EE) {
                uint32_t v = rd(g, CGSI_CNTXT_GLOB_IRQ_STTS);
                if (v & CGS_ERROR_INT) glob_err(g);
                wr(g, CGSI_CNTXT_GLOB_IRQ_CLR, v);
                v &= ~CGS_ERROR_INT;
                if (v & CGS_GP_INT1) {   /* gsi_isr_gp_int1: a generic command finished */
                    v ^= CGS_GP_INT1;
                    g->result = cgs_generic_result(rd(g, CGSI_CNTXT_SCRATCH_0));
                    if (g->result == -EIO) printf("COACHZ gsi: generic result 0x%x\n", rd(g, CGSI_CNTXT_SCRATCH_0));
                    g->completed = 1;
                }
                if (v) printf("COACHZ gsi: unexpected global interrupt 0x%08x\n", v);
            } else if (bit == CGS_IRQ_IEOB) {   /* gsi_isr_ieob: mask the rings and leave them to the poller (NAPI) */
                const uint32_t m = rd(g, CGSI_CNTXT_SRC_IEOB_IRQ);
                g->ieob_enabled &= ~m;
                if (!g->ieob_enabled) type_update(g, g->type_enabled & ~CGS_IRQ_IEOB);
                wr(g, CGSI_CNTXT_SRC_IEOB_IRQ_MSK, g->ieob_enabled);
                wr(g, CGSI_CNTXT_SRC_IEOB_IRQ_CLR, m);
                g->ieob_pending |= m;
            } else if (bit == CGS_IRQ_GENERAL) {
                const uint32_t v = rd(g, CGSI_CNTXT_GSI_IRQ_STTS);
                wr(g, CGSI_CNTXT_GSI_IRQ_CLR, v);
                printf("COACHZ gsi: unexpected general interrupt 0x%08x\n", v);
            } else {
                printf("COACHZ gsi: unrecognized interrupt type 0x%08x\n", bit);
            }
        } while (pending);
        if (++count > CGS_ISR_MAX_ITER) { printf("COACHZ gsi: interrupt flood\n"); break; }
    }
}

/* gsi_command: write, then wait for the completion the interrupt signals */
static int command(struct coachz_gsi *g, uint32_t reg, uint32_t value)
{
    g->completed = 0;
    wr(g, reg, value);
    for (unsigned waited = 0; waited < CGS_CMD_TIMEOUT_MS * 1000u; waited += 20) {
        coachz_gsi_interrupt(g);
        if (g->completed) return 1;
        delay(20);
    }
    coachz_gsi_interrupt(g);
    return g->completed;
}

/* ---------------------------------------------------------------- event ring and channel commands */
static uint32_t ev_state(struct coachz_gsi *g, uint32_t id) { return cgs_ev_state(rd(g, CH(EV_CH_E_CNTXT_0, id))); }
static uint32_t ch_state(struct coachz_gsi *g, uint32_t id) { return cgs_state(rd(g, CH(CH_C_CNTXT_0, id))); }

static void evt_command(struct coachz_gsi *g, uint32_t id, uint32_t opcode)
{
    wr(g, CGSI_CNTXT_SRC_EV_CH_IRQ_CLR, ~0u);   /* gsi_irq_ev_ctrl_enable */
    wr(g, CGSI_CNTXT_SRC_EV_CH_IRQ_MSK, 1u << id);
    type_update(g, g->type_enabled | CGS_IRQ_EV_CTRL);
    const int done = command(g, CGSI_EV_CH_CMD, cgs_ev_cmd(id, opcode));
    type_update(g, g->type_enabled & ~CGS_IRQ_EV_CTRL);   /* gsi_irq_ev_ctrl_disable */
    wr(g, CGSI_CNTXT_SRC_EV_CH_IRQ_MSK, 0);
    if (!done) printf("COACHZ gsi: command %u for event ring %u timed out, state %u\n", opcode, id, ev_state(g, id));
}

static void ch_command(struct coachz_gsi *g, uint32_t id, uint32_t opcode)
{
    wr(g, CGSI_CNTXT_SRC_CH_IRQ_CLR, ~0u);      /* gsi_irq_ch_ctrl_enable */
    wr(g, CGSI_CNTXT_SRC_CH_IRQ_MSK, 1u << id);
    type_update(g, g->type_enabled | CGS_IRQ_CH_CTRL);
    const int done = command(g, CGSI_CH_CMD, cgs_ch_cmd(id, opcode));
    type_update(g, g->type_enabled & ~CGS_IRQ_CH_CTRL);
    wr(g, CGSI_CNTXT_SRC_CH_IRQ_MSK, 0);
    if (!done) printf("COACHZ gsi: command %u for channel %u timed out, state %u\n", opcode, id, ch_state(g, id));
}

/* an event ring or channel command that must move the state from one value to another */
static int ev_transition(struct coachz_gsi *g, uint32_t id, uint32_t opcode, uint32_t from, uint32_t to)
{
    uint32_t s = ev_state(g, id);
    if (s != from) { printf("COACHZ gsi: event ring %u bad state %u before command %u\n", id, s, opcode); return -EINVAL; }
    evt_command(g, id, opcode);
    if ((s = ev_state(g, id)) == to) return 0;
    printf("COACHZ gsi: event ring %u bad state %u after command %u\n", id, s, opcode);
    return -EIO;
}
static int ch_transition(struct coachz_gsi *g, uint32_t id, uint32_t opcode, uint32_t from, uint32_t from2, uint32_t to)
{
    uint32_t s = ch_state(g, id);
    if (s != from && s != from2) { printf("COACHZ gsi: channel %u bad state %u before command %u\n", id, s, opcode); return -EINVAL; }
    ch_command(g, id, opcode);
    if ((s = ch_state(g, id)) == to) return 0;
    printf("COACHZ gsi: channel %u bad state %u after command %u\n", id, s, opcode);
    return -EIO;
}

/* gsi_generic_command: GP_INT1 enabled only while the command runs */
static int generic_command(struct coachz_gsi *g, uint32_t channel, uint32_t opcode)
{
    wr(g, CGSI_CNTXT_GLOB_IRQ_EN, CGS_ERROR_INT | CGS_GP_INT1);
    wr(g, CGSI_CNTXT_SCRATCH_0, rd(g, CGSI_CNTXT_SCRATCH_0) & ~CGSI_CNTXT_SCRATCH_0_GENERIC_EE_RESULT);
    g->result = 0;
    const int done = command(g, CGSI_GENERIC_CMD, cgs_generic_cmd(channel, opcode));
    wr(g, CGSI_CNTXT_GLOB_IRQ_EN, CGS_ERROR_INT);
    if (done) return g->result;
    printf("COACHZ gsi: generic command %u to channel %u timed out\n", opcode, channel);
    return -ETIMEDOUT;
}

/* ---------------------------------------------------------------- programming */
void coachz_gsi_evt_ring_doorbell(struct coachz_gsi *g, uint32_t id, uint32_t index)
{
    const struct cgs_channel *c = NULL;
    for (unsigned i = 0; i < CGS_CHANNEL_COUNT_MAX; i++)
        if (g->t.ch[i].defined && g->t.ch[i].evt_ring == id) c = &g->t.ch[i];
    if (!c) return;
    g->evt_index[id] = index;   /* next unused entry; the doorbell reports the one before it */
    wr(g, CH(EV_CH_E_DOORBELL_0, id), (uint32_t)ring_addr(g, 1, id) + ((index - 1u) % c->event_count) * CGS_ELEMENT);
}

void coachz_gsi_channel_doorbell(struct coachz_gsi *g, uint32_t channel, uint32_t index)
{
    const struct cgs_channel *c = &g->t.ch[channel];
    g->ch_index[channel] = index;
    wr(g, CH(CH_C_DOORBELL_0, channel), (uint32_t)ring_addr(g, 0, channel) + (index % c->tre_count) * CGS_ELEMENT);
}

void *coachz_gsi_ring_entry(struct coachz_gsi *g, uint32_t channel, int event, uint32_t index)
{
    const struct cgs_channel *c = &g->t.ch[channel];
    const uint32_t count = event ? c->event_count : c->tre_count;
    return g->ring_virt + (event ? g->place.evt_offset[c->evt_ring] : g->place.offset[channel]) + (index % count) * CGS_ELEMENT;
}

static void evt_ring_program(struct coachz_gsi *g, const struct cgs_channel *c)
{
    const uint32_t id = c->evt_ring;
    const uint64_t a = ring_addr(g, 1, id);
    wr(g, CH(EV_CH_E_CNTXT_0, id), cgs_ev_cntxt_0());
    wr(g, CH(EV_CH_E_CNTXT_1, id), cgs_ev_cntxt_1(c->event_count));
    wr(g, CH(EV_CH_E_CNTXT_2, id), (uint32_t)a);
    wr(g, CH(EV_CH_E_CNTXT_3, id), (uint32_t)(a >> 32));
    wr(g, CH(EV_CH_E_CNTXT_8, id), cgs_ev_cntxt_8());
    wr(g, CH(EV_CH_E_CNTXT_9, id), 0);    /* no MSI */
    wr(g, CH(EV_CH_E_CNTXT_10, id), 0);
    wr(g, CH(EV_CH_E_CNTXT_11, id), 0);
    wr(g, CH(EV_CH_E_CNTXT_12, id), 0);   /* no read pointer updates */
    wr(g, CH(EV_CH_E_CNTXT_13, id), 0);
    coachz_gsi_evt_ring_doorbell(g, id, g->evt_index[id]);
}

static void channel_program(struct coachz_gsi *g, uint32_t id)
{
    const struct cgs_channel *c = &g->t.ch[id];
    const uint64_t a = ring_addr(g, 0, id);
    wr(g, CH(CH_C_CNTXT_0, id), cgs_ch_cntxt_0(c));
    wr(g, CH(CH_C_CNTXT_1, id), cgs_ch_cntxt_1(c));
    wr(g, CH(CH_C_CNTXT_2, id), (uint32_t)a);
    wr(g, CH(CH_C_CNTXT_3, id), (uint32_t)(a >> 32));
    wr(g, CH(CH_C_QOS, id), cgs_ch_qos(c));
    wr(g, CH(CH_C_SCRATCH_0, id), 0);
    wr(g, CH(CH_C_SCRATCH_1, id), 0);
    wr(g, CH(CH_C_SCRATCH_2, id), cgs_ch_scratch_2(c));
    wr(g, CH(CH_C_SCRATCH_3, id), cgs_ch_scratch_3(rd(g, CH(CH_C_SCRATCH_3, id))));
}

/* gsi_channel_teardown_one's commands, used to unwind */
static void channel_teardown_one(struct coachz_gsi *g, uint32_t id)
{
    const struct cgs_channel *c = &g->t.ch[id];
    if (!c->defined) return;
    ch_transition(g, id, CGS_CH_DE_ALLOC, CGS_STATE_ALLOCATED, CGS_STATE_ALLOCATED, CGS_STATE_NOT_ALLOCATED);
    const uint32_t s = ev_state(g, c->evt_ring);
    if (s == CGS_STATE_ALLOCATED || s == CGS_STATE_ERROR) evt_command(g, c->evt_ring, CGS_EVT_RESET);
    ev_transition(g, c->evt_ring, CGS_EVT_DE_ALLOC, CGS_STATE_ALLOCATED, CGS_STATE_NOT_ALLOCATED);
}

static void modem_channel_halt(struct coachz_gsi *g, uint32_t channel)
{
    int err, retries = 10;   /* GSI_CHANNEL_MODEM_HALT_RETRIES */
    do err = generic_command(g, channel, CGS_GENERIC_HALT_CHANNEL); while (err == -EAGAIN && retries--);
    if (err) printf("COACHZ gsi: error %d halting modem channel %u\n", err, channel);
}

/* gsi_channel_setup */
static int channel_setup(struct coachz_gsi *g)
{
    int err = 0;
    uint32_t id = 0;
    wr(g, CGSI_CNTXT_GLOB_IRQ_EN, CGS_ERROR_INT);   /* gsi_irq_enable */
    type_update(g, g->type_enabled | CGS_IRQ_GLOB_EE);
    wr(g, CGSI_CNTXT_GSI_IRQ_EN, CGS_BUS_ERROR | CGS_CMD_FIFO_OVRFLOW | CGS_MCS_STACK_OVRFLOW);
    type_update(g, g->type_enabled | CGS_IRQ_GENERAL);
    for (; id < g->n.channels; id++) {   /* gsi_channel_setup_one */
        const struct cgs_channel *c = &g->t.ch[id];
        if (!c->defined) continue;
        if ((err = ev_transition(g, c->evt_ring, CGS_EVT_ALLOCATE, CGS_STATE_NOT_ALLOCATED, CGS_STATE_ALLOCATED))) goto unwind;
        evt_ring_program(g, c);
        if ((err = ch_transition(g, id, CGS_CH_ALLOCATE, CGS_STATE_NOT_ALLOCATED, CGS_STATE_NOT_ALLOCATED, CGS_STATE_ALLOCATED))) {
            ev_transition(g, c->evt_ring, CGS_EVT_DE_ALLOC, CGS_STATE_ALLOCATED, CGS_STATE_NOT_ALLOCATED);
            goto unwind;
        }
        channel_program(g, id);
    }
    for (uint32_t i = id; i < CGS_CHANNEL_COUNT_MAX; i++)
        if (g->t.ch[i].defined) { printf("COACHZ gsi: channel %u not supported by hardware\n", i); err = -EINVAL; goto unwind; }
    uint32_t mask = g->t.modem_channels, allocated = 0;   /* v4.2: the AP allocates the modem's channels */
    while (mask) {
        const uint32_t ch = (uint32_t)__builtin_ctz(mask);
        if ((err = generic_command(g, ch, CGS_GENERIC_ALLOCATE_CHANNEL))) {
            printf("COACHZ gsi: modem channel %u allocation failed, %d\n", ch, err);
            while (allocated) { const uint32_t h = 31u - (uint32_t)__builtin_clz(allocated); allocated ^= 1u << h; modem_channel_halt(g, h); }
            goto unwind;
        }
        mask ^= 1u << ch;
        allocated |= 1u << ch;
    }
    return 0;
unwind:
    while (id--) channel_teardown_one(g, id);
    type_update(g, 0);   /* gsi_irq_disable */
    wr(g, CGSI_CNTXT_GSI_IRQ_EN, 0);
    wr(g, CGSI_CNTXT_GLOB_IRQ_EN, 0);
    return err;
}

/* gsi_setup: GSI firmware loaded by the modem; gsi_irq_setup, gsi_ring_setup, the error log, the channels */
int coachz_gsi_setup(struct coachz_gsi *g)
{
    const uint32_t status = rd(g, CGSI_GSI_STATUS);
    if (!(status & CGSI_GSI_STATUS_ENABLED)) { printf("COACHZ gsi: GSI has not been enabled (0x%x)\n", status); return -EIO; }
    wr(g, CGSI_CNTXT_INTSET, CGSI_CNTXT_INTSET_INTYPE);   /* IRQ, not MSI */
    type_update(g, 0);
    wr(g, CGSI_CNTXT_SRC_CH_IRQ_MSK, 0);
    wr(g, CGSI_CNTXT_SRC_EV_CH_IRQ_MSK, 0);
    wr(g, CGSI_CNTXT_GLOB_IRQ_EN, 0);
    wr(g, CGSI_CNTXT_SRC_IEOB_IRQ_MSK, 0);
    wr(g, CGSI_INTER_EE_SRC_CH_IRQ_MSK, 0);
    wr(g, CGSI_INTER_EE_SRC_EV_CH_IRQ_MSK, 0);
    wr(g, CGSI_CNTXT_GSI_IRQ_EN, 0);
    const uint32_t hw = rd(g, CGSI_HW_PARAM_2);
    g->n = cgs_ring_counts(hw);
    printf("COACHZ gsi: HW_PARAM_2 0x%08x: %u channels, %u event rings\n", hw, g->n.channels, g->n.evt_rings);
    if (!g->n.channels || !g->n.evt_rings) return -EINVAL;
    wr(g, CGSI_ERROR_LOG, 0);
    const int err = channel_setup(g);
    if (!err) {
        printf("COACHZ gsi: setup done; AP channel states");
        for (uint32_t id = 0; id < g->n.channels; id++)
            if (g->t.ch[id].defined) printf(" %u:%u/ev%u:%u", id, ch_state(g, id), g->t.ch[id].evt_ring, ev_state(g, g->t.ch[id].evt_ring));
        printf(", modem channels 0x%x\n", g->t.modem_channels);
    }
    return err;
}

/* gsi_channel_start: the event ring's IEOB interrupt, then START from ALLOCATED or STOPPED */
int coachz_gsi_channel_start(struct coachz_gsi *g, uint32_t id)
{
    const struct cgs_channel *c = &g->t.ch[id];
    if (!c->defined) return -EINVAL;
    const int first = !g->ieob_enabled;
    g->ieob_enabled |= 1u << c->evt_ring;
    wr(g, CGSI_CNTXT_SRC_IEOB_IRQ_MSK, g->ieob_enabled);
    if (first) type_update(g, g->type_enabled | CGS_IRQ_IEOB);
    const int err = ch_transition(g, id, CGS_CH_START, CGS_STATE_ALLOCATED, CGS_STATE_STOPPED, CGS_STATE_STARTED);
    if (err) {
        g->ieob_enabled &= ~(1u << c->evt_ring);
        if (!g->ieob_enabled) type_update(g, g->type_enabled & ~CGS_IRQ_IEOB);
        wr(g, CGSI_CNTXT_SRC_IEOB_IRQ_MSK, g->ieob_enabled);
    }
    return err;
}

/* gsi_irq_ieob_enable_one: after a poll finds no more work (napi_complete), the ring interrupts again */
void coachz_gsi_ieob_enable_one(struct coachz_gsi *g, uint32_t ring)
{
    const int first = !g->ieob_enabled;
    g->ieob_enabled |= 1u << ring;
    wr(g, CGSI_CNTXT_SRC_IEOB_IRQ_MSK, g->ieob_enabled);
    if (first) type_update(g, g->type_enabled | CGS_IRQ_IEOB);
}
