/* Host model of the pending IPA RX endpoint (platform/coachz-prelink/pending/modem/coachz_ipa_rx_proto.h): buffer TREs,
   the replenish doorbell rule, status parsing as ipa_endpoint_status_parse does, and the default route, compared with the
   ROUTE value read on Debian with mainline's IPA driver set up on this machine. */
#include <stdio.h>
#include <string.h>
#include "coachz_ipa_rx_proto.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define GROUP(name, body) do { const int before = failures; body; if (failures == before) printf("PASS %s\n", name); } while (0)

static void status_put(uint8_t *p, uint32_t opcode, uint32_t exception, uint32_t mask, uint32_t length, uint32_t src, uint32_t dst, uint32_t rule)
{
    memset(p, 0, CIR_STATUS_SIZE);
    cic_le32(p, opcode | exception << 8 | mask << 16);
    cic_le32(p + 4, length | src << 16 | dst << 24);
    cic_le32(p + 8, 0x12345678u);
    cic_le32(p + 12, rule << 22);
}

static void buffers(void)
{
    const struct cipa_endpoint *lan = &cipa_endpoints[CIPA_EP_AP_LAN_RX];
    const struct cgs_table t = cgs_channel_table();
    const struct cgs_channel *c = &t.ch[lan->channel];
    CHECK(cir_buffer_size(lan) == 8192 && cir_dma_len(lan) == 8128);
    CHECK(cgs_tre_max(c) == 251 && cir_capacity(c, 256) == 251 && cir_capacity(c, 16) == 16);
    uint8_t tre[16];
    cir_tre(tre, 0x81400000ull, lan);
    CHECK(cic_rd64(tre) == 0x81400040ull && tre[8] == 0xc0 && tre[9] == 0x1f && cic_rd32(tre + 12) == 0x00020200u);
    /* 251 buffers: doorbells after 16, 32, ... 240, then at 251 when no TRE is left */
    unsigned rings = 0;
    for (uint32_t n = 1; n <= 251; n++) rings += cir_doorbell(n, 251 - n);
    CHECK(rings == 16 && cir_doorbell(251, 0) && !cir_doorbell(250, 1) && cir_doorbell(16, 235));
    const uint8_t ev[16] = { 0x40, 0, 0x40, 0x81, 0, 0, 0, 0, 0x20, 0x01, 0, 1, 0, 0, 0x11, 2 };
    CHECK(cir_event_len(ev) == 0x120 && cic_event_chid(ev) == 2);
}

static void parsing(void)
{
    const struct cipa_endpoint *lan = &cipa_endpoints[CIPA_EP_AP_LAN_RX];
    uint8_t buf[256];
    status_put(buf, 1, 0, 0x10, 24, 6, 8, 0);                  /* pipeline clear: tagged, from the command endpoint */
    struct cir_packet p = cir_status_step(buf, 256, lan);
    CHECK(p.kind == CIR_TAG && p.consumed == 32 + 24 && p.length == 24 && p.status.metadata == 0x12345678u);
    status_put(buf, 1, 0, 0x10, 24, 1, 8, 0);
    CHECK(cir_status_step(buf, 256, lan).kind == CIR_TAG_UNEXPECTED);
    status_put(buf, 1, 0, 0, 0, 6, 8, 0);                       /* no packet data */
    p = cir_status_step(buf, 256, lan);
    CHECK(p.kind == CIR_SKIP && p.consumed == 32);
    status_put(buf, 1, 0, 0, 40, 1, 9, 0);                      /* for another endpoint */
    CHECK(cir_status_step(buf, 256, lan).kind == CIR_SKIP);
    status_put(buf, 16, 0, 0, 40, 1, 8, 0);                     /* LOG: not a packet format */
    CHECK(cir_status_step(buf, 256, lan).kind == CIR_SKIP);
    status_put(buf, 1, 0, 0, 41, 1, 8, CIR_RULE_MISS);          /* routing miss; length aligned to pad_align 2 */
    p = cir_status_step(buf, 256, lan);
    CHECK(p.kind == CIR_DROP && p.consumed == 32 + 42);
    status_put(buf, 4, 1, 0, 40, 1, 8, 0);                      /* deaggregation exception */
    CHECK(cir_status_step(buf, 256, lan).kind == CIR_DROP);
    status_put(buf, 1, 4, 0, 40, 1, 8, 0);                      /* any other exception is consumed */
    CHECK(cir_status_step(buf, 256, lan).kind == CIR_DELIVER);
    status_put(buf, 1, 0, 0, 40, 1, 8, 5);
    p = cir_status_step(buf, 256, lan);
    CHECK(p.kind == CIR_DELIVER && p.data_offset == 32 && p.length == 40);
    CHECK(cir_status_step(buf, 31, lan).kind == CIR_SHORT);
    status_put(buf, 1, 0, 0, 200, 1, 8, 5);
    CHECK(cir_status_step(buf, 100, lan).consumed == 100);      /* never past the received bytes */
    const struct cipa_endpoint *modem = &cipa_endpoints[CIPA_EP_AP_MODEM_RX];
    status_put(buf, 1, 0, 0, 41, 9, 9, 5);                      /* checksum offload adds the 8-byte trailer, no padding */
    CHECK(cir_status_step(buf, 256, modem).consumed == 32 + 41 + 8);
}

static void route(void)
{
    CHECK(cir_default_route(8) == 0x01100050u);                 /* Debian's ROUTE */
    CHECK(cir_default_route(0) == 0x01000040u);                 /* ipa_endpoint_default_route_clear */
}

int main(void)
{
    GROUP("buffers and doorbells", buffers());
    GROUP("status parsing", parsing());
    GROUP("default route against Debian", route());
    return failures != 0;
}
