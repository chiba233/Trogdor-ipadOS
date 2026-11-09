/* Host model of the pending baseband bridge: QMUX and CTL against QRTR, the SAHARA device end, the AppleBaseband methods, the channel
   table, the data port, activation answers and eSIM Tape transport. Built with -fsanitize=address,undefined by tests/test_baseband_bridge.py. */
#include <stdio.h>
#include <string.h>
#include "coachz_bb_qmux.h"
#include "coachz_bb_sahara.h"
#include "coachz_bb_control.h"
#include "coachz_bb_channels.h"
#include "coachz_bb_data.h"
#include "coachz_bb_pdp.h"
#include "coachz_bb_esim.h"

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

/* BBUEUR10Loader's order: mode 3, chip id and serial number in command mode, then a switch to mode 0 and a new HELLO. */
static void test_sahara_command(void)
{
    static const uint32_t list[] = { 0x15 };
    static struct cbs_device d;
    struct cbs_out o = { 0 };
    uint8_t in[0x30] = { 0 };
    cbs_init(&d, list, 1, 0x100);
    cbs_start(&d, &o);
    CHECK(o.count == 1 && cbs_get32(o.data[0]) == CBS_HELLO);
    cbs_put32(in, CBS_HELLO_RESP); cbs_put32(in + 4, 0x30); cbs_put32(in + 8, 2); cbs_put32(in + 12, 1); cbs_put32(in + 20, CBS_MODE_COMMAND);
    CHECK(cbs_receive(&d, in, 0x30, &o) == 0x30 && o.count == 1 && cbs_get32(o.data[0]) == CBS_COMMAND_READY && o.len[0] == 8 && d.state == CBS_COMMAND);
    cbs_put32(in, CBS_EXECUTE); cbs_put32(in + 4, 0xc); cbs_put32(in + 8, CBS_EXEC_HW_ID);
    cbs_receive(&d, in, 0xc, &o);
    CHECK(o.count == 1 && cbs_get32(o.data[0]) == CBS_EXECUTE_RESP && o.len[0] == 0x10 && cbs_get32(o.data[0] + 8) == CBS_EXEC_HW_ID && cbs_get32(o.data[0] + 12) == 8);
    cbs_put32(in, CBS_EXECUTE_DATA);
    cbs_receive(&d, in, 0xc, &o);
    CHECK(o.count == 1 && o.len[0] == 8 && cbs_get32(o.data[0] + 4) == CBS_CHIP_ID);
    cbs_put32(in, CBS_EXECUTE); cbs_put32(in + 8, CBS_EXEC_SERIAL);
    cbs_receive(&d, in, 0xc, &o);
    CHECK(o.count == 1 && cbs_get32(o.data[0] + 12) == 4);
    cbs_put32(in, CBS_EXECUTE_DATA);
    cbs_receive(&d, in, 0xc, &o);
    CHECK(o.count == 1 && o.len[0] == 4 && d.executes == 2);
    cbs_put32(in, CBS_EXECUTE); cbs_put32(in + 8, 0x77);       /* unknown: answers empty */
    cbs_receive(&d, in, 0xc, &o);
    CHECK(cbs_get32(o.data[0] + 12) == 0);
    cbs_put32(in, CBS_EXECUTE_DATA);
    cbs_receive(&d, in, 0xc, &o);
    CHECK(o.count == 0);
    cbs_put32(in, CBS_SWITCH_MODE); cbs_put32(in + 8, CBS_MODE_IMAGE_PENDING);
    cbs_receive(&d, in, 0xc, &o);
    CHECK(o.count == 1 && cbs_get32(o.data[0]) == CBS_HELLO && cbs_get32(o.data[0] + 20) == CBS_MODE_IMAGE_PENDING && d.state == CBS_WAIT_HELLO_RESP);
    cbs_put32(in, CBS_HELLO_RESP); cbs_put32(in + 4, 0x30); cbs_put32(in + 20, CBS_MODE_IMAGE_PENDING);
    cbs_receive(&d, in, 0x30, &o);
    CHECK(o.count == 1 && cbs_get32(o.data[0]) == CBS_READ_DATA && cbs_get32(o.data[0] + 8) == 0x15 && d.state == CBS_WAIT_ELF);
}

static void test_registers(void)
{
    uint8_t r[0x60];
    cbs_read_register(3, r, 0x18);
    CHECK(cbs_get32(r + 4) == CBS_CHIP_ID && cbs_get32(r) == 0);
    cbs_read_register(0, r, 4);
    CHECK(cbs_get32(r) == CBS_SERIAL);
    cbs_read_register(4, r, 0x60);
    CHECK(!memcmp(r, cbs_default_pk_hash, 32) && r[0x20] == 0 && r[0x5f] == 0);
    cbs_read_register(9, r, 8);
    CHECK(cbs_get32(r) == 0 && cbs_get32(r + 4) == 0);
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
    /* ABM powercycles after an unclean stop, and every client names itself first (structure input, no scalars). */
    nout = 0; CHECK(cbc_call(&s, CBC_POWERCYCLE, in, 0, out, &nout) == CBC_OK && s.powered == 1);
    nout = 0; CHECK(cbc_call(&s, CBC_SELF_IDENTIFY, in, 0, out, &nout) == CBC_OK && nout == 0);
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

/* ------------------------------------------------------------------ data port and activation */
static void test_data(void)
{
    static struct cbq_bridge b;
    uint8_t out[256], sdu[128];
    uint32_t len = 0;
    cbq_init(&b);

    /* CommCenter's Bind Mux Data Port names Apple's PCIe endpoint: it becomes the embedded one, everything else kept */
    const uint8_t bind_pcie[] = { 0, 3, 0, 0xa2, 0, 15, 0, 0x10, 8, 0, 3, 0, 0, 0, 4, 0, 0, 0, 0x11, 1, 0, 2 };
    struct cbd_fix f = cbd_endpoint_fix(CBD_SVC_WDS, bind_pcie, sizeof bind_pcie, out, sizeof out);
    const uint8_t bind_ipa[] = { 0, 3, 0, 0xa2, 0, 15, 0, 0x10, 8, 0, 4, 0, 0, 0, 1, 0, 0, 0, 0x11, 1, 0, 2 };
    CHECK(f.len == sizeof bind_ipa && !memcmp(out, bind_ipa, sizeof bind_ipa) && f.before_type == 3 && f.before_interface == 4 && !f.inserted);
    CHECK(!cbd_endpoint_fix(CBD_SVC_WDS, bind_ipa, sizeof bind_ipa, out, sizeof out).len);   /* already right */

    /* a mux id alone (the dormancy driver's bind, QOS bind): the endpoint is added */
    const uint8_t bind_mux[] = { 0, 9, 0, 0x2b, 0, 4, 0, 0x11, 1, 0, 1 };
    f = cbd_endpoint_fix(CBD_SVC_QOS, bind_mux, sizeof bind_mux, out, sizeof out);
    const uint8_t bind_mux_ipa[] = { 0, 9, 0, 0x2b, 0, 15, 0, 0x11, 1, 0, 1, 0x10, 8, 0, 4, 0, 0, 0, 1, 0, 0, 0 };
    CHECK(f.len == sizeof bind_mux_ipa && !memcmp(out, bind_mux_ipa, sizeof bind_mux_ipa) && f.inserted);
    /* a data port without a mux id, other messages and malformed ones go as they are */
    const uint8_t bind_sio[] = { 0, 9, 0, 0x2b, 0, 5, 0, 0x12, 2, 0, 1, 0x0e };
    CHECK(!cbd_endpoint_fix(CBD_SVC_QOS, bind_sio, sizeof bind_sio, out, sizeof out).len);
    CHECK(!cbd_endpoint_fix(CBD_SVC_WDS, bind_mux, sizeof bind_mux, out, sizeof out).len);   /* WDS 0x2b is another message */
    const uint8_t broken[] = { 0, 9, 0, 0xa2, 0, 9, 0, 0x11, 9, 0, 1 };
    CHECK(!cbd_endpoint_fix(CBD_SVC_WDS, broken, sizeof broken, out, sizeof out).len);
    CHECK(!cbd_endpoint_fix(CBD_SVC_WDS, bind_pcie, sizeof bind_pcie, out, 16).len);         /* no room: unchanged */

    /* the setup: DPM Open Port, then WDA Set Data Format with QMAPv4, then QMAP when the modem keeps another */
    struct cbd_setup st;
    memset(&st, 0, sizeof st);
    st.has_dpm = st.has_wda = 1;
    uint32_t service = 0, n = cbd_next(&st, sdu, sizeof sdu, &service);
    const uint8_t dpm[] = { 0, 1, 0, 0x20, 0, 20, 0, 0x11, 17, 0, 1, 4, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 9, 0, 0, 0 };
    CHECK(service == CBD_SVC_DPM && n == sizeof dpm && !memcmp(sdu, dpm, sizeof dpm) && st.step == CBD_DPM);
    const uint8_t dpm_ok[] = { 2, 1, 0, 0x20, 0, 7, 0, 2, 4, 0, 0, 0, 0, 0 };
    CHECK(!cbd_answer(&st, CBD_SVC_WDA, dpm_ok, sizeof dpm_ok) && cbd_answer(&st, CBD_SVC_DPM, dpm_ok, sizeof dpm_ok) && st.dpm_error == 0);
    n = cbd_next(&st, sdu, sizeof sdu, &service);
    CHECK(service == CBD_SVC_WDA && n == 7 + 5 * 7 + 11 && cbd_rd16(sdu + 3) == CBD_WDA_SET_DATA_FORMAT && cbd_u32(sdu, n, 0x11) == 2 &&
          cbd_u32(sdu, n, 0x12) == CBD_DAP_QMAPV4 && cbd_u32(sdu, n, 0x13) == CBD_DAP_QMAPV4 && cbd_u32(sdu, n, 0x15) == 32 &&
          cbd_u32(sdu, n, 0x16) == 32768 && cbd_find(sdu, n, 0x17, &len) && len == 8 && cbd_rd32(cbd_find(sdu, n, 0x17, &len)) == 4);
    uint8_t wda_other[] = { 2, 2, 0, 0x20, 0, 21, 0, 2, 4, 0, 0, 0, 0, 0, 0x11, 4, 0, 2, 0, 0, 0, 0x13, 4, 0, 0, 0, 0, 0 };
    wda_other[1] = (uint8_t)st.txn;
    CHECK(cbd_answer(&st, CBD_SVC_WDA, wda_other, sizeof wda_other) && st.wda_error == 0 && st.dl_dap == 0 && st.llp == 2);
    n = cbd_next(&st, sdu, sizeof sdu, &service);
    CHECK(service == CBD_SVC_WDA && n && cbd_u32(sdu, n, 0x13) == CBD_DAP_QMAP && st.dap_index == 1);
    uint8_t wda_qmap[] = { 2, 0, 0, 0x20, 0, 21, 0, 2, 4, 0, 0, 0, 0, 0, 0x11, 4, 0, 2, 0, 0, 0, 0x13, 4, 0, 5, 0, 0, 0 };
    wda_qmap[1] = (uint8_t)st.txn;
    CHECK(cbd_answer(&st, CBD_SVC_WDA, wda_qmap, sizeof wda_qmap) && st.dl_dap == CBD_DAP_QMAP);
    CHECK(!cbd_next(&st, sdu, sizeof sdu, &service) && st.step == CBD_DONE);
    /* without DPM on the modem the setup starts at WDA; without either it ends at once */
    memset(&st, 0, sizeof st);
    st.has_wda = 1;
    CHECK(cbd_next(&st, sdu, sizeof sdu, &service) && service == CBD_SVC_WDA && st.step == CBD_WDA);
    memset(&st, 0, sizeof st);
    CHECK(!cbd_next(&st, sdu, sizeof sdu, &service) && st.step == CBD_DONE);

    /* the bridge's own clients: answers come back on the internal channel, not to ABM */
    CHECK(cbq_server_new(&b, CBD_SVC_WDA, 1, 0, 0x30));
    const int slot = cbq_client_alloc(&b, CBQ_INTERNAL, CBD_SVC_WDA);
    CHECK(slot >= 0);
    struct cbq_result r = cbq_from_modem(&b, CBQ_PORT_BASE + (uint32_t)slot, 0, wda_qmap, sizeof wda_qmap, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && r.channel == CBQ_INTERNAL && r.service == CBD_SVC_WDA);

    /* activation register: success with the request's transaction, then the indication with kActivated */
    const uint8_t act[] = { 0, 0x34, 0x12, 0x0c, 0xb0, 0, 0 };
    n = cbq_private_answer(act, CBD_SVC_BSP, 3, 0, 0, 0, out, sizeof out);
    const uint8_t act_ok[] = { 1, 19, 0, 0x80, 0xe2, 3, 2, 0x34, 0x12, 0x0c, 0xb0, 7, 0, 2, 4, 0, 0, 0, 0, 0 };
    CHECK(n == sizeof act_ok && !memcmp(out, act_ok, sizeof act_ok));
    uint8_t tlvs[CBD_ACTIVATION_SIM_TLVS];
    const uint32_t t = cbd_activation_state(tlvs, CBD_ACTIVATION_TLVS, NULL, NULL);
    n = cbq_private_indication(CBD_BSP_ACTIVATION_REGISTER, CBD_SVC_BSP, 3, tlvs, t, out, sizeof out);
    /* kActivated, service provisioned (0x4f = 2), slot one's card provisioned (0x50 = 2), both slots accepted (0x56, 0x57 = 1) */
    const uint8_t act_ind[] = { 1, 32, 0, 0x80, 0xe2, 3, 4, 0, 0, 0x0c, 0xb0, 20, 0, 1, 1, 0, 0, 0x4f, 1, 0, 2, 0x50, 1, 0, 2,
                                0x56, 1, 0, 1, 0x57, 1, 0, 1 };
    CHECK(t == CBD_ACTIVATION_TLVS && n == sizeof act_ind && !memcmp(out, act_ind, sizeof act_ind));
    CHECK(!cbd_activation_state(tlvs, CBD_ACTIVATION_TLVS - 1, NULL, NULL));   /* no room: nothing to send */
    /* CommCenter's EF_ICCID and EF_IMSI reads (synthetic values) are recognized, then go into the indication in reading order. */
    const uint8_t iccid_req[] = { 0, 7, 0, 0x20, 0, 13, 0, 1, 2, 0, 0, 0, 2, 5, 0, 0xe2, 0x2f, 2, 0, 0x3f };
    const uint8_t imsi_req[] = { 0, 8, 0, 0x20, 0, 13, 0, 1, 2, 0, 0, 0, 2, 5, 0, 0x07, 0x6f, 2, 0, 0x3f };
    CHECK(cbd_uim_read_file(iccid_req, sizeof iccid_req) == CBD_EF_ICCID && cbd_uim_read_file(imsi_req, sizeof imsi_req) == CBD_EF_IMSI);
    uint8_t iccid_ans[] = { 2, 7, 0, 0x20, 0, 22, 0, 2, 4, 0, 0, 0, 0, 0, 0x11, 12, 0, 10, 0,
                            0x98, 0x10, 0x32, 0x54, 0x76, 0x98, 0x10, 0x32, 0x54, 0x76 };
    uint8_t ef[16], iccid[CBD_ICCID_BYTES], imsi[CBD_IMSI_BYTES];
    uint32_t eflen = cbd_uim_read_content(iccid_ans, sizeof iccid_ans, ef, sizeof ef);
    CHECK(eflen == 10 && cbd_iccid_from_ef(ef, eflen, iccid) && iccid[0] == 0x89 && iccid[1] == 0x01 && iccid[9] == 0x67);
    iccid_ans[17] = 9;   /* a content length that does not match its TLV is not a read */
    CHECK(!cbd_uim_read_content(iccid_ans, sizeof iccid_ans, ef, sizeof ef));
    /* IMSI 001010123456789: length 8, digit 1 above parity 9, then pairs low nibble first */
    const uint8_t ef_imsi[9] = { 8, 0x09, 0x10, 0x10, 0x10, 0x32, 0x54, 0x76, 0x98 };
    CHECK(cbd_imsi_from_ef(ef_imsi, sizeof ef_imsi, imsi));
    const uint8_t imsi_bcd[8] = { 0x00, 0x10, 0x10, 0x12, 0x34, 0x56, 0x78, 0x9f };
    CHECK(!memcmp(imsi, imsi_bcd, sizeof imsi_bcd));
    const uint32_t ts = cbd_activation_state(tlvs, sizeof tlvs, iccid, imsi);
    CHECK(ts == CBD_ACTIVATION_SIM_TLVS && tlvs[20] == 0x42 && tlvs[21] == 10 && !memcmp(tlvs + 23, iccid, 10) &&
          tlvs[33] == 0x43 && tlvs[34] == 8 && !memcmp(tlvs + 36, imsi, 8));
    CHECK(cbd_activation_state(tlvs, sizeof tlvs, iccid, NULL) == CBD_ACTIVATION_TLVS + 13);
    CHECK(!cbd_activation_state(tlvs, CBD_ACTIVATION_SIM_TLVS - 1, iccid, imsi));
    /* Carrier bundle file push (BSP 0xa000): segment 3 of 9 of file type 9 is answered with success and the next segment, 4. */
    const uint8_t seg_req[] = { 0, 4, 0, 0x00, 0xa0, 13, 0, 1, 10, 0, 9, 0, 3, 0, 9, 0, 0xde, 0xad, 0xbe, 0xef };
    struct cbd_file_segment fs;
    CHECK(cbd_bsp_file_segment(seg_req, sizeof seg_req, &fs) && fs.type == 9 && fs.segment == 3 && fs.count == 9 && fs.bytes == 4);
    uint8_t seg_tlv[8];
    const uint32_t segt = cbd_bsp_file_answer(&fs, seg_tlv, sizeof seg_tlv);
    n = cbq_private_answer(seg_req, CBD_SVC_BSP, 1, 0, seg_tlv, segt, out, sizeof out);
    const uint8_t seg_ok[] = { 1, 24, 0, 0x80, 0xe2, 1, 2, 4, 0, 0x00, 0xa0, 12, 0, 2, 4, 0, 0, 0, 0, 0, 0x40, 2, 0, 4, 0 };
    CHECK(n == sizeof seg_ok && !memcmp(out, seg_ok, sizeof seg_ok));
    /* After the last segment: FileTransferStatus 0xa001 with {type 9, success} and subscription 1. */
    uint8_t done_tlv[12];
    const uint32_t dt = cbd_bsp_file_done(&fs, done_tlv, sizeof done_tlv);
    n = cbq_private_indication(CBD_BSP_FILE_TRANSFER_STATUS, CBD_SVC_BSP, 1, done_tlv, dt, out, sizeof out);
    const uint8_t done_ind[] = { 1, 21, 0, 0x80, 0xe2, 1, 4, 0, 0, 0x01, 0xa0, 9, 0, 1, 2, 0, 9, 0, 0x10, 1, 0, 1 };
    CHECK(dt == 9 && n == sizeof done_ind && !memcmp(out, done_ind, sizeof done_ind));
    /* BSP 0xb001: chip id and serial, little endian, chip id first. */
    uint8_t chip_tlv[12];
    CHECK(cbd_bsp_chip_info(0x009540e1u, 0x0c0ac42bu, chip_tlv, sizeof chip_tlv) == 11 && chip_tlv[0] == 1 && chip_tlv[1] == 8 &&
          chip_tlv[3] == 0xe1 && chip_tlv[5] == 0x95 && chip_tlv[7] == 0x2b && chip_tlv[10] == 0x0c);
    uint8_t seg_bad[sizeof seg_req]; memcpy(seg_bad, seg_req, sizeof seg_req); seg_bad[12] = 9;   /* segment past the count */
    CHECK(!cbd_bsp_file_segment(seg_bad, sizeof seg_bad, &fs));
    n = cbq_private_unsupported(act, 0xe5, 2, out, sizeof out);
    CHECK(n == 20 && out[4] == 0xe5 && cbq_rd16(out + 16) == 1 && cbq_rd16(out + 18) == CBQ_ERR_NOT_SUPPORTED);
    /* Apple's report configuration 0x555b on NAS and WDS is answered here; other messages, services and responses still go to the modem. */
    const uint8_t freq_req[] = { 0, 7, 0, 0x5b, 0x55, 4, 0, 0xb0, 1, 0, 1 };
    const uint8_t stats_req[] = { 0, 9, 0, 0x5b, 0x55, 24, 0, 1, 9, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0x10, 9, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(cbd_vendor_reporting(CBD_SVC_NAS, freq_req, sizeof freq_req) && cbd_vendor_reporting(CBD_SVC_WDS, stats_req, sizeof stats_req));
    CHECK(!cbd_vendor_reporting(CBD_SVC_BSP, freq_req, sizeof freq_req) && !cbd_vendor_reporting(CBD_SVC_NAS, freq_req, sizeof freq_req - 1));
    uint8_t cell_req[sizeof freq_req]; memcpy(cell_req, freq_req, sizeof freq_req); cell_req[3] = 0x56;   /* 0x5556 is not a setting */
    uint8_t freq_rsp[sizeof freq_req]; memcpy(freq_rsp, freq_req, sizeof freq_req); freq_rsp[0] = 2;
    CHECK(!cbd_vendor_reporting(CBD_SVC_NAS, cell_req, sizeof cell_req) && !cbd_vendor_reporting(CBD_SVC_NAS, freq_rsp, sizeof freq_rsp));
    n = cbq_private_answer(freq_req, CBD_SVC_NAS, 3, 0, NULL, 0, out, sizeof out);
    CHECK(n == 20 && out[4] == CBD_SVC_NAS && out[5] == 3 && out[6] == 2 && cbq_rd16(out + 7) == 7 && cbq_rd16(out + 9) == 0x555b &&
          cbq_rd16(out + 16) == 0 && cbq_rd16(out + 18) == 0);
    /* Two WDS clients on one mux: only modem success activates each family, and stopping IPv4 preserves IPv6. */
    struct cbp_client calls[2] = { { 0 }, { 0 } };
    uint8_t wire[64], reply[64], v;
    for (uint32_t i = 0; i < 2; i++) {
        v = 1;
        uint32_t wn = cbd_request(wire, 1, CBD_WDS_BIND_MUX_DATA_PORT, cbd_tlv(wire + 7, 57, 0x11, &v, 1));
        CHECK(cbp_observe(&calls[i], wire, wn) && !calls[i].mux);
        uint32_t rn = cbq_private_answer(wire, 1, 1, 0, NULL, 0, reply, sizeof reply);
        CHECK(cbp_observe(&calls[i], reply + CBQ_HDR, rn - CBQ_HDR) && calls[i].mux == 1);
        v = i ? 6 : 4;
        wn = cbd_request(wire, 2, CBP_FAMILY, cbd_tlv(wire + 7, 57, 1, &v, 1));
        CHECK(cbp_observe(&calls[i], wire, wn));
        rn = cbq_private_answer(wire, 1, 1, 0, NULL, 0, reply, sizeof reply);
        CHECK(cbp_observe(&calls[i], reply + CBQ_HDR, rn - CBQ_HDR));
        wn = cbd_request(wire, 3, CBP_START, 0);
        CHECK(cbp_observe(&calls[i], wire, wn) && !calls[i].active);
        uint8_t handle[7];
        uint32_t extra = cbd_tlv_u32(handle, sizeof handle, 1, 0x1234 + i);
        rn = cbq_private_answer(wire, 1, 1, 0, handle, extra, reply, sizeof reply);
        CHECK(cbp_observe(&calls[i], reply + CBQ_HDR, rn - CBQ_HDR) && calls[i].active);
    }
    CHECK(cbp_active_families(calls, 2, 1) == 3 && !cbp_active_families(calls, 2, 2));
    cbd_request(wire, 4, CBP_STOP, 0);
    CHECK(cbp_observe(&calls[0], wire, 7));
    uint32_t rn = cbq_private_answer(wire, 1, 1, 0, NULL, 0, reply, sizeof reply);
    CHECK(cbp_observe(&calls[0], reply + CBQ_HDR, rn - CBQ_HDR));
    CHECK(cbp_active_families(calls, 2, 1) == 2);
    const uint8_t disconnected[] = { 4, 0, 0, CBP_STATUS, 0, 5, 0, 1, 2, 0, 1, 0 };
    CHECK(cbp_observe(&calls[1], disconnected, sizeof disconnected));
    CHECK(!cbp_active_families(calls, 2, 1));
    if (!failures) printf("PASS data port and activation\n");
}

/* Synthetic wire data only; this checks transport/state, not an eUICC or RSP server. */
static struct cbe esim;
static uint8_t esim_in[8192], esim_resp[8192];
static void esim_answer(uint32_t now, const uint8_t *tlvs, uint32_t n, uint32_t error)
{
    uint8_t s[2048] = { CBQ_QMI_RESPONSE };
    cbd_wr16(s + 1, esim.wait_txn); cbd_wr16(s + 3, esim.wait_msg);
    s[7] = 2; s[8] = 4;
    cbd_wr16(s + 10, error != 0); cbd_wr16(s + 12, error);
    if (n) memcpy(s + 14, tlvs, n);
    cbd_wr16(s + 5, 7 + n);
    esim.req_len = 0;
    CHECK(cbe_answer(&esim, s, 14 + n, now));
}
static void esim_slots_card(uint32_t now, uint32_t active, uint32_t euicc, uint32_t card)
{
    uint8_t t[96], st[21] = { 2 }, info[15] = { 2 }, eid[19] = { 2, 0, 16 };
    for (unsigned i = 0; i < 2; i++) {
        cbd_wr32(st + 1 + 10 * i, i == euicc || card ? 2 : 1); cbd_wr32(st + 5 + 10 * i, i == active);
        st[9 + 10 * i] = i == active ? 1 : 0;
    }
    /* Two present cards. The eUICC's EID is a synthetic byte sequence. */
    info[7 + 7 * euicc] = 1;
    memset(eid + 1, 0, sizeof eid - 1);
    eid[1 + euicc] = 16;
    for (unsigned i = 0; i < 16; i++) eid[2 + euicc + i] = (uint8_t)(0xa0 + i);
    uint32_t n = cbd_tlv(t, sizeof t, 0x10, st, sizeof st);
    n += cbd_tlv(t + n, sizeof t - n, 0x11, info, sizeof info);
    n += cbd_tlv(t + n, sizeof t - n, 0x12, eid, sizeof eid);
    esim_answer(now, t, n, 0);
}
static void esim_slots_layout(uint32_t now, uint32_t active, uint32_t euicc)
{
    esim_slots_card(now, active, euicc, 1);
}
static void esim_slots(uint32_t now, uint32_t active)
{
    esim_slots_layout(now, active, 1);
}
static void esim_reset(void)
{
    cbe_init(&esim, esim_in, sizeof esim_in, esim_resp, sizeof esim_resp);
    cbe_tick(&esim, 0);
    CHECK(esim.step == CBE_S_SLOTS);
    esim_slots(1, 0);
    CHECK(esim.slots.euicc == 2 && esim.slots.active == 1 && esim.slots.eid_len == 16);
}
static void esim_tape_part(uint8_t command, uint8_t cid, const uint8_t *data, uint32_t n, uint32_t total, uint32_t index, uint8_t final, uint32_t now)
{
    uint8_t s[1600] = { 0 }, out[128], header[6] = { 1, 0x12, 0x34, 0x56, 0x78, 2 }, payload[1505] = { 0 };
    header[0] = command; payload[0] = final;
    cbd_wr16(payload + 1, total); cbd_wr16(payload + 3, index); memcpy(payload + 5, data, n);
    uint32_t w = cbd_tlv(s + 7, sizeof s - 7, 1, header, sizeof header);
    w += cbd_tlv(s + 7 + w, sizeof s - 7 - w, 0x10, payload, n + 5);
    cbd_request(s, 9 + index, 0x1002, w);
    const uint32_t answer = cbe_host(&esim, 1, cid, s, 7 + w, out, sizeof out, now);
    CHECK(answer && answer != CBE_DEFERRED && !cbd_error(out + 6, answer - 6));
    CHECK(cbd_rd16(out + 7) == 9 + index && out[4] == 0xe5 && out[5] == cid);
}
static void esim_tape_command(uint8_t command, uint8_t cid, const uint8_t *data, uint32_t n, uint32_t total, uint32_t index, uint32_t now)
{
    esim_tape_part(command, cid, data, n, total, index, 1, now);
}
static void esim_tape(const uint8_t *data, uint32_t n, uint32_t total, uint32_t index)
{
    esim_tape_command(1, 9, data, n, total, index, 2);
}
static void esim_open(void)
{
    cbe_tick(&esim, 3);
    CHECK(esim.step == CBE_S_SWITCH && esim.freeze);
    uint8_t caps[2]; cbe_caps(&esim, caps);
    CHECK(caps[0] == 0 && caps[1] == 2);
    esim_answer(4, NULL, 0, 0);
    cbe_tick(&esim, 6003); CHECK(esim.step == CBE_S_SETTLE);
    cbe_tick(&esim, 6004); CHECK(esim.step == CBE_S_RESLOT);
    esim_slots(6005, 1);
    const uint8_t ch[] = { 0x10, 1, 0, 1 };
    esim_answer(6006, ch, sizeof ch, 0);
    CHECK(esim.step == CBE_S_APDU && esim.channel_open);
}
/* The fresh card status every restore publishes (cbe_restore), delivered as coachz_baseband.cpp does: card state 1 present, 0 absent. */
static const uint8_t *esim_card_publish(uint32_t now, uint8_t state)
{
    CHECK(esim.step == CBE_S_CARD_STATUS && esim.wait_msg == CBE_UIM_GET_CARD_STATUS && esim.freeze);
    const uint8_t card[] = { 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 1, state, 0, 0, 0, 0, 0 };
    static uint8_t published[CBE_CARD_BYTES];
    uint8_t tlvs[32];
    esim_answer(now, tlvs, cbd_tlv(tlvs, sizeof tlvs, 0x10, card, sizeof card), 0);
    CHECK(esim.card_status_len && esim.card_status_len <= sizeof published);
    CHECK(esim.card_status[0] == CBQ_QMI_INDICATION && cbd_rd16(esim.card_status + 3) == CBE_UIM_CARD_STATUS_CHANGE);
    memcpy(published, esim.card_status, esim.card_status_len);
    esim.card_status_len = 0; esim.needs_card = esim.freeze = 0;
    return published;
}
static void esim_restore(uint32_t now)
{
    if (esim.session == CBE_SESSION_BATCH && esim.step == CBE_S_IDLE) {
        now = esim.session_deadline;
        cbe_tick(&esim, now);
    }
    CHECK(esim.step == CBE_S_CLOSE);
    esim_answer(now, NULL, 0, 0);
    CHECK(esim.step == CBE_S_RESTORE);
    esim_answer(now + 1, NULL, 0, 0);
    cbe_tick(&esim, now + 6001);
    esim_slots(now + 6002, 0);
    CHECK(!esim.route_failed && esim.slots.active == 1);
    esim_card_publish(now + 6003, 1);
    CHECK(esim.step == CBE_S_IDLE && !esim.freeze);
}
static void esim_apdu_response(uint32_t now, const uint8_t *data, uint32_t n, uint8_t sw1, uint8_t sw2)
{
    uint8_t t[1600];
    t[0] = 0x10; cbd_wr16(t + 1, n + 4); cbd_wr16(t + 3, n + 2);
    if (n) memcpy(t + 5, data, n);
    t[5 + n] = sw1; t[6 + n] = sw2;
    esim_answer(now, t, n + 7, 0);
}
static void test_esim(void)
{
    esim_reset();
    uint8_t start[7], stop[7]; cbd_request(start, 1, 0x20, 0); cbd_request(stop, 2, 0x21, 0);
    CHECK(!cbe_blocks_data_start(&esim, CBD_SVC_WDS, start, sizeof start));
    esim.freeze = 1;
    CHECK(cbe_blocks_data_start(&esim, CBD_SVC_WDS, start, sizeof start));
    CHECK(!cbe_blocks_data_start(&esim, CBD_SVC_WDS, stop, sizeof stop));
    CHECK(!cbe_blocks_data_start(&esim, CBE_SVC_UIM, start, sizeof start));
    esim.freeze = 0; esim.route_failed = 1;
    CHECK(cbe_blocks_data_start(&esim, CBD_SVC_WDS, start, sizeof start));
    esim.route_failed = 0;
    /* Card status retains its transaction/other TLVs; the capability array is replaced, not duplicated. */
    const uint8_t card[] = { 2, 0x34, 0x12, 0x2f, 0, 16, 0, 2, 4, 0, 0, 0, 0, 0,
                             0x10, 1, 0, 0xaa, 0x21, 2, 0, 0xff, 0xff };
    uint8_t out[2048], channel = 0; uint32_t len = 0;
    uint32_t n = cbe_card_status_fix(&esim, 11, card, sizeof card, out, sizeof out);
    const uint8_t *caps = cbd_find(out, n, 0x21, &len);
    CHECK(n == sizeof card && out[1] == 0x34 && out[2] == 0x12 && caps && len == 2 && !caps[0] && caps[1] == 2);
    const uint8_t *kept = cbd_find(out, n, 0x10, &len);
    CHECK(kept && len == 1 && kept[0] == 0xaa);

    /* Hotplug must not erase Tape capability: the driver replaces its vector on every indication. */
    const uint8_t change[] = { 4, 0, 0, 0x32, 0, 4, 0, 0x10, 1, 0, 0xaa };
    n = cbe_card_status_fix(&esim, 11, change, sizeof change, out, sizeof out);
    caps = cbd_find(out, n, 0x21, &len);
    CHECK(n == sizeof change + 5 && out[0] == 4 && out[3] == 0x32 && caps && len == 2 && !caps[0] && caps[1] == 2);
    kept = cbd_find(out, n, 0x10, &len);
    CHECK(kept && len == 1 && kept[0] == 0xaa);

    /* The real startup wire request is legacy GetCsn, not a Tape/APDU operation.
     * Reply with the exact absent-legacy-Vinyl result used by the native driver's Ready callback. */
    uint8_t csn[] = { 0, 0x78, 0x56, 0, 0x10, 9, 0, 2, 6, 0, 1, 1, 0, 0, 0, 1 };
    for (unsigned slot = 1; slot <= 2; slot++) {
        csn[15] = (uint8_t)slot;
        n = cbe_host(&esim, 1, 9, csn, sizeof csn, out, sizeof out, 2);
        CHECK(n == CBQ_HDR + 14 && out[4] == 0xe5 && out[5] == 9);
        CHECK(out[CBQ_HDR + 1] == 0x78 && out[CBQ_HDR + 2] == 0x56);
        CHECK(cbd_error(out + CBQ_HDR, n - CBQ_HDR) == 0x204);
        CHECK(!esim.req_len && esim.step == CBE_S_IDLE && !esim.freeze && !esim.switches);
        uint8_t current[2]; cbe_caps(&esim, current);
        CHECK(!current[0] && current[1] == CBE_CAP_TAPE);
    }
    csn[10] = 2; /* unimplemented legacy operations keep the existing fallback */
    CHECK(cbe_host(&esim, 1, 9, csn, sizeof csn, out, sizeof out, 2) == 0);

    /* Native EID/device-capability writers both use TLV 1. Capabilities leave slot zero. */
    uint8_t query[32] = { 0 }, h[5] = { 0x12, 0x34, 0x56, 0x78, 0 };
    uint32_t q = cbd_tlv(query + 7, sizeof query - 7, 1, h, sizeof h);
    cbd_request(query, 0x4321, 0x1005, q);
    n = cbe_host(&esim, 1, 9, query, 7 + q, out, sizeof out, 2);
    CHECK(n && !cbd_error(out + 6, n - 6));
    const uint8_t *value = cbd_find(out + 6, n - 6, 0x10, &len);
    CHECK(value && len == 4 && cbd_rd32(value) == 0x78563412);
    value = cbd_find(out + 6, n - 6, 0x11, &len);
    CHECK(value && len == 6 && value[0] == 12);
    h[4] = 2;
    q = cbd_tlv(query + 7, sizeof query - 7, 1, h, sizeof h);
    cbd_request(query, 0x4322, 0x1004, q);
    n = cbe_host(&esim, 1, 9, query, 7 + q, out, sizeof out, 2);
    value = cbd_find(out + 6, n - 6, 0x11, &len);
    CHECK(n && !cbd_error(out + 6, n - 6) && value && len == 16 && !memcmp(value, esim.slots.eid, 16));

    /* GetSessionData is header-only and has no ES10 APDU: the modem-local record set is empty. */
    uint8_t header[6] = { 0x15, 0x12, 0x34, 0x56, 0x78, 2 };
    q = cbd_tlv(query + 7, sizeof query - 7, 1, header, sizeof header);
    cbd_request(query, 0x4323, 0x1002, q);
    n = cbe_host(&esim, 1, 9, query, 7 + q, out, sizeof out, 2);
    CHECK(n && !cbd_error(out + 6, n - 6));
    cbe_tick(&esim, 3);
    CHECK(esim.done_pending && esim.step == CBE_S_IDLE && !esim.req_len && !esim.switches);
    n = cbe_output(&esim, &channel, out, sizeof out);
    value = cbd_find(out + 6, n - 6, 1, &len);
    CHECK(n && value && len == 11 && value[0] == 0x15 && !cbd_rd16(value + 1) && cbd_rd32(value + 5) == 0x78563412);
    CHECK(!cbd_find(out + 6, n - 6, 0x10, &len));

    /* A two-part command does not run before its last segment. Short APDUs preserve its full DER. */
    uint8_t der[1505] = { 0xbf, 0x20, 0x82, 0x05, 0xdc };
    for (unsigned i = 5; i < sizeof der; i++) der[i] = (uint8_t)i;
    esim_tape(der, 1500, 2, 0); cbe_tick(&esim, 2); CHECK(esim.step == CBE_S_IDLE);
    esim_tape(der + 1500, 5, 2, 1); esim_open();
    uint32_t at = 0, block = 0;
    while (esim.step == CBE_S_APDU && at < sizeof der) {
        const uint8_t *a = cbd_find(esim.req, esim.req_len, 2, &len);
        const uint32_t take = sizeof der - at < 255 ? sizeof der - at : 255;
        CHECK(a && len == 2 + 5 + take + (at + take == sizeof der));
        if (!a) return;
        CHECK(a[2] == 0x80 && a[3] == 0xe2 && a[4] == (at + take == sizeof der ? 0x91 : 0x11) && a[5] == block);
        CHECK(a[6] == take && !memcmp(a + 7, der + at, take));
        at += take; block++;
        if (at < sizeof der) esim_apdu_response(6010 + block, NULL, 0, 0x90, 0);
    }
    CHECK(at == sizeof der && block == 6);
    /* 61xx continuation assembles a response bigger than one Tape indication. */
    uint32_t received = 0, now = 6030;
    while (received < sizeof der) {
        const uint32_t take = sizeof der - received < 255 ? sizeof der - received : 255;
        if (received) {
            const uint8_t *a = cbd_find(esim.req, esim.req_len, 2, &len);
            CHECK(a && a[3] == 0xc0 && a[len - 1] == 0xff);
        }
        esim_apdu_response(now++, der + received, take, received + take < sizeof der ? 0x61 : 0x90,
                           received + take < sizeof der ? 0xff : 0);
        received += take;
    }
    esim_restore(now);
    CHECK(!esim.done_status && esim.done_len == sizeof der);
    for (unsigned index = 0; index < 2; index++) {
        n = cbe_output(&esim, &channel, out, sizeof out);
        CHECK(n && channel == 1 && out[4] == 0xe5 && out[5] == 9 && out[6] == 4 && cbd_rd16(out + 9) == 0x1003);
        const uint8_t *info = cbd_find(out + 6, n - 6, 1, &len);
        CHECK(info && len == 11 && !cbd_rd16(info + 1) && !cbd_rd16(info + 3) && cbd_rd32(info + 5) == 0x78563412 && !info[9] && !info[10]);
        const uint8_t *data = cbd_find(out + 6, n - 6, 0x10, &len);
        CHECK(data && len == 4 + (index ? 5 : 1500) && cbd_rd16(data) == 2 && cbd_rd16(data + 2) == index);
        CHECK(data && !memcmp(data + 4, der + index * 1500, index ? 5 : 1500));
    }
    CHECK(!esim.done_pending);

    /* BPP's structural pieces stay in order; no cryptography or real profile is modelled. */
    const uint8_t bpp[] = { 0xbf, 0x36, 17, 0xbf, 0x23, 0, 0xa0, 0, 0xa1, 3, 0x88, 1, 0xaa, 0xa2, 0, 0xa3, 3, 0x86, 1, 0xbb };
    const uint32_t offsets[] = { 0, 6, 8, 10, 13, 15, 17 }, lengths[] = { 6, 2, 2, 3, 2, 2, 3 };
    struct cbe_seg segments[8];
    CHECK(cbe_segments(bpp, sizeof bpp, segments, 8) == 7);
    for (unsigned i = 0; i < 7; i++) CHECK(segments[i].off == offsets[i] && segments[i].len == lengths[i]);

    /* Real Tape framing: one transaction, independent part indexes, final flag only on the last part.
     * The first BF36 header declares more than a transport chunk; nothing may run before its tail. */
    uint8_t large_bpp[2016] = { 0xbf, 0x36, 0x82, 0x07, 0xdb, 0xbf, 0x23, 0,
                               0xa3, 0x82, 0x07, 0xd4, 0x86, 0x82, 0x07, 0xd0 };
    esim_reset();
    esim_tape_part(CBE_TAPE_LOAD_BPP, 9, large_bpp, 8, 1, 0, 0, 2);
    cbe_tick(&esim, 2); CHECK(esim.active < 0 && !esim.req_len && !esim.done_pending);
    esim_tape_part(CBE_TAPE_LOAD_BPP, 9, large_bpp + 8, 4, 1, 0, 0, 3);
    esim_tape_part(CBE_TAPE_LOAD_BPP, 9, large_bpp + 12, 1500, 2, 0, 1, 4);
    cbe_tick(&esim, 4); CHECK(esim.active < 0 && !esim.req_len && !esim.done_pending);
    esim_tape_part(CBE_TAPE_LOAD_BPP, 9, large_bpp + 1512, 504, 2, 1, 1, 5);
    int assembled = -1;
    for (unsigned i = 0; i < CBE_JOBS; i++) if (esim.job[i].state == CBE_J_READY) assembled = (int)i;
    CHECK(assembled >= 0);
    if (assembled >= 0) {
        CHECK(esim.job[assembled].len == sizeof large_bpp && esim.job[assembled].cap == sizeof large_bpp);
        CHECK(!memcmp(esim.in + esim.job[assembled].off, large_bpp, sizeof large_bpp));
    }
    cbe_tick(&esim, 6); CHECK(esim.active >= 0 && esim.seg_count == 3 && !esim.done_pending);

    /* Challenge/authenticate reuse the same channel until LoadBoundProfilePackage completes. */
    const uint8_t session_der[] = { 0xbf, 0x2e, 0 };
    esim_reset(); esim_tape_command(CBE_TAPE_CHALLENGE, 9, session_der, sizeof session_der, 1, 0, 2);
    esim_open();
    esim_apdu_response(6007, session_der, sizeof session_der, 0x90, 0);
    CHECK(esim.session && esim.channel_open && esim.freeze && esim.step == CBE_S_IDLE && esim.done_pending);
    n = cbe_output(&esim, &channel, out, sizeof out);
    CHECK(n && !esim.done_pending);
    /* A different client's queued command cannot take the lease. */
    esim_tape_command(1, 10, session_der, sizeof session_der, 1, 0, 6008);
    cbe_tick(&esim, 6008); CHECK(esim.step == CBE_S_IDLE && esim.active < 0 && !esim.req_len);
    esim_tape_command(CBE_TAPE_AUTHENTICATE, 9, session_der, sizeof session_der, 1, 0, 6009);
    cbe_tick(&esim, 6009);
    CHECK(esim.step == CBE_S_APDU && esim.wait_msg == CBE_UIM_SEND_APDU && esim.switches == 1 && esim.channel_id == 1);
    esim_apdu_response(6010, session_der, sizeof session_der, 0x90, 0);
    CHECK(esim.session && esim.step == CBE_S_IDLE);
    n = cbe_output(&esim, &channel, out, sizeof out); CHECK(n);
    esim_tape_command(CBE_TAPE_LOAD_BPP, 9, session_der, sizeof session_der, 1, 0, 6011);
    cbe_tick(&esim, 6011); CHECK(esim.step == CBE_S_APDU && esim.switches == 1);
    esim_apdu_response(6012, session_der, sizeof session_der, 0x90, 0);
    CHECK(esim.session == CBE_SESSION_BATCH && esim.step == CBE_S_IDLE && esim.switches == 1);
    CHECK(cbe_output(&esim, &channel, out, sizeof out));
    /* The notification immediately following the package reuses its slot and channel. */
    const uint8_t notification[] = { 0xbf, 0x28, 0 };
    esim_tape_command(7, 9, notification, sizeof notification, 1, 0, 6013);
    cbe_tick(&esim, 6013); CHECK(esim.step == CBE_S_APDU && esim.switches == 1);
    esim_apdu_response(6014, session_der, sizeof session_der, 0x90, 0);
    CHECK(cbe_output(&esim, &channel, out, sizeof out));
    esim_tape_command(CBE_TAPE_ENABLE, 9, session_der, sizeof session_der, 1, 0, 6015);
    cbe_tick(&esim, 6015); CHECK(esim.step == CBE_S_APDU && esim.switches == 1);
    esim_apdu_response(6016, session_der, sizeof session_der, 0x90, 0);
    CHECK(!esim.session && esim.step == CBE_S_CLOSE);
    esim_restore(6017);
    CHECK(esim.switches == 2 && !esim.channel_open && !esim.freeze);

    /* A lease with no next command times out through close + restore, not a stuck UIM freeze. */
    esim_reset(); esim_tape_command(CBE_TAPE_CHALLENGE, 9, session_der, sizeof session_der, 1, 0, 2);
    esim_open(); esim_apdu_response(6007, session_der, sizeof session_der, 0x90, 0);
    n = cbe_output(&esim, &channel, out, sizeof out); CHECK(n);
    cbe_tick(&esim, 6007 + CBE_SESSION_IDLE_MS);
    CHECK(!esim.session && esim.step == CBE_S_CLOSE);
    esim_restore(6008 + CBE_SESSION_IDLE_MS);

    /* A timed-out APDU still closes and restores; its late response cannot complete the close. */
    const uint8_t small[] = { 0xbf, 0x20, 0 };
    esim_reset(); esim_tape(small, sizeof small, 1, 0); esim_open();
    uint8_t late[14] = { 2, 0, 0, 0x3b, 0, 7, 0, 2, 4, 0, 0, 0, 0, 0 };
    cbd_wr16(late + 1, esim.wait_txn);
    cbe_tick(&esim, 16006); CHECK(esim.step == CBE_S_CLOSE && esim.done_status == 1);
    CHECK(!cbe_answer(&esim, late, sizeof late, 16007) && esim.step == CBE_S_CLOSE);
    esim_restore(16008); CHECK(esim.done_status == 1);

    /* A failed restoration is not reported as a successful operation. */
    esim_reset(); esim_tape(small, sizeof small, 1, 0); esim_open();
    esim_apdu_response(6007, small, sizeof small, 0x90, 0);
    cbe_tick(&esim, esim.session_deadline);
    esim_answer(9010, NULL, 0, 0); CHECK(esim.step == CBE_S_RESTORE);
    esim_answer(6009, NULL, 0, CBE_ERR_INTERNAL);
    CHECK(esim.done_status == 1 && esim.route_failed && !esim.slots.known);
    if (!failures) printf("PASS esim tape transport and cleanup\n");
}

/* A repeated read-only query is answered from the cache without a slot switch; a command that changes the card clears it. */
static uint8_t esim_cache[CBE_CACHE_ENTRIES * 1024];
static void esim_drain(void)
{
    uint8_t out[2048], channel = 0;
    for (unsigned i = 0; i < 8 && cbe_output(&esim, &channel, out, sizeof out); i++) {}
    CHECK(!esim.done_pending);
}
static void test_esim_cache(void)
{
    const uint8_t query[] = { 0xbf, 0x2d, 0 }, answer[] = { 0xbf, 0x2d, 2, 0xa0, 0 }, del[] = { 0xbf, 0x33, 0 };
    const uint8_t challenge[] = { 0xbf, 0x2e, 0 };
    CHECK(cbe_cacheable(query, sizeof query) && !cbe_cacheable(challenge, sizeof challenge) && !cbe_cacheable(del, sizeof del));
    esim_reset(); cbe_set_cache(&esim, esim_cache, sizeof esim_cache);
    esim_tape_command(13, 9, query, sizeof query, 1, 0, 2); esim_open();
    esim_apdu_response(6007, answer, sizeof answer, 0x90, 0);
    esim_restore(6008);
    CHECK(esim.cached[0].used && esim.cached[0].resp_len == sizeof answer);
    esim_drain();
    const uint32_t switches = esim.switches;
    esim_tape_command(13, 9, query, sizeof query, 1, 0, 20000);
    cbe_tick(&esim, 20001);
    CHECK(esim.step == CBE_S_IDLE && !esim.freeze && !esim.req_len && esim.switches == switches);
    CHECK(esim.done_pending && esim.done_len == sizeof answer && esim.done_sw1 == 0x90 && !memcmp(esim.resp, answer, sizeof answer));
    esim_drain();
    /* ListNotification is read-only. Alternating it with GetProfilesInfo must not evict the profiles answer
     * and switch away from the physical SIM again on every initialization pass. */
    const uint8_t notifications[] = { 0xbf, 0x28, 0 }, empty_notifications[] = { 0xbf, 0x28, 2, 0xa0, 0 };
    CHECK(cbe_cacheable(notifications, sizeof notifications));
    esim_tape_command(7, 10, notifications, sizeof notifications, 1, 0, 20002); esim_open();
    esim_apdu_response(6007, empty_notifications, sizeof empty_notifications, 0x90, 0);
    esim_restore(6008); esim_drain();
    const uint32_t after_notifications = esim.switches;
    esim_tape_command(13, 11, query, sizeof query, 1, 0, 40000);
    cbe_tick(&esim, 40001);
    CHECK(esim.step == CBE_S_IDLE && !esim.req_len && esim.switches == after_notifications);
    CHECK(esim.done_pending && esim.done_len == sizeof answer && !memcmp(esim.resp, answer, sizeof answer));
    esim_drain();
    esim_tape_command(7, 12, notifications, sizeof notifications, 1, 0, 40002);
    cbe_tick(&esim, 40003);
    CHECK(esim.step == CBE_S_IDLE && !esim.req_len && esim.switches == after_notifications);
    CHECK(esim.done_pending && esim.done_len == sizeof empty_notifications && !memcmp(esim.resp, empty_notifications, sizeof empty_notifications));
    esim_drain();
    esim_tape_command(CBE_TAPE_DELETE, 9, del, sizeof del, 1, 0, 20002); esim_open();
    esim_apdu_response(6007, NULL, 0, 0x90, 0);
    esim_restore(6008);
    for (unsigned i = 0; i < CBE_CACHE_ENTRIES; i++) CHECK(!esim.cached[i].used);
    if (!failures) printf("PASS esim read-only query cache\n");
}

static void test_esim_selection(void)
{
    const uint8_t enable[] = { 0xbf, 0x31, 0 };
    const uint8_t enabled[] = { 0xbf, 0x31, 3, 0x80, 1, 0 };
    const uint8_t denied[] = { 0xbf, 0x31, 3, 0x80, 1, 2 };
    const uint8_t disabled[] = { 0xbf, 0x32, 3, 0x80, 1, 0 };
    uint8_t out[2048], channel, caps[2];
    uint32_t len;
    CHECK(cbe_profile_result(CBE_TAPE_ENABLE, enabled, sizeof enabled) == 0);
    CHECK(cbe_profile_result(CBE_TAPE_ENABLE, denied, sizeof denied) == 2);
    CHECK(cbe_profile_result(CBE_TAPE_ENABLE, disabled, sizeof disabled) == UINT32_MAX);
    CHECK(cbe_profile_result(CBE_TAPE_ENABLE, enabled, sizeof enabled - 1) == UINT32_MAX);

    /* SW9000 with a nonzero ES10c result still restores the physical SIM. */
    esim_reset();
    esim_tape_command(CBE_TAPE_ENABLE, 9, enable, sizeof enable, 1, 0, 2); esim_open();
    esim_apdu_response(6007, denied, sizeof denied, 0x90, 0);
    esim_restore(6008);
    CHECK(esim.selected_phys == 1 && esim.route_generation == 0 && !esim.needs_card);
    CHECK(esim.done_len == sizeof denied && !memcmp(esim.resp, denied, sizeof denied));

    /* A real EnableProfile result keeps the eUICC mapped, then asks for fresh card status. */
    esim_reset();
    esim_tape_command(CBE_TAPE_ENABLE, 9, enable, sizeof enable, 1, 0, 2); esim_open();
    esim_apdu_response(6007, enabled, sizeof enabled, 0x90, 0);
    CHECK(esim.route_commit && esim.restore_phys == 2 && esim.selected_phys == 1);
    esim_answer(6008, NULL, 0, 0); CHECK(esim.step == CBE_S_RESTORE_SETTLE);
    cbe_tick(&esim, 12008); esim_slots(12009, 1);
    CHECK(esim.selected_phys == 2 && esim.route_generation == 1 && esim.freeze);
    CHECK(esim.step == CBE_S_CARD_STATUS && esim.wait_msg == CBE_UIM_GET_CARD_STATUS && esim.switches == 1);
    CHECK(!cbe_output(&esim, &channel, out, sizeof out));
    /* The indication keeps the modem's card and application states, strips the response result and keeps the original Tape
     * management slot across the radio-route change. With the eUICC selected its record moves to endpoint two, and endpoint one
     * (the physical card, present) carries presence only (coachz_bb_slots.h). */
    const uint8_t card[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 1, 1, 0, 0, 0, 0, 0 };
    uint8_t tlvs[32];
    const uint32_t tlv_len = cbd_tlv(tlvs, sizeof tlvs, 0x10, card, sizeof card);
    esim_answer(12010, tlvs, tlv_len, 0);
    CHECK(esim.card_status_len && esim.freeze && !cbe_output(&esim, &channel, out, sizeof out));
    CHECK(esim.card_status[0] == CBQ_QMI_INDICATION && cbd_rd16(esim.card_status + 3) == CBE_UIM_CARD_STATUS_CHANGE);
    const uint8_t *value = cbd_find(esim.card_status, esim.card_status_len, 0x10, &len);
    CHECK(value && len == sizeof card + 6u && value[8] == 2 && !memcmp(value, card, 8));
    CHECK(value && value[9] == 1 && !value[14] && !memcmp(value + 15, card + 9, sizeof card - 9));
    CHECK(esim.card_mapped == 1);
    CHECK(!cbd_find(esim.card_status, esim.card_status_len, 0x02, &len));
    value = cbd_find(esim.card_status, esim.card_status_len, CBE_UIM_CAPABILITY_TLV, &len);
    CHECK(value && len == 2 && value[0] == CBE_CAP_NONE && value[1] == CBE_CAP_TAPE);
    esim.card_status_len = 0; esim.needs_card = esim.freeze = 0; esim_drain();
    cbe_caps(&esim, caps); CHECK(caps[0] == 0 && caps[1] == 2);
    /* Existing clients still address EID to the same endpoint after the switch. */
    uint8_t eid_request[24] = { 0 }, eid_header[] = { 3, 0, 0, 0, 2 };
    uint32_t eid_len = cbd_tlv(eid_request + 7, sizeof eid_request - 7, 1, eid_header, sizeof eid_header);
    cbd_request(eid_request, 38, CBE_VINYL_TAPE_EID, eid_len);
    const uint32_t eid_answer = cbe_host(&esim, 1, 9, eid_request, eid_len + 7, out, sizeof out, 12050);
    CHECK(eid_answer && eid_answer != CBE_DEFERRED && !cbd_error(out + CBQ_HDR, eid_answer - CBQ_HDR));
    value = cbd_find(out + CBQ_HDR, eid_answer - CBQ_HDR, 0x11, &len);
    CHECK(value && len == 16 && !memcmp(value, esim.slots.eid, len));

    /* A subsequent management operation on the selected eUICC must not resurrect
     * the boot-time physical mapping. Replay the original management slot two. */
    uint8_t query_request[64] = { 0 }, query_header[] = { 13, 2, 0, 0, 0, 2 };
    const uint8_t query_payload[] = { 1, 1, 0, 0, 0, 0xbf, 0x2d, 0 };
    uint32_t query_len = cbd_tlv(query_request + 7, sizeof query_request - 7, 1, query_header, sizeof query_header);
    query_len += cbd_tlv(query_request + 7 + query_len, sizeof query_request - 7 - query_len, 0x10, query_payload, sizeof query_payload);
    cbd_request(query_request, 39, CBE_VINYL_TAPE, query_len);
    CHECK(cbe_host(&esim, 1, 9, query_request, query_len + 7, out, sizeof out, 12100));
    cbe_tick(&esim, 12101);
    const uint8_t logical_channel[] = { 0x10, 1, 0, 1 };
    esim_answer(12102, logical_channel, sizeof logical_channel, 0);
    const uint8_t profiles[] = { 0xbf, 0x2d, 2, 0xa0, 0 };
    esim_apdu_response(12103, profiles, sizeof profiles, 0x90, 0);
    esim_drain(); cbe_tick(&esim, esim.session_deadline);
    esim_answer(15104, NULL, 0, 0);
    CHECK(esim.selected_phys == 2 && esim.switches == 1 && esim.step == CBE_S_IDLE && !esim.freeze);

    /* Disabling an active eSIM uses the same management slot, selects the physical
     * route and waits for confirmation. */
    uint8_t request[64] = { 0 }, header[] = { CBE_TAPE_DISABLE, 1, 0, 0, 0, 2 };
    const uint8_t payload[] = { 1, 1, 0, 0, 0, 0xbf, 0x32, 0 };
    uint32_t n = cbd_tlv(request + 7, sizeof request - 7, 1, header, sizeof header);
    n += cbd_tlv(request + 7 + n, sizeof request - 7 - n, 0x10, payload, sizeof payload);
    cbd_request(request, 40, CBE_VINYL_TAPE, n);
    CHECK(cbe_host(&esim, 1, 9, request, n + 7, out, sizeof out, 16000));
    cbe_tick(&esim, 16001); CHECK(esim.step == CBE_S_OPEN && esim.freeze);
    esim_answer(16002, logical_channel, sizeof logical_channel, 0);
    esim_apdu_response(16003, disabled, sizeof disabled, 0x90, 0);
    CHECK(esim.route_commit && esim.restore_phys == 1);
    esim_answer(16004, NULL, 0, 0); CHECK(esim.step == CBE_S_RESTORE);
    esim_answer(16005, NULL, 0, 0);
    cbe_tick(&esim, 22005); esim_slots(22006, 0);
    CHECK(esim.selected_phys == 1 && esim.route_generation == 2 && esim.step == CBE_S_CARD_STATUS);
    /* A failed fresh status query does not unfreeze data onto an unreported SIM. */
    esim_answer(22007, NULL, 0, CBE_ERR_INTERNAL);
    CHECK(esim.route_failed && esim.needs_card && !esim.slots.known && esim.done_status);
    const uint8_t start_network[] = { 0, 1, 0, 0x20, 0, 0, 0 };
    CHECK(cbe_blocks_data_start(&esim, CBD_SVC_WDS, start_network, sizeof start_network));
    esim_drain(); cbe_tick(&esim, 27007); esim_slots(27008, 0);
    CHECK(esim.step == CBE_S_CARD_STATUS && esim.freeze && !esim.route_failed);
    CHECK(esim.route_generation == 2);
    esim_answer(27009, tlvs, tlv_len, 0);
    CHECK(esim.card_status_len && esim.freeze && !esim.done_pending);

    /* Reverse the physical order: eUICC first, initially active physical SIM second.
     * Selection is derived from modem metadata, never a physical-slot constant. */
    cbe_init(&esim, esim_in, sizeof esim_in, esim_resp, sizeof esim_resp);
    cbe_tick(&esim, 0); esim_slots_layout(1, 1, 0);
    CHECK(esim.selected_phys == 2 && esim.physical_phys == 2 && esim.slots.euicc == 1);
    esim_tape_command(CBE_TAPE_ENABLE, 9, enable, sizeof enable, 1, 0, 2);
    cbe_tick(&esim, 3);
    value = cbd_find(esim.req, esim.req_len, 2, &len);
    CHECK(value && len == 4 && cbd_rd32(value) == 1);
    esim_answer(4, NULL, 0, 0); cbe_tick(&esim, 6004); esim_slots_layout(6005, 0, 0);
    esim_answer(6006, logical_channel, sizeof logical_channel, 0);
    esim_apdu_response(6007, enabled, sizeof enabled, 0x90, 0);
    esim_answer(6008, NULL, 0, 0); cbe_tick(&esim, 12008); esim_slots_layout(12009, 0, 0);
    CHECK(esim.selected_phys == 1 && esim.route_generation == 1 && esim.step == CBE_S_CARD_STATUS);

    /* A bridge restart with the eUICC already mapped also retains endpoint two. */
    cbe_init(&esim, esim_in, sizeof esim_in, esim_resp, sizeof esim_resp);
    cbe_tick(&esim, 0); esim_slots_layout(1, 0, 0);
    cbe_caps(&esim, caps);
    CHECK(esim.selected_phys == 1 && caps[0] == 0 && caps[1] == CBE_CAP_TAPE);

    /* Legacy unknown commands expose only the command/transaction/slot metadata. */
    esim_reset();
    header[0] = 0x14; header[5] = 2;
    n = cbd_tlv(request + 7, sizeof request - 7, 2, header, sizeof header);
    cbd_request(request, 41, CBE_VINYL_COMMAND, n);
    CHECK(!cbe_host(&esim, 1, 9, request, n + 7, out, sizeof out, 2));
    int observed = 0;
    struct cbe_event event;
    while (cbe_take_event(&esim, &event))
        if (event.kind == CBE_EV_COMMAND) {
            CHECK(event.a == 0x14 && event.c == 2 && event.d == CBQ_ERR_NOT_SUPPORTED);
            observed++;
        }
    CHECK(observed == 1);
}

/* SGP.22 refreshFlag: set on enable and disable so the eUICC re-initialises the card (cbe_refresh_flag), and the Vinyl capability vector
 * kept while the slots are re-read (cbe_caps). */
static void test_esim_refresh(void)
{
    uint8_t enable[] = { 0xbf, 0x31, 0x0f, 0xa0, 0x0a, 0x5a, 0x08, 1, 2, 3, 4, 5, 6, 7, 8, 0x81, 0x01, 0x00 };
    uint8_t disable[] = { 0xbf, 0x32, 0x0f, 0xa0, 0x0a, 0x5a, 0x08, 1, 2, 3, 4, 5, 6, 7, 8, 0x81, 0x01, 0xff };
    uint8_t none[] = { 0xbf, 0x31, 0x0c, 0xa0, 0x0a, 0x5a, 0x08, 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t other[] = { 0xbf, 0x2d, 0x03, 0x81, 0x01, 0x00 };
    CHECK(cbe_refresh_flag(enable, sizeof enable) == 0 && enable[sizeof enable - 1] == 0xff);
    CHECK(cbe_refresh_flag(disable, sizeof disable) == 1 && disable[sizeof disable - 1] == 0xff);
    CHECK(cbe_refresh_flag(none, sizeof none) == 2);
    CHECK(cbe_refresh_flag(other, sizeof other) == 2 && other[5] == 0x00);
    CHECK(cbe_refresh_flag(enable, 3) == 2);
    uint8_t caps[2];
    esim_reset();
    cbe_caps(&esim, caps); CHECK(caps[0] == CBE_CAP_NONE && caps[1] == CBE_CAP_TAPE);
    esim.slots.known = 0;
    cbe_caps(&esim, caps); CHECK(caps[0] == CBE_CAP_NONE && caps[1] == CBE_CAP_TAPE);
}

/* One card status from the modem, as an indication or a Get Card Status response: one card with a Ready USIM (0x10), its hot-swap
 * byte (0x11), its SIM busy byte (0x15) and its ICCID, ATR and EID entry (0x17), in this modem's numbering. */
static uint32_t esim_card_message(uint8_t *s, uint32_t room, int response, uint8_t iccid_seed, uint8_t hot_swap, uint32_t eid)
{
    uint8_t card[9 + 6 + 14 + 2] = { 0 }, swap[2] = { 1, hot_swap }, busy[2] = { 1, 0 }, info[1 + 11 + 3 + 17] = { 1, 10 };
    const uint8_t result[4] = { 0 };
    cbd_wr16(card, 0x0000); cbd_wr16(card + 2, 0xffff); cbd_wr16(card + 4, 0xffff); cbd_wr16(card + 6, 0xffff);
    card[8] = 1; card[9] = 1; card[14] = 1;            /* present, one application */
    card[15] = 2; card[16] = 7; card[21] = 2;          /* USIM, Ready, a 2-byte AID */
    uint32_t w = 2;
    for (unsigned i = 0; i < 10; i++) info[w++] = (uint8_t)(iccid_seed + i);
    info[w++] = 2; info[w++] = 0x3b; info[w++] = 0x9f;
    info[w++] = (uint8_t)eid;
    for (unsigned i = 0; i < eid; i++) info[w++] = (uint8_t)(0xa0 + i);
    s[0] = response ? CBQ_QMI_RESPONSE : CBQ_QMI_INDICATION; cbd_wr16(s + 1, response ? 7 : 0);
    cbd_wr16(s + 3, response ? CBE_UIM_GET_CARD_STATUS : CBE_UIM_CARD_STATUS_CHANGE);
    uint32_t n = 7;
    if (response) n += cbd_tlv(s + n, room - n, 0x02, result, sizeof result);
    n += cbd_tlv(s + n, room - n, 0x17, info, w);
    n += cbd_tlv(s + n, room - n, 0x16, (const uint8_t[]){ 1, 0, 0, 0, 0 }, 5);
    n += cbd_tlv(s + n, room - n, 0x15, busy, 2);
    n += cbd_tlv(s + n, room - n, 0x11, swap, 2);
    n += cbd_tlv(s + n, room - n, 0x10, card, sizeof card);
    cbd_wr16(s + 5, n - 7);
    return n;
}
/* The card info list in CommCenter's numbering: 0x17 in a response, 0x14 in an indication (cbe_indication_type), where SIM busy
 * (0x15, CommCenter's IsMomentaryPowerDown there) stays out. With the physical card selected only that list gains endpoint two's
 * entry, the eUICC's ICCID from the slot status and its EID; with the eUICC selected endpoint two carries the eUICC's own entry
 * and endpoint one its card's last entry. */
static void test_esim_card_arrays(void)
{
    uint8_t raw[256], out[2048];
    esim_reset();
    CHECK(esim.physical_phys == 1 && esim.selected_phys == 1);
    esim.slots.iccid_len[1] = 10;
    for (unsigned i = 0; i < 10; i++) esim.slots.iccid[1][i] = (uint8_t)(0x70 + i);
    uint32_t n = esim_card_message(raw, sizeof raw, 0, 0x10, 2, 0);
    uint32_t m = cbe_card_status_fix(&esim, CBE_SVC_UIM, raw, n, out, sizeof out);
    uint32_t l = 0;
    CHECK(m && !cbd_find(out, m, 0x17, &l) && !cbd_find(out, m, 0x15, &l));
    const uint8_t *v = cbd_find(out, m, 0x14, &l);
    CHECK(v && l == 1u + 15u + 29u && v[0] == 2 && v[1] == 10 && v[2] == 0x10);
    CHECK(v && v[16] == 10 && v[17] == 0x70 && v[26] == 0x79 && v[27] == 0 && v[28] == 16 && v[29] == 0xa0);
    CHECK(esim.array_len[0][4] == 15);
    v = cbd_find(out, m, 0x11, &l);
    CHECK(v && l == 2 && v[0] == 1);             /* the other arrays stay as the modem sent them */
    v = cbd_find(out, m, 0x10, &l);
    CHECK(v && v[8] == 1);
    n = esim_card_message(raw, sizeof raw, 1, 0x10, 2, 0);
    m = cbe_card_status_fix(&esim, CBE_SVC_UIM, raw, n, out, sizeof out);
    v = cbd_find(out, m, 0x17, &l);
    CHECK(m && v && v[0] == 2 && l == 1u + 15u + 29u && cbd_find(out, m, 0x15, &l) && !cbd_find(out, m, 0x14, &l));
    /* The route moved to the eUICC (physical slot two, the modem's logical slot one). */
    esim.slots.is_active[0] = 0; esim.slots.is_active[1] = 1; esim.slots.logical_of[1] = 1;
    esim.slots.active = 2; esim.slots.logical = 1; esim.selected_phys = 2;
    n = esim_card_message(raw, sizeof raw, 0, 0x40, 0, 16);
    m = cbe_card_status_fix(&esim, CBE_SVC_UIM, raw, n, out, sizeof out);
    v = cbd_find(out, m, 0x14, &l);
    CHECK(m && v && v[0] == 2);
    const uint32_t len = 15;                    /* {10, ICCID, 2, ATR, 0}: the physical card's cached entry */
    CHECK(l == 1u + len + len + 16);
    CHECK(v[1] == 10 && v[2] == 0x10 && v[1 + len] == 10 && v[2 + len] == 0x40 && v[1 + len + 14] == 16);
    v = cbd_find(out, m, 0x11, &l);
    CHECK(v && l == 3 && v[0] == 2 && v[1] == 2 && v[2] == 0);
    v = cbd_find(out, m, 0x10, &l);
    CHECK(v && v[8] == 2 && cbd_rd16(v) == 0x0100);
    if (!failures) printf("PASS esim card arrays\n");
}

/* CommCenter's subscription selection (cbe_route_request): taken and answered after the route to the named card is settled, at once
 * when the modem already serves it, refused when malformed or while another is open, and failed back when the switch did not land. */
static void esim_route_answer(uint16_t txn, uint16_t message, uint32_t error)
{
    uint8_t out[64], channel = 9;
    CHECK(esim.route_req == 2);
    const uint32_t n = cbe_route_output(&esim, &channel, out, sizeof out);
    CHECK(n && channel == 1 && out[4] == CBE_SVC_UIM && out[5] == 7);
    CHECK(cbd_rd16(out + CBQ_HDR + 1) == txn && cbd_rd16(out + CBQ_HDR + 3) == message);
    CHECK(cbd_error(out + CBQ_HDR, n - CBQ_HDR) == error);
    cbe_route_sent(&esim);
    CHECK(!esim.route_req);
}
static uint32_t esim_route_request(uint16_t txn, uint16_t message, uint8_t tlv, uint8_t value, uint8_t *out, uint32_t room)
{
    uint8_t s[32];
    const uint32_t w = cbd_tlv(s + 7, sizeof s - 7, tlv, &value, 1);
    cbd_request(s, txn, message, w);
    return cbe_route_request(&esim, 1, 7, s, 7 + w, out, room);
}
static void test_esim_route(void)
{
    uint8_t out[64];
    esim_reset();
    /* Not a selection: left alone. Malformed: answered at once. */
    CHECK(esim_route_request(1, CBE_UIM_GET_CARD_STATUS, 0x01, 0, out, sizeof out) == 0);
    uint32_t n = esim_route_request(2, CBE_UIM_SELECT_SINGLE_SUB, 0x01, 2, out, sizeof out);
    CHECK(n && n != CBE_DEFERRED && cbd_error(out + CBQ_HDR, n - CBQ_HDR) == CBE_ERR_INVALID_ARG && !esim.route_req);
    /* Endpoint one while the tray card is served: answered without a switch. */
    CHECK(esim_route_request(3, CBE_UIM_MAP_PROVISIONING, 0x10, 1, out, sizeof out) == CBE_DEFERRED);
    n = esim_route_request(4, CBE_UIM_SELECT_SINGLE_SUB, 0x01, 1, out, sizeof out);
    CHECK(n && n != CBE_DEFERRED && cbd_error(out + CBQ_HDR, n - CBQ_HDR) == CBE_ERR_DEVICE_IN_USE);
    cbe_tick(&esim, 10);
    CHECK(esim.step == CBE_S_IDLE && !esim.switches);
    esim_route_answer(3, CBE_UIM_MAP_PROVISIONING, 0);
    /* SelectUIMForSingleSubCfg 1 (slot two): switch to the eUICC, settle, re-read, publish its card, then answer. */
    CHECK(esim_route_request(5, CBE_UIM_SELECT_SINGLE_SUB, 0x01, 1, out, sizeof out) == CBE_DEFERRED);
    cbe_tick(&esim, 20);
    CHECK(esim.step == CBE_S_RESTORE && esim.wait_msg == CBE_UIM_SWITCH_SLOT && esim.freeze && esim.switches == 1);
    CHECK(esim.route_req == 3 && esim.route_req_target == 2);
    esim_answer(21, NULL, 0, 0);
    CHECK(esim.step == CBE_S_RESTORE_SETTLE);
    cbe_tick(&esim, 6021); CHECK(esim.step == CBE_S_RESLOT);
    esim_slots(6022, 1);
    CHECK(esim.selected_phys == 2 && esim.route_generation == 1);
    esim_card_publish(6023, 1);
    CHECK(esim.route_req == 3);
    cbe_tick(&esim, 6024);
    esim_route_answer(5, CBE_UIM_SELECT_SINGLE_SUB, 0);
    struct cbl_view view;
    CHECK(cbe_slot_view(&esim, &view) && view.active == 2 && view.logical == 1);
    /* Back to slot one, but the modem stays on the eUICC: the selection fails and the route stays where the modem is. */
    CHECK(esim_route_request(6, CBE_UIM_MAP_PROVISIONING, 0x10, 1, out, sizeof out) == CBE_DEFERRED);
    cbe_tick(&esim, 7000);
    CHECK(esim.step == CBE_S_RESTORE && esim.route_req_target == 1);
    esim_answer(7001, NULL, 0, 0);
    cbe_tick(&esim, 13001);
    esim_slots(13002, 1);
    CHECK(esim.route_failed && esim.step == CBE_S_IDLE);
    cbe_tick(&esim, 13003);
    esim_route_answer(6, CBE_UIM_MAP_PROVISIONING, CBE_ERR_INTERNAL);
    CHECK(esim.selected_phys == 2 && esim.restore_phys == 2 && !esim.route_commit);
    cbe_tick(&esim, 18003);
    CHECK(esim.step == CBE_S_SLOTS);
    esim_slots(18004, 1);
    CHECK(!esim.route_failed);
    if (!failures) printf("PASS esim route\n");
}

/* A SIM card pulled while an eSIM query had the eUICC mapped: the modem finds the empty slot only when the restore powers it up, and
 * the indication it raises then falls inside the dropped window, so the restore publishes the real status (cbe_restore). */
static void test_esim_pulled(void)
{
    const uint8_t enable[] = { 0xbf, 0x31, 0 };
    const uint8_t denied[] = { 0xbf, 0x31, 3, 0x80, 1, 2 };
    uint32_t len;
    esim_reset();
    esim_tape_command(CBE_TAPE_ENABLE, 9, enable, sizeof enable, 1, 0, 2); esim_open();
    esim_apdu_response(6007, denied, sizeof denied, 0x90, 0);
    if (esim.session == CBE_SESSION_BATCH && esim.step == CBE_S_IDLE) cbe_tick(&esim, esim.session_deadline);
    CHECK(esim.step == CBE_S_CLOSE);
    esim_answer(20000, NULL, 0, 0); CHECK(esim.step == CBE_S_RESTORE && esim.needs_card);
    esim_answer(20001, NULL, 0, 0); cbe_tick(&esim, 26001);
    esim_slots_card(26002, 0, 1, 0);
    CHECK(esim.selected_phys == 1 && esim.route_generation == 0 && !esim.route_failed);
    const uint8_t *published = esim_card_publish(26003, 0);
    const uint8_t *value = cbd_find(published, 7u + cbd_rd16(published + 5), 0x10, &len);
    CHECK(value && len == 15 && value[8] == 1 && value[9] == 0);
    CHECK(esim.step == CBE_S_IDLE && !esim.freeze);
}

/* CommCenter's endpoints against the modem's logical slot (coachz_bb_slots.h). */
static void test_endpoint_slots(void)
{
    const int before = failures;
    /* The modem's status with the eUICC's enabled profile in logical slot one: GW primary application 0 on card 0, Ready. */
    static const uint8_t aid[] = { 0xa0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x02 };
    uint8_t card[9 + 6 + 14 + sizeof aid], out[96], request[64], mapped[64];
    uint32_t n = 0;
    card[n++] = 0x00; card[n++] = 0x00;                                    /* GW primary: application 0, card 0 */
    for (unsigned i = 0; i < 6; i++) card[n++] = 0xff;                     /* 1x primary, GW and 1x secondary: none */
    card[n++] = 1;                                                         /* one card */
    card[n++] = 1; card[n++] = 0; card[n++] = 3; card[n++] = 10; card[n++] = 0; card[n++] = 1;   /* present, one application */
    card[n++] = 2; card[n++] = 7; card[n++] = 0; card[n++] = 0; card[n++] = 0; card[n++] = 0;    /* USIM, ready */
    card[n++] = sizeof aid; memcpy(card + n, aid, sizeof aid); n += sizeof aid;
    for (unsigned i = 0; i < 7; i++) card[n++] = i == 1 ? 1 : 0;
    CHECK(n == sizeof card);
    const struct cbl_view esim_on = { 2, 1, { 1, 1 } }, physical_on = { 1, 1, { 1, 1 } }, bad = { 3, 1, { 0, 0 } };
    CHECK(!cbl_swapped(&physical_on) && cbl_swapped(&esim_on) && !cbl_swapped(&bad));
    uint32_t m = cbl_card_status(&esim_on, card, sizeof card, out, sizeof out);
    CHECK(m == sizeof card + 6u && out[8] == 2);
    CHECK(cbd_rd16(out) == 0x0100u && cbd_rd16(out + 2) == 0xffffu && cbd_rd16(out + 6) == 0xffffu);   /* application 0 on card 1 */
    CHECK(out[9] == 1 && !out[14]);                                         /* endpoint one: present, no applications */
    CHECK(!memcmp(out + 15, card + 9, sizeof card - 9));                   /* endpoint two: the modem's record, unchanged */
    const struct cbl_view absent = { 2, 1, { 0, 1 } };
    m = cbl_card_status(&absent, card, sizeof card, out, sizeof out);
    CHECK(m == sizeof card + 6u && out[9] == 0);
    CHECK(!cbl_card_status(&esim_on, card, sizeof card - 1, out, sizeof out));      /* truncated application */
    CHECK(!cbl_card_status(&esim_on, card, sizeof card, out, sizeof card));         /* no room for the presence record */
    card[1] = 1;                                                                  /* an index naming card 1, which is not there */
    CHECK(!cbl_card_status(&esim_on, card, sizeof card, out, sizeof out));
    card[1] = 0;

    /* Send APDU for endpoint two goes to logical slot one; for endpoint one it is not ready; others pass. */
    static const uint8_t apdu[] = { 0x80, 0xe2, 0x91, 0x00, 0x00 };
    uint8_t slot = 2;
    uint32_t len = 7;
    request[0] = 0; cbd_wr16(request + 1, 0x21); cbd_wr16(request + 3, 0x3b);
    len += cbd_tlv(request + len, sizeof request - len, 0x01, &slot, 1);
    len += cbd_tlv(request + len, sizeof request - len, 0x02, apdu, sizeof apdu);
    cbd_wr16(request + 5, len - 7);
    CHECK(cbl_uim_request(&esim_on, request, len, mapped, sizeof mapped) == CBL_CHANGED);
    uint32_t got = 0;
    const uint8_t *selector = cbd_find(mapped, len, 0x01, &got);
    CHECK(selector && got == 1 && selector[0] == 1 && !memcmp(mapped + 7 + 4, request + 7 + 4, len - 11));
    request[7 + 3] = 1;
    CHECK(cbl_uim_request(&esim_on, request, len, mapped, sizeof mapped) == CBL_INACTIVE);
    CHECK(cbl_uim_request(&physical_on, request, len, mapped, sizeof mapped) == CBL_UNCHANGED);
    CHECK(cbl_uim_request(&esim_on, request, len, mapped, len - 1) == CBL_MALFORMED);

    /* Read Transparent on the card session of slot two (type 7) uses slot one's (6); the primary session (0) is not renumbered. */
    uint8_t session[2] = { 7, 0 };
    len = 7; cbd_wr16(request + 3, 0x20);
    len += cbd_tlv(request + len, sizeof request - len, 0x01, session, 2);
    cbd_wr16(request + 5, len - 7);
    CHECK(cbl_uim_request(&esim_on, request, len, mapped, sizeof mapped) == CBL_CHANGED && mapped[10] == 6);
    request[10] = 0;
    CHECK(cbl_uim_request(&esim_on, request, len, mapped, sizeof mapped) == CBL_UNCHANGED);
    request[10] = 6;
    CHECK(cbl_uim_request(&esim_on, request, len, mapped, sizeof mapped) == CBL_INACTIVE);
    request[10] = 10;
    CHECK(cbl_uim_request(&esim_on, request, len, mapped, sizeof mapped) == CBL_INACTIVE);

    /* Change Provisioning Session: the primary GW session stays, its application's slot two becomes slot one. */
    const uint8_t change[2] = { 0, 1 }, app[3] = { 2, 1, 0xa0 };
    len = 7; cbd_wr16(request + 3, 0x38);
    len += cbd_tlv(request + len, sizeof request - len, 0x01, change, 2);
    len += cbd_tlv(request + len, sizeof request - len, 0x10, app, 3);
    cbd_wr16(request + 5, len - 7);
    CHECK(cbl_uim_request(&esim_on, request, len, mapped, sizeof mapped) == CBL_CHANGED);
    const uint8_t *app_out = cbd_find(mapped, len, 0x10, &got);
    selector = cbd_find(mapped, len, 0x01, &got);
    CHECK(app_out && app_out[0] == 1 && selector && selector[0] == 0);
    request[len - 3] = 1;   /* the application on slot one, while the eUICC is the active card */
    CHECK(cbl_uim_request(&esim_on, request, len, mapped, sizeof mapped) == CBL_INACTIVE);

    /* Messages without a slot or session selector are left alone. */
    len = 7; cbd_wr16(request + 3, 0x2f); cbd_wr16(request + 5, 0);
    CHECK(cbl_uim_request(&esim_on, request, len, mapped, sizeof mapped) == CBL_UNCHANGED);
    if (failures == before) printf("PASS endpoint slot mapping\n");
}

int main(void)
{
    test_qmux();
    test_sahara();
    test_sahara_command();
    test_registers();
    test_control();
    test_channels();
    test_data();
    test_esim();
    test_esim_cache();
    test_esim_selection();
    test_esim_pulled();
    test_esim_card_arrays();
    test_esim_route();
    test_esim_refresh();
    test_endpoint_slots();
    return failures ? 1 : 0;
}
