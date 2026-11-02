/* Host model of the pending baseband bridge: QMUX and CTL against QRTR, the SAHARA device end, the AppleBaseband methods, the channel
   table. Built with -fsanitize=address,undefined by tests/test_baseband_bridge.py. */
#include <stdio.h>
#include <string.h>
#include "coachz_bb_qmux.h"
#include "coachz_bb_sahara.h"
#include "coachz_bb_control.h"
#include "coachz_bb_channels.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* a host CTL request: [QMUX][0, txn, id, len][TLV 0x01] */
static uint32_t ctl_request(uint8_t *f, uint8_t txn, uint16_t id, const uint8_t *v, uint32_t vlen)
{
    const uint32_t tlvs = v ? 3 + vlen : 0, n = CBQ_HDR + CBQ_CTL_HDR + tlvs;
    f[0] = 1; cbq_wr16(f + 1, n - 1); f[3] = 0; f[4] = 0; f[5] = 0;
    f[6] = 0; f[7] = txn; cbq_wr16(f + 8, id); cbq_wr16(f + 10, tlvs);
    if (v) { f[12] = 1; cbq_wr16(f + 13, vlen); memcpy(f + 15, v, vlen); }
    return n;
}
static uint32_t qmi_request(uint8_t *f, uint8_t service, uint8_t cid, uint16_t txn, uint16_t id)
{
    const uint8_t tlv[] = { 0x10, 0x01, 0x00, 0x05 };
    const uint32_t n = CBQ_HDR + CBQ_SDU_HDR + sizeof tlv;
    f[0] = 1; cbq_wr16(f + 1, n - 1); f[3] = 0; f[4] = service; f[5] = cid;
    f[6] = 0; cbq_wr16(f + 7, txn); cbq_wr16(f + 9, id); cbq_wr16(f + 11, sizeof tlv);
    memcpy(f + 13, tlv, sizeof tlv);
    return n;
}

static void test_qmux(void)
{
    static struct cbq_bridge b;
    uint8_t f[64], out[256];
    cbq_init(&b);
    CHECK(cbq_server_new(&b, 1, 1 | 0 << 8, 0, 0x10));           /* WDS */
    CHECK(cbq_server_new(&b, 3, 1 | 2 << 8, 0, 0x22));           /* NAS instance 2 */
    CHECK(cbq_server_new(&b, 3, 1 | 1 << 8, 0, 0x21));           /* NAS instance 1: preferred */
    CHECK(cbq_server_new(&b, 1, 1, 0, 0x10));                    /* repeated announcement */
    CHECK(!cbq_server_new(&b, 0x101, 1, 0, 0x30));               /* not expressible in QMUX */
    CHECK(cbq_server_find(&b, 3)->port == 0x21);

    /* GetClientId(WDS): exact answer bytes */
    uint8_t svc = 1;
    uint32_t n = ctl_request(f, 7, CBQ_CTL_GET_CLIENT_ID, &svc, 1);
    struct cbq_result r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    const uint8_t want[] = { 1, 0x17, 0, 0x80, 0, 0, 1, 7, 0x22, 0, 0x0c, 0, 2, 4, 0, 0, 0, 0, 0, 1, 2, 0, 1, 1 };
    CHECK(r.kind == CBQ_TO_HOST && r.len == sizeof want && !memcmp(out, want, sizeof want));
    CHECK(r.local_port == CBQ_PORT_BASE && r.cid == 1);

    /* a service the modem lacks, and one it lacks but the bridge serves */
    svc = 2;
    n = ctl_request(f, 8, CBQ_CTL_GET_CLIENT_ID, &svc, 1);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && cbq_rd16(out + 12 + 3) == 1 && cbq_rd16(out + 12 + 5) == CBQ_ERR_INVALID_SERVICE_TYPE);
    svc = 0xe2;
    n = ctl_request(f, 9, CBQ_CTL_GET_CLIENT_ID, &svc, 1);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && cbq_rd16(out + 15) == 0 && out[22] == 0xe2 && out[23] == 1 && r.local_port == CBQ_PORT_BASE + 1);

    /* the same service on the other channel gets its own id space and port */
    svc = 1;
    n = ctl_request(f, 1, CBQ_CTL_GET_CLIENT_ID, &svc, 1);
    r = cbq_from_host(&b, 1, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && r.cid == 1 && r.local_port == CBQ_PORT_BASE + 2 && r.channel == 1);
    n = ctl_request(f, 2, CBQ_CTL_GET_CLIENT_ID, &svc, 1);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.cid == 2 && r.local_port == CBQ_PORT_BASE + 3);

    /* a WDS request goes to the modem as the SDU; the answer comes back framed for the right channel and client */
    n = qmi_request(f, 1, 2, 0x1234, 0x0020);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_MODEM && r.node == 0 && r.port == 0x10 && r.local_port == CBQ_PORT_BASE + 3 && r.len == n - CBQ_HDR);
    uint8_t sdu[16];
    memcpy(sdu, f + CBQ_HDR, n - CBQ_HDR);
    sdu[0] = CBQ_QMI_RESPONSE;
    r = cbq_from_modem(&b, CBQ_PORT_BASE + 3, 0, sdu, n - CBQ_HDR, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && r.channel == 0 && r.len == n && out[3] == 0x80 && out[4] == 1 && out[5] == 2 && !memcmp(out + 6, sdu, n - 6));
    CHECK(cbq_from_modem(&b, CBQ_PORT_BASE + 3, 5, sdu, n - CBQ_HDR, out, sizeof out).kind == CBQ_DROP);   /* wrong node */
    CHECK(cbq_from_modem(&b, CBQ_PORT_BASE + 9, 0, sdu, n - CBQ_HDR, out, sizeof out).kind == CBQ_DROP);   /* no such client */

    /* a private-service request comes back to the caller; the default answer is NOT_SUPPORTED with the same id and txn */
    n = qmi_request(f, 0xe2, 1, 0x0042, 0x0055);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_PRIVATE && r.service == 0xe2 && r.cid == 1);
    const uint32_t m = cbq_private_unsupported(f + CBQ_HDR, r.service, r.cid, out, sizeof out);
    const uint8_t want_ns[] = { 1, 0x13, 0, 0x80, 0xe2, 1, 2, 0x42, 0, 0x55, 0, 7, 0, 2, 4, 0, 1, 0, 94, 0 };
    CHECK(m == sizeof want_ns && !memcmp(out, want_ns, sizeof want_ns));

    /* malformed frames and host indications are dropped */
    n = qmi_request(f, 1, 2, 1, 1);
    f[1]++;
    CHECK(cbq_from_host(&b, 0, f, n, out, sizeof out).kind == CBQ_DROP);
    n = qmi_request(f, 1, 2, 1, 1);
    f[3] = 0x80;
    CHECK(cbq_from_host(&b, 0, f, n, out, sizeof out).kind == CBQ_DROP);
    n = qmi_request(f, 1, 2, 1, 1);
    f[6] = CBQ_QMI_INDICATION;
    CHECK(cbq_from_host(&b, 0, f, n, out, sizeof out).kind == CBQ_DROP);
    n = qmi_request(f, 1, 9, 1, 1);
    CHECK(cbq_from_host(&b, 0, f, n, out, sizeof out).kind == CBQ_DROP);
    CHECK(b.dropped == 6);

    /* release: answer echoes the pair, the modem client's port needs DEL_CLIENT once */
    const uint8_t pair[2] = { 1, 2 };
    n = ctl_request(f, 3, CBQ_CTL_RELEASE_CLIENT_ID, pair, 2);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && cbq_rd16(out + 15) == 0 && out[19] == 1 && out[20] == 2 && out[22] == 1 && out[23] == 2);
    CHECK(cbq_next_release(&b) == CBQ_PORT_BASE + 3 && cbq_next_release(&b) == 0);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(cbq_rd16(out + 17) == CBQ_ERR_INVALID_CLIENT_ID);
    /* the freed id is not handed out again at once */
    svc = 1;
    n = ctl_request(f, 4, CBQ_CTL_GET_CLIENT_ID, &svc, 1);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.cid == 3 && r.local_port == CBQ_PORT_BASE + 3);

    /* Sync on channel 0 frees its clients (WDS 1, BSP 1, WDS 3); only modem clients need DEL_CLIENT; channel 1 keeps its client */
    n = ctl_request(f, 5, CBQ_CTL_SYNC, 0, 0);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && r.len == 19 && cbq_rd16(out + 15) == 0);
    uint32_t released[4] = { 0 }, count = 0, p;
    while ((p = cbq_next_release(&b)) && count < 4) released[count++] = p;
    CHECK(count == 2 && released[0] == CBQ_PORT_BASE && released[1] == CBQ_PORT_BASE + 3);
    CHECK(cbq_client_find(&b, 1, 1, 1) == 2);

    /* GetVersionInfo lists each service once; other CTL requests are accepted and counted */
    n = ctl_request(f, 6, CBQ_CTL_GET_VERSION_INFO, 0, 0);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && out[19] == 1 && cbq_rd16(out + 20) == 1 + 2 * 5 && out[22] == 2 && out[23] == 1 && out[28] == 3);
    n = ctl_request(f, 7, 0x0028, 0, 0);
    r = cbq_from_host(&b, 0, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && cbq_rd16(out + 15) == 0 && b.ctl_other == 1);

    const uint8_t want_sync[] = { 1, 11, 0, 0x80, 0, 0, 2, 0, 0x27, 0, 0, 0 };
    CHECK(cbq_ctl_sync_indication(out, sizeof out) == sizeof want_sync && !memcmp(out, want_sync, sizeof want_sync));

    /* a vanished server: requests to it are dropped */
    cbq_server_del(&b, 1, 0, 0x10);
    n = qmi_request(f, 1, 1, 1, 1);
    CHECK(cbq_from_host(&b, 1, f, n, out, sizeof out).kind == CBQ_DROP);
    if (!failures) printf("PASS qmux and ctl\n");
}

/* ------------------------------------------------------------------ SAHARA: a host that serves two images */
static uint8_t image_elf32[0x400], image_elf64[0x600], image_blob[100];

static void build_images(void)
{
    uint8_t *h = image_elf32;
    memset(image_elf32, 0xa5, sizeof image_elf32);
    memset(h, 0, 52);
    h[0] = 0x7f; h[1] = 'E'; h[2] = 'L'; h[3] = 'F'; h[4] = 1;
    cbs_put32(h + 0x1c, 0x40);                  /* phoff */
    h[0x2a] = 32; h[0x2c] = 3;                  /* phentsize, phnum */
    const uint32_t seg[3][2] = { { 0x100, 0x80 }, { 0x200, 0 }, { 0x200, 0x1f0 } };
    for (int i = 0; i < 3; i++) {
        uint8_t *e = h + 0x40 + i * 32;
        memset(e, 0, 32);
        cbs_put32(e + 4, seg[i][0]);
        cbs_put32(e + 16, seg[i][1]);
    }
    h = image_elf64;
    memset(image_elf64, 0x5a, sizeof image_elf64);
    memset(h, 0, 64);
    h[0] = 0x7f; h[1] = 'E'; h[2] = 'L'; h[3] = 'F'; h[4] = 2;
    cbs_put32(h + 0x20, 0x40);
    h[0x36] = 56; h[0x38] = 1;
    memset(h + 0x40, 0, 56);
    cbs_put32(h + 0x40 + 8, 0x100);
    cbs_put32(h + 0x40 + 32, 0x500);
    memset(image_blob, 0x11, sizeof image_blob);
}

static const uint8_t *image_for(uint32_t id, uint32_t *size)
{
    if (id == 0x15) { *size = sizeof image_elf32; return image_elf32; }
    if (id == 0x1f) { *size = sizeof image_elf64; return image_elf64; }
    if (id == 0x4300) { *size = sizeof image_blob; return image_blob; }
    return 0;
}

static void test_sahara(void)
{
    static struct cbs_device d;
    struct cbs_out o;
    const uint32_t list[] = { 0x15, 0x1f, 0x4300 };
    build_images();
    cbs_init(&d, list, 3, 0x100);
    cbs_start(&d, &o);
    unsigned hellos = 0, ends = 0, done_resp = 0, final_status = 9, guard = 0;
    uint64_t served = 0;
    while (o.count && guard++ < 1000) {
        struct cbs_out next = { 0 };
        for (uint32_t k = 0; k < o.count; k++) {
            const uint8_t *p = o.data[k];
            const uint32_t command = cbs_get32(p);
            uint8_t reply[0x30] = { 0 };
            struct cbs_out step = { 0 };
            if (command == CBS_HELLO) {
                CHECK(o.len[k] == 0x30 && cbs_get32(p + 4) == 0x30 && cbs_get32(p + 8) == 2 && cbs_get32(p + 20) == 0);
                hellos++;
                cbs_put32(reply, CBS_HELLO_RESP); cbs_put32(reply + 4, 0x30); cbs_put32(reply + 8, 2); cbs_put32(reply + 12, 1);
                cbs_receive(&d, reply, 0x30, &step);
            } else if (command == CBS_READ_DATA) {
                uint32_t size = 0;
                const uint8_t *img = image_for(cbs_get32(p + 8), &size);
                const uint32_t off = cbs_get32(p + 12), len = cbs_get32(p + 16);
                CHECK(img && off + len <= size && len <= 0x100);
                if (!img || off + len > size) { failures++; return; }
                /* serve in two writes to exercise partial data */
                const uint32_t half = len / 2;
                uint32_t used = cbs_receive(&d, img + off, half, &step);
                CHECK(used == half && step.count == 0);
                used = cbs_receive(&d, img + off + half, len - half, &step);
                CHECK(used == len - half);
                served += len;
            } else if (command == CBS_END_IMAGE_TX) {
                CHECK(cbs_get32(p + 8) == list[ends] && cbs_get32(p + 12) == 0);
                ends++;
                cbs_put32(reply, CBS_DONE); cbs_put32(reply + 4, 8);
                cbs_receive(&d, reply, 8, &step);
            } else if (command == CBS_DONE_RESP) {
                done_resp++;
                final_status = cbs_get32(p + 8);
                continue;
            } else {
                CHECK(0);
            }
            for (uint32_t j = 0; j < step.count && next.count < 2; j++) {
                next.len[next.count] = step.len[j];
                memcpy(next.data[next.count++], step.data[j], CBS_PACKET_MAX);
            }
        }
        o = next;
    }
    /* image 1: probe 64, headers 96, segments 0x80 + 0x1f0 in 0x100 pieces; image 2: 64 + 56 + 0x500; image 3: probe only */
    CHECK(d.state == CBS_FINISHED && hellos == 3 && ends == 3 && done_resp == 3 && final_status == 1);
    CHECK(served == 64 + 96 + 0x80 + 0x1f0 + 64 + 56 + 0x500 + 64 && d.bytes == served);
    CHECK(d.requests == 1 + 1 + 1 + 2 + 1 + 1 + 5 + 1);

    /* a refused HELLO, a RESET, a stray packet */
    cbs_init(&d, list, 1, 0);
    cbs_start(&d, &o);
    uint8_t bad[0x30] = { 0 };
    cbs_put32(bad, CBS_HELLO_RESP); cbs_put32(bad + 4, 0x30); cbs_put32(bad + 16, 1);
    cbs_receive(&d, bad, 0x30, &o);
    CHECK(d.state == CBS_FAILED && o.count == 0);
    cbs_start(&d, &o);
    cbs_put32(bad, CBS_RESET); cbs_put32(bad + 4, 8);
    cbs_receive(&d, bad, 8, &o);
    CHECK(d.state == CBS_IDLE && o.count == 1 && cbs_get32(o.data[0]) == CBS_RESET_RESP);
    cbs_start(&d, &o);
    cbs_put32(bad, CBS_DONE); cbs_put32(bad + 4, 8);
    cbs_receive(&d, bad, 8, &o);
    CHECK(d.state == CBS_FAILED);
    cbs_init(&d, list, 0, 0);
    cbs_start(&d, &o);
    CHECK(d.state == CBS_FAILED && o.count == 0);
    if (!failures) printf("PASS sahara device\n");
}

static void test_control(void)
{
    static struct cbc_state s;
    uint64_t in[3] = { 0 }, out[2] = { 0 };
    uint32_t nout;
    cbc_init(&s);
    nout = 1; CHECK(cbc_call(&s, CBC_GET_RESET, in, 0, out, &nout) == CBC_OK && nout == 1 && out[0] == 0);
    nout = 1; CHECK(cbc_call(&s, CBC_GET_PMU_EXTON, in, 0, out, &nout) == CBC_OK && out[0] == 1);
    in[0] = 0; nout = 0; CHECK(cbc_call(&s, CBC_POWER, in, 1, out, &nout) == CBC_OK);
    nout = 1; CHECK(cbc_call(&s, CBC_GET_PMU_EXTON, in, 0, out, &nout) == CBC_OK && out[0] == 0);
    in[0] = 5; nout = 0; CHECK(cbc_call(&s, CBC_SET_BASEBAND_STATE, in, 1, out, &nout) == CBC_OK);
    nout = 1; CHECK(cbc_call(&s, CBC_GET_BASEBAND_STATE, in, 0, out, &nout) == CBC_OK && nout == 1 && out[0] == 5);
    in[0] = 3; nout = 0; CHECK(cbc_call(&s, CBC_SET_MANAGER_STATE, in, 1, out, &nout) == CBC_OK);
    nout = 1; CHECK(cbc_call(&s, CBC_GET_MANAGER_STATE, in, 0, out, &nout) == CBC_OK && out[0] == 3);
    in[0] = 7; in[1] = 70; nout = 0; CHECK(cbc_call(&s, CBC_SET_PROPERTY, in, 2, out, &nout) == CBC_OK);
    in[1] = 71; CHECK(cbc_call(&s, CBC_SET_PROPERTY, in, 2, out, &nout) == CBC_OK && s.properties == 1);
    nout = 1; CHECK(cbc_call(&s, CBC_GET_PROPERTY, in, 1, out, &nout) == CBC_OK && out[0] == 71);
    in[0] = 8; nout = 1; CHECK(cbc_call(&s, CBC_GET_PROPERTY, in, 1, out, &nout) == CBC_OK && out[0] == 0);
    nout = 0; CHECK(cbc_call(&s, CBC_GET_BASEBAND_STATE, in, 0, out, &nout) == CBC_BAD_ARGUMENT);
    nout = 0; CHECK(cbc_call(&s, CBC_SET_PROPERTY, in, 1, out, &nout) == CBC_BAD_ARGUMENT);
    nout = 0; CHECK(cbc_call(&s, CBC_RESET_MODEM, in, 1, out, &nout) == CBC_OK && s.resets == 1);
    nout = 0; CHECK(cbc_call(&s, CBC_SPMI_RAW_READ, in, 3, out, &nout) == CBC_UNSUPPORTED && s.last_unsupported == CBC_SPMI_RAW_READ);
    nout = 0; CHECK(cbc_call(&s, CBC_TIMESYNC, in, 0, out, &nout) == CBC_UNSUPPORTED);
    if (!failures) printf("PASS baseband methods\n");
}

static void test_channels(void)
{
    CHECK(cbh_channel_id("qmux1") == 4 && cbh_channel_id("qmux2") == 5 && cbh_channel_id("sahara") == 1);
    CHECK(cbh_channel_id("boot_logger") == 20 && cbh_channel_id("qmux") == -1 && cbh_channel_id("qmux11") == -1);
    for (unsigned i = 0; i < CBH_PUBLISHED; i++) CHECK(cbh_channel_id(cbh_published[i]) >= 0);
    for (unsigned i = 0; i < sizeof cbh_interface_methods / sizeof cbh_interface_methods[0]; i++) CHECK(cbh_interface_methods[i].selector == i);
    for (unsigned i = 0; i < sizeof cbh_control_methods / sizeof cbh_control_methods[0]; i++) CHECK(cbh_control_methods[i].selector == i);
    CHECK(!strcmp(CBH_PROTOCOL_KEY, "ABPInterfaceProtocol") && !strcmp(CBH_INTERFACE_CLASS, "AppleBasebandPCIInterface"));
    if (!failures) printf("PASS channel table\n");
}

int main(void)
{
    test_qmux();
    test_sahara();
    test_control();
    test_channels();
    return failures ? 1 : 0;
}
