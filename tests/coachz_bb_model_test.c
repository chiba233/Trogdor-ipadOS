/* Host model of the pending baseband bridge: QMUX and CTL against QRTR, the SAHARA device end, the AppleBaseband methods, the channel
   table, the data port, activation answers and eSIM Tape transport. Built with -fsanitize=address,undefined by tests/test_baseband_bridge.py. */
#include <stdio.h>
#include <string.h>
#include "coachz_bb_qmux.h"
#include "coachz_bb_sahara.h"
#include "coachz_bb_control.h"
#include "coachz_bb_channels.h"
#include "coachz_bb_data.h"
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
    uint8_t tlvs[CBD_ACTIVATION_TLVS];
    const uint32_t t = cbd_activation_state(tlvs, sizeof tlvs);
    n = cbq_private_indication(CBD_BSP_ACTIVATION_REGISTER, CBD_SVC_BSP, 3, tlvs, t, out, sizeof out);
    /* kActivated, service provisioned (0x4f = 2), slot one's card provisioned (0x50 = 2), both slots accepted (0x56, 0x57 = 1) */
    const uint8_t act_ind[] = { 1, 32, 0, 0x80, 0xe2, 3, 4, 0, 0, 0x0c, 0xb0, 20, 0, 1, 1, 0, 0, 0x4f, 1, 0, 2, 0x50, 1, 0, 2,
                                0x56, 1, 0, 1, 0x57, 1, 0, 1 };
    CHECK(t == CBD_ACTIVATION_TLVS && n == sizeof act_ind && !memcmp(out, act_ind, sizeof act_ind));
    CHECK(!cbd_activation_state(tlvs, CBD_ACTIVATION_TLVS - 1));   /* no room: nothing to send */
    n = cbq_private_unsupported(act, 0xe5, 2, out, sizeof out);
    CHECK(n == 20 && out[4] == 0xe5 && cbq_rd16(out + 16) == 1 && cbq_rd16(out + 18) == CBQ_ERR_NOT_SUPPORTED);
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
static void esim_slots(uint32_t now, uint32_t active)
{
    uint8_t t[96], st[21] = { 2 }, info[15] = { 2 }, eid[19] = { 2, 0, 16 };
    for (unsigned i = 0; i < 2; i++) {
        cbd_wr32(st + 1 + 10 * i, 2); cbd_wr32(st + 5 + 10 * i, i == active);
        st[9 + 10 * i] = i == active ? 1 : 0;
    }
    /* Two present cards, slot two an eUICC. Its EID is a synthetic byte sequence. */
    info[14] = 1;
    for (unsigned i = 3; i < sizeof eid; i++) eid[i] = (uint8_t)(0xa0 + i);
    uint32_t n = cbd_tlv(t, sizeof t, 0x10, st, sizeof st);
    n += cbd_tlv(t + n, sizeof t - n, 0x11, info, sizeof info);
    n += cbd_tlv(t + n, sizeof t - n, 0x12, eid, sizeof eid);
    esim_answer(now, t, n, 0);
}
static void esim_reset(void)
{
    cbe_init(&esim, esim_in, sizeof esim_in, esim_resp, sizeof esim_resp);
    cbe_tick(&esim, 0);
    CHECK(esim.step == CBE_S_SLOTS);
    esim_slots(1, 0);
    CHECK(esim.slots.euicc == 2 && esim.slots.active == 1 && esim.slots.eid_len == 16);
}
static void esim_tape(const uint8_t *data, uint32_t n, uint32_t total, uint32_t index)
{
    uint8_t s[1600] = { 0 }, out[128], header[6] = { 1, 0x12, 0x34, 0x56, 0x78, 2 }, payload[1505] = { 0 };
    cbd_wr16(payload + 1, total); cbd_wr16(payload + 3, index); memcpy(payload + 5, data, n);
    uint32_t w = cbd_tlv(s + 7, sizeof s - 7, 1, header, sizeof header);
    w += cbd_tlv(s + 7 + w, sizeof s - 7 - w, 0x10, payload, n + 5);
    cbd_request(s, 9 + index, 0x1002, w);
    const uint32_t answer = cbe_host(&esim, 1, 9, s, 7 + w, out, sizeof out, 2);
    CHECK(answer && answer != CBE_DEFERRED && !cbd_error(out + 6, answer - 6));
    CHECK(cbd_rd16(out + 7) == 9 + index && out[4] == 0xe5 && out[5] == 9);
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
static void esim_restore(uint32_t now)
{
    CHECK(esim.step == CBE_S_CLOSE);
    esim_answer(now, NULL, 0, 0);
    CHECK(esim.step == CBE_S_RESTORE);
    esim_answer(now + 1, NULL, 0, 0);
    cbe_tick(&esim, now + 6001);
    esim_slots(now + 6002, 0);
    CHECK(esim.step == CBE_S_IDLE && !esim.freeze && !esim.route_failed && esim.slots.active == 1);
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
        CHECK(info && len == 9 && !info[1] && cbd_rd32(info + 3) == 0x78563412 && !info[7] && !info[8]);
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
    esim_answer(6008, NULL, 0, 0); CHECK(esim.step == CBE_S_RESTORE);
    esim_answer(6009, NULL, 0, CBE_ERR_INTERNAL);
    CHECK(esim.done_status == 1 && esim.route_failed && !esim.slots.known);
    if (!failures) printf("PASS esim tape transport and cleanup\n");
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
    return failures ? 1 : 0;
}
