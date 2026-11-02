/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * IPA v4.2 probe-time configuration: mainline ipa_config (ipa_main.c) in its order. Not in the kernel build.
 *   ipa_hardware_config, ipa_mem_config, ipa_interrupt_config, ipa_uc_config, ipa_endpoint_config, ipa_table_config,
 *   ipa_resource_config, ipa_modem_config.
 * ipa_table_config does nothing on v4.2 (ipa_table_hash_support is false for 4.2 only). ipa_modem_config registers an
 * MPSS SSR notifier; here the integrator calls coachz_ipa_modem_starting() where it powers the modem up, which is what the
 * notifier's QCOM_SSR_BEFORE_POWERUP does (ipa_uc_power, then ipa_smp2p_notify_reset on the SMP2P side).
 * The IPA core clock and interconnects must already be on (coachz-prelink-wifi.patch wifi_ipa_power). The apps SMMU is in
 * bypass (sCR0 CLIENTPD, as the Wi-Fi patch reads), so the IPA sees physical addresses.
 * Interrupts: "ipa" is GIC SPI 311 edge (sc7180.dtsi). coachz_ipa_interrupt() is ipa_isr_thread's body; call it from that
 * interrupt or from a poll.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <machine/machine_routines.h>
#include <kern/clock.h>
#include "coachz_ipa_kernel.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);


static inline uint32_t rd(vm_offset_t b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void wr(vm_offset_t b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }

/* ipa_hardware_config: bcr, tx, clkon, comp, qsb, timing (counter), hashing, dcd */
static void hardware_config(struct coachz_ipa *ipa)
{
    wr(ipa->reg, CIPA_IPA_BCR, cip_bcr());
    wr(ipa->reg, CIPA_IPA_TX_CFG, cip_tx_cfg(rd(ipa->reg, CIPA_IPA_TX_CFG)));
    wr(ipa->reg, CIPA_CLKON_CFG, cip_clkon());
    wr(ipa->reg, CIPA_COMP_CFG, cip_comp_cfg(rd(ipa->reg, CIPA_COMP_CFG)));
    wr(ipa->reg, CIPA_QSB_MAX_WRITES, cip_qsb_writes());
    wr(ipa->reg, CIPA_QSB_MAX_READS, cip_qsb_reads());
    wr(ipa->reg, CIPA_COUNTER_CFG, cip_counter_cfg());
    wr(ipa->reg, CIPA_FILT_ROUT_HASH_EN, 0);
    wr(ipa->reg, CIPA_IDLE_INDICATION_CFG, cip_idle_indication());
}

/* ipa_mem_config */
static int mem_config(struct coachz_ipa *ipa)
{
    const uint32_t reported = rd(ipa->reg, CIPA_SHARED_MEM_SIZE);
    const struct cip_mem m = cip_mem_config(reported, CIP_SHARED_BYTES);
    ipa->mem_offset = m.mem_offset;
    ipa->mem_size = m.mem_size;
    printf("COACHZ ipa: SHARED_MEM_SIZE 0x%x: mem_offset 0x%x mem_size 0x%x\n", reported, ipa->mem_offset, ipa->mem_size);
    if (!cip_mem_valid(ipa->mem_size)) { printf("COACHZ ipa: a v4.2 memory region ends beyond the memory limit\n"); return -EINVAL; }
    for (unsigned i = 0; i < CI_MEM_COUNT; i++)
        for (unsigned k = 0; k < ci_v42_mem[i].canaries; k++) {
            const uint32_t at = cip_canary_offset(ipa->mem_offset, i, k);
            if (at + 4u > CIP_SHARED_BYTES) { printf("COACHZ ipa: canary 0x%x outside ipa-shared\n", at); return -EINVAL; }
            wr(ipa->shared, at, CIP_CANARY);
        }
    __builtin_memset(ipa->zero_virt, 0, CIP_MEM_MAX);
    return 0;
}

/* ipa_interrupt_enable / _disable */
static void irq_set(struct coachz_ipa *ipa, unsigned id, int on)
{
    ipa->irq_enabled = on ? ipa->irq_enabled | 1u << id : ipa->irq_enabled & ~(1u << id);
    wr(ipa->reg, CIPA_IPA_IRQ_EN, ipa->irq_enabled);
}

/* ipa_interrupt_config: every type disabled; the IRQ itself is the integrator's */
static void interrupt_config(struct coachz_ipa *ipa)
{
    ipa->irq_enabled = 0;
    wr(ipa->reg, CIPA_IPA_IRQ_EN, 0);
}

/* ipa_uc_config */
static void uc_config(struct coachz_ipa *ipa)
{
    ipa->uc_powered = 0;
    ipa->uc_loaded = 0;
    irq_set(ipa, CIP_IRQ_UC_0, 1);
    irq_set(ipa, CIP_IRQ_UC_1, 1);
}

/* ipa_endpoint_config */
static int endpoint_config(struct coachz_ipa *ipa)
{
    const uint32_t flavor = rd(ipa->reg, CIPA_FLAVOR_0);
    const struct cip_flavor f = cip_flavor(flavor);
    const unsigned bad = cip_endpoint_check(flavor);
    printf("COACHZ ipa: FLAVOR_0 0x%x: %u TX, %u RX from %u\n", flavor, f.tx_count, f.rx_count, f.rx_base);
    if (bad > CIP_ENDPOINT_MAX) { printf("COACHZ ipa: too many endpoints, %u\n", f.limit); return -EINVAL; }
    if (bad) { printf("COACHZ ipa: endpoint %u unavailable or in the wrong direction\n", bad - 1u); return -EINVAL; }
    ipa->available_tx = f.tx_count;
    ipa->available_rx_base = f.rx_base;
    ipa->available_limit = f.limit;
    return 0;
}

/* ipa_resource_config: v4.2 has one source and one destination group, so only GRP_01's X limits are written */
static int resource_config(struct coachz_ipa *ipa)
{
    if (!cip_resource_valid(cipa_resource_src, CIP_RSRC_SRC_TYPES, CIPA_RSRC_GROUP_SRC_COUNT) ||
        !cip_resource_valid(cipa_resource_dst, CIP_RSRC_DST_TYPES, CIPA_RSRC_GROUP_DST_COUNT))
        return -EINVAL;
    for (unsigned t = 0; t < CIP_RSRC_SRC_TYPES; t++) {
        wr(ipa->reg, CIPA_SRC_RSRC_GRP_01_RSRC_TYPE + t * CIPA_SRC_RSRC_GRP_01_RSRC_TYPE_STRIDE,
            cip_resource_value(cipa_resource_src[t], 0, CIPA_RSRC_GROUP_SRC_COUNT));
        if (CIPA_RSRC_GROUP_SRC_COUNT >= 3)
            wr(ipa->reg, CIPA_SRC_RSRC_GRP_23_RSRC_TYPE + t * CIPA_SRC_RSRC_GRP_23_RSRC_TYPE_STRIDE,
                cip_resource_value(cipa_resource_src[t], 1, CIPA_RSRC_GROUP_SRC_COUNT));
    }
    for (unsigned t = 0; t < CIP_RSRC_DST_TYPES; t++) {
        wr(ipa->reg, CIPA_DST_RSRC_GRP_01_RSRC_TYPE + t * CIPA_DST_RSRC_GRP_01_RSRC_TYPE_STRIDE,
            cip_resource_value(cipa_resource_dst[t], 0, CIPA_RSRC_GROUP_DST_COUNT));
        if (CIPA_RSRC_GROUP_DST_COUNT >= 3)
            wr(ipa->reg, CIPA_DST_RSRC_GRP_23_RSRC_TYPE + t * CIPA_DST_RSRC_GRP_23_RSRC_TYPE_STRIDE,
                cip_resource_value(cipa_resource_dst[t], 1, CIPA_RSRC_GROUP_DST_COUNT));
    }
    return 0;
}

int coachz_ipa_config(struct coachz_ipa *ipa, void *zero_virt, uint64_t zero_addr, int (*qmp_send)(const char *))
{
    ipa->reg = ml_io_map(CIP_REG_PA, CIP_REG_BYTES);
    ipa->shared = ml_io_map(CIP_SHARED_PA, CIP_SHARED_BYTES);
    ipa->gsi = ml_io_map(CIP_GSI_PA, CIP_GSI_BYTES);
    ipa->zero_virt = zero_virt;
    ipa->zero_addr = zero_addr;
    ipa->qmp_send = qmp_send;
    if (!ipa->reg || !ipa->shared || !ipa->gsi || !zero_virt) { printf("COACHZ ipa: map failed\n"); return -ENOMEM; }
    hardware_config(ipa);
    int err = mem_config(ipa);
    if (err) return err;
    interrupt_config(ipa);
    uc_config(ipa);
    err = endpoint_config(ipa);
    if (!err) err = resource_config(ipa);   /* ipa_table_config: nothing on v4.2 */
    if (err) { irq_set(ipa, CIP_IRQ_UC_1, 0); irq_set(ipa, CIP_IRQ_UC_0, 0); return err; }
    printf("COACHZ ipa: configured (COMP_CFG 0x%x CLKON 0x%x QSB 0x%x/0x%x COUNTER 0x%x IRQ_EN 0x%x)\n",
        rd(ipa->reg, CIPA_COMP_CFG), rd(ipa->reg, CIPA_CLKON_CFG), rd(ipa->reg, CIPA_QSB_MAX_WRITES),
        rd(ipa->reg, CIPA_QSB_MAX_READS), rd(ipa->reg, CIPA_COUNTER_CFG), rd(ipa->reg, CIPA_IPA_IRQ_EN));
    return 0;
}

/* QCOM_SSR_BEFORE_POWERUP: ipa_uc_power holds power for the uC once, on first boot; the SMP2P reset is the integrator's */
void coachz_ipa_modem_starting(struct coachz_ipa *ipa)
{
    static int already;
    if (already) return;
    already = 1;
    ipa->uc_powered = 1;
}

static uint32_t uc_word(struct coachz_ipa *ipa, uint32_t field)
{
    return rd(ipa->shared, ipa->mem_offset + ci_v42_mem[CI_UC_SHARED].offset + field);
}

/* ipa_uc_interrupt_handler */
static void uc_interrupt(struct coachz_ipa *ipa, unsigned id)
{
    if (id == CIP_IRQ_UC_0) {
        const uint32_t event = uc_word(ipa, CIP_UC_EVENT) & 0xffu;
        if (event == CIP_UC_EVENT_ERROR) printf("COACHZ ipa: microcontroller error event\n");
        else if (event != CIP_UC_EVENT_LOG_INFO) printf("COACHZ ipa: unsupported microcontroller event %u\n", event);
        return;
    }
    const uint32_t response = uc_word(ipa, CIP_UC_RESPONSE);
    if (cip_uc_response_loads(response, ipa->uc_powered)) {
        ipa->uc_loaded = 1;
        const int sent = ipa->qmp_send ? ipa->qmp_send(CIP_RETENTION_ON) : -1;
        ipa->uc_powered = 0;   /* Linux drops its proxy vote here; the Wi-Fi patch keeps the clock on */
        printf("COACHZ ipa: microcontroller loaded, retention request %d\n", sent);
    } else if ((response & 0xffu) == CIP_UC_RESPONSE_INIT_COMPLETED) {
        printf("COACHZ ipa: unexpected init_completed response\n");
    } else {
        printf("COACHZ ipa: unsupported microcontroller response %u\n", response & 0xffu);
    }
}

/* ipa_isr_thread: handle the enabled pending types until none remain, then clear any disabled ones */
void coachz_ipa_interrupt(struct coachz_ipa *ipa)
{
    uint32_t pending = rd(ipa->reg, CIPA_IPA_IRQ_STTS), mask;
    while ((mask = pending & ipa->irq_enabled)) {
        do {
            const unsigned id = (unsigned)__builtin_ctz(mask);
            mask ^= 1u << id;
            if (id == CIP_IRQ_UC_0 || id == CIP_IRQ_UC_1) {
                wr(ipa->reg, CIPA_IPA_IRQ_CLR, 1u << id);   /* cleared first, "to avoid clearing unhandled interrupts" */
                uc_interrupt(ipa, id);
                continue;
            }
            if (id == CIP_IRQ_TX_SUSPEND)   /* ipa_interrupt_suspend_clear_all: one 32-endpoint unit on v4.2 */
                for (uint32_t unit = 0; unit < (ipa->available_limit + 31u) / 32u; unit++) {
                    const uint32_t info = rd(ipa->reg, CIPA_IRQ_SUSPEND_INFO + unit * CIPA_IRQ_SUSPEND_INFO_STRIDE);
                    if (info) wr(ipa->reg, CIPA_IRQ_SUSPEND_CLR + unit * CIPA_IRQ_SUSPEND_CLR_STRIDE, info);
                }
            wr(ipa->reg, CIPA_IPA_IRQ_CLR, 1u << id);
        } while (mask);
        pending = rd(ipa->reg, CIPA_IPA_IRQ_STTS);
    }
    if (pending) wr(ipa->reg, CIPA_IPA_IRQ_CLR, pending);
}

/* ipa_uc_panic_notifier: tell a loaded uC the AP is going down, then give it 100 us */
void coachz_ipa_uc_panic(struct coachz_ipa *ipa)
{
    if (!ipa->uc_loaded) return;
    const uint32_t base = ipa->mem_offset + ci_v42_mem[CI_UC_SHARED].offset;
    wr(ipa->shared, base + CIP_UC_COMMAND, CIP_UC_COMMAND_ERR_FATAL);   /* command byte; reserved bytes zero */
    wr(ipa->shared, base + CIP_UC_COMMAND_PARAM, 0);
    wr(ipa->shared, base + CIP_UC_COMMAND_PARAM_HI, 0);
    wr(ipa->shared, base + CIP_UC_RESPONSE, 0);
    wr(ipa->shared, base + CIP_UC_RESPONSE_PARAM, 0);
    wr(ipa->reg, CIPA_IPA_IRQ_UC, CIPA_IPA_IRQ_UC_UC_INTR);
    delay(100);
}

/* ipa_interrupt_suspend_control: the endpoint's IRQ_SUSPEND_EN bit, with the TX_SUSPEND type on while any endpoint has it */
void coachz_ipa_suspend_enable(struct coachz_ipa *ipa, uint32_t endpoint, int enable)
{
    const uint32_t mask = 1u << (endpoint % 32u), unit = endpoint / 32u;
    const uint32_t offset = CIPA_IRQ_SUSPEND_EN + unit * CIPA_IRQ_SUSPEND_EN_STRIDE;
    const uint32_t weight = (uint32_t)__builtin_popcount(ipa->suspend_enabled);
    if (weight == 1u && !enable) irq_set(ipa, CIP_IRQ_TX_SUSPEND, 0);
    const uint32_t v = rd(ipa->reg, offset);
    wr(ipa->reg, offset, enable ? v | mask : v & ~mask);
    ipa->suspend_enabled = enable ? ipa->suspend_enabled | mask : ipa->suspend_enabled & ~mask;
    if (!weight && enable) irq_set(ipa, CIP_IRQ_TX_SUSPEND, 1);
}
