/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Portable half of the Coachz SDHCI driver (sdhc_1 eMMC, sdhc_2 microSD): register and command encodings,
 * card-register decoding and every decision the mainline Linux drivers make that does not touch hardware,
 * so the host test can run it. The values and the order come from Linux 7.4-rc1 (6628e7d4c6b2), listed in
 * SOURCES.md; the code is our own. No MMIO, no allocation, no libc beyond stdint/stddef.
 */
#ifndef COACHZ_MMC_PROTO_H
#define COACHZ_MMC_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- SDHCI (drivers/mmc/host/sdhci.h) */
#define CM_SDHCI_DMA_ADDRESS     0x00u /* also ARGUMENT2 */
#define CM_SDHCI_BLOCK_SIZE      0x04u
#define CM_SDHCI_BLOCK_COUNT     0x06u
#define CM_SDHCI_ARGUMENT        0x08u
#define CM_SDHCI_TRANSFER_MODE   0x0cu
#define CM_SDHCI_COMMAND         0x0eu
#define CM_SDHCI_RESPONSE        0x10u
#define CM_SDHCI_PRESENT_STATE   0x24u
#define CM_SDHCI_HOST_CONTROL    0x28u
#define CM_SDHCI_POWER_CONTROL   0x29u
#define CM_SDHCI_CLOCK_CONTROL   0x2cu
#define CM_SDHCI_TIMEOUT_CONTROL 0x2eu
#define CM_SDHCI_SOFTWARE_RESET  0x2fu
#define CM_SDHCI_INT_STATUS      0x30u
#define CM_SDHCI_INT_ENABLE      0x34u
#define CM_SDHCI_SIGNAL_ENABLE   0x38u
#define CM_SDHCI_AUTO_CMD_STATUS 0x3cu
#define CM_SDHCI_HOST_CONTROL2   0x3eu
#define CM_SDHCI_CAPABILITIES    0x40u
#define CM_SDHCI_CAPABILITIES_1  0x44u
#define CM_SDHCI_MAX_CURRENT     0x48u
#define CM_SDHCI_ADMA_ERROR      0x54u
#define CM_SDHCI_ADMA_ADDRESS    0x58u
#define CM_SDHCI_ADMA_ADDRESS_HI 0x5cu
#define CM_SDHCI_HOST_VERSION    0xfeu

#define CM_TRNS_DMA         0x01u
#define CM_TRNS_BLK_CNT_EN  0x02u
#define CM_TRNS_AUTO_CMD12  0x04u
#define CM_TRNS_AUTO_CMD23  0x08u
#define CM_TRNS_READ        0x10u
#define CM_TRNS_MULTI       0x20u

#define CM_CMD_RESP_NONE       0x00u
#define CM_CMD_RESP_LONG       0x01u
#define CM_CMD_RESP_SHORT      0x02u
#define CM_CMD_RESP_SHORT_BUSY 0x03u
#define CM_CMD_CRC             0x08u
#define CM_CMD_INDEX           0x10u
#define CM_CMD_DATA            0x20u

#define CM_PS_CMD_INHIBIT  0x00000001u
#define CM_PS_DATA_INHIBIT 0x00000002u

#define CM_CTRL_4BITBUS  0x02u
#define CM_CTRL_HISPD    0x04u
#define CM_CTRL_DMA_MASK 0x18u
#define CM_CTRL_ADMA32   0x10u
#define CM_CTRL_ADMA64   0x18u
#define CM_CTRL_8BITBUS  0x20u

#define CM_POWER_ON  0x01u
#define CM_POWER_180 0x0au
#define CM_POWER_300 0x0cu
#define CM_POWER_330 0x0eu

#define CM_CLOCK_INT_EN     0x0001u
#define CM_CLOCK_INT_STABLE 0x0002u
#define CM_CLOCK_CARD_EN    0x0004u

#define CM_RESET_ALL  0x01u
#define CM_RESET_CMD  0x02u
#define CM_RESET_DATA 0x04u

#define CM_INT_RESPONSE      0x00000001u
#define CM_INT_DATA_END      0x00000002u
#define CM_INT_DMA_END       0x00000008u
#define CM_INT_SPACE_AVAIL   0x00000010u
#define CM_INT_DATA_AVAIL    0x00000020u
#define CM_INT_CARD_INT      0x00000100u
#define CM_INT_ERROR         0x00008000u
#define CM_INT_TIMEOUT       0x00010000u
#define CM_INT_CRC           0x00020000u
#define CM_INT_END_BIT       0x00040000u
#define CM_INT_INDEX         0x00080000u
#define CM_INT_DATA_TIMEOUT  0x00100000u
#define CM_INT_DATA_CRC      0x00200000u
#define CM_INT_DATA_END_BIT  0x00400000u
#define CM_INT_BUS_POWER     0x00800000u
#define CM_INT_AUTO_CMD_ERR  0x01000000u
#define CM_INT_ADMA_ERROR    0x02000000u
#define CM_INT_TUNING_ERROR  0x04000000u
/* sdhci_set_default_irqs, without RETUNE: the stage-1 host advertises no tuning mode. */
#define CM_INT_DEFAULT (CM_INT_BUS_POWER | CM_INT_DATA_END_BIT | CM_INT_DATA_CRC | CM_INT_DATA_TIMEOUT | CM_INT_INDEX | \
                        CM_INT_END_BIT | CM_INT_CRC | CM_INT_TIMEOUT | CM_INT_DATA_END | CM_INT_RESPONSE)
#define CM_INT_CMD_ERRORS  (CM_INT_TIMEOUT | CM_INT_CRC | CM_INT_END_BIT | CM_INT_INDEX)
#define CM_INT_DATA_ERRORS (CM_INT_DATA_TIMEOUT | CM_INT_DATA_CRC | CM_INT_DATA_END_BIT | CM_INT_ADMA_ERROR)

#define CM_AUTO_CMD_TIMEOUT 0x0002u

#define CM_CTRL2_UHS_MASK 0x0007u
#define CM_CTRL2_UHS_SDR104 0x0003u
#define CM_CTRL2_UHS_SDR12 0x0000u
#define CM_CTRL2_UHS_SDR25 0x0001u
#define CM_CTRL2_VDD_180  0x0008u
#define CM_CTRL2_DRV_TYPE_MASK 0x0030u
#define CM_CTRL2_PRESET_VAL_ENABLE 0x8000u

#define CM_CAP_TIMEOUT_CLK_MASK 0x0000003fu
#define CM_CAP_TIMEOUT_CLK_UNIT 0x00000080u
#define CM_CAP_CAN_DO_8BIT  0x00040000u
#define CM_CAP_CAN_DO_ADMA2 0x00080000u
#define CM_CAP_CAN_DO_HISPD 0x00200000u
#define CM_CAP_CAN_DO_SDMA  0x00400000u
#define CM_CAP_CAN_VDD_330  0x01000000u
#define CM_CAP_CAN_VDD_300  0x02000000u
#define CM_CAP_CAN_VDD_180  0x04000000u
#define CM_CAP_CAN_64BIT    0x10000000u

#define CM_MAX_CURRENT_300_SHIFT 8
#define CM_MAX_CURRENT_MULTIPLIER 4u

#define CM_SPEC_200 1u
#define CM_SPEC_300 2u

/* ADMA2 (sdhci.h): attributes, alignment, the 64-bit descriptor size outside v4 mode, the largest length. */
#define CM_ADMA2_TRAN_VALID    0x21u
#define CM_ADMA2_NOP_END_VALID 0x03u
#define CM_ADMA2_64_DESC_SZ    12u
#define CM_ADMA2_ALIGN         4u
#define CM_ADMA2_MAX_LEN       65536u

/* ---------------------------------------------------------------- sdhci-msm v5 (offsets from the HC base) */
#define CM_MSM_DLL_CONFIG       0x200u
#define CM_MSM_DLL_STATUS       0x208u
#define CM_MSM_DDR_200_CFG      0x224u
#define CM_MSM_DLL_CONFIG_2     0x254u
#define CM_MSM_DLL_CONFIG_3     0x258u
#define CM_MSM_DDR_CONFIG       0x25cu
#define CM_MSM_DLL_USR_CTL      0x388u
/* sdhci_msm's DLL bits, v5 (sdhci-msm.c). */
#define CM_MSM_CLK_PWRSAVE         (1u << 1)   /* VENDOR_SPEC */
#define CM_MSM_DLL_LOCK            (1u << 7)   /* DLL_STATUS */
#define CM_MSM_DDR_DLL_LOCK        (1u << 11)
#define CM_MSM_DLL_EN              (1u << 16)  /* DLL_CONFIG */
#define CM_MSM_CK_OUT_EN           (1u << 18)
#define CM_MSM_CMUX_SHIFT_PHASE_SHIFT 24u
#define CM_MSM_CMUX_SHIFT_PHASE_MASK  (7u << 24)
#define CM_MSM_DDR_CAL_EN          (1u << 0)   /* DLL_CONFIG_2 */
#define CM_MSM_FLL_CYCLE_CNT       (1u << 18)
#define CM_MSM_DLL_CLOCK_DISABLE   (1u << 21)
#define CM_MSM_CMDIN_RCLK_EN       (1u << 1)   /* DDR_200_CFG */
#define CM_MSM_DDR_CONFIG_POR      0x80040873u
#define CM_MSM_DLL_USR_CTL_VALUE   (0x10800u | (1u << 26) | (1u << 27) | (1u << 29))   /* POR, lock status, fine tune, bias ok */
#define CM_MSM_DLL_CONFIG_3_HIGH   0x10u       /* clk_rate from 150 MHz */
#define CM_MSM_DLL_CONFIG_3_LOW    0x08u
#define CM_MSM_HC_MCLK_SEL_HS400   (3u << 8)
#define CM_MSM_HC_SELECT_IN_HS400  (6u << 19)
#define CM_MSM_VENDOR_SPEC      0x20cu
#define CM_MSM_VENDOR_CAPS0     0x21cu
#define CM_MSM_PWRCTL_STATUS    0x240u
#define CM_MSM_PWRCTL_MASK      0x244u
#define CM_MSM_PWRCTL_CLEAR     0x248u
#define CM_MSM_PWRCTL_CTL       0x24cu
#define CM_MSM_VENDOR_SPEC3     0x250u
#define CM_MSM_MCI_VERSION      0x318u

#define CM_MSM_VENDOR_SPEC_POR     0x00000a9cu
#define CM_MSM_HC_MCLK_SEL_DFLT    (2u << 8)
#define CM_MSM_HC_MCLK_SEL_MASK    (3u << 8)
#define CM_MSM_IO_PAD_PWR_SWITCH_EN (1u << 15)
#define CM_MSM_IO_PAD_PWR_SWITCH   (1u << 16)
#define CM_MSM_HC_SELECT_IN_EN     (1u << 18)
#define CM_MSM_HC_SELECT_IN_MASK   (7u << 19)
#define CM_MSM_PWRSAVE_DLL         (1u << 3)  /* VENDOR_SPEC3 */
#define CM_MSM_DLL_PDN             (1u << 29)
#define CM_MSM_DLL_RST             (1u << 30)
#define CM_MSM_VERSION_MAJOR(v)    (((v) >> 28) & 0xfu)
#define CM_MSM_VERSION_MINOR(v)    ((v) & 0xffu)

/* PWRCTL status bits and the matching acknowledgements, and the driver's own request types. */
#define CM_PWRCTL_BUS_OFF     (1u << 0)
#define CM_PWRCTL_BUS_ON      (1u << 1)
#define CM_PWRCTL_IO_LOW      (1u << 2)
#define CM_PWRCTL_IO_HIGH     (1u << 3)
#define CM_PWRCTL_BUS_SUCCESS (1u << 0)
#define CM_PWRCTL_BUS_FAIL    (1u << 1)
#define CM_PWRCTL_IO_SUCCESS  (1u << 2)
#define CM_PWRCTL_IO_FAIL     (1u << 3)
#define CM_PWRCTL_INT_MASK    0xfu
#define CM_REQ_BUS_OFF (1u << 0)
#define CM_REQ_BUS_ON  (1u << 1)
#define CM_REQ_IO_LOW  (1u << 2)
#define CM_REQ_IO_HIGH (1u << 3)
/* msm_host->caps_0: what the vqmmc regulator can supply. */
#define CM_CORE_3_0V_SUPPORT (1u << 25)
#define CM_CORE_1_8V_SUPPORT (1u << 26)
#define CM_CORE_VOLT_SUPPORT (CM_CORE_3_0V_SUPPORT | CM_CORE_1_8V_SUPPORT)

#define CM_MSM_MIN_CLOCK 400000u
#define CM_MSM_CORE_FREQ_100MHZ 100000000u
#define CM_MSM_PWR_IRQ_TIMEOUT_MS 5000u
#define CM_MSM_MAX_TIMEOUT_COUNT 0xfu

/* ---------------------------------------------------------------- MMC/SD commands (include/linux/mmc) */
#define CM_RSP_PRESENT (1u << 0)
#define CM_RSP_136     (1u << 1)
#define CM_RSP_CRC     (1u << 2)
#define CM_RSP_BUSY    (1u << 3)
#define CM_RSP_OPCODE  (1u << 4)
#define CM_RSP_NONE 0u
#define CM_RSP_R1  (CM_RSP_PRESENT | CM_RSP_CRC | CM_RSP_OPCODE)
#define CM_RSP_R1B (CM_RSP_PRESENT | CM_RSP_CRC | CM_RSP_OPCODE | CM_RSP_BUSY)
#define CM_RSP_R2  (CM_RSP_PRESENT | CM_RSP_136 | CM_RSP_CRC)
#define CM_RSP_R3  (CM_RSP_PRESENT)
#define CM_RSP_R6  (CM_RSP_PRESENT | CM_RSP_CRC | CM_RSP_OPCODE)
#define CM_RSP_R7  (CM_RSP_PRESENT | CM_RSP_CRC | CM_RSP_OPCODE)

#define CM_MMC_GO_IDLE_STATE       0u
#define CM_MMC_SEND_OP_COND        1u
#define CM_MMC_ALL_SEND_CID        2u
#define CM_MMC_SET_RELATIVE_ADDR   3u
#define CM_MMC_SWITCH              6u
#define CM_MMC_SELECT_CARD         7u
#define CM_MMC_SEND_EXT_CSD        8u
#define CM_MMC_SEND_CSD            9u
#define CM_MMC_STOP_TRANSMISSION  12u
#define CM_MMC_SEND_STATUS        13u
#define CM_MMC_READ_SINGLE_BLOCK  17u
#define CM_MMC_READ_MULTIPLE_BLOCK 18u
#define CM_MMC_WRITE_BLOCK 24u
#define CM_MMC_WRITE_MULTIPLE_BLOCK 25u
#define CM_MMC_SET_BLOCK_COUNT    23u
#define CM_MMC_APP_CMD            55u
#define CM_SD_SEND_RELATIVE_ADDR   3u
#define CM_SD_SWITCH               6u
#define CM_SD_APP_SET_BUS_WIDTH    6u
#define CM_SD_SEND_IF_COND         8u
#define CM_SD_APP_OP_COND         41u
#define CM_SD_APP_SEND_SCR        51u

#define CM_MMC_CARD_BUSY   0x80000000u
#define CM_SD_OCR_S18R     (1u << 24)
#define CM_SD_OCR_2T       (1u << 27)
#define CM_SD_OCR_XPC      (1u << 28)
#define CM_SD_OCR_CCS      (1u << 30)
#define CM_R1_APP_CMD      (1u << 5)
#define CM_R1_SWITCH_ERROR (1u << 7)
#define CM_R1_READY_FOR_DATA (1u << 8)
#define CM_R1_STATE(x)     (((x) >> 9) & 0xfu)
#define CM_R1_STATE_TRAN   4u
/* R1_STATUS (only warned about after CMD6, where R1_SWITCH_ERROR alone fails) and block.c's CMD_ERRORS for reads:
   OUT_OF_RANGE, ADDRESS_ERROR, BLOCK_LEN_ERROR, WP_VIOLATION, CARD_ECC_FAILED, CC_ERROR, ERROR. */
#define CM_R1_STATUS_MASK 0xfff9a000u
#define CM_R1_CMD_ERRORS  0xe4380000u

#define CM_MMC_VDD_165_195 0x00000080u
#define CM_MMC_VDD_29_30   0x00020000u
#define CM_MMC_VDD_35_36_BIT 23

#define CM_EXT_CSD_PARTITION_SUPPORT 160u
#define CM_EXT_CSD_RPMB_MULT       168u
#define CM_EXT_CSD_PART_CONFIG     179u
#define CM_EXT_CSD_ERASED_MEM_CONT 181u
#define CM_EXT_CSD_BUS_WIDTH       183u
#define CM_EXT_CSD_STROBE_SUPPORT  184u
#define CM_EXT_CSD_HS_TIMING       185u
#define CM_EXT_CSD_REV             192u
#define CM_EXT_CSD_STRUCTURE       194u
#define CM_EXT_CSD_CARD_TYPE       196u
#define CM_EXT_CSD_PART_SWITCH_TIME 199u
#define CM_EXT_CSD_SEC_CNT         212u
#define CM_EXT_CSD_BOOT_MULT       226u
#define CM_EXT_CSD_GENERIC_CMD6_TIME 248u
#define CM_EXT_CSD_DATA_SECTOR_SIZE 61u
#define CM_EXT_CSD_CMD_SET_NORMAL  1u
#define CM_EXT_CSD_BUS_WIDTH_1 0u
#define CM_EXT_CSD_BUS_WIDTH_4 1u
#define CM_EXT_CSD_BUS_WIDTH_8 2u
#define CM_EXT_CSD_TIMING_HS   1u
#define CM_EXT_CSD_PART_CONFIG_ACC_MASK 0x7u
#define CM_EXT_CSD_CARD_TYPE_HS400_1_8V (1u << 6)
#define CM_EXT_CSD_DDR_BUS_WIDTH_8 6u
#define CM_EXT_CSD_BUS_WIDTH_STROBE (1u << 7)
#define CM_EXT_CSD_TIMING_HS400    3u
#define CM_MMC_HS200_MAX_DTR       200000000u
#define CM_EXT_CSD_CARD_TYPE_HS_26 (1u << 0)
#define CM_EXT_CSD_CARD_TYPE_HS_52 (1u << 1)
#define CM_MMC_SWITCH_MODE_WRITE_BYTE 3u
#define CM_DEFAULT_CMD6_TIMEOUT_MS 500u
#define CM_MMC_MIN_PART_SWITCH_TIME 300u
#define CM_MMC_HIGH_26_MAX_DTR 26000000u
#define CM_MMC_HIGH_52_MAX_DTR 52000000u
#define CM_SD_HIGH_SPEED_MAX_DTR 50000000u
#define CM_SD_HIGH_SPEED_BUS_SPEED 1u
#define CM_SD_BUS_WIDTH_4 2u
#define CM_SD_SCR_BUS_WIDTH_1 (1u << 0)
#define CM_SD_SCR_BUS_WIDTH_4 (1u << 2)
#define CM_SD_SCR_CMD23 (1u << 1)  /* scr.cmds bit 1: SET_BLOCK_COUNT */
#define CM_CCC_SWITCH (1u << 10)

#define CM_MMC_CMD_RETRIES 3u
#define CM_MMC_OP_COND_PERIOD_US 4000u
#define CM_MMC_OP_COND_TIMEOUT_MS 1000u
#define CM_SD_APP_OP_COND_PERIOD_US 10000u
#define CM_SD_APP_OP_COND_TIMEOUT_MS 2000u

/* host->ios (include/linux/mmc/host.h) */
enum { CM_BUS_WIDTH_1 = 0, CM_BUS_WIDTH_4 = 2, CM_BUS_WIDTH_8 = 3 };
enum { CM_TIMING_LEGACY = 0, CM_TIMING_MMC_HS = 1, CM_TIMING_SD_HS = 2, CM_TIMING_MMC_HS400 = 3 };
enum { CM_SIGNAL_330 = 0, CM_SIGNAL_180 = 1 };

/* ---------------------------------------------------------------- bit fields of card registers */
/* unstuff_bits (drivers/mmc/core/mmc_ops.h): resp[0] holds bits 127..96. */
static inline uint32_t cm_unstuff(const uint32_t *resp, unsigned start, unsigned size)
{
    const uint32_t mask = (size < 32 ? 1u << size : 0u) - 1u;
    const unsigned off = 3u - start / 32u, shift = start & 31u;
    uint32_t value = resp[off] >> shift;
    if (size + shift > 32u) value |= resp[off - 1u] << ((32u - shift) % 32u);
    return value & mask;
}

/* sdhci_read_rsp_136: regs[k] is RESPONSE + 4k; the controller strips the CRC byte, so shift left by 8. */
static inline void cm_rsp136(const uint32_t regs[4], uint32_t resp[4])
{
    for (unsigned i = 0; i < 4; i++) resp[i] = regs[3 - i];
    for (unsigned i = 0; i < 4; i++) {
        resp[i] <<= 8;
        if (i != 3) resp[i] |= resp[i + 1] >> 24;
    }
}

/* ---------------------------------------------------------------- SDHCI command and transfer encodings */
/* sdhci_send_command's flags, then SDHCI_MAKE_CMD. */
static inline uint16_t cm_sdhci_command(unsigned opcode, unsigned rsp, int data)
{
    unsigned flags;
    if (!(rsp & CM_RSP_PRESENT)) flags = CM_CMD_RESP_NONE;
    else if (rsp & CM_RSP_136) flags = CM_CMD_RESP_LONG;
    else if (rsp & CM_RSP_BUSY) flags = CM_CMD_RESP_SHORT_BUSY;
    else flags = CM_CMD_RESP_SHORT;
    if (rsp & CM_RSP_CRC) flags |= CM_CMD_CRC;
    if (rsp & CM_RSP_OPCODE) flags |= CM_CMD_INDEX;
    if (data) flags |= CM_CMD_DATA;
    return (uint16_t)(((opcode & 0xffu) << 8) | (flags & 0xffu));
}

/* sdhci_data_line_cmd */
static inline int cm_data_line_cmd(unsigned rsp, int data) { return data || (rsp & CM_RSP_BUSY); }

/* sdhci_set_transfer_mode with sdhci_auto_cmd_select (no v4 mode, no SUPPORT_SINGLE quirk). */
static inline uint16_t cm_transfer_mode(unsigned opcode, unsigned blocks, int read, int dma, int auto12, int auto23)
{
    uint16_t mode = CM_TRNS_BLK_CNT_EN;
    if (opcode == CM_MMC_READ_MULTIPLE_BLOCK || blocks > 1) {
        mode = CM_TRNS_BLK_CNT_EN | CM_TRNS_MULTI;
        if (auto12) mode |= CM_TRNS_AUTO_CMD12;
        else if (auto23) mode |= CM_TRNS_AUTO_CMD23;
    }
    if (read) mode |= CM_TRNS_READ;
    if (dma) mode |= CM_TRNS_DMA;
    return mode;
}

/* SDHCI_MAKE_BLKSZ with sdhci-msm's sdma_boundary of 0. */
static inline uint16_t cm_block_size(unsigned blksz) { return (uint16_t)(blksz & 0xfffu); }

/*
 * sdhci_adma_table_pre for one 4-byte aligned segment, 12-byte (64-bit, non-v4) descriptors, then the
 * terminating NOP/END/VALID entry. Returns the bytes written, or 0 when the table or the alignment does not fit.
 */
static inline size_t cm_adma_table(uint8_t *table, size_t room, uint64_t addr, uint32_t len)
{
    size_t at = 0;
    if (!len || (addr & (CM_ADMA2_ALIGN - 1u))) return 0;
    while (len) {
        uint32_t n = len;
        if (len > CM_ADMA2_MAX_LEN) n = 32u * 1024u;   /* while (len > max_adma) write 32 KiB entries */
        if (at + CM_ADMA2_64_DESC_SZ > room) return 0;
        const uint16_t cmd = CM_ADMA2_TRAN_VALID, l16 = (uint16_t)n;   /* 65536 encodes as 0 */
        table[at + 0] = (uint8_t)cmd; table[at + 1] = (uint8_t)(cmd >> 8);
        table[at + 2] = (uint8_t)l16; table[at + 3] = (uint8_t)(l16 >> 8);
        for (unsigned i = 0; i < 8; i++) table[at + 4 + i] = (uint8_t)(addr >> (8 * i));
        at += CM_ADMA2_64_DESC_SZ;
        addr += n;
        len -= n;
    }
    if (at + CM_ADMA2_64_DESC_SZ > room) return 0;
    for (unsigned i = 0; i < CM_ADMA2_64_DESC_SZ; i++) table[at + i] = 0;
    table[at] = CM_ADMA2_NOP_END_VALID;
    return at + CM_ADMA2_64_DESC_SZ;
}

/* sdhci_calc_timeout: the count for a target in microseconds; the maximum when nothing is specified. */
static inline unsigned cm_timeout_count(unsigned timeout_clk_khz, uint64_t target_us, unsigned max_count)
{
    if (!target_us || !timeout_clk_khz) return max_count;
    unsigned count = 0;
    uint64_t current = (1ull << 13) * 1000ull / timeout_clk_khz;
    while (current < target_us) {
        count++;
        current <<= 1;
        if (count > max_count) return max_count;
    }
    return count;
}

/* The timeout clock in kHz from CAPABILITIES (sdhci_setup_host). */
static inline unsigned cm_timeout_clk_khz(uint32_t caps)
{
    unsigned clk = caps & CM_CAP_TIMEOUT_CLK_MASK;
    return (caps & CM_CAP_TIMEOUT_CLK_UNIT) ? clk * 1000u : clk;
}

/* ---------------------------------------------------------------- MSM vendor decisions */
/* The vendor capability write in sdhci_msm_probe: VDD_300 and 8-bit on cores that do not advertise them. */
static inline int cm_msm_needs_caps0(uint32_t mci_version)
{
    const unsigned major = CM_MSM_VERSION_MAJOR(mci_version), minor = CM_MSM_VERSION_MINOR(mci_version);
    return major >= 1 && minor != 0x11 && minor != 0x12;
}
static inline int cm_msm_uses_cdclp533(uint32_t mci_version)
{
    return CM_MSM_VERSION_MAJOR(mci_version) == 1 && CM_MSM_VERSION_MINOR(mci_version) < 0x34;
}

/* sdhci_msm_set_regulator_caps: VENDOR_SPEC with the pad switch enabled and set for the IO level. */
static inline uint32_t cm_msm_regulator_caps_spec(uint32_t spec, uint32_t caps0, uint32_t io_level)
{
    if (!(caps0 & CM_CORE_VOLT_SUPPORT)) return spec;
    spec |= CM_MSM_IO_PAD_PWR_SWITCH_EN;
    if ((io_level & CM_REQ_IO_HIGH) && (caps0 & CM_CORE_3_0V_SUPPORT)) spec &= ~CM_MSM_IO_PAD_PWR_SWITCH;
    else if ((io_level & CM_REQ_IO_LOW) || (caps0 & CM_CORE_1_8V_SUPPORT)) spec |= CM_MSM_IO_PAD_PWR_SWITCH;
    return spec;
}

/* The tail of sdhci_msm_handle_pwr_irq: the pad switch after an acknowledged change. */
static inline uint32_t cm_msm_io_pad_spec(uint32_t spec, uint32_t caps0, uint32_t io_level)
{
    if (!(caps0 & CM_CORE_VOLT_SUPPORT)) return spec;
    if ((io_level & CM_REQ_IO_HIGH) && (caps0 & CM_CORE_3_0V_SUPPORT)) spec &= ~CM_MSM_IO_PAD_PWR_SWITCH;
    else if ((io_level & CM_REQ_IO_LOW) || (caps0 & CM_CORE_1_8V_SUPPORT)) spec |= CM_MSM_IO_PAD_PWR_SWITCH;
    return spec;
}

/* What one power interrupt asks the driver to do (sdhci_msm_handle_pwr_irq, before the regulator calls). */
struct cm_pwr_request {
    uint32_t status;       /* PWRCTL_STATUS & INT_MASK */
    uint32_t pwr_state;    /* REQ_BUS_ON / REQ_BUS_OFF / 0 */
    uint32_t io_level;     /* REQ_IO_HIGH / REQ_IO_LOW / 0 */
    int card_gone;         /* BUS_ON for a registered card the card detect says is gone: acknowledge BUS_FAIL only */
    int set_vqmmc;         /* an IO change without a bus change: set vqmmc for the current signal voltage */
};
static inline struct cm_pwr_request cm_msm_pwr_decode(uint32_t status, int card_registered, int card_present)
{
    struct cm_pwr_request r = { status & CM_PWRCTL_INT_MASK, 0, 0, 0, 0 };
    if ((r.status & CM_PWRCTL_BUS_ON) && card_registered && !card_present) { r.card_gone = 1; return r; }
    if (r.status & CM_PWRCTL_BUS_ON) { r.pwr_state = CM_REQ_BUS_ON; r.io_level = CM_REQ_IO_HIGH; }
    if (r.status & CM_PWRCTL_BUS_OFF) { r.pwr_state = CM_REQ_BUS_OFF; r.io_level = CM_REQ_IO_LOW; }
    if (r.status & CM_PWRCTL_IO_LOW) r.io_level = CM_REQ_IO_LOW;
    if (r.status & CM_PWRCTL_IO_HIGH) r.io_level = CM_REQ_IO_HIGH;
    r.set_vqmmc = r.io_level && !r.pwr_state;
    return r;
}
/* The acknowledgement: bus success or failure from the supply steps, IO success unless the vqmmc change failed. */
static inline uint32_t cm_msm_pwr_ack(const struct cm_pwr_request *r, int bus_ok, int io_ok)
{
    if (r->card_gone) return CM_PWRCTL_BUS_FAIL;
    uint32_t ack = 0;
    if (r->pwr_state) ack |= bus_ok ? CM_PWRCTL_BUS_SUCCESS : CM_PWRCTL_BUS_FAIL;
    if (r->io_level) ack |= CM_PWRCTL_IO_SUCCESS;
    if (r->set_vqmmc && !io_ok) ack |= CM_PWRCTL_IO_FAIL;
    return ack;
}

/* __sdhci_msm_check_write: whether a register write will raise a power interrupt worth waiting for. */
static inline uint32_t cm_msm_write_request(unsigned reg, uint32_t value, int host_pwr)
{
    switch (reg) {
    case CM_SDHCI_HOST_CONTROL2: return (value & CM_CTRL2_VDD_180) ? CM_REQ_IO_LOW : CM_REQ_IO_HIGH;
    case CM_SDHCI_SOFTWARE_RESET: return (host_pwr && (value & CM_RESET_ALL)) ? CM_REQ_BUS_OFF : 0;
    case CM_SDHCI_POWER_CONTROL: return value ? CM_REQ_BUS_ON : CM_REQ_BUS_OFF;
    default: return 0;
    }
}
/* sdhci_msm_check_power_status's early exits: no interrupt comes, or the state is already there. */
static inline int cm_msm_should_wait(uint32_t req, int host_pwr, uint32_t curr_pwr_state, uint32_t curr_io_level)
{
    if ((req & CM_REQ_IO_HIGH) && !host_pwr) return 0;
    if ((req & curr_pwr_state) || (req & curr_io_level)) return 0;
    return 1;
}

/* sdhci_msm_set_uhs_signaling for the stage-1 timings (legacy, MMC HS, SD HS): CTRL2 UHS field. */
static inline uint16_t cm_msm_uhs_ctrl2(uint16_t ctrl2, unsigned timing)
{
    ctrl2 &= (uint16_t)~CM_CTRL2_UHS_MASK;
    if (timing == CM_TIMING_MMC_HS400) return (uint16_t)(ctrl2 | CM_CTRL2_UHS_SDR104);
    (void)timing;   /* legacy/HS map to no UHS mode in the msm switch: the field stays SDR12 (0) */
    return ctrl2;
}

/* ---------------------------------------------------------------- GCC RCG2 and branch clocks */
enum { CM_SRC_XO = 0, CM_SRC_GPLL0_MAIN, CM_SRC_GPLL0_EVEN, CM_SRC_GPLL6_MAIN, CM_SRC_GPLL7_MAIN };
struct cm_freq { uint32_t hz; uint8_t src; uint8_t pre_div; uint16_t m, n; };
#define CM_F(f, s, h2, mm, nn) { (f), (s), (uint8_t)((h2) - 1u), (mm), (nn) }   /* h2 is twice Linux's h */
/* ftbl_gcc_sdcc1_apps_clk_src and ftbl_gcc_sdcc2_apps_clk_src (gcc-sc7180.c). */
static const struct cm_freq cm_sdcc1_freqs[] = {
    CM_F(144000, CM_SRC_XO, 32, 3, 25),
    CM_F(400000, CM_SRC_XO, 24, 1, 4),
    CM_F(19200000, CM_SRC_XO, 2, 0, 0),
    CM_F(20000000, CM_SRC_GPLL0_EVEN, 10, 1, 3),
    CM_F(25000000, CM_SRC_GPLL0_EVEN, 12, 1, 2),
    CM_F(50000000, CM_SRC_GPLL0_EVEN, 12, 0, 0),
    CM_F(100000000, CM_SRC_GPLL0_EVEN, 6, 0, 0),
    CM_F(192000000, CM_SRC_GPLL6_MAIN, 4, 0, 0),
    CM_F(384000000, CM_SRC_GPLL6_MAIN, 2, 0, 0),
};
static const struct cm_freq cm_sdcc2_freqs[] = {
    CM_F(400000, CM_SRC_XO, 24, 1, 4),
    CM_F(9600000, CM_SRC_XO, 4, 0, 0),
    CM_F(19200000, CM_SRC_XO, 2, 0, 0),
    CM_F(25000000, CM_SRC_GPLL0_EVEN, 24, 0, 0),
    CM_F(50000000, CM_SRC_GPLL0_EVEN, 12, 0, 0),
    CM_F(100000000, CM_SRC_GPLL0_EVEN, 6, 0, 0),
    CM_F(202000000, CM_SRC_GPLL7_MAIN, 8, 0, 0),
};
/* gcc_parent_map_1 (sdcc1) and gcc_parent_map_5 (sdcc2): the CFG source-select value of each source in enum order
   (XO, GPLL0 main, GPLL0 even, GPLL6 main, GPLL7 main), -1 where the mux has no such input. */
static const int8_t cm_sdcc1_parent_cfg[] = { 0, 1, 6, 2, -1 };
static const int8_t cm_sdcc2_parent_cfg[] = { 0, 1, 6, -1, 3 };
/* Stage 1 drives only sources that are already running for the rest of the SoC. */
#define CM_STAGE1_SOURCES ((1u << CM_SRC_XO) | (1u << CM_SRC_GPLL0_EVEN))
#define CM_HS400_SOURCES (CM_STAGE1_SOURCES | (1u << CM_SRC_GPLL6_MAIN))

/* clk_rcg2_floor_ops (qcom_find_freq_floor): the fastest entry not above the rate, among the allowed sources. */
static inline const struct cm_freq *cm_rcg_floor(const struct cm_freq *t, size_t n, uint32_t rate, uint32_t sources)
{
    const struct cm_freq *best = NULL;
    for (size_t i = 0; i < n; i++)
        if (t[i].hz <= rate && (sources & (1u << t[i].src)) && (!best || t[i].hz > best->hz)) best = &t[i];
    return best;
}
/* The largest allowed rate (clk_round_rate(core, ULONG_MAX) within the stage's sources). */
static inline uint32_t cm_rcg_max(const struct cm_freq *t, size_t n, uint32_t sources)
{
    const struct cm_freq *f = cm_rcg_floor(t, n, 0xffffffffu, sources);
    return f ? f->hz : 0;
}

struct cm_rcg_regs { uint32_t cfg_mask, cfg; int mnd; uint32_t mnd_mask, m, n, d; };
/* __clk_rcg2_configure (parent, then MND and CFG fields): the masked register updates for one entry. */
static inline int cm_rcg_regs(const struct cm_freq *f, const int8_t *parent_cfg, unsigned mnd_width, unsigned hid_width,
                              struct cm_rcg_regs *out)
{
    if (!f || parent_cfg[f->src] < 0) return 0;
    out->mnd = mnd_width && f->n;
    out->mnd_mask = (1u << mnd_width) - 1u;
    out->m = out->n = out->d = 0;
    if (out->mnd) {
        uint32_t d = f->n, nm2 = (uint32_t)(f->n - f->m) * 2u;
        if (d < f->m) d = f->m;
        if (d > nm2) d = nm2;
        out->m = f->m & out->mnd_mask;
        out->n = ~(uint32_t)(f->n - f->m) & out->mnd_mask;
        out->d = ~d & out->mnd_mask;
    }
    out->cfg_mask = (0x7u << 8) | ((1u << hid_width) - 1u) | (0x3u << 12) | (1u << 20);
    out->cfg = ((uint32_t)parent_cfg[f->src] << 8) | f->pre_div;
    if (mnd_width && f->n && f->m != f->n) out->cfg |= 0x2u << 12;   /* dual edge */
    return 1;
}
#define CM_RCG_CMD_UPDATE (1u << 0)
#define CM_RCG_CFG 0x4u
#define CM_RCG_M   0x8u
#define CM_RCG_N   0xcu
#define CM_RCG_D   0x10u
#define CM_RCG_UPDATE_POLLS 500u
#define CM_CBCR_ENABLE  (1u << 0)
#define CM_CBCR_CLK_OFF (1u << 31)
#define CM_BRANCH_POLLS 200u
/* clk_branch2_check_halt while enabling: CLK_OFF clear, or the NOC FSM reporting ON. */
static inline int cm_branch_on(uint32_t cbcr) { return !(cbcr & CM_CBCR_CLK_OFF) || ((cbcr >> 28) & 0x7u) == 2u; }

/* ---------------------------------------------------------------- OCR and supplies */
/* mmc_vdd_to_ocrbitnum / mmc_vddrange_to_ocrmask / mmc_ocrbitnum_to_vdd (core.c, regulator.c). */
static inline int cm_vdd_to_ocrbit(int mv, int low_bits)
{
    if (mv < 1650 || mv > 3600) return -1;
    if (mv <= 1950) return 7;
    if (low_bits) mv -= 1;
    int bit = (mv - 2000) / 100 + 8;
    return bit > CM_MMC_VDD_35_36_BIT ? CM_MMC_VDD_35_36_BIT : bit;
}
static inline uint32_t cm_vddrange_to_ocrmask(int min_mv, int max_mv)
{
    if (max_mv < min_mv) return 0;
    int hi = cm_vdd_to_ocrbit(max_mv, 0), lo = cm_vdd_to_ocrbit(min_mv, 1);
    if (hi < 0 || lo < 0) return 0;
    uint32_t mask = 0;
    while (hi >= lo) mask |= 1u << hi--;
    return mask;
}
static inline void cm_ocrbit_to_vdd(int bit, int *min_uv, int *max_uv)
{
    int t = bit - 7;
    if (t == 0) { *min_uv = 1650000; *max_uv = 1950000; }
    else { *min_uv = 1900000 + t * 100000; *max_uv = *min_uv + 100000; }
}
static inline int cm_fls(uint32_t v) { int b = 0; while (v) { b++; v >>= 1; } return b; }
/* mmc_select_voltage without MMC_CAP2_FULL_PWR_CYCLE. */
static inline uint32_t cm_select_voltage(uint32_t ocr, uint32_t ocr_avail)
{
    ocr &= ~0x7fu;
    ocr &= ocr_avail;
    if (!ocr) return 0;
    const int bit = cm_fls(ocr) - 1;
    return ocr & (3u << (bit - 1));
}
/* sdhci_get_vdd_value */
static inline uint8_t cm_sdhci_vdd(int bit)
{
    switch (1u << bit) {
    case 0x00000080u: case 0x00000100u: return CM_POWER_180;   /* 1.65-1.95, 2.0-2.1 */
    case 0x00020000u: case 0x00040000u: return CM_POWER_300;   /* 2.9-3.0, 3.0-3.1 */
    case 0x00100000u: case 0x00200000u: case 0x00400000u: case 0x00800000u: return CM_POWER_330;
    default: return 0;
    }
}

/* An RPMh VRM LDO: its linear range (qcom-rpmh-regulator.c) and its device-tree constraints. */
struct cm_vreg {
    const char *resource;     /* cmd-db name */
    uint32_t range_min_uv, step_uv, n_voltages;
    uint32_t dt_min_uv, dt_max_uv;
};
#define CM_VRM_VOLTAGE 0x0u
#define CM_VRM_ENABLE  0x4u
#define CM_VRM_MODE    0x8u
#define CM_PMIC5_LDO_MODE_HPM 7u
/* regulator_set_voltage on a linear-range regulator: clamp to the constraints, take the lowest selector in range;
   the VRM command carries millivolts rounded up. 0 when the request cannot be met. */
static inline uint32_t cm_vreg_mv(const struct cm_vreg *v, int min_uv, int max_uv)
{
    if (min_uv < (int)v->dt_min_uv) min_uv = (int)v->dt_min_uv;
    if (max_uv > (int)v->dt_max_uv) max_uv = (int)v->dt_max_uv;
    if (min_uv > max_uv) return 0;
    uint32_t sel = 0;
    if ((uint32_t)min_uv > v->range_min_uv) sel = ((uint32_t)min_uv - v->range_min_uv + v->step_uv - 1u) / v->step_uv;
    if (sel >= v->n_voltages) return 0;
    const uint32_t uv = v->range_min_uv + sel * v->step_uv;
    if (uv > (uint32_t)max_uv) return 0;
    return (uv + 999u) / 1000u;
}
/* regulator_is_supported_voltage within the constraints */
static inline int cm_vreg_supports(const struct cm_vreg *v, int min_uv, int max_uv) { return cm_vreg_mv(v, min_uv, max_uv) != 0; }
/* mmc_regulator_set_vqmmc: the millivolts for a signal voltage, given the vmmc OCR bit. */
static inline uint32_t cm_vqmmc_mv(const struct cm_vreg *v, int signal, int vdd_bit)
{
    if (signal == CM_SIGNAL_180) return cm_vreg_supports(v, 1700000, 1950000) ? cm_vreg_mv(v, 1800000, 1950000) : 0;
    if (!vdd_bit) return 0;   /* mmc_ocrbitnum_to_vdd(0) is -EINVAL */
    int volt, max_uv;
    cm_ocrbit_to_vdd(vdd_bit, &volt, &max_uv);
    int min_uv = volt - 300000 > 2700000 ? volt - 300000 : 2700000;
    max_uv = max_uv + 200000 < 3600000 ? max_uv + 200000 : 3600000;
    /* mmc_regulator_set_voltage_if_supported(min, target, max), then (2.7 V, target, 3.6 V); each tries
       regulator_set_voltage(target, max) before (min, max). */
    if (cm_vreg_supports(v, min_uv, max_uv)) {
        uint32_t mv = cm_vreg_mv(v, volt, max_uv);
        return mv ? mv : cm_vreg_mv(v, min_uv, max_uv);
    }
    if (cm_vreg_supports(v, 2700000, 3600000)) {
        uint32_t mv = cm_vreg_mv(v, volt, 3600000);
        return mv ? mv : cm_vreg_mv(v, 2700000, 3600000);
    }
    return 0;
}

/* rpmhpd: the ARC level table from cmd-db aux data and the corner for a requested level. */
#define CM_ARC_MAX_LEVELS 16u
#define CM_LEVEL_RETENTION 16u
#define CM_LEVEL_LOW_SVS 64u
#define CM_LEVEL_NOM 256u
struct cm_arc { uint16_t level[CM_ARC_MAX_LEVELS]; unsigned count, enable_corner; };
/* rpmhpd_update_level_mapping: aux is level_count u16 values (bytes / 2). */
static inline int cm_arc_map(const uint16_t *aux, unsigned count, struct cm_arc *a)
{
    if (count > CM_ARC_MAX_LEVELS) return 0;
    for (unsigned i = 0; i < CM_ARC_MAX_LEVELS; i++) a->level[i] = 0;
    a->count = count;
    a->enable_corner = 0;
    for (unsigned i = 0; i < count; i++) {
        if (aux[i] == CM_LEVEL_RETENTION) continue;
        a->level[i] = aux[i];
        if (!a->level[a->enable_corner] && a->level[i]) a->enable_corner = i;
        if (i > 0 && a->level[i] == 0) { a->count = i; break; }
    }
    return 1;
}
/* rpmhpd_set_performance_state for an enabled domain. */
static inline unsigned cm_arc_corner(const struct cm_arc *a, unsigned level)
{
    unsigned i;
    for (i = 0; i < a->count; i++) if (level <= a->level[i]) break;
    if (i == a->count) i--;
    if (i < a->enable_corner) i = a->enable_corner;
    return i;
}
/* The SDCC OPP tables (sc7180.dtsi): the performance level a core clock needs. */
static inline unsigned cm_sdcc_opp_level(int sdcc1, uint32_t hz)
{
    if (hz <= 100000000u) return CM_LEVEL_LOW_SVS;
    return (sdcc1 ? hz <= 384000000u : hz <= 202000000u) ? CM_LEVEL_NOM : 0;
}

/* ---------------------------------------------------------------- TLMM pin configuration */
/* msm_config_group_set for one field: read-modify-write of pull (2 bits) or drive strength (3 bits). */
#define CM_PULL_NONE 0u
#define CM_PULL_DOWN 1u
#define CM_PULL_UP   3u
struct cm_pin { uint8_t pull_bit, drv_bit; uint8_t pull; uint8_t drive_ma; /* 0: leave the drive alone */ };
static inline uint32_t cm_pin_apply(uint32_t value, const struct cm_pin *p)
{
    value = (value & ~(3u << p->pull_bit)) | ((uint32_t)p->pull << p->pull_bit);
    if (p->drive_ma) value = (value & ~(7u << p->drv_bit)) | ((uint32_t)(p->drive_ma / 2u - 1u) << p->drv_bit);
    return value;
}
/* sc7180-trogdor.dtsi sdc1_on/off and sdc2_on/off on the SDC1/SDC2 HDRV_PULL_CTL registers (pinctrl-sc7180.c bits). */
static const struct cm_pin cm_sdc1_on[] = { {13, 6, CM_PULL_NONE, 16}, {11, 3, CM_PULL_UP, 16}, {9, 0, CM_PULL_UP, 16},
                                            {15, 0, CM_PULL_DOWN, 0} };
static const struct cm_pin cm_sdc1_off[] = { {13, 6, CM_PULL_NONE, 2}, {11, 3, CM_PULL_UP, 2}, {9, 0, CM_PULL_UP, 2},
                                             {15, 0, CM_PULL_DOWN, 0} };
static const struct cm_pin cm_sdc2_on[] = { {14, 6, CM_PULL_NONE, 16}, {11, 3, CM_PULL_UP, 10}, {9, 0, CM_PULL_UP, 10} };
static const struct cm_pin cm_sdc2_off[] = { {14, 6, CM_PULL_NONE, 2}, {11, 3, CM_PULL_UP, 2}, {9, 0, CM_PULL_UP, 2} };
/* gpio69 in both sdc2 states: a normal TLMM GPIO (pull bits 1:0, drive bits 8:6), pull-up, 2 mA. */
static const struct cm_pin cm_sd_cd_pin = { 0, 6, CM_PULL_UP, 2 };
static inline uint32_t cm_pins_apply(uint32_t value, const struct cm_pin *p, size_t n)
{
    for (size_t i = 0; i < n; i++) value = cm_pin_apply(value, &p[i]);
    return value;
}

/* ---------------------------------------------------------------- card registers */
struct cm_csd {
    unsigned structure, mmca_vsn, cmdclass, read_blkbits, dsr_imp;
    uint32_t max_dtr;
    uint64_t capacity;   /* in units of 1 << read_blkbits (MMC) or 512-byte sectors (SD v2/v3) */
    int blockaddr, readonly;
};
static const uint32_t cm_tran_exp[] = { 10000, 100000, 1000000, 10000000, 0, 0, 0, 0 };
static const uint8_t cm_tran_mant[] = { 0, 10, 12, 13, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80 };
/* mmc_decode_csd (mmc.c) */
static inline int cm_mmc_decode_csd(const uint32_t r[4], struct cm_csd *c)
{
    c->structure = cm_unstuff(r, 126, 2);
    if (c->structure == 0) return 0;
    c->mmca_vsn = cm_unstuff(r, 122, 4);
    c->max_dtr = cm_tran_exp[cm_unstuff(r, 96, 3)] * cm_tran_mant[cm_unstuff(r, 99, 4)];
    c->cmdclass = cm_unstuff(r, 84, 12);
    const unsigned e = cm_unstuff(r, 47, 3), m = cm_unstuff(r, 62, 12);
    c->capacity = (uint64_t)(1u + m) << (e + 2u);
    c->read_blkbits = cm_unstuff(r, 80, 4);
    c->dsr_imp = cm_unstuff(r, 76, 1);
    c->blockaddr = 0;
    c->readonly = 0;
    return 1;
}
/* mmc_decode_csd (sd.c) */
static inline int cm_sd_decode_csd(const uint32_t r[4], struct cm_csd *c)
{
    c->structure = cm_unstuff(r, 126, 2);
    c->mmca_vsn = 0;
    c->max_dtr = cm_tran_exp[cm_unstuff(r, 96, 3)] * cm_tran_mant[cm_unstuff(r, 99, 4)];
    c->cmdclass = cm_unstuff(r, 84, 12);
    c->dsr_imp = 0;
    c->readonly = (int)cm_unstuff(r, 13, 1);
    if (c->structure == 0) {
        const unsigned e = cm_unstuff(r, 47, 3), m = cm_unstuff(r, 62, 12);
        c->capacity = (uint64_t)(1u + m) << (e + 2u);
        c->read_blkbits = cm_unstuff(r, 80, 4);
        c->dsr_imp = cm_unstuff(r, 76, 1);
        c->blockaddr = 0;
        return 1;
    }
    if (c->structure == 1 || c->structure == 2) {
        const uint32_t m = c->structure == 1 ? cm_unstuff(r, 48, 22) : cm_unstuff(r, 48, 28);
        c->capacity = (uint64_t)(1u + m) << 10;
        c->read_blkbits = 9;
        c->blockaddr = 1;
        return 1;
    }
    return 0;
}

struct cm_cid { unsigned manfid, oemid, prv, month, year; uint32_t serial; char name[8]; };
/* mmc_decode_cid (mmc.c) for MMC v2..v4 CID layouts; product names keep 6 characters, trailing blanks trimmed. */
static inline int cm_mmc_decode_cid(const uint32_t r[4], unsigned mmca_vsn, struct cm_cid *c)
{
    if (mmca_vsn < 2 || mmca_vsn > 4) return 0;   /* v1.x layouts are not on SC7180 boards */
    c->manfid = cm_unstuff(r, 120, 8);
    c->oemid = cm_unstuff(r, 104, 16);
    for (unsigned i = 0; i < 6; i++) c->name[i] = (char)cm_unstuff(r, 96 - 8 * i, 8);
    c->name[6] = c->name[7] = 0;
    for (int i = 5; i >= 0 && (c->name[i] == ' ' || c->name[i] == 0); i--) c->name[i] = 0;
    c->prv = cm_unstuff(r, 48, 8);
    c->serial = cm_unstuff(r, 16, 32);
    c->month = cm_unstuff(r, 12, 4);
    c->year = cm_unstuff(r, 8, 4) + 1997u;
    return 1;
}

/* mmc_decode_cid (sd.c): the SD CID layout, five product characters, year from 2000. */
static inline void cm_sd_decode_cid(const uint32_t r[4], struct cm_cid *c)
{
    c->manfid = cm_unstuff(r, 120, 8);
    c->oemid = cm_unstuff(r, 104, 16);
    for (unsigned i = 0; i < 5; i++) c->name[i] = (char)cm_unstuff(r, 96 - 8 * i, 8);
    c->name[5] = c->name[6] = c->name[7] = 0;
    c->prv = cm_unstuff(r, 56, 8);   /* hwrev:fwrev */
    c->serial = cm_unstuff(r, 24, 32);
    c->year = cm_unstuff(r, 12, 8) + 2000u;
    c->month = cm_unstuff(r, 8, 4);
}

struct cm_ext_csd {
    unsigned rev, card_type, part_config, strobe_support, boot_mult, rpmb_mult, data_sector_size;
    uint32_t sectors, generic_cmd6_ms, part_time_ms;
    int blockaddr;
};
/* The fields of mmc_decode_ext_csd that stage 1 uses. */
static inline void cm_ext_csd_parse(const uint8_t *x, struct cm_ext_csd *e)
{
    e->rev = x[CM_EXT_CSD_REV];
    e->sectors = 0;
    e->blockaddr = 0;
    if (e->rev >= 2) {
        e->sectors = (uint32_t)x[CM_EXT_CSD_SEC_CNT] | (uint32_t)x[CM_EXT_CSD_SEC_CNT + 1] << 8 |
                     (uint32_t)x[CM_EXT_CSD_SEC_CNT + 2] << 16 | (uint32_t)x[CM_EXT_CSD_SEC_CNT + 3] << 24;
        if (e->sectors > (2u * 1024u * 1024u * 1024u) / 512u) e->blockaddr = 1;
    }
    e->strobe_support = x[CM_EXT_CSD_STROBE_SUPPORT];
    e->card_type = x[CM_EXT_CSD_CARD_TYPE];
    e->boot_mult = x[CM_EXT_CSD_BOOT_MULT];
    e->rpmb_mult = x[CM_EXT_CSD_RPMB_MULT];
    e->part_config = 0;
    e->part_time_ms = 0;
    if (e->rev >= 3) {
        e->part_config = x[CM_EXT_CSD_PART_CONFIG];
        e->part_time_ms = 10u * x[CM_EXT_CSD_PART_SWITCH_TIME];
    }
    e->generic_cmd6_ms = CM_DEFAULT_CMD6_TIMEOUT_MS;
    e->data_sector_size = 512;
    if (e->rev >= 6) {
        e->generic_cmd6_ms = 10u * x[CM_EXT_CSD_GENERIC_CMD6_TIME];
        e->data_sector_size = x[CM_EXT_CSD_DATA_SECTOR_SIZE] == 1 ? 4096u : 512u;
    }
    if (!e->part_time_ms) e->part_time_ms = e->generic_cmd6_ms;
    if (e->part_time_ms < CM_MMC_MIN_PART_SWITCH_TIME) e->part_time_ms = CM_MMC_MIN_PART_SWITCH_TIME;
}
/* mmc_compare_ext_csds: the read-only fields that must survive a bus-width change. */
static const uint8_t cm_ext_csd_compare[] = { 160, 181, 192, 194, 196, 217, 221, 223, 224, 229, 230, 231, 232,
                                              212, 213, 214, 215, 200, 201, 202, 203, 236, 237, 238, 239, 253 };
static inline int cm_ext_csd_same(const uint8_t *a, const uint8_t *b)
{
    for (size_t i = 0; i < sizeof(cm_ext_csd_compare); i++)
        if (a[cm_ext_csd_compare[i]] != b[cm_ext_csd_compare[i]]) return 0;
    return 1;
}
/* mmc_select_card_type for a host with only MMC_CAP_MMC_HIGHSPEED: the HS rate the card allows, 0 for none. */
static inline uint32_t cm_mmc_hs_max_dtr(unsigned card_type, int host_hispd)
{
    if (!host_hispd) return 0;
    if (card_type & CM_EXT_CSD_CARD_TYPE_HS_52) return CM_MMC_HIGH_52_MAX_DTR;
    if (card_type & CM_EXT_CSD_CARD_TYPE_HS_26) return CM_MMC_HIGH_26_MAX_DTR;
    return 0;
}

struct cm_scr { unsigned sda_vsn, bus_widths, sda_spec3, sda_spec4, sda_specx, cmds; };
/* mmc_decode_scr: raw[0..1] are the two big-endian words already converted. */
static inline int cm_sd_decode_scr(const uint32_t raw[2], struct cm_scr *s)
{
    const uint32_t r[4] = { 0, 0, raw[0], raw[1] };
    if (cm_unstuff(r, 60, 4) != 0) return 0;
    s->sda_vsn = cm_unstuff(r, 56, 4);
    s->bus_widths = cm_unstuff(r, 48, 4);
    s->sda_spec3 = s->sda_vsn == 2 ? cm_unstuff(r, 47, 1) : 0;
    s->sda_spec4 = s->sda_specx = 0;
    if (s->sda_spec3) { s->sda_spec4 = cm_unstuff(r, 42, 1); s->sda_specx = cm_unstuff(r, 38, 4); }
    s->cmds = s->sda_spec4 ? cm_unstuff(r, 32, 4) : s->sda_spec3 ? cm_unstuff(r, 32, 2) : 0;
    return (s->bus_widths & CM_SD_SCR_BUS_WIDTH_1) && (s->bus_widths & CM_SD_SCR_BUS_WIDTH_4);
}

/* Argument builders: __mmc_switch and mmc_sd_switch. */
static inline uint32_t cm_mmc_switch_arg(unsigned index, unsigned value)
{
    return (CM_MMC_SWITCH_MODE_WRITE_BYTE << 24) | ((index & 0xffu) << 16) | ((value & 0xffu) << 8) | CM_EXT_CSD_CMD_SET_NORMAL;
}
static inline uint32_t cm_sd_switch_arg(int set, unsigned group, unsigned value)
{
    uint32_t arg = ((uint32_t)(set ? 1u : 0u) << 31) | 0x00ffffffu;
    arg &= ~(0xfu << (group * 4u));
    return arg | ((value & 0xfu) << (group * 4u));
}
/* __mmc_send_if_cond's argument */
static inline uint32_t cm_sd_if_cond_arg(uint32_t ocr) { return (uint32_t)((ocr & 0xff8000u) != 0) << 8 | 0xaau; }
/* sd_get_host_max_current for the selected vdd (mA). */
static inline uint32_t cm_sd_max_current_ma(uint32_t max_current_reg, int vdd_bit)
{
    uint32_t field;
    switch (1u << vdd_bit) {
    case 0x00000080u: field = (max_current_reg >> 16) & 0xffu; break;
    case 0x00020000u: case 0x00040000u: field = (max_current_reg >> 8) & 0xffu; break;
    case 0x00100000u: case 0x00200000u: field = max_current_reg & 0xffu; break;
    default: return 0;
    }
    return field * CM_MAX_CURRENT_MULTIPLIER;
}

#endif /* COACHZ_MMC_PROTO_H */
