/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Coachz usb_1 SoC bring-up, stage 1: the mainline sequence that leaves the DWC3 core in host mode with both PHYs up,
 * so that any xHCI driver (the existing libpayload stack in coachz_usb.c, or a later one) starts from a controller this
 * kernel initialised instead of firmware's leftovers. Not in the kernel build. Port of Linux 7.4-rc1 (SOURCES.md):
 *
 *   regulator-fixed pp3300_hub  gpio84 high (boot-on; onboard_usb_dev enables vdd, reset_us 0, for both RTS5411 halves)
 *   genpd + OPP                 USB30_PRIM_GDSC on, CX at nom (required-opps)
 *   assigned-clock-rates        master 150 MHz -> 200 MHz (clk_rcg2_ops rounds up), mock_utmi 19.2 MHz
 *   dwc3-qcom-legacy probe      GCC_USB30_PRIM_BCR assert/deassert, cfg_noc, core, iface, sleep, mock_utmi clocks
 *   dwc3_core_probe             GSNPSID, hwparams, xHCI port count, event buffer, dwc3_core_init, host mode
 *   dwc3_core_init              GUSB3PIPECTL/GUSB2PHYCFG setup, QUSB2 init, QMP usb init (com_init + power_on),
 *                               SUSPHY, device-side soft reset, GUID, GCTL, event buffer, GUCTL2/GUCTL1
 *   dwc3_core_init_mode         PRTCAPDIR host (SUSPHY off first on DRD cores)
 *
 * After it returns 0 the xHCI at 0x0a600000 is ready for its driver's HCRST. Not done here (stage limits): the wake
 * interrupts and suspend paths of the glue, interconnect bandwidth votes, Type-C (cros-ec-typec) and DisplayPort.
 *
 * Boot argument: coachz-usb-soc=1. Without it coachz_usb_soc_init() returns 1 at once and firmware's state is used.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include "coachz_usb_proto.h"
#include "coachz_qmp_tables.h"
#include "coachz_usb_rpmh.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
int coachz_usb_soc_init(void);

/* The event buffer: one page below 4 GiB outside the XNU window, after the SDHCI pool (0x81100000 + 0x30000) and below
   the speaker ring (0x81200000). The core only writes it in device mode; dwc3_core_init programs it regardless. */
#define EVT_PA 0x81130000ul

static vm_offset_t dwc, qscratch, qusb2, qmp, qfprom, gpio, evt;
/* GCC is mapped page by page: the registers used here are spread over 0x5000-0x8c010. */
static const uint32_t gcc_pages[] = { 0x05000, 0x0f000, 0x26000, 0x50000, 0x6a000, 0x82000, 0x8c000 };
static vm_offset_t gcc_va[sizeof(gcc_pages) / sizeof(gcc_pages[0])];

static inline uint32_t rd(vm_offset_t b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void wr(vm_offset_t b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void setb(vm_offset_t b, uint32_t o, uint32_t v) { wr(b, o, rd(b, o) | v); (void)rd(b, o); }
static inline void clrb(vm_offset_t b, uint32_t o, uint32_t v) { wr(b, o, rd(b, o) & ~v); (void)rd(b, o); }

static vm_offset_t gcc_reg(uint32_t offset)
{
    for (unsigned i = 0; i < sizeof(gcc_pages) / sizeof(gcc_pages[0]); i++)
        if ((offset & ~0xfffu) == gcc_pages[i]) return gcc_va[i] + (offset & 0xfffu);
    panic("COACHZ usb: GCC offset 0x%x outside the mapped pages", offset);
}
static inline uint32_t grd(uint32_t o) { return *(volatile uint32_t *)gcc_reg(o); }
static inline void gwr(uint32_t o, uint32_t v) { *(volatile uint32_t *)gcc_reg(o) = v; }

static uint64_t now_us(void)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000u;
}
static void spin_us(unsigned us)
{
    const uint64_t end = now_us() + us;
    while (now_us() < end) __asm__ volatile ("yield");
}

/* ------------------------------------------------------------------ GCC */
static int branch_on(uint32_t cbcr, int skip_halt)
{
    gwr(cbcr, grd(cbcr) | CU_CBCR_ENABLE);
    if (skip_halt || (grd(cbcr) & CU_CBCR_HWCG)) return 1;   /* BRANCH_HALT_SKIP, or hardware clock gating */
    for (unsigned i = 0; i < CU_BRANCH_POLLS; i++) {
        if (cu_branch_on(grd(cbcr))) return 1;
        spin_us(1);
    }
    printf("COACHZ usb: branch 0x%x status stuck at off (0x%x)\n", cbcr, grd(cbcr));
    return 0;
}
static int rcg_set(uint32_t rcg, uint32_t cfg)
{
    gwr(rcg + CU_RCG_CFG, (grd(rcg + CU_RCG_CFG) & ~CU_RCG_CFG_MASK) | cfg);
    gwr(rcg, grd(rcg) | CU_RCG_CMD_UPDATE);
    for (unsigned i = 0; i < CU_RCG_UPDATE_POLLS; i++) {
        if (!(grd(rcg) & CU_RCG_CMD_UPDATE)) return 1;
        spin_us(1);
    }
    printf("COACHZ usb: rcg 0x%x didn't update its configuration\n", rcg);
    return 0;
}
static void bcr(uint32_t reg, int assert) { gwr(reg, assert ? grd(reg) | 1u : grd(reg) & ~1u); }

static int gdsc_on(void)
{
    gwr(CU_GCC_USB30_PRIM_GDSCR, cu_gdsc_init(grd(CU_GCC_USB30_PRIM_GDSCR)));   /* gdsc_init */
    gwr(CU_GCC_USB30_PRIM_GDSCR, grd(CU_GCC_USB30_PRIM_GDSCR) & ~CU_GDSC_SW_COLLAPSE);
    const uint64_t end = now_us() + CU_GDSC_POLL_US;
    while (!(grd(CU_GCC_USB30_PRIM_GDSCR) & CU_GDSC_PWR_ON)) {
        if (now_us() > end) { printf("COACHZ usb: usb30_prim_gdsc status stuck at 'off' (0x%x)\n", grd(CU_GCC_USB30_PRIM_GDSCR)); return 0; }
    }
    spin_us(1);
    return 1;
}

static int supplies_on(unsigned consumer, const struct cu_supply *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (coachz_rpmh_vreg_request(consumer, s[i].resource, s[i].mv, s[i].hpm, 1)) {
            printf("COACHZ usb: %s enable failed\n", s[i].resource);
            return 0;
        }
    return 1;
}

/* ------------------------------------------------------------------ QUSB2 (qusb2_phy_init) */
static int qusb2_init(void)
{
    if (!supplies_on(COACHZ_VREG_USB_HSPHY, cu_qusb2_supplies, sizeof(cu_qusb2_supplies) / sizeof(cu_qusb2_supplies[0])))
        return 0;
    if (!branch_on(CU_GCC_USB_PHY_CFG_AHB2PHY_CBCR, 0)) return 0;   /* cfg_ahb; no iface clock in the device tree */
    bcr(CU_GCC_QUSB2PHY_PRIM_BCR, 1);
    spin_us(100);
    bcr(CU_GCC_QUSB2PHY_PRIM_BCR, 0);
    setb(qusb2, CU_QUSB2_PORT_POWERDOWN, CU_QUSB2_DISABLE_CTRL);
    for (size_t i = 0; i < sizeof(cu_qusb2_v2_init) / sizeof(cu_qusb2_v2_init[0]); i++)
        wr(qusb2, cu_qusb2_v2_init[i].offset, cu_qusb2_v2_init[i].val);
    for (size_t i = 0; i < sizeof(cu_qusb2_trogdor_overrides) / sizeof(cu_qusb2_trogdor_overrides[0]); i++) {
        const struct cu_rmw *r = &cu_qusb2_trogdor_overrides[i];
        wr(qusb2, r->offset, cu_rmw_apply(rd(qusb2, r->offset), r));
    }
    const uint8_t trim = cu_hstx_trim(rd(qfprom, 0x258));
    if (trim) wr(qusb2, CU_QUSB2_PORT_TUNE1, cu_tune1_trim(rd(qusb2, CU_QUSB2_PORT_TUNE1), trim));
    clrb(qusb2, CU_QUSB2_PORT_POWERDOWN, CU_QUSB2_POWER_DOWN);
    spin_us(150);
    /* se_clk_scheme_default and no qcom,tcsr-syscon: the single-ended clock, so the ref clock stays off */
    spin_us(100);
    const uint32_t status = *(volatile uint8_t *)(qusb2 + CU_QUSB2_PLL_STATUS);
    printf("COACHZ usb: qusb2 hstx trim %u, pll status 0x%x\n", trim, status);
    if (!(status & CU_QUSB2_CORE_READY)) { printf("COACHZ usb: QUSB2PHY pll lock failed\n"); return 0; }
    return 1;
}

/* ------------------------------------------------------------------ QMP combo, USB part (qmp_combo_usb_init) */
static void qmp_table(uint32_t block, const struct cu_qmp_reg *t, size_t n, uint8_t lane)
{
    for (size_t i = 0; i < n; i++)
        if (t[i].lane_mask & lane) wr(qmp, block + t[i].offset, t[i].val);
}
#define QMP_TABLE(block, name, lane) qmp_table(block, name, sizeof(name) / sizeof(name[0]), lane)

static int qmp_init(void)
{
    /* qmp_combo_com_init in QMPPHY_MODE_USB3DP, orientation none */
    if (!supplies_on(COACHZ_VREG_USB_SSPHY, cu_qmp_supplies, sizeof(cu_qmp_supplies) / sizeof(cu_qmp_supplies[0]))) return 0;
    bcr(CU_GCC_USB3_PHY_PRIM_BCR, 1);   /* reset_list = { "phy" } */
    bcr(CU_GCC_USB3_PHY_PRIM_BCR, 0);
    if (!branch_on(CU_GCC_USB3_PHY_AUX_CBCR, 0) || !branch_on(CU_GCC_USB_PHY_CFG_AHB2PHY_CBCR, 0) ||
        !branch_on(CU_GCC_USB3_PRIM_CLKREF_CBCR, 0) || !branch_on(CU_GCC_USB3_PHY_COM_AUX_CBCR, 0)) return 0;   /* aux, cfg_ahb, ref, com_aux */
    (void)branch_on(CU_GCC_USB3_PHY_PIPE_CBCR, 1);
    setb(qmp, CU_QMP_COM + CU_DP_COM_POWER_DOWN_CTRL, CU_SW_PWRDN);
    setb(qmp, CU_QMP_COM + CU_DP_COM_RESET_OVRD_CTRL, CU_RESET_OVRD_ALL);
    wr(qmp, CU_QMP_COM + CU_DP_COM_TYPEC_CTRL, CU_SW_PORTSELECT_MUX);
    wr(qmp, CU_QMP_COM + CU_DP_COM_PHY_MODE_CTRL, CU_USB3_MODE | CU_DP_MODE);
    clrb(qmp, CU_QMP_COM + CU_DP_COM_RESET_OVRD_CTRL, CU_RESET_OVRD_USB3DP);
    clrb(qmp, CU_QMP_COM + CU_DP_COM_SWI_CTRL, 0x03u);
    clrb(qmp, CU_QMP_COM + CU_DP_COM_SW_RESET, CU_SW_RESET);
    setb(qmp, CU_QMP_PCS + CU_PCS_POWER_DOWN_CONTROL, CU_SW_PWRDN);

    /* qmp_combo_usb_power_on */
    QMP_TABLE(CU_QMP_SERDES, cu_v3_usb3_serdes_tbl, 0xff);
    QMP_TABLE(CU_QMP_TXA, cu_v3_usb3_tx_tbl, 1);
    QMP_TABLE(CU_QMP_TXB, cu_v3_usb3_tx_tbl, 2);
    QMP_TABLE(CU_QMP_RXA, cu_v3_usb3_rx_tbl, 1);
    QMP_TABLE(CU_QMP_RXB, cu_v3_usb3_rx_tbl, 2);
    QMP_TABLE(CU_QMP_PCS, cu_v3_usb3_pcs_tbl, 0xff);
    spin_us(10);   /* has_pwrdn_delay */
    clrb(qmp, CU_QMP_PCS + CU_PCS_SW_RESET, CU_SW_RESET);
    setb(qmp, CU_QMP_PCS + CU_PCS_START_CONTROL, CU_SERDES_START | CU_PCS_START);
    const uint64_t end = now_us() + CU_PHY_INIT_TIMEOUT_US;
    while (rd(qmp, CU_QMP_PCS + CU_PCS_STATUS) & CU_PHYSTATUS) {
        if (now_us() > end) { printf("COACHZ usb: qmp phy initialization timed-out (status 0x%x)\n", rd(qmp, CU_QMP_PCS + CU_PCS_STATUS)); return 0; }
        spin_us(200);
    }
    return 1;
}

static uint32_t dwc_read32(void *ctx, uint32_t offset) { return rd((vm_offset_t)ctx, offset); }

/* ------------------------------------------------------------------ the whole sequence */
int coachz_usb_soc_init(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("coachz-usb-soc", &enabled, sizeof(enabled)) || enabled != 1) return 1;
    const uint64_t start = now_us();
    for (unsigned i = 0; i < sizeof(gcc_pages) / sizeof(gcc_pages[0]); i++)
        if (!(gcc_va[i] = ml_io_map(CU_GCC_PA + gcc_pages[i], 0x1000))) return 0;
    dwc = ml_io_map(CU_DWC3_PA, CU_DWC3_BYTES);
    qscratch = ml_io_map(CU_QSCRATCH_PA & ~0xfffu, 0x1000);
    qusb2 = ml_io_map(CU_QUSB2_PA, 0x1000);
    qmp = ml_io_map(CU_QMP_PA, 0x3000);
    qfprom = ml_io_map(CU_QFPROM_PA, 0x1000);
    gpio = ml_io_map(CU_TLMM_WEST_PA + 0x1000u * CU_HUB_GPIO, 0x1000);
    evt = ml_io_map_wcomb(EVT_PA, 0x1000);
    if (!dwc || !qscratch || !qusb2 || !qmp || !qfprom || !gpio || !evt) { printf("COACHZ usb: map failed\n"); return 0; }

    /* pp3300_hub: the fixed regulator's enable GPIO (en_pp3300_hub pin state), high */
    wr(gpio, 4, rd(gpio, 4) | CU_GPIO_OUT);
    wr(gpio, 0, cu_hub_gpio_cfg(rd(gpio, 0)));

    /* power domain and its performance state, then the assigned rates */
    if (!gdsc_on()) return 0;
    if (coachz_rpmh_cx_request(COACHZ_CX_USB, CU_LEVEL_NOM)) printf("COACHZ usb: cx vote failed\n");
    if (!rcg_set(CU_GCC_USB30_MOCK_UTMI_RCG, CU_MOCK_UTMI_CFG) || !rcg_set(CU_GCC_USB30_MASTER_RCG, CU_MASTER_CFG)) return 0;

    /* dwc3_qcom_probe (legacy binding): reset, clocks in device-tree order */
    bcr(CU_GCC_USB30_PRIM_BCR, 1);
    spin_us(10);
    bcr(CU_GCC_USB30_PRIM_BCR, 0);
    if (!branch_on(CU_GCC_CFG_NOC_USB3_AXI_CBCR, 0) || !branch_on(CU_GCC_USB30_MASTER_CBCR, 0) ||
        !branch_on(CU_GCC_AGGRE_USB3_AXI_CBCR, 0) || !branch_on(CU_GCC_USB30_SLEEP_CBCR, 0) ||
        !branch_on(CU_GCC_USB30_MOCK_UTMI_CBCR, 0)) return 0;

    /* dwc3_core_probe */
    struct cu_dwc3 ver;
    if (!cu_dwc3_identify(rd(dwc, CU_DWC3_GSNPSID), rd(dwc, CU_DWC3_VER_NUMBER), &ver)) {
        printf("COACHZ usb: this is not a DesignWare USB3 controller (GSNPSID 0x%x)\n", rd(dwc, CU_DWC3_GSNPSID));
        return 0;
    }
    const uint32_t hw0 = rd(dwc, CU_DWC3_GHWPARAMS0), hw1 = rd(dwc, CU_DWC3_GHWPARAMS1);
    unsigned usb2 = 1, usb3 = 1;
    if (CU_HW_MODE(hw0) == CU_HW_MODE_HOST && !cu_xhci_ports(dwc_read32, (void *)dwc, &usb2, &usb3)) return 0;
    const struct cu_props *p = &cu_trogdor_props;
    printf("COACHZ usb: dwc3 ip 0x%x revision 0x%x hwparams0 0x%x 1 0x%x 3 0x%x, %u usb2 / %u usb3 ports\n", ver.ip, ver.revision,
        hw0, hw1, rd(dwc, CU_DWC3_GHWPARAMS3), usb2, usb3);

    /* dwc3_core_init */
    for (unsigned i = 0; i < usb3; i++) wr(dwc, CU_DWC3_GUSB3PIPECTL + 4u * i, cu_gusb3pipectl_setup(rd(dwc, CU_DWC3_GUSB3PIPECTL + 4u * i)));
    for (unsigned i = 0; i < usb2; i++) wr(dwc, CU_DWC3_GUSB2PHYCFG + 4u * i, cu_gusb2phycfg_setup(rd(dwc, CU_DWC3_GUSB2PHYCFG + 4u * i), p));
    if (!qusb2_init() || !qmp_init()) return 0;
    if (cu_susphy_after_phy_init(&ver)) {
        for (unsigned i = 0; i < usb3; i++) wr(dwc, CU_DWC3_GUSB3PIPECTL + 4u * i, cu_u3_susphy(rd(dwc, CU_DWC3_GUSB3PIPECTL + 4u * i), 1, p));
        for (unsigned i = 0; i < usb2; i++) wr(dwc, CU_DWC3_GUSB2PHYCFG + 4u * i, cu_u2_susphy(rd(dwc, CU_DWC3_GUSB2PHYCFG + 4u * i), 1, p));
    }
    wr(dwc, CU_DWC3_DCTL, cu_dctl_soft_reset(rd(dwc, CU_DWC3_DCTL)));
    unsigned polls = cu_soft_reset_polls(&ver);
    while (rd(dwc, CU_DWC3_DCTL) & CU_DCTL_CSFTRST) {
        if (!--polls) { printf("COACHZ usb: DWC3 controller soft reset failed\n"); return 0; }
        spin_us(cu_soft_reset_poll_ms(&ver) ? 20000u : 1u);
    }
    if (cu_soft_reset_wait_50ms(&ver)) spin_us(50000);
    wr(dwc, CU_DWC3_GUID, CU_LINUX_VERSION_CODE);
    wr(dwc, CU_DWC3_GCTL, cu_gctl_setup(rd(dwc, CU_DWC3_GCTL), hw0, hw1, &ver));
    /* no suspend clock, frame length, reference period, INCR burst or bus property in the device tree */
    __builtin_memset((void *)evt, 0, 0x1000);
    wr(dwc, CU_DWC3_GEVNTADRLO, (uint32_t)EVT_PA);
    wr(dwc, CU_DWC3_GEVNTADRHI, (uint32_t)((uint64_t)EVT_PA >> 32));
    wr(dwc, CU_DWC3_GEVNTSIZ, CU_EVENT_BUFFER_BYTES & 0xffffu);
    wr(dwc, CU_DWC3_GEVNTCOUNT, rd(dwc, CU_DWC3_GEVNTCOUNT));
    wr(dwc, CU_DWC3_GUCTL2, cu_guctl2(rd(dwc, CU_DWC3_GUCTL2), &ver));
    if (cu_guctl1_applies(&ver)) wr(dwc, CU_DWC3_GUCTL1, cu_guctl1(rd(dwc, CU_DWC3_GUCTL1), &ver, p));

    /* dwc3_core_init_mode: host */
    if (cu_prtcap_needs_susphy_off(rd(dwc, CU_DWC3_GCTL), hw0)) {
        for (unsigned i = 0; i < usb3; i++) wr(dwc, CU_DWC3_GUSB3PIPECTL + 4u * i, cu_u3_susphy(rd(dwc, CU_DWC3_GUSB3PIPECTL + 4u * i), 0, p));
        for (unsigned i = 0; i < usb2; i++) wr(dwc, CU_DWC3_GUSB2PHYCFG + 4u * i, cu_u2_susphy(rd(dwc, CU_DWC3_GUSB2PHYCFG + 4u * i), 0, p));
    }
    wr(dwc, CU_DWC3_GCTL, cu_gctl_host(rd(dwc, CU_DWC3_GCTL)));
    printf("COACHZ usb: soc ready after %llu ms, GCTL 0x%x GUSB2PHYCFG 0x%x GUSB3PIPECTL 0x%x GUCTL1 0x%x\n",
        (now_us() - start) / 1000u, rd(dwc, CU_DWC3_GCTL), rd(dwc, CU_DWC3_GUSB2PHYCFG), rd(dwc, CU_DWC3_GUSB3PIPECTL),
        rd(dwc, CU_DWC3_GUCTL1));
    return 1;
}
