/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Portable decisions of mainline drivers/net/ipa's probe-time configuration for IPA v4.2 (ipa_main.c ipa_config ->
 * ipa_hardware_config, ipa_mem.c ipa_mem_config). Register numbers come from the generated coachz_ipa_v42.h.
 * Linux 7.4-rc1 (6628e7d4c6b2); the code is our own.
 */
#ifndef COACHZ_IPA_PROTO_H
#define COACHZ_IPA_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include "coachz_ipa_v42.h"
#include "coachz_ipa_qmi.h"

/* sc7180.dtsi &ipa: "ipa-reg", "ipa-shared", "gsi" */
#define CIP_REG_PA     0x01e40000u
#define CIP_REG_BYTES  0x7000u
#define CIP_SHARED_PA  0x01e47000u
#define CIP_SHARED_BYTES 0x2000u   /* ipa->mem_size before SHARED_MEM_SIZE is read */
#define CIP_GSI_PA     0x01e04000u
#define CIP_GSI_BYTES  0x2c000u

/* reg.h reg_encode/reg_decode: a value placed under a field mask */
static inline uint32_t cip_encode(uint32_t mask, uint32_t value) { return (value << __builtin_ctz(mask)) & mask; }
static inline uint32_t cip_decode(uint32_t mask, uint32_t reg) { return (reg & mask) >> __builtin_ctz(mask); }

/* ------------------------------------------------------------------ ipa_hardware_config for v4.2, in order */
/* bcr: backward_compat; tx: PA_MASK_EN off (4.0 < v < 4.5); clkon: CLKON_GLOBAL | GLOBAL_2X_CLK (4.0 <= v < 4.5) */
static inline uint32_t cip_bcr(void) { return CIPA_BACKWARD_COMPAT; }
static inline uint32_t cip_tx_cfg(uint32_t reg) { return reg & ~CIPA_IPA_TX_CFG_PA_MASK_EN; }
static inline uint32_t cip_clkon(void) { return CIPA_CLKON_CFG_CLKON_GLOBAL | CIPA_CLKON_CFG_GLOBAL_2X_CLK; }
/* comp: 4.0 < v < 4.5 sets GSI_MULTI_AXI_MASTERS_DIS; every v4.0+ sets the two in-order disables */
static inline uint32_t cip_comp_cfg(uint32_t reg)
{
    return reg | CIPA_COMP_CFG_GSI_MULTI_AXI_MASTERS_DIS | CIPA_COMP_CFG_GSI_MULTI_INORDER_RD_DIS | CIPA_COMP_CFG_GSI_MULTI_INORDER_WR_DIS;
}
/* qsb: one master (DDR) on v4.2 */
static inline uint32_t cip_qsb_writes(void) { return cip_encode(CIPA_QSB_MAX_WRITES_GEN_QMB_0_MAX_WRITES, cipa_qsb[0].max_writes); }
static inline uint32_t cip_qsb_reads(void)
{
    return cip_encode(CIPA_QSB_MAX_READS_GEN_QMB_0_MAX_READS, cipa_qsb[0].max_reads) |
           cip_encode(CIPA_QSB_MAX_READS_GEN_QMB_0_MAX_READS_BEATS, cipa_qsb[0].max_reads_beats);
}
/* timing before v4.5: COUNTER_CFG.AGGR_GRANULARITY = DIV_ROUND_CLOSEST(500 us * 32 kHz, 1 s) - 1 */
#define CIP_AGGR_GRANULARITY_US 500u
#define CIP_TIMER_FREQUENCY 32000u
static inline uint32_t cip_aggr_granularity_val(uint32_t usec)
{
    return (uint32_t)(((uint64_t)usec * CIP_TIMER_FREQUENCY + 500000u) / 1000000u) - 1u;
}
static inline uint32_t cip_counter_cfg(void) { return cip_encode(CIPA_COUNTER_CFG_AGGR_GRANULARITY, cip_aggr_granularity_val(CIP_AGGR_GRANULARITY_US)); }
/* hashing: v4.2 only, FILT_ROUT_HASH_EN = 0; dcd: idle indication, debounce 256, no constant non-idle */
static inline uint32_t cip_idle_indication(void) { return cip_encode(CIPA_IDLE_INDICATION_CFG_ENTER_IDLE_DEBOUNCE_THRESH, 256); }

/* ------------------------------------------------------------------ ipa_mem_config */
struct cip_mem { uint32_t mem_offset, mem_size; };
/* SHARED_MEM_SIZE: BADDR and SIZE in 8-byte units; a smaller reported size limits the region, a larger one is ignored */
static inline struct cip_mem cip_mem_config(uint32_t shared_mem_size, uint32_t resource_size)
{
    struct cip_mem m;
    m.mem_offset = 8u * cip_decode(CIPA_SHARED_MEM_SIZE_MEM_BADDR, shared_mem_size);
    const uint32_t size = 8u * cip_decode(CIPA_SHARED_MEM_SIZE_MEM_SIZE, shared_mem_size);
    m.mem_size = m.mem_offset + size < resource_size ? size : resource_size;
    return m;
}
/* ipa_mem_size_valid (every region ends within mem_size) and ipa_mem_valid_one for the v4.2 table */
static inline int cip_mem_valid(uint32_t mem_size)
{
    for (unsigned i = 0; i < CI_MEM_COUNT; i++) {
        const struct ci_region *r = &ci_v42_mem[i];
        const uint32_t multiple = i == CI_MODEM ? 4u : 8u;
        if ((uint32_t)r->offset + r->size > mem_size) return 0;
        if (!r->size && !r->canaries) return 0;
        if (r->size % multiple || r->offset % 8u || r->offset < r->canaries * 4u) return 0;
    }
    return 1;
}
#define CIP_CANARY 0xdeadbeefu
/* The canaries sit in the words just below a region: offsets mem_offset + offset - 4, - 8, ... */
static inline uint32_t cip_canary_offset(uint32_t mem_offset, unsigned region, unsigned k)
{
    return mem_offset + ci_v42_mem[region].offset - 4u * (k + 1u);
}
#define CIP_MEM_MAX 0x2000u   /* IPA_MEM_MAX = 2 * PAGE_SIZE (4 KiB pages): the zeroed DMA buffer for ipa_mem_zero */

/* ------------------------------------------------------------------ ipa_interrupt.c, ipa_uc.c */
/* enum ipa_irq_id (ipa_reg.h): bits of IPA_IRQ_STTS / _EN / _CLR */
#define CIP_IRQ_UC_0       2u    /* microcontroller event */
#define CIP_IRQ_UC_1       3u    /* microcontroller response */
#define CIP_IRQ_TX_SUSPEND 14u
/* struct ipa_uc_mem_area at mem_offset + IPA_MEM_UC_SHARED: byte fields read as the 32-bit words holding them */
#define CIP_UC_COMMAND        0x00u
#define CIP_UC_COMMAND_PARAM  0x04u
#define CIP_UC_COMMAND_PARAM_HI 0x08u
#define CIP_UC_RESPONSE       0x0cu
#define CIP_UC_RESPONSE_PARAM 0x10u
#define CIP_UC_EVENT          0x14u
#define CIP_UC_RESPONSE_INIT_COMPLETED 1u
#define CIP_UC_EVENT_ERROR    1u
#define CIP_UC_EVENT_LOG_INFO 2u
#define CIP_UC_COMMAND_ERR_FATAL 4u
/* ipa_power_retention through the AOSS QMP (qcom,qmp on the sc7180 IPA node) */
#define CIP_RETENTION_ON  "{ class: bcm, res: ipa_pc, val: 1 }"
#define CIP_RETENTION_OFF "{ class: bcm, res: ipa_pc, val: 0 }"

/* What ipa_uc_response_hdlr does with a response: 1 = the uC is loaded now (drop the proxy vote, enable retention) */
static inline int cip_uc_response_loads(uint32_t response_word, int uc_powered)
{
    return (response_word & 0xffu) == CIP_UC_RESPONSE_INIT_COMPLETED && uc_powered;
}

/* ------------------------------------------------------------------ ipa_endpoint_config (v3.5+) */
#define CIP_ENDPOINT_MAX 36u   /* IPA_ENDPOINT_MAX */
/* ipa_gsi_endpoint_data_empty: an AP entry with no TLVs is a hole in the table */
static inline int cip_endpoint_defined(const struct cipa_endpoint *e) { return e->ee != 0 || e->tlv_count != 0; }
struct cip_flavor { uint32_t tx_count, rx_count, rx_base, limit; };
static inline struct cip_flavor cip_flavor(uint32_t reg)
{
    struct cip_flavor f;
    f.tx_count = cip_decode(CIPA_FLAVOR_0_MAX_CONS_PIPES, reg);   /* our TX is an IPA consumer */
    f.rx_count = cip_decode(CIPA_FLAVOR_0_MAX_PROD_PIPES, reg);
    f.rx_base = cip_decode(CIPA_FLAVOR_0_PROD_LOWEST, reg);
    f.limit = f.rx_base + f.rx_count;
    return f;
}
/* 0 when every defined endpoint exists in hardware and points the right way; otherwise the endpoint id + 1 that fails, or
   CIP_ENDPOINT_MAX + 1 when the hardware reports more endpoints than the driver (36) or IPA before v5.0 (32) supports. */
static inline unsigned cip_endpoint_check(uint32_t flavor_reg)
{
    const struct cip_flavor f = cip_flavor(flavor_reg);
    if (f.limit > CIP_ENDPOINT_MAX || f.limit > 32u) return CIP_ENDPOINT_MAX + 1u;
    for (unsigned i = 0; i < sizeof cipa_endpoints / sizeof cipa_endpoints[0]; i++) {
        const struct cipa_endpoint *e = &cipa_endpoints[i];
        if (!cip_endpoint_defined(e)) continue;
        const uint32_t id = e->endpoint;
        const int available = id < f.tx_count || (id >= f.rx_base && id < f.limit);
        if (id >= f.limit || !available) return id + 1u;
        if (e->toward_ipa ? id >= f.tx_count : id < f.rx_base) return id + 1u;
    }
    return 0;
}

/* ------------------------------------------------------------------ ipa_resource_config */
/* ipa_resource_limits_valid: 1..8 groups, and no limit in a group the hardware lacks */
static inline int cip_resource_valid(const uint32_t (*limits)[8][2], unsigned types, unsigned groups)
{
    if (!groups || groups > 8u) return 0;
    for (unsigned i = 0; i < types; i++)
        for (unsigned g = groups; g < 8u; g++)
            if (limits[i][g][0] || limits[i][g][1]) return 0;
    return 1;
}
/* ipa_resource_config_common for group pair (2k, 2k+1): Y only when the group count reaches it. The SRC and DST GRP_01/23
   registers share one field layout on v4.2. */
static inline uint32_t cip_resource_value(const uint32_t (*limit)[2], unsigned pair, unsigned groups)
{
    uint32_t v = cip_encode(CIPA_SRC_RSRC_GRP_01_RSRC_TYPE_X_MIN_LIM, limit[2 * pair][0]) |
                 cip_encode(CIPA_SRC_RSRC_GRP_01_RSRC_TYPE_X_MAX_LIM, limit[2 * pair][1]);
    if (groups > 2 * pair + 1)
        v |= cip_encode(CIPA_SRC_RSRC_GRP_01_RSRC_TYPE_Y_MIN_LIM, limit[2 * pair + 1][0]) |
             cip_encode(CIPA_SRC_RSRC_GRP_01_RSRC_TYPE_Y_MAX_LIM, limit[2 * pair + 1][1]);
    return v;
}
#define CIP_RSRC_SRC_TYPES (sizeof cipa_resource_src / sizeof cipa_resource_src[0])
#define CIP_RSRC_DST_TYPES (sizeof cipa_resource_dst / sizeof cipa_resource_dst[0])

#endif /* COACHZ_IPA_PROTO_H */
