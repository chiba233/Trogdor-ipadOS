/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Portable half of the Coachz usb_1 (DWC3 + QUSB2 + QMP USB3/DP combo) bring-up: register values and the decisions the
 * mainline drivers make from the controller version, hwparams and the trogdor device tree, without MMIO. Values and
 * order from Linux 7.4-rc1 (6628e7d4c6b2, SOURCES.md); the code is our own. The QMP tables are generated into
 * coachz_qmp_tables.h.
 */
#ifndef COACHZ_USB_PROTO_H
#define COACHZ_USB_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ board (sc7180.dtsi, sc7180-trogdor.dtsi) */
#define CU_DWC3_PA        0x0a600000u   /* usb_1_dwc3: xHCI + globals, 0xe000 */
#define CU_DWC3_BYTES     0xe000u
#define CU_QSCRATCH_PA    0x0a6f8800u   /* usb_1 glue */
#define CU_QUSB2_PA       0x088e3000u   /* usb_1_hsphy, 0x400 */
#define CU_QMP_PA         0x088e8000u   /* usb_1_qmpphy, 0x3000 */
#define CU_QFPROM_PA      0x00784000u   /* corrected region; qusb2p_hstx_trim at byte 0x25b, bits 1..3 */
#define CU_TLMM_WEST_PA   0x03500000u
#define CU_HUB_GPIO       84u           /* pp3300_hub enable, active high, boot-on */
#define CU_XHCI_SPI       133u
#define CU_GCC_PA         0x00100000u

/* GCC offsets (gcc-sc7180.c) */
#define CU_GCC_USB30_PRIM_BCR        0xf000u
#define CU_GCC_USB30_PRIM_GDSCR      0xf004u
#define CU_GCC_USB30_MASTER_CBCR     0xf010u
#define CU_GCC_USB30_SLEEP_CBCR      0xf014u
#define CU_GCC_USB30_MOCK_UTMI_CBCR  0xf018u
#define CU_GCC_USB30_MASTER_RCG      0xf01cu
#define CU_GCC_USB30_MOCK_UTMI_RCG   0xf034u
#define CU_GCC_USB3_PHY_AUX_CBCR     0xf050u
#define CU_GCC_USB3_PHY_COM_AUX_CBCR 0xf054u
#define CU_GCC_USB3_PHY_PIPE_CBCR    0xf058u   /* BRANCH_HALT_SKIP: the PHY drives this clock */
#define CU_GCC_CFG_NOC_USB3_AXI_CBCR 0x502cu
#define CU_GCC_AGGRE_USB3_AXI_CBCR   0x8201cu
#define CU_GCC_USB_PHY_CFG_AHB2PHY_CBCR 0x6a004u   /* hardware gated (hwcg bit 1): no halt check while gated */
#define CU_GCC_USB3_PRIM_CLKREF_CBCR 0x8c010u
#define CU_GCC_QUSB2PHY_PRIM_BCR     0x26000u
#define CU_GCC_USB3_PHY_PRIM_BCR     0x50000u
#define CU_GCC_USB3_DP_PHY_PRIM_BCR  0x50008u

/* RCG2 (clk-rcg2.c) for the assigned rates: the master uses clk_rcg2_ops, so 150 MHz rounds up to the 200 MHz entry
   (GPLL0 main / 3, source 1 of gcc_parent_map_0, pre_div 2*3-1); mock_utmi is XO 19.2 MHz (source 0, pre_div 1). */
#define CU_RCG_CMD_UPDATE   (1u << 0)
#define CU_RCG_CFG          0x4u
#define CU_RCG_CFG_MASK     ((7u << 8) | 0x1fu | (3u << 12) | (1u << 20))
#define CU_MASTER_CFG       ((1u << 8) | 5u)
#define CU_MOCK_UTMI_CFG    ((0u << 8) | 1u)
#define CU_RCG_UPDATE_POLLS 500u

#define CU_CBCR_ENABLE  (1u << 0)
#define CU_CBCR_HWCG    (1u << 1)
#define CU_CBCR_CLK_OFF (1u << 31)
#define CU_BRANCH_POLLS 200u
static inline int cu_branch_on(uint32_t cbcr) { return !(cbcr & CU_CBCR_CLK_OFF) || ((cbcr >> 28) & 7u) == 2u; }

/* GDSC (gdsc.c): gdsc_init's wait values, then SW_COLLAPSE cleared and PWR_ON polled for up to 2 ms. */
#define CU_GDSC_PWR_ON      (1u << 31)
#define CU_GDSC_SW_COLLAPSE (1u << 0)
#define CU_GDSC_INIT_MASK   ((1u << 1) | (1u << 2) | (0xfu << 20) | (0xfu << 16) | (0xfu << 12))
#define CU_GDSC_INIT_VAL    ((0x2u << 20) | (0x8u << 16) | (0x2u << 12))
#define CU_GDSC_POLL_US     2000u
static inline uint32_t cu_gdsc_init(uint32_t gdscr) { return (gdscr & ~CU_GDSC_INIT_MASK) | CU_GDSC_INIT_VAL; }

/* TLMM: gpio84 as en_pp3300_hub (function gpio, 2 mA, bias-disable), driven high (pinctrl-msm.c bits). */
static inline uint32_t cu_hub_gpio_cfg(uint32_t ctl) { return (ctl & ~((0xfu << 2) | 3u | (7u << 6))) | (1u << 9); }
#define CU_GPIO_OUT (1u << 1)

/* ------------------------------------------------------------------ DWC3 (drivers/usb/dwc3/core.[ch]) */
#define CU_DWC3_GSBUSCFG0   0xc100u
#define CU_DWC3_GCTL        0xc110u
#define CU_DWC3_GUCTL1      0xc11cu
#define CU_DWC3_GSNPSID     0xc120u
#define CU_DWC3_GUID        0xc128u
#define CU_DWC3_GHWPARAMS0  0xc140u
#define CU_DWC3_GHWPARAMS1  0xc144u
#define CU_DWC3_GHWPARAMS3  0xc14cu
#define CU_DWC3_GHWPARAMS6  0xc158u
#define CU_DWC3_GUCTL2      0xc19cu
#define CU_DWC3_VER_NUMBER  0xc1a0u
#define CU_DWC3_VER_TYPE    0xc1a4u
#define CU_DWC3_GUSB2PHYCFG 0xc200u
#define CU_DWC3_GUSB3PIPECTL 0xc2c0u
#define CU_DWC3_GEVNTADRLO  0xc400u
#define CU_DWC3_GEVNTADRHI  0xc404u
#define CU_DWC3_GEVNTSIZ    0xc408u
#define CU_DWC3_GEVNTCOUNT  0xc40cu
#define CU_DWC3_GUCTL3      0xc60cu
#define CU_DWC3_DCTL        0xc704u
#define CU_DWC3_LLUCTL      0xd024u   /* DWC_usb31 only; not touched on DWC_usb3 */

#define CU_GCTL_PRTCAPDIR(n)       ((uint32_t)(n) << 12)
#define CU_GCTL_PRTCAP_MASK        CU_GCTL_PRTCAPDIR(3)
#define CU_GCTL_PRTCAP_HOST        1u
#define CU_GCTL_U2RSTECN           (1u << 16)
#define CU_GCTL_SOFITPSYNC         (1u << 10)
#define CU_GCTL_SCALEDOWN_MASK     (3u << 4)
#define CU_GCTL_DISSCRAMBLE        (1u << 3)
#define CU_GCTL_GBLHIBERNATIONEN   (1u << 1)
#define CU_GCTL_DSBLCLKGTNG        (1u << 0)
#define CU_GUCTL1_DEV_DECOUPLE_L1L2_EVT (1u << 31)
#define CU_GUCTL1_DEV_FORCE_20_CLK_FOR_30_CLK (1u << 26)
#define CU_GUCTL1_DEV_L1_EXIT_BY_HW (1u << 24)
#define CU_GUCTL1_PARKMODE_DISABLE_SS (1u << 17)
#define CU_GUCTL2_RST_ACTBITLATER  (1u << 14)
#define CU_GUCTL2_LC_TIMER         (1u << 19)
#define CU_GUSB2PHYCFG_SUSPHY      (1u << 6)
#define CU_GUSB2PHYCFG_ENBLSLPM    (1u << 8)
#define CU_GUSB3PIPECTL_SUSPHY     (1u << 17)
#define CU_GUSB3PIPECTL_UX_EXIT_PX (1u << 27)
#define CU_DCTL_RUN_STOP           (1u << 31)
#define CU_DCTL_CSFTRST            (1u << 30)
#define CU_DCTL_ULSTCHNGREQ_MASK   (0xfu << 5)
#define CU_EVENT_BUFFER_BYTES      4096u

#define CU_HW_MODE(hw0)     ((hw0) & 3u)
#define CU_HW_MODE_HOST     1u
#define CU_HW_MODE_DRD      2u
#define CU_PWROPT(hw1)      (((hw1) >> 24) & 3u)
#define CU_PWROPT_CLK       1u
#define CU_PWROPT_HIB       2u
#define CU_HW6_EN_FPGA      (1u << 7)

/* dwc3_core_is_valid and the DWC3_VER_* comparisons. */
enum { CU_IP_NONE = 0, CU_IP_DWC3 = 0x5533, CU_IP_DWC31 = 0x3331, CU_IP_DWC32 = 0x3332 };
struct cu_dwc3 { unsigned ip; uint32_t revision; };
static inline int cu_dwc3_identify(uint32_t gsnpsid, uint32_t ver_number, struct cu_dwc3 *d)
{
    d->ip = gsnpsid >> 16;
    if (d->ip == CU_IP_DWC3) { d->revision = gsnpsid; return 1; }
    if (d->ip == CU_IP_DWC31 || d->ip == CU_IP_DWC32) { d->revision = ver_number; return 1; }
    d->ip = CU_IP_NONE;
    return 0;
}
#define CU_DWC3_REV(v)  (0x55330000u | (v))   /* DWC3_REVISION_xxxA, e.g. 0x310a */
#define CU_DWC31_REV(a, b, c) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | 0x2au)   /* "170*" */
static inline int cu_is(const struct cu_dwc3 *d, unsigned ip, uint32_t rev) { return d->ip == ip && d->revision == rev; }
static inline int cu_prior(const struct cu_dwc3 *d, unsigned ip, uint32_t rev) { return d->ip == ip && d->revision < rev; }
static inline int cu_within(const struct cu_dwc3 *d, unsigned ip, uint32_t lo, uint32_t hi)
{
    return d->ip == ip && d->revision >= lo && (!hi || d->revision <= hi);
}

/* The trogdor usb_1_dwc3 node's properties that change the core setup. */
struct cu_props { int dis_u2_susphy, dis_u3_susphy, dis_enblslpm, parkmode_disable_ss, max_speed_super; };
static const struct cu_props cu_trogdor_props = { 1, 0, 1, 1, 1 };

/* dwc3_ss_phy_setup with no SS quirks in the device tree */
static inline uint32_t cu_gusb3pipectl_setup(uint32_t reg) { return reg & ~(CU_GUSB3PIPECTL_UX_EXIT_PX | CU_GUSB3PIPECTL_SUSPHY); }
/* dwc3_hs_phy_setup: no hsphy_interface/phy_type properties, so the interface bits keep their default */
static inline uint32_t cu_gusb2phycfg_setup(uint32_t reg, const struct cu_props *p)
{
    reg &= ~CU_GUSB2PHYCFG_SUSPHY;
    if (p->dis_enblslpm) reg &= ~CU_GUSB2PHYCFG_ENBLSLPM; else reg |= CU_GUSB2PHYCFG_ENBLSLPM;
    return reg;
}
/* dwc3_enable_susphy */
static inline uint32_t cu_u3_susphy(uint32_t reg, int enable, const struct cu_props *p)
{
    return (enable && !p->dis_u3_susphy) ? reg | CU_GUSB3PIPECTL_SUSPHY : reg & ~CU_GUSB3PIPECTL_SUSPHY;
}
static inline uint32_t cu_u2_susphy(uint32_t reg, int enable, const struct cu_props *p)
{
    return (enable && !p->dis_u2_susphy) ? reg | CU_GUSB2PHYCFG_SUSPHY : reg & ~CU_GUSB2PHYCFG_SUSPHY;
}
/* dwc3_phy_init's tail: SUSPHY on after PHY init unless the core is DWC_usb3 1.94a or older */
static inline int cu_susphy_after_phy_init(const struct cu_dwc3 *d)
{
    return !cu_within(d, CU_IP_DWC3, 0, CU_DWC3_REV(0x194a));
}
/* dwc3_core_setup_global_control for dr_mode host, no scramble/LFPS quirks */
static inline uint32_t cu_gctl_setup(uint32_t reg, uint32_t hw0, uint32_t hw1, const struct cu_dwc3 *d)
{
    reg &= ~CU_GCTL_SCALEDOWN_MASK;
    switch (CU_PWROPT(hw1)) {
    case CU_PWROPT_CLK:
        if (cu_within(d, CU_IP_DWC3, CU_DWC3_REV(0x210a), CU_DWC3_REV(0x250a))) reg |= CU_GCTL_DSBLCLKGTNG | CU_GCTL_SOFITPSYNC;
        else reg &= ~CU_GCTL_DSBLCLKGTNG;
        break;
    case CU_PWROPT_HIB:
        reg |= CU_GCTL_GBLHIBERNATIONEN;
        break;
    default:
        break;
    }
    if (CU_PWROPT(hw1) != 0 && CU_HW_MODE(hw0) != 0 && cu_is(d, CU_IP_DWC31, CU_DWC31_REV('2', '0', '0'))) reg |= CU_GCTL_DSBLCLKGTNG;
    reg &= ~CU_GCTL_DISSCRAMBLE;
    if (cu_prior(d, CU_IP_DWC3, CU_DWC3_REV(0x190a))) reg |= CU_GCTL_U2RSTECN;
    return reg;
}
/* dwc3_core_soft_reset: the device-side reset runs because the role is not host yet; DCTL write made safe */
static inline uint32_t cu_dctl_soft_reset(uint32_t dctl) { return ((dctl | CU_DCTL_CSFTRST) & ~CU_DCTL_RUN_STOP) & ~CU_DCTL_ULSTCHNGREQ_MASK; }
static inline unsigned cu_soft_reset_polls(const struct cu_dwc3 *d)
{
    return (cu_within(d, CU_IP_DWC31, CU_DWC31_REV('1', '9', '0'), 0) || d->ip == CU_IP_DWC32) ? 10u : 1000u;
}
static inline int cu_soft_reset_poll_ms(const struct cu_dwc3 *d) { return cu_soft_reset_polls(d) == 10u; }   /* 20 ms steps */
static inline int cu_soft_reset_wait_50ms(const struct cu_dwc3 *d) { return cu_within(d, CU_IP_DWC31, 0, CU_DWC31_REV('1', '8', '0')); }
/* dwc3_core_init after the event buffers */
static inline uint32_t cu_guctl2(uint32_t reg, const struct cu_dwc3 *d)
{
    if (cu_within(d, CU_IP_DWC3, CU_DWC3_REV(0x310a), 0)) reg |= CU_GUCTL2_RST_ACTBITLATER;
    if (cu_is(d, CU_IP_DWC3, CU_DWC3_REV(0x320a))) reg &= ~CU_GUCTL2_LC_TIMER;
    return reg;
}
static inline int cu_guctl1_applies(const struct cu_dwc3 *d) { return !cu_prior(d, CU_IP_DWC3, CU_DWC3_REV(0x250a)); }
static inline uint32_t cu_guctl1(uint32_t reg, const struct cu_dwc3 *d, const struct cu_props *p)
{
    if (!cu_prior(d, CU_IP_DWC3, CU_DWC3_REV(0x290a))) reg |= CU_GUCTL1_DEV_L1_EXIT_BY_HW;
    if (!cu_prior(d, CU_IP_DWC3, CU_DWC3_REV(0x300a))) reg |= CU_GUCTL1_DEV_DECOUPLE_L1L2_EVT;
    if (p->parkmode_disable_ss) reg |= CU_GUCTL1_PARKMODE_DISABLE_SS;
    if (cu_within(d, CU_IP_DWC3, CU_DWC3_REV(0x290a), 0)) {
        if (p->max_speed_super) reg &= ~CU_GUCTL1_DEV_FORCE_20_CLK_FOR_30_CLK;
        else reg |= CU_GUCTL1_DEV_FORCE_20_CLK_FOR_30_CLK;
    }
    return reg;
}
#define CU_LINUX_VERSION_CODE ((7u << 16) | (4u << 8))   /* written to GUID, as dwc3_core_init does */
/* dwc3_set_prtcap: DRD cores leave SUSPHY off while the role changes */
static inline int cu_prtcap_needs_susphy_off(uint32_t gctl, uint32_t hw0) { return CU_HW_MODE(hw0) == CU_HW_MODE_DRD && ((gctl >> 12) & 3u) != CU_GCTL_PRTCAP_HOST; }
static inline uint32_t cu_gctl_host(uint32_t gctl) { return (gctl & ~CU_GCTL_PRTCAP_MASK) | CU_GCTL_PRTCAPDIR(CU_GCTL_PRTCAP_HOST); }

/* dwc3_get_num_ports via xhci_find_next_ext_cap: count protocol capabilities by major revision. */
typedef uint32_t (*cu_read32)(void *ctx, uint32_t offset);
static inline int cu_xhci_ports(cu_read32 rd, void *ctx, unsigned *usb2, unsigned *usb3)
{
    *usb2 = *usb3 = 0;
    uint32_t offset = ((rd(ctx, 0x10) >> 16) & 0xffffu) << 2;   /* HCCPARAMS1.xECP */
    for (unsigned guard = 0; offset && guard < 64; guard++) {
        const uint32_t val = rd(ctx, offset);
        if (val == 0xffffffffu) break;
        if ((val & 0xffu) == 2u) {   /* XHCI_EXT_CAPS_PROTOCOL */
            const unsigned major = val >> 24, count = (rd(ctx, offset + 8) >> 8) & 0xffu;
            if (major == 3) *usb3 += count;
            else if (major <= 2) *usb2 += count;
        }
        const uint32_t next = (val >> 8) & 0xffu;
        if (!next) break;
        offset += next << 2;
    }
    return *usb2 <= 16 && *usb3 <= 16;   /* DWC3_USB2_MAX_PORTS / DWC3_USB3_MAX_PORTS */
}

/* ------------------------------------------------------------------ QUSB2 v2 (phy-qcom-qusb2.c) */
#define CU_QUSB2_PLL_STATUS     0x1a0u
#define CU_QUSB2_PORT_TUNE1     0x240u
#define CU_QUSB2_PORT_TUNE2     0x244u
#define CU_QUSB2_PORT_POWERDOWN 0x210u
#define CU_QUSB2_IMP_CTRL1      0x220u
#define CU_QUSB2_PLL_BIAS_CONTROL_2 0x198u
#define CU_QUSB2_CHG_CTRL2      0x23cu
#define CU_QUSB2_POWER_DOWN     (1u << 0)
#define CU_QUSB2_DISABLE_CTRL   ((1u << 5) | (1u << 1) | (1u << 0))   /* VREF_SUPPLY_TRIM | CLAMP_N_EN | POWER_DOWN */
#define CU_QUSB2_CORE_READY     (1u << 0)
struct cu_reg { uint16_t offset; uint8_t val; };
/* qusb2_v2_init_tbl, the _L entries resolved through qusb2_v2_regs_layout */
static const struct cu_reg cu_qusb2_v2_init[] = {
    {0x004, 0x03}, {0x18c, 0x7c}, {0x02c, 0x80}, {0x184, 0x0a}, {0x0b4, 0x19}, {0x194, 0x40}, {0x198, 0x20},
    {0x214, 0x21}, {0x220, 0x00}, {0x224, 0x58},
    {0x240, 0x30}, {0x244, 0x29}, {0x248, 0xca}, {0x24c, 0x04}, {0x250, 0x03},
    {0x23c, 0x00},
};
struct cu_rmw { uint16_t offset; uint32_t mask, value; };
/* qusb2_phy_override_phy_params for &usb_1_hsphy: imp-res-offset 8, bias-ctrl 0x22, charge-ctrl 3,
   preemphasis 15 % (3), half-bit width, hsdisc-trim 0. No qcom,hstx-trim-value in the device tree. */
static const struct cu_rmw cu_qusb2_trogdor_overrides[] = {
    { CU_QUSB2_IMP_CTRL1, 0x3fu, 8u },
    { CU_QUSB2_PLL_BIAS_CONTROL_2, 0x3fu, 0x22u },
    { CU_QUSB2_CHG_CTRL2, 0x30u, 3u << 4 },
    { CU_QUSB2_PORT_TUNE1, 0x03u, 3u },
    { CU_QUSB2_PORT_TUNE1, 1u << 2, 1u << 2 },
    { CU_QUSB2_PORT_TUNE2, 0x03u, 0u },
};
static inline uint32_t cu_rmw_apply(uint32_t reg, const struct cu_rmw *r) { return (reg & ~r->mask) | (r->value & r->mask); }
/* nvmem cell qusb2p_hstx_trim: byte 0x25b of the corrected QFPROM, bits 1..3, read as the word at 0x258. 0: no trim. */
static inline uint8_t cu_hstx_trim(uint32_t word_258) { return (uint8_t)(((word_258 >> 24) >> 1) & 7u); }
/* qusb2_phy_set_tune2_param with update_tune1_with_efuse: TUNE1[7:4] */
static inline uint32_t cu_tune1_trim(uint32_t tune1, uint8_t trim) { return (tune1 & ~0xf0u) | ((uint32_t)trim << 4); }

/* ------------------------------------------------------------------ QMP USB3/DP combo, v3 (phy-qcom-qmp-combo.c) */
#define CU_QMP_COM          0x0000u
#define CU_QMP_TXA          0x1200u
#define CU_QMP_RXA          0x1400u
#define CU_QMP_TXB          0x1600u
#define CU_QMP_RXB          0x1800u
#define CU_QMP_SERDES       0x1000u
#define CU_QMP_PCS          0x1c00u
#define CU_DP_COM_PHY_MODE_CTRL   0x00u
#define CU_DP_COM_SW_RESET        0x04u
#define CU_DP_COM_POWER_DOWN_CTRL 0x08u
#define CU_DP_COM_SWI_CTRL        0x0cu
#define CU_DP_COM_TYPEC_CTRL      0x10u
#define CU_DP_COM_RESET_OVRD_CTRL 0x1cu
#define CU_PCS_SW_RESET           0x000u
#define CU_PCS_POWER_DOWN_CONTROL 0x004u
#define CU_PCS_START_CONTROL      0x008u
#define CU_PCS_STATUS             0x174u
#define CU_SW_RESET  (1u << 0)
#define CU_SW_PWRDN  (1u << 0)
#define CU_SW_DPPHY_RESET (1u << 0)
#define CU_SW_DPPHY_RESET_MUX (1u << 1)
#define CU_SW_USB3PHY_RESET (1u << 2)
#define CU_SW_USB3PHY_RESET_MUX (1u << 3)
#define CU_SW_USB4PHY_RESET (1u << 4)
#define CU_SW_USB4PHY_RESET_MUX (1u << 5)
#define CU_RESET_OVRD_ALL (CU_SW_DPPHY_RESET | CU_SW_DPPHY_RESET_MUX | CU_SW_USB3PHY_RESET | CU_SW_USB3PHY_RESET_MUX | \
                           CU_SW_USB4PHY_RESET | CU_SW_USB4PHY_RESET_MUX)
#define CU_RESET_OVRD_USB3DP (CU_SW_DPPHY_RESET_MUX | CU_SW_DPPHY_RESET | CU_SW_USB3PHY_RESET_MUX | CU_SW_USB3PHY_RESET)
#define CU_USB3_MODE (1u << 0)
#define CU_DP_MODE   (1u << 1)
#define CU_SW_PORTSELECT_MUX (1u << 1)   /* orientation NONE: no SW_PORTSELECT_VAL; no typec switch on Coachz */
#define CU_SERDES_START (1u << 0)
#define CU_PCS_START    (1u << 1)
#define CU_PHYSTATUS    (1u << 6)
#define CU_PHY_INIT_TIMEOUT_US 10000u

/* ------------------------------------------------------------------ supplies (sc7180-trogdor.dtsi, regulator core) */
/* Consumers enable without a voltage request; a range regulator was set to its minimum at registration, a fixed one to
   its only value. initial-mode HPM where the device tree says so. */
struct cu_supply { const char *resource; uint32_t mv; int hpm; };
static const struct cu_supply cu_qusb2_supplies[] = {   /* "vdd", "vdda-pll", "vdda-phy-dpdm" */
    { "ldoa4", 824, 1 }, { "ldoa11", 1800, 1 }, { "ldoa17", 2920, 1 },
};
static const struct cu_supply cu_qmp_supplies[] = {     /* "vdda-phy", "vdda-pll" */
    { "ldoa4", 824, 1 }, { "ldoc3", 1200, 1 },
};
#define CU_LEVEL_NOM 256u   /* usb_1 required-opps = <&rpmhpd_opp_nom> */

#endif /* COACHZ_USB_PROTO_H */
