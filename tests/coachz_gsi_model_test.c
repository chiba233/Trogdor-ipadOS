/* Host model of the pending GSI setup (platform/coachz-prelink/pending/modem/coachz_gsi_proto.h). The register values
   expected here were read on Debian with mainline's IPA driver set up on this machine: CH_C/EV_CH_E contexts of the four
   AP channels, QOS, scratch, HW_PARAM_2 and CNTXT_SCRATCH_0; hardware-owned state, channel id and EE fields are masked. */
#include <stdio.h>
#include "coachz_gsi_proto.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define GROUP(name, body) do { const int before = failures; body; if (failures == before) printf("PASS %s\n", name); } while (0)

static const uint32_t debian_ch0[4] = { 0x1010800au, 0x1020010au, 0x10204202u, 0x1010c302u };   /* CH_C_CNTXT_0[0..3] */
static const uint32_t debian_ch1[4] = { 0x2000u, 0x1000u, 0x1000u, 0x1000u };
static const uint32_t debian_qos[4] = { 0x400u, 0xfu, 0x400u, 0x400u };
static const uint32_t debian_scratch2[4] = { 0x00800000u, 0x01400000u, 0x00600000u, 0x00600000u };
static const uint32_t debian_ev0[4] = { 0x10110002u, 0x10110102u, 0x10110202u, 0x10110302u };    /* EV_CH_E_CNTXT_0[0..3] */
static const uint32_t debian_ev1[4] = { 0x1000u, 0x1000u, 0x2000u, 0x1000u };

static void table(void)
{
    const struct cgs_table t = cgs_channel_table();
    CHECK(t.bad == 0 && t.modem_channels == 0xfu && t.event_bitmap == (0xf001fc00u | 0xfu));
    CHECK(cgs_event_bitmap_init(28) == 0xf001fc00u);
    /* endpoint-name order hands out event rings: COMMAND_TX ch1 -> 0, LAN_RX ch2 -> 1, MODEM_TX ch0 -> 2, MODEM_RX ch3 -> 3 */
    CHECK(t.ch[1].command && t.ch[1].evt_ring == 0 && t.ch[2].evt_ring == 1 && t.ch[0].evt_ring == 2 && t.ch[3].evt_ring == 3);
    CHECK(t.ch[0].tre_count == 512 && t.ch[1].tre_count == 256 && t.ch[1].trans_tre_max == 20 && cgs_tre_max(&t.ch[1]) == 237);
    for (unsigned i = 4; i < CGS_CHANNEL_COUNT_MAX; i++) CHECK(!t.ch[i].defined);
    struct cipa_endpoint e = cipa_endpoints[CIPA_EP_AP_COMMAND_TX];
    CHECK(cgs_channel_valid(&e, 1));
    e.tlv_count = 7; CHECK(!cgs_channel_valid(&e, 1) && cgs_channel_valid(&e, 0));   /* command needs 8 TLVs */
    e.tlv_count = 20; e.tre_count = 32; CHECK(!cgs_channel_valid(&e, 1));           /* fewer than 2 * 20 - 1 TREs */
    e.tre_count = 384; CHECK(!cgs_channel_valid(&e, 1));                            /* not a power of 2 */
    e = cipa_endpoints[CIPA_EP_AP_LAN_RX]; CHECK(!cgs_channel_valid(&e, 1));       /* command must be TX */
}

static void programming(void)
{
    const struct cgs_table t = cgs_channel_table();
    const uint32_t hw = CGSI_CH_C_CNTXT_0_CHSTATE | CGSI_CH_C_CNTXT_0_CHID | CGSI_CH_C_CNTXT_0_CH_EE;
    const uint32_t ev_hw = CGSI_EV_CH_E_CNTXT_0_EV_CHSTATE | CGSI_EV_CH_E_CNTXT_0_EV_EVCHID;
    for (unsigned i = 0; i < 4; i++) {
        CHECK(cgs_ch_cntxt_0(&t.ch[i]) == (debian_ch0[i] & ~hw));
        CHECK(cgs_ch_cntxt_1(&t.ch[i]) == debian_ch1[i]);
        CHECK(cgs_ch_qos(&t.ch[i]) == debian_qos[i]);
        CHECK(cgs_ch_scratch_2(&t.ch[i]) == debian_scratch2[i]);
        CHECK(cgs_ch_scratch_3(0) == 0x00200000u);
        CHECK(cgs_ev_cntxt_0() == (debian_ev0[i] & ~ev_hw) && cgs_ev_state(debian_ev0[i]) == CGS_STATE_ALLOCATED);
    }
    for (unsigned i = 0; i < 4; i++) CHECK(cgs_ev_cntxt_1(t.ch[i].event_count) == debian_ev1[t.ch[i].evt_ring]);
    CHECK(cgs_ev_cntxt_8() == 0x00010020u && cgs_ch_scratch_3(0xabcd1234u) == 0x00201234u);
    CHECK(cgs_state(debian_ch0[1]) == CGS_STATE_STARTED && cgs_state(debian_ch0[0]) == CGS_STATE_ALLOCATED);
    const struct cgs_counts n = cgs_ring_counts(0xe4046b59u);
    CHECK(n.channels == 11 && n.evt_rings == 11);
}

static void commands(void)
{
    CHECK(cgs_ch_cmd(1, CGS_CH_START) == 0x01000001u && cgs_ev_cmd(3, CGS_EVT_ALLOCATE) == 3u && cgs_ev_cmd(0, CGS_EVT_DE_ALLOC) == 0x0a000000u);
    CHECK(cgs_generic_cmd(2, CGS_GENERIC_ALLOCATE_CHANNEL) == 0x442u);
    CHECK(cgs_generic_result(0x111c4335u) == 0);                 /* Debian's last generic command: SUCCESS */
    CHECK(cgs_generic_result(2u << 5) == 0 && cgs_generic_result(6u << 5) == -CGS_EAGAIN && cgs_generic_result(7u << 5) == -CGS_EIO);
}

static void rings(void)
{
    const struct cgs_table t = cgs_channel_table();
    struct cgs_ring_place p;
    CHECK(cgs_place_rings(&t, 0xa000u, &p) == 0xa000u && cgs_place_rings(&t, 0x9fffu, &p) == 0);
    cgs_place_rings(&t, 0x10000u, &p);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(p.offset[i] % (t.ch[i].tre_count * CGS_ELEMENT) == 0);
        CHECK(p.evt_offset[t.ch[i].evt_ring] % (t.ch[i].event_count * CGS_ELEMENT) == 0);
    }
    CHECK(p.offset[0] == 0 && p.evt_offset[2] == 0x2000u);
}

int main(void)
{
    GROUP("channel table", table());
    GROUP("programming against Debian", programming());
    GROUP("commands", commands());
    GROUP("ring placement", rings());
    return failures != 0;
}
