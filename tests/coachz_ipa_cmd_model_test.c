/* Host model of the pending IPA command layer (platform/coachz-prelink/pending/modem/coachz_ipa_cmd_proto.h). Endpoint
   register values are compared with those read on Debian with mainline's IPA driver set up on this machine; command
   payloads and TREs follow ipa_cmd.c and gsi_trans.c field by field. */
#include <stdio.h>
#include "coachz_ipa_cmd_proto.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define GROUP(name, body) do { const int before = failures; body; if (failures == before) printf("PASS %s\n", name); } while (0)

/* Debian: ENDP_INIT_{CFG,NAT,HDR,MODE,AGGR,HOL_BLOCK_EN,DEAGGR,RSRC_GRP,SEQ}, ENDP_STATUS for endpoints 1, 6, 8, 9 */
static const struct { unsigned name; uint32_t v[10]; } debian[4] = {
    { CIPA_EP_AP_MODEM_TX,   { 0x0au, 0, 0x48u, 0, 0, 0, 0, 0, 0x806u, 0x15u } },
    { CIPA_EP_AP_COMMAND_TX, { 0, 0, 0, 0x83u, 0, 0, 0, 0, 0, 0 } },
    { CIPA_EP_AP_LAN_RX,     { 0, 0, 0, 0, 0x4d1u, 0, 0, 0, 0, 0x1u } },
    { CIPA_EP_AP_MODEM_RX,   { 0x4u, 0, 0x002800c4u, 0, 0x002004d1u, 0, 0, 0, 0, 0 } },
};
static const uint32_t regs[10] = { CIPA_ENDP_INIT_CFG, CIPA_ENDP_INIT_NAT, CIPA_ENDP_INIT_HDR, CIPA_ENDP_INIT_MODE, CIPA_ENDP_INIT_AGGR,
    CIPA_ENDP_INIT_HOL_BLOCK_EN, CIPA_ENDP_INIT_DEAGGR, CIPA_ENDP_INIT_RSRC_GRP, CIPA_ENDP_INIT_SEQ, CIPA_ENDP_STATUS };

static int find(const struct cic_write *w, unsigned n, uint32_t offset, uint32_t *value)
{
    for (unsigned i = 0; i < n; i++) if (w[i].offset == offset) { *value = w[i].value; return 1; }
    return 0;
}

static void endpoints(void)
{
    for (unsigned k = 0; k < 4; k++) {
        const struct cipa_endpoint *e = &cipa_endpoints[debian[k].name];
        struct cic_write w[CIC_ENDPOINT_WRITES_MAX];
        const unsigned n = cic_endpoint_program(e, w);
        CHECK(n <= CIC_ENDPOINT_WRITES_MAX);
        for (unsigned r = 0; r < 10; r++) {
            uint32_t v = 0;
            const int written = find(w, n, regs[r] + e->endpoint * 0x70u, &v);
            CHECK(written ? v == debian[k].v[r] : debian[k].v[r] == 0);   /* a register not written for this direction reads 0 */
        }
    }
    struct cic_write w[CIC_ENDPOINT_WRITES_MAX];
    uint32_t v = 0;
    unsigned n = cic_endpoint_program(&cipa_endpoints[CIPA_EP_AP_MODEM_RX], w);
    CHECK(find(w, n, CIPA_ENDP_INIT_HDR_METADATA_MASK + 9u * 0x70u, &v) && v == 0xff000000u);
    CHECK(find(w, n, CIPA_ENDP_INIT_HDR_EXT + 9u * 0x70u, &v) && v == 0xbu);            /* big endian, total len valid, inc padding */
    n = cic_endpoint_program(&cipa_endpoints[CIPA_EP_AP_LAN_RX], w);
    CHECK(find(w, n, CIPA_ENDP_INIT_HDR_EXT + 8u * 0x70u, &v) && v == 2u << 10);        /* pad_align 2 */
    CHECK(!find(w, n, CIPA_ENDP_INIT_MODE + 8u * 0x70u, &v));                             /* MODE is TX only */
    CHECK(cic_aggr_size_kb(8192, 0) == 6 && cic_aggr_size_kb(8192, 1) == 7);
}

static void transactions(void)
{
    uint8_t tre[16];
    cic_tre(tre, 0x81234560ull, 16, CIC_CMD_HDR_INIT_LOCAL, 0, 1);
    CHECK(cic_rd64(tre) == 0x81234560ull && tre[8] == 0x09 && tre[9] == 0 && tre[10] == 0 && cic_rd32(tre + 12) == 0x00030001u);
    cic_tre(tre, 0x1000, 16, CIC_CMD_DMA_SHARED_MEM, 1, 1);
    CHECK(tre[8] == 0x13 && cic_rd32(tre + 12) == 0x00030600u);
    cic_tre(tre, 0x2000, 24, CIC_CMD_NONE, 1, 0);
    CHECK(tre[8] == 24 && cic_rd32(tre + 12) == 0x00020200u);                            /* data transfer, RX: no BEI */
    const uint8_t ev[16] = { 0x40, 0x30, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0x11, 1 };
    CHECK(cic_event_tre(ev) == 0xffff3040u && cic_event_chid(ev) == 1 && cic_event_code(ev) == 1);
}

static void commands(void)
{
    struct cic_cmd c = cic_hdr_init_local(0, 0x4a8, 0x140, 0x8170c000ull);
    CHECK(c.opcode == 9 && c.len == 16 && cic_rd64(c.payload) == 0x8170c000ull && cic_rd32(c.payload + 8) == (0x140u | 0x4a8u << 12));
    c = cic_dma_shared_mem(0, 0xbf0, 0x140c, 0x8170c000ull, 1);
    CHECK(c.opcode == 0x13 && cic_rd32(c.payload) == 0x140c0000u && cic_rd32(c.payload + 4) == 0x00000bf0u && cic_rd64(c.payload + 8) == 0x8170c000ull);
    c = cic_dma_shared_mem(8, 0x10, 4, 0, 0);
    CHECK(cic_rd32(c.payload + 4) == 0x00010018u);                                        /* read from IPA, offset + mem_offset */
    c = cic_register_write(0x12345, 0xabcd, 0xffff, 1);
    CHECK(c.opcode == (0xcu | 2u << 9) && cic_rd32(c.payload) == (0x2345u << 16 | 1u << 11) && cic_rd32(c.payload + 4) == 0xabcd && cic_rd32(c.payload + 8) == 0xffff);
    c = cic_table_init(CIC_CMD_IP_V4_FILTER_INIT, 0, 0x290, 24, 0x81000008ull);
    CHECK(c.len == 24 && cic_rd64(c.payload) == 0 && cic_rd64(c.payload + 8) == (24ull << 28 | 0x290ull << 40) && cic_rd64(c.payload + 16) == 0x81000008ull);
    struct cic_cmd p[4];
    CHECK(cic_pipeline_clear(p) == 4 && p[0].opcode == 0x40cu && p[1].payload[0] == 8 && p[2].opcode == 0x14 && p[3].opcode == 0 && p[3].len == 24);
    CHECK(cic_cmd_init_valid(0) && !cic_cmd_init_valid(0xffffu));
}

static void setup_lists(void)
{
    struct cic_cmd c[8];
    CHECK(cic_mem_setup(0, 0x81100000ull, c) == 4);
    CHECK(c[0].opcode == CIC_CMD_HDR_INIT_LOCAL && (cic_rd32(c[0].payload + 8) & 0xfffu) == 0x140u);
    CHECK(cic_rd32(c[1].payload + 4) == 0x5f0u && cic_rd32(c[2].payload + 4) == 0x7f0u && cic_rd32(c[3].payload + 4) == 0xbf0u);
    CHECK(cic_local_pkt_proc_cntxt(0) == 0x5f0u);                                         /* Debian's LOCAL_PKT_PROC_CNTXT */
    CHECK(cic_filtered() == 0x12u && cic_filter_count() == 14 && cic_route_count() == 15 && cic_table_valid());
    CHECK(cic_table_bytes() == 136u);
    uint8_t table[136];
    cic_table_fill(table, 0x81200000ull);
    CHECK(cic_rd64(table) == 0 && cic_rd64(table + 8) == 0x24u && cic_rd64(table + 16) == 0x81200000ull && cic_rd64(table + 128) == 0x81200000ull);
    CHECK(cic_table_setup(0, 0x81200000ull, 0x81100000ull, c) == 6);
    CHECK(c[0].opcode == 3 && cic_rd64(c[0].payload + 8) == (24ull << 28 | 0x290ull << 40) && cic_rd64(c[0].payload + 16) == 0x81200008ull);
    CHECK(c[1].opcode == 0x13 && cic_rd32(c[1].payload) == 0x00600000u && cic_rd32(c[1].payload + 4) == 0x2a8u);
    CHECK(c[2].opcode == 4 && c[3].opcode == 0x13 && cic_rd32(c[3].payload + 4) == 0x318u + 24u);
    CHECK(c[4].opcode == 7 && cic_rd64(c[4].payload + 8) == (0x78ull << 28 | 0x3a0ull << 40) && cic_rd64(c[4].payload + 16) == 0x81200010ull);
    CHECK(c[5].opcode == 8 && cic_rd64(c[5].payload + 8) == (0x78ull << 28 | 0x428ull << 40));
}

int main(void)
{
    GROUP("endpoint programming against Debian", endpoints());
    GROUP("TREs and events", transactions());
    GROUP("immediate commands", commands());
    GROUP("memory and table setup", setup_lists());
    return failures != 0;
}
