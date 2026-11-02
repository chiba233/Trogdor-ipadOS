/* Host model of the pending Coachz usb_1 bring-up's portable layer (platform/coachz-prelink/pending/usb). Expected
   values are worked out from the mainline rules and device-tree values the code ports. No hardware, no kernel. */
#include <stdio.h>
#include <string.h>
#include "coachz_usb_proto.h"
#include "coachz_qmp_tables.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define GROUP(name, body) do { const int before = failures; body; if (failures == before) printf("PASS %s\n", name); } while (0)
#define N(a) (sizeof(a) / sizeof((a)[0]))

static void gcc(void)
{
    /* F(200000000, P_GPLL0_OUT_MAIN, 3, 0, 0): source 1 of gcc_parent_map_0, pre_div 2*3-1 = 5; XO 19.2 MHz: 0, 1 */
    CHECK(CU_MASTER_CFG == 0x105 && CU_MOCK_UTMI_CFG == 0x001);
    CHECK(cu_gdsc_init(0) == 0x00282000u && cu_gdsc_init(0x00f0f007u) == 0x00282001u && cu_gdsc_init(0x80000000u) == 0x80282000u);
    CHECK(cu_branch_on(1) && !cu_branch_on(0x80000001u) && cu_branch_on(0xa0000001u));
    CHECK(cu_hub_gpio_cfg(0x0000003du) == 0x00000200u && cu_hub_gpio_cfg(0x000001c0u) == 0x00000200u);
}

static void dwc3_versions(void)
{
    struct cu_dwc3 d;
    CHECK(cu_dwc3_identify(0x5533310au, 0, &d) && d.ip == CU_IP_DWC3 && d.revision == 0x5533310au);
    CHECK(cu_dwc3_identify(0x33310000u, 0x3139302au, &d) && d.ip == CU_IP_DWC31 && d.revision == 0x3139302au);
    CHECK(!cu_dwc3_identify(0x12340000u, 0, &d));
    CHECK(CU_DWC31_REV('1', '9', '0') == 0x3139302au);
    const struct cu_dwc3 v310 = { CU_IP_DWC3, 0x5533310au }, v320 = { CU_IP_DWC3, 0x5533320au }, v194 = { CU_IP_DWC3, 0x5533194au },
        v220 = { CU_IP_DWC3, 0x5533220au }, v185 = { CU_IP_DWC3, 0x5533185au }, v31 = { CU_IP_DWC31, 0x3139302au };
    CHECK(cu_susphy_after_phy_init(&v310) && !cu_susphy_after_phy_init(&v194) && cu_susphy_after_phy_init(&v31));
    CHECK(cu_guctl2(0, &v310) == CU_GUCTL2_RST_ACTBITLATER && cu_guctl2(CU_GUCTL2_LC_TIMER, &v320) == CU_GUCTL2_RST_ACTBITLATER);
    CHECK(cu_guctl2(0, &v31) == 0);
    CHECK(cu_guctl1_applies(&v310) && cu_guctl1(0x04000000u, &v310, &cu_trogdor_props) == 0x81020000u);
    CHECK(cu_soft_reset_polls(&v310) == 1000 && cu_soft_reset_polls(&v31) == 10 && !cu_soft_reset_wait_50ms(&v310));
    /* global control: clock-gating option off on 3.10a, kept with SOF/ITP sync on 2.10a-2.50a, hibernation, U2RSTECN */
    CHECK(cu_gctl_setup(0x00002039u, 2, 1u << 24, &v310) == 0x00002000u);
    CHECK(cu_gctl_setup(0x00002000u, 2, 1u << 24, &v220) == 0x00002401u);
    CHECK(cu_gctl_setup(0x00002000u, 2, 2u << 24, &v310) == 0x00002002u);
    CHECK(cu_gctl_setup(0, 2, 0, &v185) == CU_GCTL_U2RSTECN);
}

static void dwc3_registers(void)
{
    const struct cu_props *p = &cu_trogdor_props;
    CHECK(cu_gusb3pipectl_setup(0x0a0e0002u) == 0x020c0002u);
    CHECK(cu_gusb2phycfg_setup(0x00001548u, p) == 0x00001408u);
    /* dis_u2_susphy_quirk: only the USB3 pipe gets SUSPHY */
    CHECK(cu_u3_susphy(0, 1, p) == CU_GUSB3PIPECTL_SUSPHY && cu_u2_susphy(0, 1, p) == 0 && cu_u2_susphy(0x40, 0, p) == 0);
    CHECK(cu_dctl_soft_reset(0x800001e0u) == 0x40000000u);
    CHECK(cu_prtcap_needs_susphy_off(0x00002000u, 2) && !cu_prtcap_needs_susphy_off(0x00001000u, 2) && !cu_prtcap_needs_susphy_off(0x2000u, 1));
    CHECK(cu_gctl_host(0x00002000u) == 0x00001000u && cu_gctl_host(0x00003005u) == 0x00001005u);
}

static uint32_t fake_regs[0x500 / 4];
static uint32_t fake_read(void *ctx, uint32_t offset) { (void)ctx; return fake_regs[offset / 4]; }
static void xhci(void)
{
    memset(fake_regs, 0, sizeof(fake_regs));
    fake_regs[0x10 / 4] = 0x01000000u;            /* xECP = 0x100 dwords: 0x400 */
    fake_regs[0x400 / 4] = 0x00000401u;           /* legacy support, next +0x10 */
    fake_regs[0x410 / 4] = 0x02000402u;           /* USB 2 protocol, next +0x10 */
    fake_regs[0x418 / 4] = 0x00000101u;           /* one port from 1 */
    fake_regs[0x420 / 4] = 0x03000002u;           /* USB 3 protocol, last */
    fake_regs[0x428 / 4] = 0x00000102u;           /* one port from 2 (count in bits 15:8) */
    unsigned u2, u3;
    CHECK(cu_xhci_ports(fake_read, NULL, &u2, &u3) && u2 == 1 && u3 == 1);
    fake_regs[0x10 / 4] = 0;
    CHECK(cu_xhci_ports(fake_read, NULL, &u2, &u3) && u2 == 0 && u3 == 0);
}

static void qusb2(void)
{
    uint32_t regs[0x260 / 4];
    memset(regs, 0, sizeof(regs));
    for (size_t i = 0; i < N(cu_qusb2_v2_init); i++) regs[cu_qusb2_v2_init[i].offset / 4] = cu_qusb2_v2_init[i].val;
    CHECK(N(cu_qusb2_v2_init) == 16 && regs[0x240 / 4] == 0x30 && regs[0x224 / 4] == 0x58);
    for (size_t i = 0; i < N(cu_qusb2_trogdor_overrides); i++) {
        const struct cu_rmw *r = &cu_qusb2_trogdor_overrides[i];
        regs[r->offset / 4] = cu_rmw_apply(regs[r->offset / 4], r);
    }
    CHECK(regs[CU_QUSB2_IMP_CTRL1 / 4] == 0x08 && regs[CU_QUSB2_PLL_BIAS_CONTROL_2 / 4] == 0x22);
    CHECK(regs[CU_QUSB2_CHG_CTRL2 / 4] == 0x30 && regs[CU_QUSB2_PORT_TUNE1 / 4] == 0x37 && regs[CU_QUSB2_PORT_TUNE2 / 4] == 0x28);
    CHECK(cu_hstx_trim(0x0a000000u) == 5 && cu_hstx_trim(0x01ffffffu) == 0 && cu_tune1_trim(0x37, 5) == 0x57);
    CHECK(CU_QUSB2_DISABLE_CTRL == 0x23);
}

static void qmp(void)
{
    CHECK(N(cu_v3_usb3_serdes_tbl) > 30 && N(cu_v3_usb3_tx_tbl) >= 3 && N(cu_v3_usb3_rx_tbl) >= 5 && N(cu_v3_usb3_pcs_tbl) > 20);
    const struct { const struct cu_qmp_reg *t; size_t n; uint32_t limit; } tables[] = {
        { cu_v3_usb3_serdes_tbl, N(cu_v3_usb3_serdes_tbl), 0x200 }, { cu_v3_usb3_tx_tbl, N(cu_v3_usb3_tx_tbl), 0x200 },
        { cu_v3_usb3_rx_tbl, N(cu_v3_usb3_rx_tbl), 0x200 }, { cu_v3_usb3_pcs_tbl, N(cu_v3_usb3_pcs_tbl), 0x300 },
    };
    for (size_t k = 0; k < N(tables); k++)
        for (size_t i = 0; i < tables[k].n; i++) {
            CHECK(tables[k].t[i].offset < tables[k].limit && (tables[k].t[i].offset & 3) == 0);
            CHECK(tables[k].t[i].lane_mask == 0xff || tables[k].t[i].lane_mask == 1 || tables[k].t[i].lane_mask == 2);
        }
    /* first entries of qmp_v3_usb3_tx_tbl and _serdes_tbl: HIGHZ_DRVR_EN (0x060) = 0x10, PLL_IVCO (0x048) = 0x07 */
    CHECK(cu_v3_usb3_tx_tbl[0].offset == 0x0060 && cu_v3_usb3_tx_tbl[0].val == 0x10);
    CHECK(cu_v3_usb3_serdes_tbl[0].offset == 0x0048 && cu_v3_usb3_serdes_tbl[0].val == 0x07);
    /* block bases inside the 0x3000 window, the reset override bits and the mode */
    CHECK(CU_QMP_PCS + CU_PCS_STATUS < 0x3000 && CU_RESET_OVRD_ALL == 0x3f && CU_RESET_OVRD_USB3DP == 0x0f);
    CHECK((CU_USB3_MODE | CU_DP_MODE) == 3 && CU_SW_PORTSELECT_MUX == 2);
}

static void supplies(void)
{
    CHECK(N(cu_qusb2_supplies) == 3 && strcmp(cu_qusb2_supplies[0].resource, "ldoa4") == 0 && cu_qusb2_supplies[2].mv == 2920);
    CHECK(N(cu_qmp_supplies) == 2 && strcmp(cu_qmp_supplies[1].resource, "ldoc3") == 0 && cu_qmp_supplies[1].mv == 1200);
    for (size_t i = 0; i < N(cu_qusb2_supplies); i++) CHECK(cu_qusb2_supplies[i].hpm);
    CHECK(CU_LEVEL_NOM == 256);
}

int main(void)
{
    GROUP("gcc gdsc gpio", gcc());
    GROUP("dwc3 versions", dwc3_versions());
    GROUP("dwc3 registers", dwc3_registers());
    GROUP("xhci port count", xhci());
    GROUP("qusb2 init and tuning", qusb2());
    GROUP("qmp tables", qmp());
    GROUP("supplies", supplies());
    return failures != 0;
}
