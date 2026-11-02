/* SPDX-License-Identifier: BSD-2-Clause */
/* Kernel-side state shared by the pending IPA files (coachz_ipa_config.c, coachz_gsi.c, coachz_ipa_cmd.c). */
#ifndef COACHZ_IPA_KERNEL_H
#define COACHZ_IPA_KERNEL_H

#include <sys/param.h>
#include <machine/machine_routines.h>
#include "coachz_gsi_proto.h"

struct coachz_ipa {
    vm_offset_t reg, shared, gsi;
    uint32_t mem_offset, mem_size;
    void *zero_virt;           /* IPA_MEM_MAX zeroed bytes, DMA-visible, for ipa_mem_zero */
    uint64_t zero_addr;
    uint32_t irq_enabled;      /* ipa_interrupt.enabled */
    uint32_t suspend_enabled;  /* ipa_interrupt.suspend_enabled: endpoints with TX_SUSPEND on (one 32-endpoint unit on v4.2) */
    uint32_t available_tx, available_rx_base, available_limit;
    int uc_powered, uc_loaded;
    int (*qmp_send)(const char *text);   /* the AOSS QMP sender (the Wi-Fi patch's wifi_qmp_send) */
};
int coachz_ipa_config(struct coachz_ipa *ipa, void *zero_virt, uint64_t zero_addr, int (*qmp_send)(const char *));
void coachz_ipa_modem_starting(struct coachz_ipa *ipa);
void coachz_ipa_interrupt(struct coachz_ipa *ipa);
void coachz_ipa_uc_panic(struct coachz_ipa *ipa);
void coachz_ipa_suspend_enable(struct coachz_ipa *ipa, uint32_t endpoint, int enable);

struct coachz_gsi {
    vm_offset_t virt;                       /* the "gsi" region, 0x01e04000 */
    struct cgs_table t;
    struct cgs_counts n;
    struct cgs_ring_place place;
    uint8_t *ring_virt;
    uint64_t ring_phys;
    uint32_t type_enabled, ieob_enabled;
    uint32_t ch_index[CGS_CHANNEL_COUNT_MAX], evt_index[CGS_EVT_RING_COUNT_MAX];
    volatile int completed;
    int result;
    uint32_t ieob_pending;                  /* event rings whose IEOB fired; the transaction layer polls these */
};
int coachz_gsi_init(struct coachz_gsi *gsi, vm_offset_t gsi_virt, void *ring_virt, uint64_t ring_phys, uint32_t ring_bytes);
int coachz_gsi_setup(struct coachz_gsi *gsi);
void coachz_gsi_interrupt(struct coachz_gsi *gsi);
int coachz_gsi_channel_start(struct coachz_gsi *gsi, uint32_t channel);
void coachz_gsi_channel_doorbell(struct coachz_gsi *gsi, uint32_t channel, uint32_t index);
void coachz_gsi_evt_ring_doorbell(struct coachz_gsi *gsi, uint32_t ring, uint32_t index);
void *coachz_gsi_ring_entry(struct coachz_gsi *gsi, uint32_t channel, int event, uint32_t index);
void coachz_gsi_ieob_enable_one(struct coachz_gsi *gsi, uint32_t ring);

struct coachz_ipa_rx {
    struct coachz_ipa *ipa;
    struct coachz_gsi *gsi;
    const struct cipa_endpoint *e;
    uint32_t channel, evt_ring, tre_count, capacity, outstanding, replenish_count;
    uint32_t fill;              /* next ring entry to fill (unwrapped); the doorbell reports it */
    uint8_t *buf_virt;
    uint64_t buf_phys;
    uint32_t buffers, free_count;
    uint16_t free_list[256], tre_buffer[256];
    volatile uint32_t pipeline_clear_done;
    uint64_t packets, delivered, dropped, tagged, skipped, bytes;
    void (*deliver)(void *context, const uint8_t *data, uint32_t length, uint32_t metadata);   /* a network device, later */
    void *context;
};
int coachz_ipa_rx_init(struct coachz_ipa_rx *rx, struct coachz_ipa *ipa, struct coachz_gsi *gsi, unsigned name,
    void *buf_virt, uint64_t buf_phys, uint32_t bytes);
int coachz_ipa_rx_enable(struct coachz_ipa_rx *rx);
unsigned coachz_ipa_rx_poll(struct coachz_ipa_rx *rx);
int coachz_ipa_setup_lan_rx(struct coachz_ipa_rx *rx);

#endif /* COACHZ_IPA_KERNEL_H */
