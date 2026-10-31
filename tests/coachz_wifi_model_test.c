/* SPDX-License-Identifier: BSD-2-Clause */
/* Fixed protocol fixtures, not a remote processor or radio emulator. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "coachz_wifi_smem.h"

static uint8_t memory[CW_SMEM_SIZE];
static unsigned protected_reads;

static int read_memory(void *context, uint32_t off, void *data, uint32_t bytes) {
    (void)context;
    if (!cw_span(sizeof(memory), off, bytes)) return -1;
    if (off < 0x7000 && off + bytes > 0x6000) { protected_reads++; return -1; }
    memcpy(data, memory + off, bytes);
    return 0;
}
static void partition(unsigned index, uint32_t off, uint32_t bytes, uint16_t h0, uint16_t h1) {
    uint8_t *e = memory + CW_SMEM_SIZE - 4096 + 32 + 48 * index;
    cw_put32(e, off); cw_put32(e + 4, bytes); cw_put16(e + 12, h0); cw_put16(e + 14, h1); cw_put32(e + 16, 64);
    uint8_t *p = memory + off;
    cw_put32(p, 0x54525024); cw_put16(p + 4, h0); cw_put16(p + 6, h1); cw_put32(p + 8, bytes);
    cw_put32(p + 12, 32); cw_put32(p + 16, bytes);
}
static uint32_t uncached(uint32_t base, uint16_t id, const uint8_t *data, uint32_t bytes) {
    const uint32_t off = cw_u32(memory + base + 12), aligned = (bytes + 7) & ~7u;
    uint8_t *h = memory + base + off;
    cw_put16(h, 0xa5a5); cw_put16(h + 2, id); cw_put32(h + 4, aligned); cw_put16(h + 8, (uint16_t)(aligned - bytes));
    memcpy(h + 16, data, bytes); cw_put32(memory + base + 12, off + 16 + aligned);
    return base + off + 16;
}
static void fixture(void) {
    memset(memory, 0, sizeof(memory)); protected_reads = 0;
    uint8_t *toc = memory + sizeof(memory) - 4096;
    cw_put32(toc, 0x434f5424); cw_put32(toc + 4, 1); cw_put32(toc + 8, 3);
    partition(0, 0x6000, 0x1000, 2, 3); /* protected ADSP/SLPI: must never read */
    partition(1, 0x2000, 0x1000, CW_SMEM_APPS, CW_SMEM_MODEM);
    partition(2, 0x4000, 0x1000, CW_SMEM_GLOBAL, CW_SMEM_GLOBAL);
}

static void smem_tests(void) {
    const struct cw_reader r = {NULL, sizeof(memory), read_memory};
    struct cw_smem layout; struct cw_item item; struct cw_smp2p state;
    fixture();
    uint8_t incoming[40] = {0}, desc[16] = {0};
    cw_put32(incoming, 0x504d5324); incoming[4] = 1; incoming[5] = 1;
    cw_put16(incoming + 8, 1); cw_put16(incoming + 10, 0); cw_put16(incoming + 12, 16); cw_put16(incoming + 14, 1);
    memcpy(incoming + 20, "slave-kernel", 13); cw_put32(incoming + 36, 6);
    uncached(0x4000, CW_SMP2P_IN, incoming, sizeof(incoming));
    uncached(0x2000, CW_GLINK_DESC, desc, sizeof(desc));
    assert(cw_smem_layout(&r, &layout) == CW_OK && layout.has_modem && layout.has_global);
    assert(protected_reads == 0 && layout.count == 3);
    assert(cw_smem_find(&r, &layout.global, CW_SMP2P_IN, &item) == CW_OK);
    assert(item.bytes == sizeof(incoming));
    assert(cw_smp2p_read(&r, &item, &state) == CW_OK && state.has_slave && state.state == 6 && state.features == 1);
    assert(cw_smem_find(&r, &layout.modem, CW_GLINK_DESC, &item) == CW_OK && item.bytes == 16);
    assert(cw_smem_find(&r, &layout.modem, CW_GLINK_RX, &item) == CW_ABSENT);
    puts("PASS SMEM APPS partition selection and SMP2P snapshot");

    /* Cached records grow backwards. The item has 64 bytes allocated, six useful bytes. */
    uint8_t *h = memory + 0x4000 + 0x1000 - 64;
    cw_put16(h, 0xa5a5); cw_put16(h + 2, 42); cw_put32(h + 4, 64); cw_put16(h + 8, 58);
    memcpy(h - 64, "cached", 6); cw_put32(memory + 0x4000 + 16, 0x1000 - 128);
    assert(cw_smem_layout(&r, &layout) == CW_OK);
    assert(cw_smem_find(&r, &layout.global, 42, &item) == CW_OK && item.bytes == 6);
    uint8_t data[6]; assert(cw_read(&r, item.offset, data, 6) == CW_OK && !memcmp(data, "cached", 6));
    puts("PASS SMEM cached item and data padding");

    fixture();
    cw_put16(memory + CW_SMEM_SIZE - 4096 + 32 + 48 + 12, 2); /* modem partition now belongs to another host pair */
    cw_put16(memory + CW_SMEM_SIZE - 4096 + 32 + 48 + 14, 4);
    assert(cw_smem_layout(&r, &layout) == CW_OK && !layout.has_modem);
    assert(!protected_reads);
    fixture(); cw_put32(memory + 0x2000 + 12, 0x1001);
    assert(cw_smem_layout(&r, &layout) == CW_FORMAT);
    fixture(); cw_put32(memory + CW_SMEM_SIZE - 4096, 0);
    assert(cw_smem_layout(&r, &layout) == CW_UNSUPPORTED);
    fixture();
    uint8_t small[4] = {0}; uncached(0x4000, 20, small, sizeof(small));
    assert(cw_smem_layout(&r, &layout) == CW_OK);
    cw_put32(memory + 0x4000 + 32 + 4, UINT32_MAX);
    assert(cw_smem_find(&r, &layout.global, 20, &item) == CW_FORMAT);
    puts("PASS absent remote, unavailable table and incomplete allocation");
}

static void protocol_tests(void) {
    uint8_t p[128]; struct cw_qmi m;
    assert(cw_disjoint(CW_SMEM_BASE, CW_SMEM_SIZE, 0x100000000ull, 0x40000000));
    assert(!cw_disjoint(CW_SMEM_BASE, CW_SMEM_SIZE, 0x80000000, 0x10000000));
    assert(!cw_disjoint(CW_SMEM_BASE, CW_SMEM_SIZE, UINT64_MAX - 4, 8));
    static const uint8_t registration[] = {
        0, 0x34, 0x12, 0x20, 0, 19, 0,
        0x10, 1, 0, 1, 0x13, 1, 0, 1, 0x14, 1, 0, 1, 0x15, 4, 0, 0x4c, 0x45, 0x4e, 0x4b
    };
    size_t n = cw_wlfw_register(p, sizeof(p), 0x1234);
    assert(n == sizeof(registration) && !memcmp(p, registration, n));
    assert(cw_qmi_parse(p, n, &m) == CW_OK && m.transaction == 0x1234 && m.message == CW_WLFW_IND_REGISTER);
    assert(cw_wlfw_register(p, 10, 1) == 0);
    n = cw_wlfw_cap_request(p, sizeof(p), 7);
    static const uint8_t cap[] = {0, 7, 0, 0x24, 0, 0, 0};
    assert(n == sizeof(cap) && !memcmp(p, cap, n));
    n = cw_wlfw_msa_request(p, sizeof(p), 9, CW_MSA_BASE, CW_MSA_SIZE);
    assert(n == 25 && cw_u64(p + 10) == CW_MSA_BASE && cw_u32(p + 21) == 0x200000);
    assert(cw_wlfw_msa_request(p, sizeof(p), 9, 0x8bc00000, 0x100000) == 0);
    puts("PASS WLFW registration, capability and target MSA request bytes");

    uint8_t result[] = {2, 7, 0, 0x24, 0, 7, 0, 2, 4, 0, 0, 0, 0, 0};
    uint16_t remote = 0;
    assert(cw_qmi_parse(result, sizeof(result), &m) == CW_OK);
    assert(cw_qmi_response(&m, 7, CW_WLFW_CAP, &remote) == CW_OK);
    assert(cw_qmi_response(&m, 8, CW_WLFW_CAP, &remote) == CW_FORMAT);
    assert(cw_qmi_response(&m, 7, CW_WLFW_MSA_INFO, &remote) == CW_FORMAT);
    result[10] = 1; result[12] = 0x5e;
    assert(cw_qmi_parse(result, sizeof(result), &m) == CW_OK);
    assert(cw_qmi_response(&m, 7, CW_WLFW_CAP, &remote) == CW_REMOTE && remote == 0x5e);
    struct cw_capability c;
    assert(cw_wlfw_capability(&m, &c) == CW_REMOTE && c.present == 0);
    result[5] = 8;
    assert(cw_qmi_parse(result, sizeof(result), &m) == CW_FORMAT);
    uint8_t missing[] = {2, 7, 0, 0x24, 0, 0, 0};
    assert(cw_qmi_parse(missing, sizeof(missing), &m) == CW_OK);
    assert(cw_qmi_response(&m, 7, CW_WLFW_CAP, &remote) == CW_FORMAT);
    puts("PASS response transaction, message, result and wire length checks");

    struct cw_builder b; uint8_t ok[4] = {0}, chip[8], board[4];
    cw_put32(chip, 0x3990); cw_put32(chip + 4, 1); cw_put32(board, 0x123);
    cw_qmi_begin(&b, p, sizeof(p), 7, CW_WLFW_CAP);
    cw_qmi_add(&b, 2, ok, 4); cw_qmi_add(&b, 0x10, chip, 8); cw_qmi_add(&b, 0x11, board, 4);
    n = cw_qmi_finish(&b); p[0] = CW_QMI_RESPONSE;
    assert(cw_qmi_parse(p, n, &m) == CW_OK && cw_wlfw_capability(&m, &c) == CW_OK);
    assert(c.present == 3 && c.chip == 0x3990 && c.family == 1 && c.board == 0x123 && c.soc == 0);
    cw_qmi_add(&b, 0x11, board, 4); n = cw_qmi_finish(&b);
    assert(cw_qmi_parse(p, n, &m) == CW_OK && cw_wlfw_capability(&m, &c) == CW_FORMAT);
    puts("PASS capability presence and duplicate field rejection");

    uint8_t server[52] = {0}; struct cw_service s; int up = 0;
    cw_put32(server, 1); cw_put32(server + 4, 4); cw_put32(server + 20, 20);
    cw_put32(server + 32, 4); cw_put32(server + 36, CW_WLFW_SERVICE); cw_put32(server + 40, 0x301);
    cw_put32(server + 44, 0); cw_put32(server + 48, 0x1234);
    assert(cw_qrtr_wlfw_service(server, sizeof(server), &s, &up) == CW_OK && up && s.instance == 3 && s.port == 0x1234);
    cw_put32(server + 4, 5); cw_put32(server + 32, 5);
    assert(cw_qrtr_wlfw_service(server, sizeof(server), &s, &up) == CW_OK && !up);
    server[40] = 2;
    assert(cw_qrtr_wlfw_service(server, sizeof(server), &s, &up) == CW_ABSENT);
    puts("PASS QRTR WLFW service add/remove and version matching");
}

int main(void) { smem_tests(); protocol_tests(); return 0; }
