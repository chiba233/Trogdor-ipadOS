/* Host model of the Coachz SDHCI driver's portable layer (platform/coachz-prelink/drivers/RamBlock/mmc). Every expected
   value is worked out from the mainline Linux formula or device tree it ports, or from public card data, never from
   this code. No hardware, no kernel. */
#include <stdio.h>
#include <string.h>
#include "coachz_mmc_proto.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define GROUP(name, body) do { const int before = failures; body; if (failures == before) printf("PASS %s\n", name); } while (0)

/* Set a field of a 128-bit register in the unstuff_bits layout (resp[0] holds bits 127..96). */
static void put(uint32_t *r, unsigned start, unsigned size, uint32_t value)
{
    for (unsigned i = 0; i < size; i++) {
        const unsigned bit = start + i, word = 3u - bit / 32u, shift = bit % 32u;
        r[word] = (r[word] & ~(1u << shift)) | (((value >> i) & 1u) << shift);
    }
}

static void card_registers(void)
{
    /* The Samsung DX68MB eMMC recorded in UniXNU's gta4l device page (sysfs order: raw[0] first). */
    const uint32_t cid[4] = { 0x15010044u, 0x5836384du, 0x42009005u, 0xf4661900u };
    const uint32_t csd[4] = { 0xd0270132u, 0x0f5903ffu, 0xf6dbffefu, 0x8e404000u };
    struct cm_csd c;
    struct cm_cid id;
    CHECK(cm_mmc_decode_csd(csd, &c));
    CHECK(c.structure == 3 && c.mmca_vsn == 4);
    CHECK(c.max_dtr == 25000000u);          /* TRAN_SPEED 0x32: mmc.c's tran_mant[6] is 25, times 1 MHz / 10 */
    CHECK(c.cmdclass == 0x0f5u && c.read_blkbits == 9);
    CHECK(c.capacity == (4096ull << 9));    /* C_SIZE 0xfff, C_SIZE_MULT 7: the >2 GiB placeholder */
    CHECK(cm_mmc_decode_cid(cid, c.mmca_vsn, &id));
    CHECK(id.manfid == 0x15 && id.oemid == 0x0100 && strcmp(id.name, "DX68MB") == 0);
    CHECK(id.prv == 0 && id.serial == 0x9005f466u && id.month == 1 && id.year == 2006);   /* before the rev>=5 +16 fix-up */
    CHECK(!cm_mmc_decode_cid(cid, 1, &id));

    /* sdhci_read_rsp_136: the controller drops the CRC byte, the driver shifts it back. */
    uint32_t regs[4], back[4];
    const uint64_t hi = (uint64_t)cid[0] << 32 | cid[1], lo = (uint64_t)cid[2] << 32 | cid[3];
    const uint64_t shi = hi >> 8, slo = (lo >> 8) | (hi << 56);
    regs[0] = (uint32_t)slo; regs[1] = (uint32_t)(slo >> 32); regs[2] = (uint32_t)shi; regs[3] = (uint32_t)(shi >> 32);
    cm_rsp136(regs, back);
    CHECK(memcmp(back, cid, sizeof(cid)) == 0);   /* this CID's CRC byte reads back as its own low byte, 0x00 */

    /* An SDHC/SDXC CSD v2: C_SIZE 0xedc8 gives (C_SIZE + 1) x 1024 sectors. */
    uint32_t sd[4] = { 0, 0, 0, 0 };
    put(sd, 126, 2, 1); put(sd, 96, 8, 0x32); put(sd, 84, 12, 0x5b5); put(sd, 48, 22, 0xedc8); put(sd, 13, 1, 0);
    CHECK(cm_sd_decode_csd(sd, &c));
    CHECK(c.structure == 1 && c.blockaddr && c.capacity == (0xedc9ull << 10) && c.read_blkbits == 9);
    CHECK(c.max_dtr == 25000000u && (c.cmdclass & CM_CCC_SWITCH) && !c.readonly);
    put(sd, 126, 2, 3);
    CHECK(!cm_sd_decode_csd(sd, &c));

    /* SD CID: manfid 0x03, "SU32G", year 2000 + 0x12, month 6. */
    uint32_t sc[4] = { 0, 0, 0, 0 };
    const char *nm = "SU32G";
    put(sc, 120, 8, 0x03); put(sc, 104, 16, 0x5344);
    for (unsigned i = 0; i < 5; i++) put(sc, 96 - 8 * i, 8, (uint8_t)nm[i]);
    put(sc, 24, 32, 0x12345678u); put(sc, 12, 8, 0x12); put(sc, 8, 4, 6);
    cm_sd_decode_cid(sc, &id);
    CHECK(id.manfid == 0x03 && id.oemid == 0x5344 && strcmp(id.name, "SU32G") == 0);
    CHECK(id.serial == 0x12345678u && id.year == 2018 && id.month == 6);

    /* SCR 0x02b58003: structure 0, spec v2 with spec3, bus widths 1 and 4, CMD20 and CMD23. */
    const uint32_t scr[2] = { 0x02b58003u, 0 };
    struct cm_scr s;
    CHECK(cm_sd_decode_scr(scr, &s));
    CHECK(s.sda_vsn == 2 && s.sda_spec3 == 1 && s.sda_spec4 == 0 && s.bus_widths == 5 && s.cmds == 3);
    CHECK(s.cmds & CM_SD_SCR_CMD23);
    const uint32_t bad_scr[2] = { 0x12b58003u, 0 }, narrow[2] = { 0x02b18003u, 0 };
    CHECK(!cm_sd_decode_scr(bad_scr, &s) && !cm_sd_decode_scr(narrow, &s));
}

static void ext_csd(void)
{
    uint8_t x[512], y[512];
    memset(x, 0, sizeof(x));
    x[CM_EXT_CSD_REV] = 8;
    /* 61,071,360 sectors, the DX68MB's count in the same UniXNU record */
    x[212] = 0x00; x[213] = 0xe0; x[214] = 0xa3; x[215] = 0x03;
    x[CM_EXT_CSD_CARD_TYPE] = 0x57;
    x[CM_EXT_CSD_PART_CONFIG] = 0x48;
    x[CM_EXT_CSD_GENERIC_CMD6_TIME] = 0x19;
    x[CM_EXT_CSD_BOOT_MULT] = 0x20;
    x[CM_EXT_CSD_RPMB_MULT] = 0x20;
    struct cm_ext_csd e;
    cm_ext_csd_parse(x, &e);
    CHECK(e.rev == 8 && e.sectors == 61071360u && e.blockaddr);
    CHECK(e.generic_cmd6_ms == 250 && e.part_time_ms == 300 && e.data_sector_size == 512);
    CHECK((e.part_config & CM_EXT_CSD_PART_CONFIG_ACC_MASK) == 0 && e.boot_mult == 0x20);
    memcpy(y, x, sizeof(x));
    CHECK(cm_ext_csd_same(x, y));
    y[CM_EXT_CSD_PART_CONFIG] ^= 1;   /* not one of the compared read-only fields */
    CHECK(cm_ext_csd_same(x, y));
    y[CM_EXT_CSD_CARD_TYPE] ^= 1;
    CHECK(!cm_ext_csd_same(x, y));
    x[CM_EXT_CSD_REV] = 1;
    cm_ext_csd_parse(x, &e);
    CHECK(e.sectors == 0 && !e.blockaddr && e.generic_cmd6_ms == CM_DEFAULT_CMD6_TIMEOUT_MS && e.part_config == 0);
    CHECK(cm_mmc_hs_max_dtr(0x57, 1) == 52000000u && cm_mmc_hs_max_dtr(0x01, 1) == 26000000u && cm_mmc_hs_max_dtr(0x57, 0) == 0);
}

static void sdhci_encodings(void)
{
    /* sdhci_send_command: response type, CRC, index and data-present bits, opcode in the high byte. */
    CHECK(cm_sdhci_command(0, CM_RSP_NONE, 0) == 0x0000);
    CHECK(cm_sdhci_command(1, CM_RSP_R3, 0) == 0x0102);
    CHECK(cm_sdhci_command(2, CM_RSP_R2, 0) == 0x0209);
    CHECK(cm_sdhci_command(3, CM_RSP_R1, 0) == 0x031a);
    CHECK(cm_sdhci_command(6, CM_RSP_R1B, 0) == 0x061b);
    CHECK(cm_sdhci_command(8, CM_RSP_R7, 0) == 0x081a);
    CHECK(cm_sdhci_command(8, CM_RSP_R1, 1) == 0x083a);
    CHECK(cm_sdhci_command(17, CM_RSP_R1, 1) == 0x113a);
    CHECK(cm_sdhci_command(41, CM_RSP_R3, 0) == 0x2902);
    CHECK(cm_data_line_cmd(CM_RSP_R1B, 0) && !cm_data_line_cmd(CM_RSP_R1, 0) && cm_data_line_cmd(CM_RSP_R1, 1));
    /* sdhci_set_transfer_mode with Auto-CMD12/23 selection */
    CHECK(cm_transfer_mode(17, 1, 1, 1, 0, 0) == 0x13);
    CHECK(cm_transfer_mode(18, 8, 1, 1, 0, 1) == 0x3b);
    CHECK(cm_transfer_mode(18, 8, 1, 1, 1, 0) == 0x37);
    CHECK(cm_transfer_mode(18, 8, 1, 1, 1, 1) == 0x37);   /* CMD12 wins when both are offered */
    CHECK(cm_block_size(512) == 0x0200);
    /* sdhci_calc_timeout: (2^13 x 1000 / kHz) us doubled until it covers the target */
    CHECK(cm_timeout_count(19200, 500000, 15) == 11);
    CHECK(cm_timeout_count(19200, 0, 15) == 15 && cm_timeout_count(19200, 1ull << 40, 15) == 15);
    CHECK(cm_timeout_clk_khz(0x80u | 19) == 19000 && cm_timeout_clk_khz(19) == 19);
}

static void adma(void)
{
    uint8_t t[64];
    memset(t, 0xee, sizeof(t));
    CHECK(cm_adma_table(t, sizeof(t), 0x81102000ull, 4096) == 24);
    const uint8_t first[12] = { 0x21, 0x00, 0x00, 0x10, 0x00, 0x20, 0x10, 0x81, 0, 0, 0, 0 };
    const uint8_t end[12] = { 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(memcmp(t, first, 12) == 0 && memcmp(t + 12, end, 12) == 0);
    CHECK(cm_adma_table(t, sizeof(t), 0x81102000ull, 65536) == 24 && t[2] == 0 && t[3] == 0);   /* 65536 encodes as 0 */
    CHECK(cm_adma_table(t, sizeof(t), 0x81102000ull, 98304) == 36);
    CHECK(t[2] == 0x00 && t[3] == 0x80 && t[12 + 2] == 0 && t[12 + 3] == 0 && t[12 + 6] == 0x10 && t[24] == 0x03);
    CHECK(cm_adma_table(t, sizeof(t), 0x81102002ull, 512) == 0);   /* not 4-byte aligned */
    CHECK(cm_adma_table(t, 20, 0x81102000ull, 512) == 0);          /* no room for the end entry */
}

static void clocks(void)
{
    const size_t n1 = sizeof(cm_sdcc1_freqs) / sizeof(cm_sdcc1_freqs[0]), n2 = sizeof(cm_sdcc2_freqs) / sizeof(cm_sdcc2_freqs[0]);
    const uint32_t all = 0x1fu;
    /* clk_rcg2_floor_ops */
    CHECK(cm_rcg_floor(cm_sdcc1_freqs, n1, 400000, CM_STAGE1_SOURCES)->hz == 400000);
    CHECK(cm_rcg_floor(cm_sdcc1_freqs, n1, 52000000, CM_STAGE1_SOURCES)->hz == 50000000);
    CHECK(cm_rcg_floor(cm_sdcc1_freqs, n1, 26000000, CM_STAGE1_SOURCES)->hz == 25000000);
    CHECK(cm_rcg_max(cm_sdcc1_freqs, n1, CM_STAGE1_SOURCES) == 100000000 && cm_rcg_max(cm_sdcc1_freqs, n1, all) == 384000000);
    CHECK(cm_rcg_max(cm_sdcc2_freqs, n2, CM_STAGE1_SOURCES) == 100000000 && cm_rcg_max(cm_sdcc2_freqs, n2, all) == 202000000);
    CHECK(cm_rcg_floor(cm_sdcc1_freqs, n1, 100000, all) == NULL);
    /* __clk_rcg2_configure_mnd: 400 kHz = 19.2 MHz / 12 x 1/4, dual edge, N = ~(n-m), D = ~clamp(n, m, 2(n-m)) */
    struct cm_rcg_regs r;
    CHECK(cm_rcg_regs(cm_rcg_floor(cm_sdcc1_freqs, n1, 400000, all), cm_sdcc1_parent_cfg, 8, 5, &r));
    CHECK(r.mnd && r.cfg == 0x2017 && r.m == 0x01 && r.n == 0xfc && r.d == 0xfb && r.mnd_mask == 0xff);
    CHECK(r.cfg_mask == ((7u << 8) | 0x1fu | (3u << 12) | (1u << 20)));
    /* 25 MHz = GPLL0 even 300 MHz / 6 x 1/2 */
    CHECK(cm_rcg_regs(cm_rcg_floor(cm_sdcc1_freqs, n1, 25000000, all), cm_sdcc1_parent_cfg, 8, 5, &r));
    CHECK(r.mnd && r.cfg == 0x260b && r.m == 0x01 && r.n == 0xfe && r.d == 0xfd);
    /* 50 MHz = GPLL0 even / 6, no MND; source select 6 */
    CHECK(cm_rcg_regs(cm_rcg_floor(cm_sdcc1_freqs, n1, 50000000, all), cm_sdcc1_parent_cfg, 8, 5, &r));
    CHECK(!r.mnd && r.cfg == 0x060b);
    /* sdcc2 25 MHz = GPLL0 even / 12; 202 MHz needs GPLL7 (select 3), which sdcc1's mux lacks */
    CHECK(cm_rcg_regs(cm_rcg_floor(cm_sdcc2_freqs, n2, 25000000, all), cm_sdcc2_parent_cfg, 8, 5, &r) && r.cfg == 0x0617);
    CHECK(cm_rcg_regs(cm_rcg_floor(cm_sdcc2_freqs, n2, 202000000, all), cm_sdcc2_parent_cfg, 8, 5, &r) && r.cfg == 0x0307);
    const struct cm_freq gpll7 = { 202000000, CM_SRC_GPLL7_MAIN, 7, 0, 0 };
    CHECK(!cm_rcg_regs(&gpll7, cm_sdcc1_parent_cfg, 8, 5, &r));
    /* clk_branch2_check_halt */
    CHECK(cm_branch_on(0x00000001u) && !cm_branch_on(0x80000001u) && cm_branch_on(0xa0000001u));
}

static void supplies(void)
{
    /* OCR masks of the device-tree supplies, the selected voltage, the SDHCI power value */
    CHECK(cm_vddrange_to_ocrmask(2960, 2960) == 0x00020000u && cm_vddrange_to_ocrmask(2952, 2952) == 0x00020000u);
    /* 3.3 V: the high bound prefers bit 21, the low bound (3299 mV) bit 20 */
    CHECK(cm_vddrange_to_ocrmask(1800, 1800) == 0x80u && cm_vddrange_to_ocrmask(3300, 3300) == 0x00300000u);
    CHECK(cm_select_voltage(0x00ff8080u, 0x00020000u) == 0x00020000u);
    CHECK(cm_select_voltage(0x00ff8000u, 0x00300000u) == 0x00300000u);
    CHECK(cm_select_voltage(0x00000080u, 0x00020000u) == 0);
    CHECK(cm_sdhci_vdd(17) == CM_POWER_300 && cm_sdhci_vdd(7) == CM_POWER_180 && cm_sdhci_vdd(21) == CM_POWER_330);
    int lo, hi;
    cm_ocrbit_to_vdd(17, &lo, &hi);
    CHECK(lo == 2900000 && hi == 3000000);
    /* RPMh VRM millivolts: lowest linear-range selector inside the constraints, rounded up to mV */
    const struct cm_vreg a19 = { "ldoa19", 1504000, 8000, 256, 2960000, 2960000 };
    const struct cm_vreg a12 = { "ldoa12", 1504000, 8000, 63, 1800000, 1800000 };
    const struct cm_vreg c9 = { "ldoc9", 1504000, 8000, 256, 2952000, 2952000 };
    const struct cm_vreg c6 = { "ldoc6", 1504000, 8000, 256, 1800000, 2952000 };
    CHECK(cm_vreg_mv(&a19, lo, hi) == 2960 && cm_vreg_mv(&c9, lo, hi) == 2952);
    CHECK(cm_vqmmc_mv(&a12, CM_SIGNAL_180, 17) == 1800 && cm_vqmmc_mv(&a12, CM_SIGNAL_330, 17) == 0);
    CHECK(cm_vqmmc_mv(&c6, CM_SIGNAL_330, 17) == 2904);   /* [2.9, 3.2] V clamped to <= 2.952 V: selector 175 */
    CHECK(cm_vqmmc_mv(&c6, CM_SIGNAL_180, 17) == 1800 && cm_vqmmc_mv(&c6, CM_SIGNAL_330, 0) == 0);
    CHECK(cm_vreg_supports(&a12, 1700000, 1950000) && !cm_vreg_supports(&a12, 2700000, 3600000));
    CHECK(cm_vreg_supports(&c6, 1700000, 1950000) && cm_vreg_supports(&c6, 2700000, 3600000));
    CHECK(cm_sd_max_current_ma(0x00808080u, 17) == 512 && cm_sd_max_current_ma(0x00002500u, 17) == 148);
}

static void arc(void)
{
    /* An illustrative cx.lvl aux table: retention first, then rising levels, zero padding. */
    const uint16_t aux[12] = { 16, 48, 64, 128, 192, 256, 320, 384, 416, 0, 0, 0 };
    struct cm_arc a;
    CHECK(cm_arc_map(aux, 12, &a));
    CHECK(a.level[0] == 0 && a.level[1] == 48 && a.enable_corner == 1 && a.count == 9);
    CHECK(cm_arc_corner(&a, CM_LEVEL_LOW_SVS) == 2 && cm_arc_corner(&a, CM_LEVEL_NOM) == 5);
    CHECK(cm_arc_corner(&a, CM_LEVEL_RETENTION) == 1 && cm_arc_corner(&a, 1000) == 8);
    CHECK(!cm_arc_map(aux, 17, &a));
    CHECK(cm_sdcc_opp_level(1, 100000000) == CM_LEVEL_LOW_SVS && cm_sdcc_opp_level(1, 384000000) == CM_LEVEL_NOM);
    CHECK(cm_sdcc_opp_level(0, 202000000) == CM_LEVEL_NOM && cm_sdcc_opp_level(0, 384000000) == 0);
}

static void pins(void)
{
    /* sdc1_on / sdc2_on reproduce the drive-strength values coreboot's configure_sdhci writes (0x9fff, 0x1fe4). */
    CHECK(cm_pins_apply(0, cm_sdc1_on, 4) == 0x9fffu);
    CHECK(cm_pins_apply(0, cm_sdc2_on, 3) == 0x1fe4u);
    CHECK(cm_pins_apply(0x9fffu, cm_sdc1_off, 4) == 0x9e00u && cm_pins_apply(0x1fe4u, cm_sdc2_off, 3) == 0x1e00u);
    CHECK(cm_pins_apply(0x9e00u, cm_sdc1_on, 4) == 0x9fffu);
    CHECK(cm_pin_apply(0x000001c0u, &cm_sd_cd_pin) == 0x00000003u);   /* gpio69: pull-up, 2 mA, other bits kept */
    CHECK(cm_pin_apply(0x00000204u, &cm_sd_cd_pin) == 0x00000207u);
}

static void power_irq(void)
{
    struct cm_pwr_request r = cm_msm_pwr_decode(CM_PWRCTL_BUS_ON, 0, 1);
    CHECK(r.pwr_state == CM_REQ_BUS_ON && r.io_level == CM_REQ_IO_HIGH && !r.set_vqmmc && !r.card_gone);
    CHECK(cm_msm_pwr_ack(&r, 1, 1) == 0x5 && cm_msm_pwr_ack(&r, 0, 1) == 0x6);
    r = cm_msm_pwr_decode(CM_PWRCTL_BUS_OFF | 0xf0u, 0, 1);
    CHECK(r.status == CM_PWRCTL_BUS_OFF && r.pwr_state == CM_REQ_BUS_OFF && r.io_level == CM_REQ_IO_LOW);
    r = cm_msm_pwr_decode(CM_PWRCTL_IO_LOW, 1, 1);
    CHECK(!r.pwr_state && r.io_level == CM_REQ_IO_LOW && r.set_vqmmc);
    CHECK(cm_msm_pwr_ack(&r, 1, 1) == 0x4 && cm_msm_pwr_ack(&r, 1, 0) == 0xc);
    r = cm_msm_pwr_decode(CM_PWRCTL_BUS_ON | CM_PWRCTL_IO_LOW, 0, 1);
    CHECK(r.pwr_state == CM_REQ_BUS_ON && r.io_level == CM_REQ_IO_LOW && !r.set_vqmmc);
    r = cm_msm_pwr_decode(CM_PWRCTL_BUS_ON, 1, 0);
    CHECK(r.card_gone && cm_msm_pwr_ack(&r, 1, 1) == CM_PWRCTL_BUS_FAIL);
    /* __sdhci_msm_check_write */
    CHECK(cm_msm_write_request(CM_SDHCI_POWER_CONTROL, 0x0d, 0) == CM_REQ_BUS_ON);
    CHECK(cm_msm_write_request(CM_SDHCI_POWER_CONTROL, 0, 0x0c) == CM_REQ_BUS_OFF);
    CHECK(cm_msm_write_request(CM_SDHCI_HOST_CONTROL2, CM_CTRL2_VDD_180, 0) == CM_REQ_IO_LOW);
    CHECK(cm_msm_write_request(CM_SDHCI_HOST_CONTROL2, 0, 0) == CM_REQ_IO_HIGH);
    CHECK(cm_msm_write_request(CM_SDHCI_SOFTWARE_RESET, CM_RESET_ALL, 0x0c) == CM_REQ_BUS_OFF);
    CHECK(cm_msm_write_request(CM_SDHCI_SOFTWARE_RESET, CM_RESET_ALL, 0) == 0);
    CHECK(cm_msm_write_request(CM_SDHCI_SOFTWARE_RESET, CM_RESET_CMD, 0x0c) == 0);
    /* sdhci_msm_check_power_status */
    CHECK(!cm_msm_should_wait(CM_REQ_IO_HIGH, 0, 0, 0));
    CHECK(!cm_msm_should_wait(CM_REQ_BUS_ON, 0x0c, CM_REQ_BUS_ON, CM_REQ_IO_HIGH));
    CHECK(cm_msm_should_wait(CM_REQ_IO_LOW, 0x0c, CM_REQ_BUS_ON, CM_REQ_IO_HIGH));
    CHECK(cm_msm_should_wait(CM_REQ_BUS_ON, 0x0c, CM_REQ_BUS_OFF, CM_REQ_IO_LOW));
    /* IO pad switch: 1.8 V-only vqmmc keeps the 1.8 V pads; a 3 V capable one drops it for IO high */
    CHECK(cm_msm_io_pad_spec(0, CM_CORE_1_8V_SUPPORT, CM_REQ_IO_HIGH) == CM_MSM_IO_PAD_PWR_SWITCH);
    CHECK(cm_msm_io_pad_spec(CM_MSM_IO_PAD_PWR_SWITCH, CM_CORE_VOLT_SUPPORT, CM_REQ_IO_HIGH) == 0);
    CHECK(cm_msm_io_pad_spec(0, CM_CORE_VOLT_SUPPORT, CM_REQ_IO_LOW) == CM_MSM_IO_PAD_PWR_SWITCH);
    CHECK(cm_msm_io_pad_spec(0x1234, 0, CM_REQ_IO_LOW) == 0x1234);
    CHECK(cm_msm_regulator_caps_spec(CM_MSM_VENDOR_SPEC_POR, CM_CORE_VOLT_SUPPORT, 0) ==
          (CM_MSM_VENDOR_SPEC_POR | CM_MSM_IO_PAD_PWR_SWITCH_EN | CM_MSM_IO_PAD_PWR_SWITCH));
    /* core version gates */
    CHECK(cm_msm_needs_caps0(0x10000071u) && !cm_msm_needs_caps0(0x10000011u) && !cm_msm_needs_caps0(0x00000071u));
    CHECK(!cm_msm_uses_cdclp533(0x10000071u) && cm_msm_uses_cdclp533(0x10000033u));
}

static void arguments(void)
{
    CHECK(cm_mmc_switch_arg(CM_EXT_CSD_HS_TIMING, CM_EXT_CSD_TIMING_HS) == 0x03b90101u);
    CHECK(cm_mmc_switch_arg(CM_EXT_CSD_BUS_WIDTH, CM_EXT_CSD_BUS_WIDTH_8) == 0x03b70201u);
    CHECK(cm_sd_switch_arg(0, 0, 0) == 0x00fffff0u && cm_sd_switch_arg(1, 0, 1) == 0x80fffff1u);
    CHECK(cm_sd_if_cond_arg(0x00020000u) == 0x1aau && cm_sd_if_cond_arg(0x80u) == 0xaau);
    CHECK(cm_unstuff((const uint32_t[4]){ 0x80000000u, 0, 0, 1 }, 127, 1) == 1);
    CHECK(cm_unstuff((const uint32_t[4]){ 0, 0, 0x00000001u, 0x80000000u }, 31, 2) == 3);   /* spans two words */
}

int main(void)
{
    GROUP("card registers", card_registers());
    GROUP("ext_csd", ext_csd());
    GROUP("sdhci encodings", sdhci_encodings());
    GROUP("adma2 table", adma());
    GROUP("gcc rcg and branch", clocks());
    GROUP("ocr and rpmh supplies", supplies());
    GROUP("cx arc corners", arc());
    GROUP("sdc pin states", pins());
    GROUP("msm power interrupt", power_irq());
    GROUP("command arguments", arguments());
    return failures != 0;
}
