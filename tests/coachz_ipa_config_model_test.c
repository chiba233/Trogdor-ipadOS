/* Host model of the pending IPA v4.2 probe-time configuration (platform/coachz-prelink/pending/modem). Expected values
   are worked out from mainline ipa_main.c / ipa_mem.c rules and the generated v4.2 register table. */
#include <stdio.h>
#include "coachz_ipa_proto.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define GROUP(name, body) do { const int before = failures; body; if (failures == before) printf("PASS %s\n", name); } while (0)

static void hardware(void)
{
    CHECK(cip_encode(0x01f0u, 15) == 0xf0u && cip_decode(0xffff0000u, 0x07c00400u) == 0x7c0u);
    /* DIV_ROUND_CLOSEST(500 * 32000, 1000000) - 1 = 15, in COUNTER_CFG bits 8:4 */
    CHECK(cip_aggr_granularity_val(500) == 15 && cip_counter_cfg() == 0xf0u);
    CHECK(cip_comp_cfg(0) == 0x00008180u && cip_comp_cfg(0x3u) == 0x00008183u);
    CHECK(cip_clkon() == 0x30000000u && cip_tx_cfg(0x00001fffu) == 0x00000fffu && cip_bcr() == 0);
    CHECK(cip_qsb_writes() == 8u && cip_qsb_reads() == 12u && cip_idle_indication() == 256u);
    CHECK(CIPA_VERSION == 6 && CIPA_CORE_CLOCK_RATE == 100000000u && CIPA_MODEM_ROUTE_COUNT == CI_V42_MODEM_ROUTE_COUNT);
}

static void memory(void)
{
    struct cip_mem m = cip_mem_config(0x00000400u, CIP_SHARED_BYTES);   /* BADDR 0, SIZE 0x400 x 8 = 0x2000 */
    CHECK(m.mem_offset == 0 && m.mem_size == 0x2000u && cip_mem_valid(m.mem_size));
    m = cip_mem_config(0x00000200u, CIP_SHARED_BYTES);                  /* reported 0x1000: limits, and the table no longer fits */
    CHECK(m.mem_size == 0x1000u && !cip_mem_valid(m.mem_size));
    m = cip_mem_config(0x00000415u, CIP_SHARED_BYTES);                  /* read on hardware: offset 0, 8360 bytes reported */
    CHECK(m.mem_offset == 0 && m.mem_size == 0x2000u && cip_mem_valid(m.mem_size) && cip_canary_offset(m.mem_offset, CI_V4_FILTER, 1) == 0x288u);
    m = cip_mem_config(0x00010800u, CIP_SHARED_BYTES);                  /* BADDR 1 (8 bytes), SIZE 0x4000: larger, ignored */
    CHECK(m.mem_offset == 8u && m.mem_size == 0x2000u);
    CHECK(cip_mem_valid(0x1ffcu) && !cip_mem_valid(0x1ff8u));             /* the modem region ends at 0xbf0 + 0x140c */
    CHECK(cip_canary_offset(0, CI_V4_FILTER, 0) == 0x28cu && cip_canary_offset(0, CI_V4_FILTER, 1) == 0x288u);
    CHECK(cip_canary_offset(0x10, CI_PDN_CONFIG, 0) == 0x10u + 0x9f8u - 4u);
}

static void endpoints(void)
{
    const struct cipa_endpoint *cmd = &cipa_endpoints[CIPA_EP_AP_COMMAND_TX], *lan = &cipa_endpoints[CIPA_EP_AP_LAN_RX];
    CHECK(cmd->endpoint == CI_EP_AP_COMMAND_TX && cmd->toward_ipa && cmd->dma_mode && cmd->dma_endpoint == CIPA_EP_AP_LAN_RX);
    CHECK(lan->endpoint == CI_EP_AP_LAN_RX && lan->aggregation && lan->status_enable && lan->rx_buffer_size == 8192 && lan->rx_aggr_time_limit == 500);
    CHECK(cipa_endpoints[CIPA_EP_AP_MODEM_TX].endpoint == CI_EP_AP_MODEM_TX && cipa_endpoints[CIPA_EP_AP_MODEM_TX].qmap);
    CHECK(cipa_endpoints[CIPA_EP_AP_MODEM_RX].endpoint == CI_EP_AP_MODEM_RX && cipa_endpoints[CIPA_EP_AP_MODEM_RX].rx_aggr_close_eof);
    for (unsigned i = 0; i < sizeof cipa_endpoints / sizeof cipa_endpoints[0]; i++)
        if (cipa_endpoints[i].tre_count) CHECK(!(cipa_endpoints[i].tre_count & (cipa_endpoints[i].tre_count - 1u)));
}

static void checks(void)
{
    /* FLAVOR_0 fixture: 7 consumer (TX) pipes 0-6, 5 producer (RX) pipes from 7: every v4.2 endpoint (TX 1 4 5 6, RX 8 9 10 11) fits */
    const uint32_t flavor = 7u << 8 | 5u << 16 | 7u << 24;
    const struct cip_flavor f = cip_flavor(flavor);
    CHECK(f.tx_count == 7 && f.rx_count == 5 && f.rx_base == 7 && f.limit == 12 && cip_endpoint_check(flavor) == 0);
    CHECK(cip_endpoint_check(5u << 8 | 5u << 16 | 7u << 24) == CI_EP_AP_COMMAND_TX + 1u);   /* TX 6 beyond 5 consumers */
    CHECK(cip_endpoint_check(7u << 8 | 4u << 16 | 7u << 24) == 11u + 1u);                    /* RX 11 beyond the limit */
    CHECK(cip_endpoint_check(9u << 8 | 3u << 16 | 9u << 24) == CI_EP_AP_LAN_RX + 1u);        /* RX 8 is a TX pipe: wrong direction */
    CHECK(cip_endpoint_check(7u << 8 | 31u << 16 | 15u << 24) == CIP_ENDPOINT_MAX + 1u);     /* limit 46 */
    CHECK(cip_endpoint_check(0x08090811u) == 0);   /* FLAVOR_0 read on Debian: 8 TX, 9 RX from 8 */
    CHECK(!cip_endpoint_defined(&cipa_endpoints[5]) && cip_endpoint_defined(&cipa_endpoints[4]));

    /* resources: one group each way, so X limits only */
    CHECK(cip_resource_valid(cipa_resource_src, CIP_RSRC_SRC_TYPES, CIPA_RSRC_GROUP_SRC_COUNT));
    CHECK(cip_resource_valid(cipa_resource_dst, CIP_RSRC_DST_TYPES, CIPA_RSRC_GROUP_DST_COUNT));
    CHECK(!cip_resource_valid(cipa_resource_src, CIP_RSRC_SRC_TYPES, 0) && !cip_resource_valid(cipa_resource_src, CIP_RSRC_SRC_TYPES, 9));
    static const uint32_t stray[1][8][2] = { { { 1, 2 }, { 0, 1 } } };
    CHECK(!cip_resource_valid(stray, 1, 1) && cip_resource_valid(stray, 1, 2));
    CHECK(cip_resource_value(cipa_resource_src[0], 0, 1) == 0x3f03u && cip_resource_value(cipa_resource_dst[1], 0, 1) == 0x3f01u);
    CHECK(cip_resource_value(stray[0], 0, 2) == (0x0201u | 0x01000000u));
    CHECK(CIP_RSRC_SRC_TYPES == 5 && CIP_RSRC_DST_TYPES == 2);

    /* uC response: INIT_COMPLETED only counts while the proxy vote is held; the reserved bytes are ignored */
    CHECK(cip_uc_response_loads(1, 1) && cip_uc_response_loads(0xabcdef01u, 1) && !cip_uc_response_loads(1, 0) && !cip_uc_response_loads(2, 1));
    CHECK(CIPA_IPA_IRQ_EN - CIPA_IPA_IRQ_STTS == 4 && CIPA_IPA_IRQ_CLR - CIPA_IPA_IRQ_STTS == 8);
}

int main(void)
{
    GROUP("hardware config", hardware());
    GROUP("memory config", memory());
    GROUP("endpoint data", endpoints());
    GROUP("endpoint, resource and uC checks", checks());
    return failures != 0;
}
