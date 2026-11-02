/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Connects modem/ (mainline's ipa_config, ipa_setup and the IPA QMI handshake) to the modem start in bsd/dev/coachz_wifi.c, in
 * mainline's order: coachz_modem_configure before the modem boots, coachz_modem_starting as its start begins, coachz_modem_setup once
 * it raises ipa-setup-ready, then the handshake over QRTR. It stops where mainline calls ipa_modem_start.
 *
 * Polled from the Wi-Fi thread's millisecond loop instead of SPI 311 and 432. The WLAN firmware shares this modem, so every failure is
 * printed and turns the rest of this sequence off; nothing here stops the Wi-Fi sequence.
 *
 * DMA memory is low DRAM outside the kernel's window (apps SMMU in bypass, so physical addresses): no reserved-memory node covers it and
 * the other pools here (USB, SDHCI, audio, microphone, copy engines) lie elsewhere.
 */
#include "coachz_ipa_cmd.c"
#include "coachz_ipa_rx.c"
#include "coachz_ipa_qmi.h"

extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
int coachz_modem_configure(int (*)(const char *), int (*)(int, uint32_t, uint32_t, const void *, uint32_t));
void coachz_modem_starting(void);
int coachz_modem_setup(void);
void coachz_modem_new_server(uint32_t, uint32_t, uint32_t, uint32_t);
void coachz_modem_qmi(int, uint32_t, uint32_t, const void *, uint32_t);
void coachz_modem_poll(void);
void coachz_modem_report(unsigned);

#define CM_RX_PA     0x81400000ull
#define CM_RX_BYTES  0x200000u
#define CM_RING_PA   0x81700000ull
#define CM_RING_BYTES 0x10000u
#define CM_CMD_OFFSET 0xc000u
#define CM_CMD_BYTES 0x4000u
#define CM_ZERO_PA   0x81710000ull
#define CM_ZERO_BYTES 0x2000u

enum { CM_OFF, CM_CONFIGURED, CM_SET_UP };

static struct coachz_ipa cm_ipa;
static struct coachz_gsi cm_gsi;
static struct coachz_ipa_cmd cm_cmd;
static struct coachz_ipa_rx cm_lan;
static struct ci_handshake cm_hs;
static int cm_state;
static int (*cm_send)(int client, uint32_t node, uint32_t port, const void *message, uint32_t length);
static uint32_t cm_modem_node, cm_modem_port, cm_peer_node, cm_peer_port;
static int cm_modem_known, cm_peer_known, cm_init_driver_sent;
static uint16_t cm_txn;

/* wifi_ipa_power() is done and the modem has not started: ipa_config, and the rings and buffers Linux also allocates at probe. */
int
coachz_modem_configure(int (*qmp_send)(const char *), int (*send)(int, uint32_t, uint32_t, const void *, uint32_t))
{
    void *zero = (void *)ml_io_map_wcomb((vm_offset_t)CM_ZERO_PA, CM_ZERO_BYTES);
    uint8_t *ring = (uint8_t *)ml_io_map_wcomb((vm_offset_t)CM_RING_PA, CM_RING_BYTES);
    void *rx = (void *)ml_io_map_wcomb((vm_offset_t)CM_RX_PA, CM_RX_BYTES);
    if (!zero || !ring || !rx) {
        printf("COACHZ ipa: no DMA mappings\n");
        return -1;
    }
    cm_send = send;
    int err = coachz_ipa_config(&cm_ipa, zero, CM_ZERO_PA, qmp_send);
    if (!err) err = coachz_gsi_init(&cm_gsi, cm_ipa.gsi, ring, CM_RING_PA, CM_RING_BYTES);
    if (!err) err = coachz_ipa_cmd_init(&cm_cmd, &cm_ipa, &cm_gsi, ring + CM_CMD_OFFSET, CM_RING_PA + CM_CMD_OFFSET, CM_CMD_BYTES);
    if (!err) err = coachz_ipa_rx_init(&cm_lan, &cm_ipa, &cm_gsi, CIPA_EP_AP_LAN_RX, rx, CM_RX_PA, CM_RX_BYTES);
    if (err) {
        printf("COACHZ ipa: configuration failed (%d), cellular stays off\n", err);
        return err;
    }
    cm_state = CM_CONFIGURED;
    return 0;
}

void
coachz_modem_starting(void)
{
    if (cm_state != CM_OFF) coachz_ipa_modem_starting(&cm_ipa);
}

static void
cm_step(void)
{
    for (;;) {
        const struct ci_step step = ci_ready_step(&cm_hs);
        if (step.send_init_complete) {
            uint8_t message[32];
            const size_t length = ci_init_complete_ind(message, sizeof message, ++cm_txn);
            const int sent = cm_peer_known && length && !cm_send(0, cm_peer_node, cm_peer_port, message, (uint32_t)length);
            printf("COACHZ ipa: INIT_COMPLETE %s\n", sent ? "sent" : "not sent");
            if (!sent) return;
            ci_indication_sent(&cm_hs);
            continue;
        }
        if (step.start_modem) printf("COACHZ ipa: handshake complete; ipa_modem_start is next\n");
        return;
    }
}

static void
cm_init_driver(void)
{
    uint8_t message[160];
    if (cm_state != CM_SET_UP || !cm_modem_known || cm_init_driver_sent) return;
    const size_t length = ci_init_driver_req(message, sizeof message, ++cm_txn, cm_ipa.mem_offset, cm_ipa.uc_loaded);
    const int result = length ? cm_send(1, cm_modem_node, cm_modem_port, message, (uint32_t)length) : -1;
    cm_init_driver_sent = !result;
    printf("COACHZ ipa: INIT_DRIVER %u bytes to %u:0x%x, mem_offset 0x%x, uc_loaded %d, send %d\n", (unsigned)length, cm_modem_node,
        cm_modem_port, cm_ipa.mem_offset, cm_ipa.uc_loaded, result);
}

/* ipa-setup-ready: the rest of ipa_setup, ending where mainline calls ipa_qmi_setup. Returns 1 when the handshake may start. */
int
coachz_modem_setup(void)
{
    if (cm_state != CM_CONFIGURED) {
        printf("COACHZ ipa: setup ready, but the IPA is not configured\n");
        return 0;
    }
    int err = coachz_gsi_setup(&cm_gsi);
    if (!err) err = coachz_ipa_setup_commands(&cm_cmd);
    if (!err) err = coachz_ipa_setup_lan_rx(&cm_lan);
    if (err) {
        printf("COACHZ ipa: setup failed (%d), cellular stays off\n", err);
        cm_state = CM_OFF;
        return 0;
    }
    cm_state = CM_SET_UP;
    ci_setup(&cm_hs);
    cm_init_driver();
    return 1;
}

/* ipa_client_new_server: QRTR's NEW_SERVER, whichever order it comes in relative to ipa-setup-ready. */
void
coachz_modem_new_server(uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
    if (!ci_is_modem_service(service, instance)) return;
    cm_modem_node = node;
    cm_modem_port = port;
    cm_modem_known = 1;
    cm_init_driver();
}

/* A QMI message on one of the two ports: the client's (INIT_DRIVER's answer) or the server's (the modem's requests). */
void
coachz_modem_qmi(int client, uint32_t node, uint32_t port, const void *body, uint32_t length)
{
    struct ci_msg m;
    if (!ci_parse(body, length, &m)) {
        printf("COACHZ ipa: bad QMI message of %u bytes from %u:0x%x\n", length, node, port);
        return;
    }
    if (client) {
        struct ci_init_driver_rsp r;
        if (!ci_init_driver_rsp_parse(&m, &r)) {
            printf("COACHZ ipa: QMI type %u message 0x%x on the client port\n", m.type, m.id);
            return;
        }
        printf("COACHZ ipa: INIT_DRIVER answer: ok %d error %u, ctrl %d/%u default %d/%u pending %d/%u\n", r.ok, r.error, r.has_ctrl,
            r.ctrl_end_pt, r.has_default, r.default_end_pt, r.has_pending, r.pending);
        if (r.ok) ci_on_init_driver_success(&cm_hs);
    } else {
        uint8_t message[32];
        const int known = m.type == CI_QMI_REQUEST && (m.id == CI_INDICATION_REGISTER || m.id == CI_DRIVER_INIT_COMPLETE);
        printf("COACHZ ipa: QMI type %u message 0x%x from %u:0x%x%s\n", m.type, m.id, node, port, known ? "" : ", not handled");
        if (!known) return;
        cm_peer_node = node;
        cm_peer_port = port;
        cm_peer_known = 1;
        const size_t size = ci_success_rsp(message, sizeof message, m.txn, m.id);
        if (!size || cm_send(0, node, port, message, (uint32_t)size)) {
            printf("COACHZ ipa: response to 0x%x not sent\n", m.id);
            return;
        }
        if (m.id == CI_INDICATION_REGISTER) ci_on_indication_register_answered(&cm_hs);
        else ci_on_driver_init_complete_answered(&cm_hs);
    }
    cm_step();
}

/* SPI 311, SPI 432 and the LAN RX ring's IEOB, polled. */
void
coachz_modem_poll(void)
{
    if (cm_state == CM_OFF) return;
    coachz_ipa_interrupt(&cm_ipa);
    if (cm_state != CM_SET_UP) return;
    coachz_gsi_interrupt(&cm_gsi);
    if (cm_gsi.ieob_pending & (1u << cm_lan.evt_ring)) {
        cm_gsi.ieob_pending &= ~(1u << cm_lan.evt_ring);
        coachz_ipa_rx_poll(&cm_lan);
    }
}

/* What the handshake and LAN RX have seen, with the Wi-Fi thread's periodic report. */
void
coachz_modem_report(unsigned second)
{
    if (cm_state != CM_SET_UP) return;
    printf("COACHZ ipa: %u s: uc_loaded %d modem_ready %d uc_ready %d indication %d/%d, LAN RX packets %llu delivered %llu dropped %llu\n",
        second, cm_ipa.uc_loaded, cm_hs.modem_ready, cm_hs.uc_ready, cm_hs.indication_requested, cm_hs.indication_sent,
        cm_lan.packets, cm_lan.delivered, cm_lan.dropped);
}
