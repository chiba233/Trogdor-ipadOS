/* Host model of the pending cellular data path (platform/coachz-prelink/pending/modem/coachz_ipa_modem_proto.h): rmnet's
   MAPv4 egress and ingress deaggregation as rmnet_map_data.c and rmnet_handlers.c do them, the MTUs, the data format the
   IPA endpoints fix, and the TX TRE. */
#include <stdio.h>
#include <string.h>
#include "coachz_ipa_modem_proto.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define GROUP(name, body) do { const int before = failures; body; if (failures == before) printf("PASS %s\n", name); } while (0)

static void format(void)
{
    CHECK(CIM_DATA_FORMAT == 0x0du && cim_format_matches_endpoints(CIM_DATA_FORMAT));
    CHECK(!cim_format_matches_endpoints(CIM_INGRESS_DEAGGREGATION));          /* the endpoints have checksum offload */
    CHECK(cim_headroom(CIM_DATA_FORMAT) == 8 && cim_data_mtu(CIM_DATA_FORMAT) == 1492 && cim_data_mtu(0) == 1496);
    uint8_t tre[16];
    cim_tx_tre(tre, 0x81500000ull, 72);
    CHECK(cic_rd64(tre) == 0x81500000ull && tre[8] == 72 && cic_rd32(tre + 12) == 0x00020600u);
}

static void egress(void)
{
    uint8_t ip[60], out[2048];
    for (unsigned i = 0; i < sizeof ip; i++) ip[i] = (uint8_t)(0x45 + i);
    const uint32_t n = cim_egress(out, sizeof out, ip, sizeof ip, 1, CIM_DATA_FORMAT);
    CHECK(n == 68 && out[0] == 0 && out[1] == 1 && out[2] == 0 && out[3] == 60);
    CHECK(!out[4] && !out[5] && !out[6] && !out[7] && !memcmp(out + 8, ip, sizeof ip));
    CHECK(cim_egress(out, sizeof out, ip, sizeof ip, 1, 0) == 64 && out[3] == 60 && !memcmp(out + 4, ip, sizeof ip));
    CHECK(!cim_egress(out, 67, ip, sizeof ip, 1, CIM_DATA_FORMAT) && !cim_egress(out, sizeof out, ip, 0, 1, CIM_DATA_FORMAT));
    CHECK(!cim_egress(out, sizeof out, ip, sizeof ip, 255, CIM_DATA_FORMAT));
    CHECK(cim_tx_acceptable(68, 2048) && !cim_tx_acceptable(0, 2048) && !cim_tx_acceptable(2049, 2048));
}

static void ingress(void)
{
    /* an aggregate as AP_MODEM_RX delivers it: [hdr pad 0 mux 1 len 20][IPv4 20][trailer 8]
       [hdr pad 2 mux 1 len 42][IPv6 40 + 2 pad][trailer 8][hdr cmd mux 1 len 4][4][trailer 8][hdr len 100 (truncated)] */
    uint8_t buf[256];
    memset(buf, 0, sizeof buf);
    uint32_t o = 0;
    buf[o] = 0; buf[o + 1] = 1; buf[o + 3] = 20; buf[o + 4] = 0x45; o += 4 + 20 + 8;
    buf[o] = 2; buf[o + 1] = 1; buf[o + 3] = 42; buf[o + 4] = 0x60; o += 4 + 42 + 8;
    buf[o] = 0x80; buf[o + 1] = 1; buf[o + 3] = 4; o += 4 + 4 + 8;
    buf[o + 1] = 1; buf[o + 3] = 100; o += 4 + 10;
    uint8_t mux[CIM_MAX_LOGICAL_EP] = { 0 };
    mux[1] = 1;
    const uint8_t *p = buf;
    uint32_t resid = o;
    struct cim_packet k = cim_ingress_next(p, resid, CIM_DATA_FORMAT);
    CHECK(k.consumed == 32 && k.mux_id == 1 && k.ip_offset == 4 && k.ip_len == 20 && k.ip_version == 4 && cim_ingress_deliverable(&k, CIM_DATA_FORMAT, mux));
    p += k.consumed; resid -= k.consumed;
    k = cim_ingress_next(p, resid, CIM_DATA_FORMAT);
    CHECK(k.consumed == 54 && k.ip_len == 40 && k.ip_version == 6 && cim_ingress_deliverable(&k, CIM_DATA_FORMAT, mux));
    p += k.consumed; resid -= k.consumed;
    k = cim_ingress_next(p, resid, CIM_DATA_FORMAT);
    CHECK(k.consumed == 16 && k.command && !cim_ingress_deliverable(&k, CIM_DATA_FORMAT, mux));
    p += k.consumed; resid -= k.consumed;
    k = cim_ingress_next(p, resid, CIM_DATA_FORMAT);
    CHECK(k.consumed == 0 && resid == 14);                                     /* longer than what is left: deaggregation ends */
    uint8_t one[32] = { 0, 7, 0, 20, 0x45 };
    k = cim_ingress_next(one, 32, CIM_DATA_FORMAT);
    CHECK(k.consumed == 32 && !cim_ingress_deliverable(&k, CIM_DATA_FORMAT, mux));   /* mux 7 has no device */
    const uint8_t zero[8] = { 0 };
    CHECK(!cim_ingress_next(zero, 8, CIM_DATA_FORMAT).consumed && !cim_ingress_next(one, 3, CIM_DATA_FORMAT).consumed);
}

int main(void)
{
    GROUP("data format and TRE", format());
    GROUP("egress", egress());
    GROUP("ingress deaggregation", ingress());
    return failures != 0;
}
