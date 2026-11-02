/* Host model of the pending IPA QMI handshake (platform/coachz-prelink/pending/modem). Expected values are worked out
   from mainline ipa_qmi.c / ipa_qmi_msg.c / ipa_data-v4.2.c and qmi_interface.c framing, not from this code. */
#include <stdio.h>
#include <string.h>
#include "coachz_ipa_qmi.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define GROUP(name, body) do { const int before = failures; body; if (failures == before) printf("PASS %s\n", name); } while (0)

static uint32_t tlv32(const struct ci_msg *m, uint8_t t, int second)
{
    uint16_t l;
    const uint8_t *v = ci_find(m, t, &l);
    return v ? ci_le32(v + (second ? 4 : 0)) : 0xdeadbeefu;
}

static void init_driver(void)
{
    uint8_t b[CI_INIT_DRIVER_REQ_MAX];
    const size_t n = ci_init_driver_req(b, sizeof(b), 0x0102, 0x3e00, 0);
    /* 7-byte header + 0x10(7) 0x11..0x13(3x11) 0x14 0x15(2x7) 0x16(11) 0x17(7) 0x18(4) 0x19(11) 0x1f 0x20(2x7) */
    CHECK(n == 108 && n <= CI_INIT_DRIVER_REQ_MAX);
    CHECK(b[0] == 0 && b[1] == 0x02 && b[2] == 0x01 && b[3] == 0x21 && b[4] == 0 && b[5] == 101 && b[6] == 0);
    struct ci_msg m;
    CHECK(ci_parse(b, n, &m) && m.type == CI_QMI_REQUEST && m.id == CI_INIT_DRIVER && m.txn == 0x0102);
    CHECK(tlv32(&m, 0x10, 0) == 3);
    CHECK(tlv32(&m, 0x11, 0) == 0x42a8 && tlv32(&m, 0x11, 1) == 0x43e7);   /* modem header 0x4a8 + 0x140 */
    CHECK(tlv32(&m, 0x12, 0) == 0x41a0 && tlv32(&m, 0x12, 1) == 7);         /* v4 route, modem_route_count - 1 */
    CHECK(tlv32(&m, 0x13, 0) == 0x4228 && tlv32(&m, 0x13, 1) == 7);
    CHECK(tlv32(&m, 0x14, 0) == 0x4090 && tlv32(&m, 0x15, 0) == 0x4118);
    CHECK(tlv32(&m, 0x16, 0) == 0x49f0 && tlv32(&m, 0x16, 1) == 0x140c);    /* modem region start and size */
    CHECK(tlv32(&m, 0x17, 0) == 9);                                         /* AP_MODEM_RX endpoint */
    uint16_t l;
    const uint8_t *skip = ci_find(&m, 0x18, &l);
    CHECK(skip && l == 1 && skip[0] == 0);
    CHECK(tlv32(&m, 0x19, 0) == 0x43f0 && tlv32(&m, 0x19, 1) == 0x45ef);    /* modem proc ctx */
    CHECK(tlv32(&m, 0x1f, 0) == 0x4850 && tlv32(&m, 0x20, 0) == 0x3e60);    /* quota base; size + mem_offset, as mainline */
    CHECK(!ci_find(&m, 0x1a, &l) && !ci_find(&m, 0x1b, &l) && !ci_find(&m, 0x1d, &l) && !ci_find(&m, 0x21, &l));
    CHECK(ci_init_driver_req(b, sizeof(b), 1, 0x3e00, 1) == 108 && ci_find((ci_parse(b, 108, &m), &m), 0x18, &l)[0] == 1);
    CHECK(ci_init_driver_req(b, 50, 1, 0x3e00, 0) == 0);   /* no room: refused, nothing half-built is sent */
}

static void responses(void)
{
    uint8_t b[32];
    const uint8_t rsp[] = { 0x02, 0x34, 0x12, 0x20, 0x00, 0x07, 0x00, 0x02, 0x04, 0x00, 0, 0, 0, 0 };
    CHECK(ci_success_rsp(b, sizeof(b), 0x1234, CI_INDICATION_REGISTER) == sizeof(rsp) && memcmp(b, rsp, sizeof(rsp)) == 0);
    const uint8_t ind[] = { 0x04, 0x05, 0x00, 0x22, 0x00, 0x07, 0x00, 0x02, 0x04, 0x00, 0, 0, 0, 0 };
    CHECK(ci_init_complete_ind(b, sizeof(b), 5) == sizeof(ind) && memcmp(b, ind, sizeof(ind)) == 0);

    /* an INIT_DRIVER response: success, ctrl end point 9, default end point 8, init pending 1 */
    const uint8_t ok[] = { 0x02, 0x02, 0x01, 0x21, 0x00, 25, 0x00,
                           0x02, 0x04, 0x00, 0, 0, 0, 0,
                           0x10, 0x04, 0x00, 9, 0, 0, 0,
                           0x11, 0x04, 0x00, 8, 0, 0, 0,
                           0x12, 0x01, 0x00, 1 };
    struct ci_msg m;
    struct ci_init_driver_rsp r;
    CHECK(ci_parse(ok, sizeof(ok), &m) && ci_init_driver_rsp_parse(&m, &r));
    CHECK(r.ok && r.error == 0 && r.has_ctrl && r.ctrl_end_pt == 9 && r.has_default && r.default_end_pt == 8 && r.pending == 1);
    const uint8_t bad[] = { 0x02, 0x02, 0x01, 0x21, 0x00, 7, 0x00, 0x02, 0x04, 0x00, 1, 0, 0x10, 0 };
    CHECK(ci_parse(bad, sizeof(bad), &m) && ci_init_driver_rsp_parse(&m, &r) && !r.ok && r.error == 0x10 && !r.has_ctrl);
    const uint8_t cut[] = { 0x02, 0x02, 0x01, 0x21, 0x00, 30, 0x00, 0x02 };
    CHECK(!ci_parse(cut, sizeof(cut), &m));
    const uint8_t dic[] = { 0x00, 0x09, 0x00, 0x35, 0x00, 4, 0x00, 0x01, 0x01, 0x00, 1 };   /* DRIVER_INIT_COMPLETE request */
    CHECK(ci_parse(dic, sizeof(dic), &m) && m.type == CI_QMI_REQUEST && m.id == CI_DRIVER_INIT_COMPLETE && ci_find(&m, 0x01, &(uint16_t){0})[0] == 1);
}

static void handshake(void)
{
    CHECK(CI_QRTR_INSTANCE(CI_HOST_INSTANCE) == 0x101 && ci_is_modem_service(0x31, 0x201) && !ci_is_modem_service(0x31, 0x101));
    struct ci_handshake h;
    struct ci_step s;
    ci_setup(&h);
    ci_on_init_driver_success(&h);
    s = ci_ready_step(&h); CHECK(!s.send_init_complete && !s.start_modem);
    ci_on_driver_init_complete_answered(&h);
    s = ci_ready_step(&h); CHECK(!s.send_init_complete && !s.start_modem);   /* first boot: waits for INDICATION_REGISTER */
    ci_on_indication_register_answered(&h);
    s = ci_ready_step(&h); CHECK(s.send_init_complete && !s.start_modem);
    ci_indication_sent(&h);
    s = ci_ready_step(&h); CHECK(!s.send_init_complete && s.start_modem && !h.initial_boot);
    s = ci_ready_step(&h); CHECK(!s.send_init_complete && !s.start_modem);
    /* the modem restarts: uC and boot state kept, INIT_DRIVER alone makes it ready */
    ci_teardown(&h);
    CHECK(h.uc_ready && !h.initial_boot && !h.modem_ready);
    s = ci_ready_step(&h); CHECK(!s.start_modem);
    ci_on_init_driver_success(&h);
    s = ci_ready_step(&h); CHECK(s.start_modem && !s.send_init_complete);
    /* an INDICATION_REGISTER on a later boot still gets its indication (optional, but mainline sends it) */
    ci_on_indication_register_answered(&h);
    s = ci_ready_step(&h); CHECK(s.send_init_complete);
}

int main(void)
{
    GROUP("init driver request", init_driver());
    GROUP("responses and parsing", responses());
    GROUP("handshake readiness", handshake());
    return failures != 0;
}
