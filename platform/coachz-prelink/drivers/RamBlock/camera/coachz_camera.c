/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Coachz rear camera (/chosen/coachz-camera): the OV8856 streams its colour bar through CSIPHY1, PPI1, CSID0 RDI0 and VFE0 write
 * master 0 into low DRAM for a few seconds; frames are counted and the bars checked, then everything is powered down. With 2 the
 * CoachzCamera service (CoachzCamera.cpp) then powers the whole path up only while a client streams.
 *
 * Sources (docs/plans/camera.md):
 *   same-board ChromeOS kernel 3f8e1538 (sc7180-camera.dtsi, sc7180-trogdor-mipi-camera.dtsi): addresses, interrupts, clock
 *     rates, rails, pins and drive strengths, CCI master 1 and CSIPHY 1 for the rear sensor;
 *   ChromeOS camx driver at 44fb6321 (factory-trogdor-13443.B-chromeos-5.4): CSIPHY 1.2.2 sequence (cam_csiphy_core.c), PPI 170
 *     (cam_csid_ppi_core.c), CSID 170 (cam_ife_csid_core.c), VFE 170 top, RDI and bus ver2 (cam_vfe_*.c), 37.5 MHz CCI timing;
 *   mainline Linux: camcc-sc7180.c and clk-alpha-pll.c (PLLs, RCGs, branches, GDSCs), gcc-sc7180.c, i2c-qcom-cci.c (the CCI
 *     protocol and its v2 timing, the same values as ChromeOS), ov8856.c (power order, tables, controls), sc7180.c (interconnect),
 *     camss-csiphy-3ph-1-0.c (settle count).
 * The register tables come from those GPL files at build time (tools/camera_tables.py) and are not in this repository.
 *
 * The apps SMMU leaves unmatched streams in bypass (sCR0.USFCFG clear), so the IFE stream IDs 0x820/0x840/0x860 write physical
 * addresses; the frames go to 0x84000000 and 0x84400000, free low DRAM between the presenter's buffers and mpss_mem. The block
 * interrupts (CSID 464, IFE 465) stay disabled: their status registers are polled.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>

struct cam_sensor_reg { uint16_t addr; uint8_t val; };
enum cam_phy_type { CAM_PHY_DEFAULT_PARAMS, CAM_PHY_LANE_ENABLE, CAM_PHY_SETTLE_CNT_LOWER_BYTE, CAM_PHY_SETTLE_CNT_HIGHER_BYTE,
	CAM_PHY_DNP_PARAMS, CAM_PHY_2PH_REGS, CAM_PHY_3PH_REGS };
struct cam_phy_reg { uint16_t addr; uint8_t val; uint8_t delay_ms; enum cam_phy_type type; };
#include "coachz_camera_tables.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern struct vnode *rootvnode;
extern kern_return_t thread_terminate(thread_t);
extern int coachz_rpmh_cx_request(unsigned consumer, unsigned level);
extern int coachz_camera_bus_vote(uint64_t avg_kbps, uint64_t peak_kbps);
void coachz_camera_start(void);

/* ---- addresses (sc7180-camera.dtsi, sc7180.dtsi) ---- */
#define GCC_PA          0x00100000u
#define CAMCC_PA        0x0ad00000u
#define CAMCC_SIZE      0x10000u
#define CPAS_PA         0x0ac40000u
#define CCI0_PA         0x0ac4a000u
#define CSIPHY_PA(n)    (0x0ac65000u + 0x1000u * (n))
#define PPI_PAGE_PA     0x0ace0000u
#define PPI_OFFSET(n)   (0x200u * (n))
#define CSID0_PA        0x0acb3000u
#define VFE0_PA         0x0acaf000u
#define VFE0_SIZE       0x4000u
#define TLMM_NORTH_PA   0x03900000u
#define TLMM_SOUTH_PA   0x03d00000u
#define RING            4u
#define RING_PA(i)      (0x84000000ull + (uint64_t)((i) % RING) * 0x400000ull)   /* 16 MiB, ends at 0x85000000 */
#define FRAME_PA0       RING_PA(0)
#define FRAME_PA1       RING_PA(1)

/* ---- votes: cx consumer 6 (coachz-prelink-camera.patch), RPMH_REGULATOR_LEVEL_SVS for the svs clock rates below ---- */
#define CX_CONSUMER_CAMERA 6u
#define CX_LEVEL_SVS    128u
/* The frame traffic (1640 * 1232 * 10 / 8 bytes, 30 frames a second) as average; ChromeOS's camnoc-axi-min-ib-bw as peak. */
#define BUS_AVG_KBPS    76000u
#define BUS_PEAK_KBPS   3000000u

/* ---- GCC (gcc-sc7180.c) ---- */
#define GCC_CAMERA_AHB  0xb008u
#define GCC_CAMERA_HF_AXI 0xb020u
#define GCC_CAMERA_XO   0xb02cu

/* ---- CAMCC (camcc-sc7180.c) ---- */
#define PLL0            0x0000u
#define PLL3            0x3000u
#define PLL_MODE        0x00u
#define PLL_L           0x04u
#define PLL_USER        0x0cu
#define PLL_USER_U      0x10u
#define PLL_CONFIG      0x14u
#define PLL_CONFIG_U    0x18u
#define PLL_TEST        0x1cu
#define PLL_TEST_U      0x20u
#define PLL_OPMODE      0x2cu
#define PLL_FRAC        0x38u
#define PLL_OUTCTRL     (1u << 0)
#define PLL_RESET_N     (1u << 2)
#define PLL_VOTE_FSM    (1u << 20)
#define PLL_UPDATE_BYPASS (1u << 23)
#define PLL_LOCK_DET    (1u << 31)
#define PLL_OUT_MASK    0x7u
#define RCG_SLOW_AHB    0x6058u
#define RCG_CCI0        0xb0d8u
#define RCG_MCLK0       0x4004u
#define RCG_MCLK1       0x4024u
#define RCG_CSI0PHYTIMER 0x5004u
#define RCG_CPHY_RX     0x9064u
#define RCG_CSI1PHYTIMER 0x5028u
#define RCG_IFE0_CSID   0x903cu
#define RCG_IFE0        0x9010u
#define CBCR_SOC_AHB    0xb140u
#define CBCR_CPAS_AHB   0xb11cu
#define CBCR_CAMNOC_AXI 0xb124u
#define CBCR_CCI0       0xb0f0u
#define CBCR_CSI0PHYTIMER 0x501cu
#define CBCR_CSIPHY0    0x5020u
#define CBCR_CSIPHY1    0x5044u
#define CBCR_CSI1PHYTIMER 0x5040u
#define CBCR_IFE0       0x9028u
#define CBCR_IFE0_CSID  0x9054u
#define CBCR_IFE0_CPHY_RX 0x907cu
#define CBCR_IFE0_AXI   0x9080u
#define CBCR_MCLK0      0x401cu
#define CBCR_MCLK1      0x403cu
#define GDSC_TITAN      0xb134u
#define GDSC_IFE0       0x9004u
#define GDSC_PWR_ON     (1u << 31)
#define GDSC_SW_COLLAPSE (1u << 0)
#define CBCR_OFF        (1u << 31)
/* Parent selects (camcc parent maps) and the RCG pre-divider field (2 * divider - 1). */
#define SRC_XO          0u
#define SRC_PLL3_MAIN   5u
#define SRC_PLL0_EVEN   6u

/* ---- CCI (i2c-qcom-cci.c) ---- */
#define CCI_HW_VERSION  0x000u
#define CCI_RESET_CMD   0x004u
#define CCI_RESET_ALL   0x0f73f3f7u
#define CCI_QUEUE_START 0x008u
#define CCI_IRQ_GLOBAL_CLEAR 0xc00u
#define CCI_IRQ_MASK    0xc04u
#define CCI_IRQ_CLEAR   0xc08u
#define CCI_IRQ_STATUS  0xc0cu
#define CCI_IRQ_MASK1   0xc10u
#define CCI_IRQ_CLEAR1  0xc14u
#define CCI_IRQ_MASK0_ALL 0x7fff7ff7u   /* camx cam_cci_hwreg.h CCI_IRQ_MASK_0_RMSK */
#define CCI_IRQ_MASK1_ALL 0x00110000u   /* CCI_IRQ_MASK_1_RMSK */
#define CCI_SCL_CTL(m)  (0x100u + 0x100u * (m))
#define CCI_SDA_CTL0(m) (0x104u + 0x100u * (m))
#define CCI_SDA_CTL1(m) (0x108u + 0x100u * (m))
#define CCI_SDA_CTL2(m) (0x10cu + 0x100u * (m))
#define CCI_MISC_CTL(m) (0x110u + 0x100u * (m))
#define CCI_READ_DATA(m) (0x118u + 0x100u * (m))
#define CCI_READ_LEVEL(m) (0x11cu + 0x100u * (m))
#define CCI_Q(m, q)     (0x300u + 0x200u * (m) + 0x100u * (q))
#define CCI_EXEC_WORDS(m, q) (CCI_Q(m, q) + 0x00u)
#define CCI_CUR_WORDS(m, q)  (CCI_Q(m, q) + 0x04u)
#define CCI_LOAD(m, q)  (CCI_Q(m, q) + 0x10u)
/* camx cam_cci_dev.h enum cci_i2c_cmd_type. */
#define CMD_SET_PARAM   1u
#define CMD_LOCK        6u
#define CMD_UNLOCK      7u
#define CMD_REPORT      8u
#define CMD_REPORT_IRQ  (1u << 8)
#define CMD_WRITE       9u
#define CMD_READ        10u
#define CMD_WRITE_DISABLE_P 11u
#define CCI_RETRIES     3u             /* cam_sensor_core.c: cci_client->retries = 3, id_map 0 */
#define CCI_MAX_WRITE   11u
#define ST_RST_DONE     (1u << 24)
#define ST_DONE(m)      ((m) ? ((1u << 12) | (1u << 16) | (1u << 20)) : ((1u << 0) | (1u << 4) | (1u << 8)))
#define ST_ERR(m)       ((m) ? 0x60ee6000u : 0x18000ee6u)
#define ST_NACK(m)      ((m) ? ((1u << 29) | (1u << 30)) : ((1u << 27) | (1u << 28)))
#define CCI_MASK_ALL    (ST_DONE(0) | ST_DONE(1) | ST_RST_DONE | (1u << 25) | (1u << 26) | ST_ERR(0) | ST_ERR(1))
#define CCI_REAR        1u

/* ---- PPI 170 (cam_csid_ppi170.h, cam_csid_ppi_core.h) ---- */
#define PPI_MODULE_CFG  0x60u
#define PPI_IRQ_STATUS  0x68u
#define PPI_IRQ_MASK    0x6cu
#define PPI_IRQ_SET     0x70u
#define PPI_IRQ_CLEAR   0x74u
#define PPI_IRQ_CMD     0x78u
#define PPI_RST_CMD     0x7cu
#define PPI_FIFO_OVERFLOWS 0xeu
#define PPI_DL_EN(x)    (1u << (4 + (x)))

/* ---- CSIPHY 1.2.2 (cam_csiphy_1_2_2_hwreg.h) ---- */
#define CSIPHY_IRQ_STATUS0 0x8b0u
#define CSIPHY_IRQ_CLEAR0  0x858u
#define CSIPHY_GLBL_IRQ_CMD 0x828u
#define CSIPHY_LANE_MASK   0x1fu   /* data 0, clock, data 1, 2, 3 */
#define CSIPHY_TIMER_HZ    300000000ull

/* ---- CSID 170 (cam_ife_csid170.h) ---- */
#define CSID_HW_VERSION 0x000u
#define CSID_RST_STROBES 0x010u
#define CSID_CSI2_IRQ_STATUS 0x020u
#define CSID_CSI2_IRQ_MASK 0x024u
#define CSID_CSI2_IRQ_CLEAR 0x028u
#define CSID_IPP_IRQ_MASK 0x034u
#define CSID_IPP_IRQ_CLEAR 0x038u
#define CSID_RDI_IRQ_STATUS(n) (0x040u + 0x10u * (n))
#define CSID_RDI_IRQ_MASK(n) (0x044u + 0x10u * (n))
#define CSID_RDI_IRQ_CLEAR(n) (0x048u + 0x10u * (n))
#define CSID_TOP_IRQ_STATUS 0x070u
#define CSID_TOP_IRQ_MASK 0x074u
#define CSID_TOP_IRQ_CLEAR 0x078u
#define CSID_IRQ_CMD    0x080u
#define CSID_RX_CFG0    0x100u
#define CSID_RX_CFG1    0x104u
#define CSID_RX_CAPTURE_CTRL 0x108u
#define CSID_IPP_CFG0   0x200u
#define CSID_RDI_CFG0(n) (0x300u + 0x100u * (n))
#define CSID_RDI_CFG1(n) (0x304u + 0x100u * (n))
#define CSID_RDI_CTRL(n) (0x308u + 0x100u * (n))
#define CSID_RDI_FRM_DROP_PATTERN 0x30cu
#define CSID_RDI_FRM_DROP_PERIOD 0x310u
#define CSID_RDI_IRQ_SUB_PATTERN 0x314u
#define CSID_RDI_IRQ_SUB_PERIOD 0x318u
#define CSID_RDI_PIX_DROP_PATTERN 0x324u
#define CSID_RDI_PIX_DROP_PERIOD 0x328u
#define CSID_RDI_LINE_DROP_PATTERN 0x32cu
#define CSID_RDI_LINE_DROP_PERIOD 0x330u
#define CSID_CSI2_RST_DONE (1u << 27)
#define CSID_CSI2_ERRORS 0x05ffb800u  /* camx cam_ife_csid_core.h CSID_CSI2_RX_ERROR_*: CPHY EOT/SOT/PH CRC 11-13, lane FIFO overflows
                                       * 15-18, CRC, ECC, MMAPPED, UNMAPPED, UNDERFLOW, UNBOUNDED 19-24, TG FIFO 26. Bits 0-10 are INFO. */
#define CSID_RDI_RST_DONE (1u << 1)
#define CSID_RDI_FIFO_OVERFLOW (1u << 2)
#define CSID_RDI_EOF    (1u << 9)
#define CSID_RDI_SOF    (1u << 12)
#define CSID_RESUME_AT_FRAME_BOUNDARY 1u
#define CSID_HALT_IMMEDIATELY 2u
#define CSID_TIMESTAMP_STB_POST_IRQ 2u
#define CSID_LANE_CFG   0x3210u
#define CSID_PHY_SEL    1u             /* CAM_ISP_IFE_IN_RES_PHY_1 - 1 */
#define MIPI_DT_RAW10   0x2bu
#define CSID_DECODE_PAYLOAD 0xfu       /* MIPI RAW10 in and out: payload only, no crop */

/* ---- VFE 170 (cam_vfe170.h, cam_vfe_top_ver2.c, cam_vfe_bus_ver2.c) ---- */
#define VFE_HW_VERSION  0x0000u
#define VFE_RESET_CMD   0x0018u
#define VFE_RESET_HW_AND_REG 0x3f9fu
#define VFE_IRQ_GLOBAL_CLEAR 0x0058u
#define VFE_IRQ_MASK0   0x005cu
#define VFE_IRQ_MASK1   0x0060u
#define VFE_IRQ_CLEAR0  0x0064u
#define VFE_IRQ_CLEAR1  0x0068u
#define VFE_IRQ_STATUS0 0x006cu
#define VFE_IRQ_STATUS1 0x0070u
#define VFE_REG_UPDATE  0x04acu
#define VFE_RDI0_REG_UPDATE 0x2u
#define VFE_IRQ0_RESET  (1u << 31)
#define VFE_IRQ0_BUS    (1u << 9)
#define VFE_IRQ0_RDI0_SOF (1u << 27)
#define BUS_HW_VERSION  0x2000u
#define BUS_CGC_OVD     0x200cu
#define BUS_IRQ_MASK(n) (0x2044u + 4u * (n))
#define BUS_IRQ_CLEAR(n) (0x2050u + 4u * (n))
#define BUS_IRQ_STATUS(n) (0x205cu + 4u * (n))
#define BUS_IRQ_GLOBAL_CLEAR 0x2068u
#define BUS_ADDR_SYNC_FRAME_HDR 0x2080u
#define BUS_ADDR_NO_SYNC 0x2084u
#define BUS_TEST_CTRL   0x211cu
#define BUS_DEBUG_CFG   0x226cu
#define WM0_STATUS0     0x2200u
#define WM0_CFG         0x2208u
#define WM0_IMAGE_ADDR  0x2214u
#define WM0_WIDTH       0x221cu
#define WM0_HEIGHT      0x2220u
#define WM0_PACKER      0x2224u
#define WM0_STRIDE      0x2228u
#define WM0_IRQ_SUB_PERIOD 0x2248u
#define WM0_IRQ_SUB_PATTERN 0x224cu
#define WM0_FRMDROP_PERIOD 0x2250u
#define WM0_FRMDROP_PATTERN 0x2254u
#define WM0_FRAME_INC   0x2258u
#define WM0_BURST_LIMIT 0x225cu
#define WM_RDI_DEFAULT  0xff01u        /* CAM_VFE_RDI_BUS_DEFAULT_WIDTH / _STRIDE: frame-based RDI writes */
#define WM_EN_FRAME_BASED 0x3u

/* ---- OV8856 (ov8856.c) ---- */
#define OV8856_REG_CHIP_ID 0x300au
#define OV8856_CHIP_ID  0x00885au
#define OV8856_REG_MODE_SELECT 0x0100u
#define OV8856_REG_EXPOSURE 0x3500u
#define OV8856_REG_ANALOG_GAIN 0x3508u
#define OV8856_REG_DIGITAL_GAIN 0x350au
#define OV8856_REG_VTS  0x380eu
#define OV8856_REG_TEST_PATTERN 0x5e00u
#define OV8856_COLOUR_BAR 0x80u        /* pattern 1, "Standard Color Bar": (1 - 1) << 2 | enable */
#define OV8856_EXPOSURE_MAX_MARGIN 6u
#define OV8856_ANAL_GAIN_MIN 128u
#define OV8856_DGTL_GAIN_DEFAULT 1024u
/* MIPI RAW10 (five bytes carry four pixels), written by the frame-based RDI write master with the PLAIN128 packer, which ends each
 * line on a 16-byte word: mainline camss gives the SDM845 VFE 170 RDI (the same 0xff01 width and stride, packer 0, frame-based)
 * bytesperline = roundup(width * bpp / 8, 16) (camss-video.c video_mbus_to_pix_mp, camss-vfe.c vfe_bpl_align_rdi). */
#define LINE_BYTES      ((cam->width * 10u + 7u) / 8u)
#define FRAME_STRIDE    ((LINE_BYTES + 15u) & ~15u)
#define FRAME_BYTES     (FRAME_STRIDE * cam->height)
#define FILL            0xa5u

/* ---- TLMM (pinctrl-msm.c): pull 1:0, function 5:2, drive 8:6 ((mA / 2) - 1), output enable 9; IO output bit 1 ---- */
#define TLMM_PULL_UP    3u
#define TLMM_FUNC(f)    ((uint32_t)(f) << 2)
#define TLMM_DRIVE(ma)  ((uint32_t)((ma) / 2 - 1) << 6)
#define TLMM_OE         (1u << 9)
#define TLMM_OUT        (1u << 1)

struct cam_pin { unsigned gpio; bool north; uint32_t cfg; volatile uint8_t *base; uint32_t saved_cfg, saved_io; };
enum { PIN_RAIL, PIN_RESET, PIN_MCLK, PIN_SDA, PIN_SCL, PIN_COUNT };
/* sc7180-trogdor-mipi-camera.dtsi: cci1_active (pull-up, 2 mA), cam_sensor_mclk1_active and cam_sensor_rear_active (no pull,
 * 2 mA); GPIO7 enables pp1800_wf_cam and pp2800_wf_cam; GPIO29 is CAM_RESET1 (active low). */
static struct cam_pin cam_pins[PIN_COUNT] = {
	[PIN_RAIL] = {.gpio = 7, .north = true, .cfg = TLMM_DRIVE(2) | TLMM_OE, .base = NULL, .saved_cfg = 0, .saved_io = 0},
	[PIN_RESET] = {.gpio = 29, .north = true, .cfg = TLMM_DRIVE(2) | TLMM_OE, .base = NULL, .saved_cfg = 0, .saved_io = 0},
	[PIN_MCLK] = {.gpio = 14, .north = false, .cfg = TLMM_FUNC(1) | TLMM_DRIVE(2), .base = NULL, .saved_cfg = 0, .saved_io = 0},
	[PIN_SDA] = {.gpio = 19, .north = false, .cfg = TLMM_FUNC(1) | TLMM_DRIVE(2) | TLMM_PULL_UP, .base = NULL, .saved_cfg = 0, .saved_io = 0},
	[PIN_SCL] = {.gpio = 20, .north = false, .cfg = TLMM_FUNC(1) | TLMM_DRIVE(2) | TLMM_PULL_UP, .base = NULL, .saved_cfg = 0, .saved_io = 0},
};

/* ---- OV5675, the front sensor (ov5675.c): the same OmniVision register layout for ID, stream, exposure, gain and VTS ---- */
#define OV5675_CHIP_ID  0x005675u
#define OV5675_REG_TEST_PATTERN 0x4503u
#define OV5675_REG_FORMAT1 0x3820u
#define OV5675_REG_FORMAT2 0x373du
#define OV5675_REG_GROUP_ACCESS 0x3208u
#define OV5675_REG_MWB_GAIN(c) ((uint16_t)(0x5019u + 2u * (c)))   /* R, G, B */
#define OV5675_EXPOSURE_MAX_MARGIN 4u

/* ---- the sensor this run drives: the rear OV8856, or with /chosen/coachz-camera bit 0x800 the front OV5675, from the table in
 * docs/plans/camera.md (CCI0 master 0, CSIPHY0, MCLK0 on GPIO13, CAM_RESET0 on GPIO30, GPIO6 rails). Chosen once per boot. ---- */
struct cam_desc {
	const char *name, *where;
	unsigned cci;                   /* CCI0 master */
	uint8_t addrs[2];               /* the addresses its SID pin allows, the expected one first */
	uint32_t chip_id;
	unsigned phy;                   /* CSIPHY n, PPI n, CSID PHY select n */
	uint32_t rcg_mclk, cbcr_mclk, rcg_phytimer, cbcr_phytimer, cbcr_csiphy;
	uint32_t lane_mask;             /* CSIPHY lanes: data 0, clock, data 1, 2, 3 */
	unsigned lanes;
	uint32_t lane_cfg;              /* CSID lane_cfg: the physical lane of each logical lane, four bits each */
	uint64_t link_hz;
	uint32_t width, height, vts, vts_reg, exposure_margin, exposure_shift, test_pattern_reg;
	uint32_t rail_to_reset_us, reset_to_bus_us;
	uint32_t bayer;                 /* 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR */
};
static const struct cam_desc cam_rear = {
	.name = "OV8856", .where = "rear", .cci = CCI_REAR, .addrs = {0x10, 0x36}, .chip_id = OV8856_CHIP_ID, .phy = 1,
	.rcg_mclk = RCG_MCLK1, .cbcr_mclk = CBCR_MCLK1, .rcg_phytimer = RCG_CSI1PHYTIMER, .cbcr_phytimer = CBCR_CSI1PHYTIMER,
	.cbcr_csiphy = CBCR_CSIPHY1, .lane_mask = CSIPHY_LANE_MASK, .lanes = 4, .lane_cfg = CSID_LANE_CFG, .link_hz = OV8856_LINK_HZ,
	.width = OV8856_MODE_WIDTH, .height = OV8856_MODE_HEIGHT, .vts = OV8856_MODE_VTS, .vts_reg = OV8856_MODE_VTS,
	.exposure_margin = OV8856_EXPOSURE_MAX_MARGIN, .exposure_shift = 4, .test_pattern_reg = OV8856_REG_TEST_PATTERN,
	.rail_to_reset_us = 1000, .reset_to_bus_us = 1800, .bayer = 1,
};
/* ov5675.c: 2 lanes at 450 MHz (900 Mbit/s a lane) from a 19.2 MHz XVCLK, SBGGR10 whatever the flips; the exposure register holds
 * half the exposure in its fixed point (val << 3), and V4L2_CID_VBLANK writes VTS as height + vblank + 10; power on as
 * ov5675_power_on: clock, reset asserted, supplies, 2 ms, reset released, 1.5 ms. */
static const struct cam_desc cam_front = {
	.name = "OV5675", .where = "front", .cci = 0, .addrs = {0x36, 0x10}, .chip_id = OV5675_CHIP_ID, .phy = 0,
	.rcg_mclk = RCG_MCLK0, .cbcr_mclk = CBCR_MCLK0, .rcg_phytimer = RCG_CSI0PHYTIMER, .cbcr_phytimer = CBCR_CSI0PHYTIMER,
	.cbcr_csiphy = CBCR_CSIPHY0, .lane_mask = 0x7u, .lanes = 2, .lane_cfg = 0x10u, .link_hz = OV5675_LINK_HZ,
	.width = OV5675_MODE_WIDTH, .height = OV5675_MODE_HEIGHT, .vts = OV5675_MODE_VTS, .vts_reg = OV5675_MODE_VTS + 10u,
	.exposure_margin = OV5675_EXPOSURE_MAX_MARGIN, .exposure_shift = 3, .test_pattern_reg = OV5675_REG_TEST_PATTERN,
	.rail_to_reset_us = 2000, .reset_to_bus_us = 1500, .bayer = 3,
};
static const struct cam_desc *cam = &cam_rear;
static struct cam_pin front_pins[PIN_COUNT];
static struct cam_pin *pins = cam_pins;

/* The service side (CoachzCamera.cpp, /chosen/coachz-camera = 2): the thread keeps the camera powered after the self-test and streams
 * real images into the ring while a client asks for them. */
volatile uint32_t coachz_camera_state;    /* 0 off, 1 powered and idle, 2 streaming, 3 failed */
volatile uint32_t coachz_camera_want;     /* the client: 1 stream, 0 stop */
volatile uint32_t coachz_camera_seq;      /* frames completed into the ring */
volatile uint32_t coachz_camera_last;     /* ring index of the newest complete frame */
volatile uint32_t coachz_camera_exposure = 1250u, coachz_camera_gain = 128u, coachz_camera_settings;
uint64_t coachz_camera_frame_pa(uint32_t index);
uint32_t coachz_camera_geometry(uint32_t which);
extern void coachz_camera_publish(void);
static uint32_t cam_mode;

static volatile uint8_t *cam_gcc, *cam_cc, *cam_cpas, *cam_cci, *cam_phy, *cam_ppi, *cam_csid, *cam_vfe;
/* Two pages at the start, the middle rows and the end of each frame buffer: enough to check the bars and the frame length. */
static volatile uint8_t *cam_view[2][3];
static uint8_t cam_sensor_addr;

static uint32_t rd(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(uintptr_t)(b + o); }
static void wr(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(uintptr_t)(b + o) = v; __asm__ volatile ("dsb sy" ::: "memory"); }
static void set_bits(volatile uint8_t *b, uint32_t o, uint32_t mask, uint32_t v) { wr(b, o, (rd(b, o) & ~mask) | (v & mask)); }
static bool poll(volatile uint8_t *b, uint32_t o, uint32_t mask, uint32_t want, unsigned us)
{
	for (unsigned i = 0; i < us; i++) {
		if ((rd(b, o) & mask) == want) return true;
		delay(1);
	}
	return (rd(b, o) & mask) == want;
}

/* ---- clocks and power (clk-branch.c, clk-rcg2.c, clk-alpha-pll.c, gdsc.c) ---- */
static bool branch(volatile uint8_t *b, uint32_t cbcr, bool on, const char *name)
{
	set_bits(b, cbcr, 1u, on ? 1u : 0u);
	if (!on) return true;
	if (poll(b, cbcr, CBCR_OFF, 0, 500)) return true;
	printf("COACHZ camera: %s did not start: CBCR 0x%x\n", name, rd(b, cbcr));
	return false;
}

/* clk_rcg2 configure: CFG source at 10:8, pre-divider at 4:0, MND mode 13:12 off for the MND roots (no M/N here); CMD UPDATE. */
static bool rcg(uint32_t cmd, uint32_t src, uint32_t div2, bool mnd, const char *name)
{
	uint32_t mask = (0x7u << 8) | 0x1fu | (mnd ? (0x3u << 12) : 0u);
	set_bits(cam_cc, cmd + 4u, mask, (src << 8) | div2);
	set_bits(cam_cc, cmd, 1u, 1u);
	if (poll(cam_cc, cmd, 1u, 0, 500)) return true;
	printf("COACHZ camera: %s did not update: CMD 0x%x CFG 0x%x\n", name, rd(cam_cc, cmd), rd(cam_cc, cmd + 4u));
	return false;
}

/* clk_fabia_pll_configure (zero fields are not written) and alpha_pll_fabia_enable. */
static bool pll_on(uint32_t base, uint32_t l, uint32_t user, const char *name)
{
	uint32_t mode = rd(cam_cc, base + PLL_MODE), opmode = rd(cam_cc, base + PLL_OPMODE);
	if (mode & PLL_VOTE_FSM) { printf("COACHZ camera: %s is in FSM vote mode (0x%x)\n", name, mode); return false; }
	if ((opmode & 1u) && (mode & PLL_OUTCTRL)) return true;
	wr(cam_cc, base + PLL_L, l);
	wr(cam_cc, base + PLL_FRAC, 0x4000u);
	wr(cam_cc, base + PLL_CONFIG, 0x20485699u);
	wr(cam_cc, base + PLL_CONFIG_U, 0x00002067u);
	if (user) wr(cam_cc, base + PLL_USER, user);
	wr(cam_cc, base + PLL_USER_U, 0x00004805u);
	wr(cam_cc, base + PLL_TEST, 0x40000000u);
	set_bits(cam_cc, base + PLL_MODE, PLL_UPDATE_BYPASS, PLL_UPDATE_BYPASS);
	set_bits(cam_cc, base + PLL_MODE, PLL_RESET_N, PLL_RESET_N);
	set_bits(cam_cc, base + PLL_MODE, PLL_OUTCTRL, 0);
	wr(cam_cc, base + PLL_OPMODE, 0);
	set_bits(cam_cc, base + PLL_MODE, PLL_RESET_N, PLL_RESET_N);
	wr(cam_cc, base + PLL_OPMODE, 1);
	if (!poll(cam_cc, base + PLL_MODE, PLL_LOCK_DET, PLL_LOCK_DET, 1000)) {
		printf("COACHZ camera: %s did not lock: MODE 0x%x\n", name, rd(cam_cc, base + PLL_MODE));
		return false;
	}
	set_bits(cam_cc, base + PLL_USER, PLL_OUT_MASK, PLL_OUT_MASK);
	set_bits(cam_cc, base + PLL_MODE, PLL_OUTCTRL, PLL_OUTCTRL);
	return true;
}

/* alpha_pll_fabia_disable: output off, outputs masked, standby. */
static void pll_off(uint32_t base)
{
	set_bits(cam_cc, base + PLL_MODE, PLL_OUTCTRL, 0);
	set_bits(cam_cc, base + PLL_USER, PLL_OUT_MASK, 0);
	wr(cam_cc, base + PLL_OPMODE, 0);
}

static bool gdsc(uint32_t gdscr, bool on, const char *name)
{
	if (on) {
		uint32_t v = rd(cam_cc, gdscr);
		v = (v & ~((0xfu << 20) | (0xfu << 16) | (0xfu << 12))) | (0x2u << 20) | (0x8u << 16) | (0x2u << 12);
		wr(cam_cc, gdscr, v & ~GDSC_SW_COLLAPSE);
		if (!poll(cam_cc, gdscr, GDSC_PWR_ON, GDSC_PWR_ON, 500)) {
			printf("COACHZ camera: %s did not power on: 0x%x\n", name, rd(cam_cc, gdscr));
			return false;
		}
		delay(1);
		return true;
	}
	set_bits(cam_cc, gdscr, GDSC_SW_COLLAPSE, GDSC_SW_COLLAPSE);
	return poll(cam_cc, gdscr, GDSC_PWR_ON, 0, 500);
}

static void pin_set(struct cam_pin *p, uint32_t cfg, bool high)
{
	wr(p->base, 4, high ? TLMM_OUT : 0);
	wr(p->base, 0, cfg);
}

/* ---- CCI: ChromeOS camx cam_cci_init (reset, then the interrupt masks), its queue commands for reads and writes
 * (cam_cci_read, cam_cci_data_queue, cam_cci_transfer_end), and the standard-mode timing at 37.5 MHz. camx initialises the CCI
 * only after the sensor is powered (cam_sensor_power_up: cam_sensor_core_power_up, then camera_io_init). ---- */
static uint32_t cci_last_status;
static bool cci_reset(void)
{
	wr(cam_cci, CCI_IRQ_MASK, CCI_MASK_ALL);
	wr(cam_cci, CCI_RESET_CMD, CCI_RESET_ALL);
	wr(cam_cci, CCI_RESET_CMD, 1u);
	bool done = poll(cam_cci, CCI_IRQ_STATUS, ST_RST_DONE, ST_RST_DONE, 100000);
	wr(cam_cci, CCI_IRQ_MASK, CCI_IRQ_MASK0_ALL);
	wr(cam_cci, CCI_IRQ_CLEAR, CCI_IRQ_MASK0_ALL);
	wr(cam_cci, CCI_IRQ_MASK1, CCI_IRQ_MASK1_ALL);
	wr(cam_cci, CCI_IRQ_CLEAR1, CCI_IRQ_MASK1_ALL);
	wr(cam_cci, CCI_IRQ_GLOBAL_CLEAR, 1);
	for (unsigned m = 0; m < 2; m++) {
		wr(cam_cci, CCI_SCL_CTL(m), (201u << 16) | 174u);     /* thigh, tlow */
		wr(cam_cci, CCI_SDA_CTL0(m), (204u << 16) | 231u);    /* tsu_sto, tsu_sta */
		wr(cam_cci, CCI_SDA_CTL1(m), (22u << 16) | 162u);     /* thd_dat, thd_sta */
		wr(cam_cci, CCI_SDA_CTL2(m), 227u);                   /* tbuf */
		wr(cam_cci, CCI_MISC_CTL(m), (0u << 8) | (6u << 4) | 3u); /* scl_stretch_en, trdhld, tsp */
	}
	if (!done) printf("COACHZ camera: CCI reset not acknowledged: status 0x%x\n", rd(cam_cci, CCI_IRQ_STATUS));
	return done;
}

/* 0 done, -1 no acknowledge, -2 other error, -3 timeout; each failure resets the controller. */
static int cci_run(unsigned m, unsigned q)
{
	wr(cam_cci, CCI_EXEC_WORDS(m, q), rd(cam_cci, CCI_CUR_WORDS(m, q)));
	wr(cam_cci, CCI_QUEUE_START, 1u << (m * 2 + q));
	uint32_t st = 0;
	for (unsigned i = 0; i < 100000; i++) {
		st = rd(cam_cci, CCI_IRQ_STATUS);
		if (st & (ST_DONE(m) | ST_ERR(m))) break;
		delay(1);
	}
	cci_last_status = st;
	wr(cam_cci, CCI_IRQ_CLEAR, st);
	wr(cam_cci, CCI_IRQ_GLOBAL_CLEAR, 1);
	if (st & ST_ERR(m)) {
		(void)cci_reset();
		return (st & ST_NACK(m)) ? -1 : -2;
	}
	if (!(st & ST_DONE(m))) {
		(void)cci_reset();
		return -3;
	}
	return 0;
}

static uint32_t cci_param(uint8_t addr) { return CMD_SET_PARAM | ((uint32_t)(addr & 0x7f) << 4) | (CCI_RETRIES << 16); }

/* cam_cci_data_queue on queue 0: SET_PARAM, LOCK, WRITE with the bytes packed little-endian into words, then
 * cam_cci_transfer_end: UNLOCK, REPORT with its interrupt. */
static int cci_write(unsigned m, uint8_t addr, const uint8_t *bytes, unsigned len)
{
	uint8_t load[12] = {0};
	if (!len || len > CCI_MAX_WRITE) return -5;
	load[0] = (uint8_t)(CMD_WRITE | (len << 4));
	for (unsigned i = 0; i < len; i++) load[1 + i] = bytes[i];
	wr(cam_cci, CCI_LOAD(m, 0), cci_param(addr));
	wr(cam_cci, CCI_LOAD(m, 0), CMD_LOCK);
	for (unsigned j = 0; j < 1 + len; j += 4) {
		wr(cam_cci, CCI_LOAD(m, 0), load[j] | (uint32_t)load[j + 1] << 8 | (uint32_t)load[j + 2] << 16 | (uint32_t)load[j + 3] << 24);
	}
	wr(cam_cci, CCI_LOAD(m, 0), CMD_UNLOCK);
	wr(cam_cci, CCI_LOAD(m, 0), CMD_REPORT | CMD_REPORT_IRQ);
	return cci_run(m, 0);
}

/* cam_cci_read on queue 1: SET_PARAM, LOCK, WRITE_DISABLE_P with the 16-bit register address (no stop, so the read follows with a
 * repeated start), READ, UNLOCK; done is RD_DONE. READ_BUF_LEVEL is len / 4 + 1 words; the first word's low byte is not data. */
/* regbytes is the register address width: 2 for 16-bit register addresses, 1 for 8-bit ones (the address byte only), 0 for a plain
 * read with no address written at all. */
static int cci_read(unsigned m, uint8_t addr, uint16_t reg, unsigned regbytes, uint8_t *out, unsigned len)
{
	wr(cam_cci, CCI_LOAD(m, 1), cci_param(addr));
	wr(cam_cci, CCI_LOAD(m, 1), CMD_LOCK);
	if (regbytes == 2)
		wr(cam_cci, CCI_LOAD(m, 1), CMD_WRITE_DISABLE_P | (2u << 4) | ((uint32_t)(reg >> 8) << 8) | ((uint32_t)(reg & 0xff) << 16));
	else if (regbytes == 1)
		wr(cam_cci, CCI_LOAD(m, 1), CMD_WRITE_DISABLE_P | (1u << 4) | ((uint32_t)(reg & 0xff) << 8));
	wr(cam_cci, CCI_LOAD(m, 1), CMD_READ | (len << 4));
	wr(cam_cci, CCI_LOAD(m, 1), CMD_UNLOCK);
	int e = cci_run(m, 1);
	if (e) return e;
	unsigned words = rd(cam_cci, CCI_READ_LEVEL(m)), want = len / 4 + 1, got = 0;
	if (words != want) {
		for (unsigned i = 0; i < words; i++) (void)rd(cam_cci, CCI_READ_DATA(m));
		return -4;
	}
	for (unsigned i = 0; i < words; i++) {
		uint32_t v = rd(cam_cci, CCI_READ_DATA(m));
		for (unsigned b = i ? 0 : 1; b < 4 && got < len; b++) out[got++] = (uint8_t)(v >> (8 * b));
	}
	return 0;
}

/* ov8856_write_reg / ov8856_read_reg: a 16-bit register address and a big-endian value of 1 to 3 bytes. */
static int sensor_write(uint16_t reg, uint32_t value, unsigned bytes)
{
	uint8_t buf[5] = {(uint8_t)(reg >> 8), (uint8_t)reg, 0, 0, 0};
	for (unsigned i = 0; i < bytes; i++) buf[2 + i] = (uint8_t)(value >> (8 * (bytes - 1 - i)));
	return cci_write(cam->cci, cam_sensor_addr, buf, 2 + bytes);
}

static int sensor_read(uint8_t addr, uint16_t reg, uint32_t *value, unsigned bytes)
{
	uint8_t buf[4] = {0};
	int e = cci_read(cam->cci, addr, reg, 2, buf, bytes);
	*value = 0;
	for (unsigned i = 0; !e && i < bytes; i++) *value = (*value << 8) | buf[i];
	return e;
}

static bool sensor_list(const struct cam_sensor_reg *list, unsigned count, const char *name)
{
	for (unsigned i = 0; i < count; i++) {
		int e = sensor_write(list[i].addr, list[i].val, 1);
		if (e) {
			printf("COACHZ camera: %s entry %u (0x%04x) failed: %d\n", name, i, list[i].addr, e);
			return false;
		}
	}
	return true;
}

/* ov8856_identify_module / ov5675_identify_module (both 24 bits at 0x300a) at the two addresses the SID pin allows. */
static bool sensor_identify(void)
{
	for (unsigned i = 0; i < sizeof(cam->addrs); i++) {
		uint32_t id = 0;
		int e = sensor_read(cam->addrs[i], OV8856_REG_CHIP_ID, &id, 3);
		printf("COACHZ camera: %s address 0x%02x chip id %d 0x%06x (CCI status 0x%x)\n", cam->where, cam->addrs[i], e, id, cci_last_status);
		if (!e && id == cam->chip_id) {
			cam_sensor_addr = cam->addrs[i];
			return true;
		}
	}
	return false;
}

/* ov8856_start_streaming: PLL list, mode list, Bayer order, the control defaults (__v4l2_ctrl_handler_setup) with test pattern
 * register value `pattern` (0 off), MODE_SELECT. */
static bool sensor_stream(bool on, uint8_t pattern)
{
	if (!on) return sensor_write(OV8856_REG_MODE_SELECT, 0, 1) == 0;
	if (cam == &cam_front) {
		/* ov5675_start_streaming: the 900 Mbit/s list, the mode list, then __v4l2_ctrl_handler_setup in the controls' creation order
		 * at their defaults (VBLANK, analogue gain, digital gain through the MWB gains under a group hold, exposure, test pattern,
		 * HFLIP 0 setting 0x3820 bit 3, VFLIP 0 clearing 0x3820 bits 4-5 and 0x373d bit 1), then streaming. */
		uint32_t f1 = 0, f2 = 0;
		if (!sensor_list(ov5675_pll_900mbps, sizeof(ov5675_pll_900mbps) / sizeof(ov5675_pll_900mbps[0]), "PLL") ||
		    !sensor_list(ov5675_mode_1296x972, sizeof(ov5675_mode_1296x972) / sizeof(ov5675_mode_1296x972[0]), "mode")) {
			return false;
		}
		bool ok = sensor_write(OV8856_REG_VTS, cam->vts_reg, 2) == 0 && sensor_write(OV8856_REG_ANALOG_GAIN, OV8856_ANAL_GAIN_MIN, 2) == 0 &&
		    sensor_write(OV5675_REG_GROUP_ACCESS, 0x00u, 1) == 0;
		for (unsigned c = 0; ok && c < 3; c++) ok = sensor_write(OV5675_REG_MWB_GAIN(c), OV8856_DGTL_GAIN_DEFAULT, 2) == 0;
		ok = ok && sensor_write(OV5675_REG_GROUP_ACCESS, 0x10u, 1) == 0 && sensor_write(OV5675_REG_GROUP_ACCESS, 0xa0u, 1) == 0 &&
		    sensor_write(OV8856_REG_EXPOSURE, (cam->vts - cam->exposure_margin) << cam->exposure_shift, 3) == 0 &&
		    sensor_write(OV5675_REG_TEST_PATTERN, pattern, 1) == 0 &&
		    sensor_read(cam_sensor_addr, OV5675_REG_FORMAT1, &f1, 1) == 0 && sensor_write(OV5675_REG_FORMAT1, f1 | 0x08u, 1) == 0 &&
		    sensor_read(cam_sensor_addr, OV5675_REG_FORMAT1, &f1, 1) == 0 && sensor_write(OV5675_REG_FORMAT1, f1 & ~0x30u, 1) == 0 &&
		    sensor_read(cam_sensor_addr, OV5675_REG_FORMAT2, &f2, 1) == 0 && sensor_write(OV5675_REG_FORMAT2, f2 & ~0x02u, 1) == 0;
		return ok && sensor_write(OV8856_REG_MODE_SELECT, 1, 1) == 0;
	}
	if (!sensor_list(ov8856_pll_360mbps, sizeof(ov8856_pll_360mbps) / sizeof(ov8856_pll_360mbps[0]), "PLL") ||
	    !sensor_list(ov8856_mode_1640x1232, sizeof(ov8856_mode_1640x1232) / sizeof(ov8856_mode_1640x1232[0]), "mode") ||
	    !sensor_list(ov8856_bayer_sgrbg10, sizeof(ov8856_bayer_sgrbg10) / sizeof(ov8856_bayer_sgrbg10[0]), "Bayer order")) {
		return false;
	}
	return sensor_write(OV8856_REG_ANALOG_GAIN, OV8856_ANAL_GAIN_MIN, 2) == 0 &&
	    sensor_write(OV8856_REG_DIGITAL_GAIN, OV8856_DGTL_GAIN_DEFAULT, 2) == 0 &&
	    sensor_write(OV8856_REG_EXPOSURE, (OV8856_MODE_VTS - OV8856_EXPOSURE_MAX_MARGIN) << 4, 3) == 0 &&
	    sensor_write(OV8856_REG_VTS, OV8856_MODE_VTS, 2) == 0 &&
	    sensor_write(OV8856_REG_TEST_PATTERN, pattern, 1) == 0 &&
	    sensor_write(OV8856_REG_MODE_SELECT, 1, 1) == 0;
}

/* ---- CSIPHY (cam_csiphy_core.c: cam_csiphy_reset, cam_csiphy_config_dev for 2-phase, cam_csiphy_cphy_irq_config) ---- */
static void phy_list(const struct cam_phy_reg *list, unsigned count, uint32_t lane_enable, uint32_t settle)
{
	for (unsigned i = 0; i < count; i++) {
		const struct cam_phy_reg *r = &list[i];
		switch (r->type) {
		case CAM_PHY_LANE_ENABLE: wr(cam_phy, r->addr, lane_enable); break;
		case CAM_PHY_DEFAULT_PARAMS: case CAM_PHY_2PH_REGS: wr(cam_phy, r->addr, r->val); break;
		case CAM_PHY_SETTLE_CNT_LOWER_BYTE: wr(cam_phy, r->addr, settle & 0xffu); break;
		case CAM_PHY_SETTLE_CNT_HIGHER_BYTE: wr(cam_phy, r->addr, (settle >> 8) & 0xffu); break;
		case CAM_PHY_3PH_REGS: case CAM_PHY_DNP_PARAMS: default: continue;
		}
		if (r->delay_ms) delay(r->delay_ms * 1000);
	}
}

static uint32_t phy_lane_enable(void)
{
	/* Data lanes take two bits each, the clock lane bit 7. */
	uint32_t enable = 0, mask = 1;
	for (int i = 0; i < 4; i++, mask <<= 1) {
		if (mask == 0x2) {
			if (cam->lane_mask & mask) enable |= 0x80u;
			i--;
		} else if (cam->lane_mask & mask) {
			enable |= 1u << (i << 1);
		}
	}
	return enable;
}

/* csiphy_settle_cnt_calc: t_hs_settle = 85 ns + 6 UI (the maximum T_HS-PREPARE), in timer periods, less 6. */
static uint32_t phy_settle_count(void)
{
	uint64_t ui = 1000000000000ull / cam->link_hz / 2, timer = 1000000000000ull / CSIPHY_TIMER_HZ;
	return (uint32_t)((85000ull + 6ull * ui) / timer - 6ull);
}

static void phy_start(void)
{
	uint32_t enable = phy_lane_enable(), settle = phy_settle_count();
	phy_list(csiphy_reset, sizeof(csiphy_reset) / sizeof(csiphy_reset[0]), 0, 0);
	phy_list(csiphy_common, sizeof(csiphy_common) / sizeof(csiphy_common[0]), enable, settle);
	for (unsigned pos = 0; pos < 5; pos++) {
		if (cam->lane_mask & (1u << pos)) phy_list(csiphy_2ph[pos], CSIPHY_2PH_SIZE, enable, settle);
	}
	for (unsigned i = 0; i < sizeof(csiphy_irq) / sizeof(csiphy_irq[0]); i++) wr(cam_phy, csiphy_irq[i].addr, csiphy_irq[i].val);
	printf("COACHZ camera: CSIPHY%u lane enable 0x%x settle count %u\n", cam->phy, enable, settle);
}

static void phy_stop(void)
{
	phy_list(csiphy_reset, sizeof(csiphy_reset) / sizeof(csiphy_reset[0]), 0, 0);
}

/* The PHY's eleven status registers, read and cleared as cam_csiphy_irq does; returns their OR. */
static uint32_t phy_status(void)
{
	uint32_t any = 0;
	for (unsigned i = 0; i < 11; i++) {
		uint32_t v = rd(cam_phy, CSIPHY_IRQ_STATUS0 + 4u * i);
		any |= v;
		wr(cam_phy, CSIPHY_IRQ_CLEAR0 + 4u * i, v);
		wr(cam_phy, CSIPHY_IRQ_CLEAR0 + 4u * i, 0);
	}
	wr(cam_phy, CSIPHY_GLBL_IRQ_CMD, 1);
	wr(cam_phy, CSIPHY_GLBL_IRQ_CMD, 0);
	return any;
}

/* ---- PPI (cam_csid_ppi_core.c: reset, enable, D-PHY lane configuration) ---- */
static bool ppi_reset(void)
{
	wr(cam_ppi, PPI_IRQ_MASK, 0);
	wr(cam_ppi, PPI_IRQ_SET, 1);
	wr(cam_ppi, PPI_RST_CMD, 1);
	wr(cam_ppi, PPI_IRQ_CMD, 1u << 1);
	bool done = poll(cam_ppi, PPI_IRQ_STATUS, 1u, 1u, 500000);
	wr(cam_ppi, PPI_IRQ_CLEAR, 1);
	wr(cam_ppi, PPI_IRQ_CMD, 1u << 0);
	if (!done) printf("COACHZ camera: PPI%u reset not done: status 0x%x\n", cam->phy, rd(cam_ppi, PPI_IRQ_STATUS));
	return done;
}

static bool ppi_start(void)
{
	if (!ppi_reset()) return false;
	wr(cam_ppi, PPI_IRQ_MASK, PPI_FIFO_OVERFLOWS);
	wr(cam_ppi, PPI_IRQ_CLEAR, 1);
	wr(cam_ppi, PPI_IRQ_CMD, 1u << 0);
	/* Lanes 0 and 1 of lane_cfg select DL0, lanes 2 and 3 DL1; D-PHY selects neither as C-PHY. */
	uint32_t cfg = 0;
	for (unsigned i = 0; i < cam->lanes; i++) cfg |= ((cam->lane_cfg >> (4 * i)) & 0x3u) < 2 ? PPI_DL_EN(0) : PPI_DL_EN(1);
	wr(cam_ppi, PPI_MODULE_CFG, cfg);
	printf("COACHZ camera: PPI%u version 0x%x cfg 0x%x\n", cam->phy, rd(cam_ppi, 0), rd(cam_ppi, PPI_MODULE_CFG));
	return true;
}

static void ppi_stop(void)
{
	(void)ppi_reset();
	wr(cam_ppi, PPI_IRQ_MASK, 0);
	wr(cam_ppi, PPI_MODULE_CFG, 0);
}

/* ---- CSID (cam_ife_csid_core.c: global reset, CSI2 RX enable, RDI path init and enable) ---- */
static bool csid_reset(void)
{
	wr(cam_csid, CSID_CSI2_IRQ_MASK, 0);
	wr(cam_csid, CSID_IPP_IRQ_MASK, 0);
	for (unsigned n = 0; n < 3; n++) wr(cam_csid, CSID_RDI_IRQ_MASK(n), 0);
	wr(cam_csid, CSID_TOP_IRQ_CLEAR, 1);
	wr(cam_csid, CSID_CSI2_IRQ_CLEAR, 0x0fffffffu);
	wr(cam_csid, CSID_IPP_IRQ_CLEAR, 0x7fffu);
	for (unsigned n = 0; n < 3; n++) wr(cam_csid, CSID_RDI_IRQ_CLEAR(n), 0x7fffu);
	wr(cam_csid, CSID_IRQ_CMD, 1);
	wr(cam_csid, CSID_RX_CFG1, 0x80);
	wr(cam_csid, CSID_IPP_CFG0, 1);
	for (unsigned n = 0; n < 3; n++) wr(cam_csid, CSID_RDI_CFG0(n), 2);
	wr(cam_csid, CSID_RST_STROBES, 0x1e);
	bool hw = poll(cam_csid, CSID_TOP_IRQ_STATUS, 1u, 1u, 100000);
	wr(cam_csid, CSID_RST_STROBES, 1);
	bool reg = poll(cam_csid, CSID_TOP_IRQ_STATUS, 1u, 1u, 100000);
	delay(3000);
	if (!hw || !reg) printf("COACHZ camera: CSID0 reset %d/%d, top status 0x%x\n", hw, reg, rd(cam_csid, CSID_TOP_IRQ_STATUS));
	return hw && reg;
}

static void csid_clear(uint32_t csi2, uint32_t rdi)
{
	wr(cam_csid, CSID_CSI2_IRQ_CLEAR, csi2);
	wr(cam_csid, CSID_RDI_IRQ_CLEAR(0), rdi);
	wr(cam_csid, CSID_TOP_IRQ_CLEAR, rd(cam_csid, CSID_TOP_IRQ_STATUS));
	wr(cam_csid, CSID_IRQ_CMD, 1);
}

static void csid_start(void)
{
	wr(cam_csid, CSID_RX_CFG0, (cam->lanes - 1u) | (cam->lane_cfg << 4) | (0u << 24) | ((cam->phy & 0x3u) << 20));
	wr(cam_csid, CSID_RX_CFG1, (1u << 6) | 1u);
	wr(cam_csid, CSID_CSI2_IRQ_MASK, CSID_CSI2_RST_DONE | CSID_CSI2_ERRORS);
	wr(cam_csid, CSID_RDI_CFG0(0), (0u << 22) | (MIPI_DT_RAW10 << 16) | (0u << 27) | (CSID_DECODE_PAYLOAD << 12) | (1u << 2) | 3u);
	wr(cam_csid, CSID_RDI_CFG1(0), CSID_TIMESTAMP_STB_POST_IRQ);
	wr(cam_csid, CSID_RDI_FRM_DROP_PERIOD, 1);
	wr(cam_csid, CSID_RDI_FRM_DROP_PATTERN, 0);
	wr(cam_csid, CSID_RDI_IRQ_SUB_PERIOD, 1);
	wr(cam_csid, CSID_RDI_IRQ_SUB_PATTERN, 0);
	wr(cam_csid, CSID_RDI_PIX_DROP_PATTERN, 0);
	wr(cam_csid, CSID_RDI_PIX_DROP_PERIOD, 1);
	wr(cam_csid, CSID_RDI_LINE_DROP_PATTERN, 0);
	wr(cam_csid, CSID_RDI_LINE_DROP_PERIOD, 1);
	wr(cam_csid, CSID_RDI_CTRL(0), 0);
	set_bits(cam_csid, CSID_RDI_CFG0(0), 1u << 31, 1u << 31);
	wr(cam_csid, CSID_RX_CAPTURE_CTRL, 0);
	/* cam_ife_csid_enable_rdi_path, with SOF and EOF as the debug flags add them. */
	wr(cam_csid, CSID_RDI_CTRL(0), CSID_RESUME_AT_FRAME_BOUNDARY);
	wr(cam_csid, CSID_RDI_IRQ_MASK(0), CSID_RDI_RST_DONE | CSID_RDI_FIFO_OVERFLOW | CSID_RDI_EOF | CSID_RDI_SOF);
}

static void csid_stop(void)
{
	wr(cam_csid, CSID_RDI_IRQ_MASK(0), 0);
	wr(cam_csid, CSID_RDI_CTRL(0), CSID_HALT_IMMEDIATELY);
	set_bits(cam_csid, CSID_RDI_CFG0(0), 1u << 31, 0);
	wr(cam_csid, CSID_CSI2_IRQ_MASK, 0);
	wr(cam_csid, CSID_RX_CFG0, 0);
}

/* ---- VFE (cam_vfe_top_reset, cam_vfe_bus_init_hw, cam_vfe_bus_start_wm and the RDI buffer update, cam_vfe_rdi reg update) ---- */
static bool vfe_reset(void)
{
	wr(cam_vfe, VFE_IRQ_MASK0, VFE_IRQ0_RESET);
	wr(cam_vfe, VFE_RESET_CMD, VFE_RESET_HW_AND_REG);
	bool done = poll(cam_vfe, VFE_IRQ_STATUS0, VFE_IRQ0_RESET, VFE_IRQ0_RESET, 100000);
	wr(cam_vfe, VFE_IRQ_CLEAR0, VFE_IRQ0_RESET);
	wr(cam_vfe, VFE_IRQ_GLOBAL_CLEAR, 1);
	if (!done) printf("COACHZ camera: VFE0 reset not acknowledged: status0 0x%x\n", rd(cam_vfe, VFE_IRQ_STATUS0));
	return done;
}

/* The ring, as mainline camss-vfe-17x.c drives the VFE 170 RDI write master: the image address register queues addresses, two are
 * queued before the stream starts (vfe_enable_output_v2: an address and a register update each), and each write-master done
 * completes the older of the two (vfe_isr_wm_done: ready_buf = buf[0]) and queues the next address (vfe_wm_update: the address and
 * frame increment only). So the buffer that is done is the one queued first, never the one queued at the frame's start; the earlier
 * scheme, which queued at each start of frame and published RING_PA(frame) after it, published the buffer the next frame was being
 * written into. ring_q holds the two queued ring numbers, oldest first. */
static uint32_t ring_q[2], ring_next;

static void vfe_wm_update(uint64_t pa)
{
	wr(cam_vfe, WM0_IMAGE_ADDR, (uint32_t)pa);
	wr(cam_vfe, WM0_FRAME_INC, FRAME_BYTES);
}

static void ring_start(uint32_t first)
{
	ring_q[0] = first;
	ring_q[1] = first + 1u;
	ring_next = first + 2u;
	for (unsigned i = 0; i < 2; i++) {
		vfe_wm_update(RING_PA(ring_q[i]));
		wr(cam_vfe, VFE_REG_UPDATE, VFE_RDI0_REG_UPDATE);
	}
}

/* A write-master done: the ring number now complete. */
static uint32_t ring_done(void)
{
	const uint32_t ready = ring_q[0];
	ring_q[0] = ring_q[1];
	ring_q[1] = ring_next++;
	vfe_wm_update(RING_PA(ring_q[1]));
	return ready;
}

static void vfe_start(void)
{
	wr(cam_vfe, BUS_DEBUG_CFG, 0x82);
	wr(cam_vfe, BUS_ADDR_SYNC_FRAME_HDR, 0);
	wr(cam_vfe, BUS_CGC_OVD, 0xfffff);
	wr(cam_vfe, BUS_TEST_CTRL, 0);
	wr(cam_vfe, BUS_ADDR_NO_SYNC, (1u << 24) - 1u);
	for (unsigned n = 0; n < 3; n++) wr(cam_vfe, BUS_IRQ_CLEAR(n), 0xffffffffu);
	wr(cam_vfe, BUS_IRQ_GLOBAL_CLEAR, 1);
	wr(cam_vfe, BUS_IRQ_MASK(1), 1u << 0);   /* write master 0 done (CAM_VFE_BUS_IRQ_REG1) */
	wr(cam_vfe, VFE_IRQ_MASK0, VFE_IRQ0_RESET | VFE_IRQ0_BUS | VFE_IRQ0_RDI0_SOF);
	/* RDI write master: frame-based (frame period and IRQ every frame: pattern 1, period 0). */
	wr(cam_vfe, WM0_FRMDROP_PERIOD, 0);
	wr(cam_vfe, WM0_FRMDROP_PATTERN, 1);
	wr(cam_vfe, WM0_IRQ_SUB_PERIOD, 0);
	wr(cam_vfe, WM0_IRQ_SUB_PATTERN, 1);
	wr(cam_vfe, WM0_BURST_LIMIT, 0xf);
	wr(cam_vfe, WM0_WIDTH, WM_RDI_DEFAULT);
	wr(cam_vfe, WM0_HEIGHT, 0);
	wr(cam_vfe, WM0_PACKER, 0);
	wr(cam_vfe, WM0_STRIDE, WM_RDI_DEFAULT);
	wr(cam_vfe, WM0_CFG, WM_EN_FRAME_BASED);
}

static void vfe_stop(void)
{
	wr(cam_vfe, WM0_CFG, 0);
	wr(cam_vfe, VFE_REG_UPDATE, VFE_RDI0_REG_UPDATE);
	wr(cam_vfe, BUS_IRQ_MASK(1), 0);
	wr(cam_vfe, VFE_IRQ_MASK0, 0);
}

/* ---- frames ---- */
static uint32_t view_offset(unsigned which)
{
	return which == 0 ? 0 : which == 1 ? (cam->height / 2) * FRAME_STRIDE : FRAME_BYTES - FRAME_STRIDE;
}

static bool map_views(void)
{
	for (unsigned b = 0; b < 2; b++) {
		for (unsigned v = 0; v < 3; v++) {
			uint64_t pa = (b ? FRAME_PA1 : FRAME_PA0) + view_offset(v), page = pa & ~0xfffull;
			vm_offset_t va = ml_io_map((vm_offset_t)page, 0x2000);
			if (!va) return false;
			cam_view[b][v] = (volatile uint8_t *)va + (pa - page);
			for (unsigned i = 0; i < 0x2000u - (unsigned)(pa - page); i++) cam_view[b][v][i] = FILL;
		}
	}
	return true;
}

/* Bytes of rows a and b (each LINE_BYTES long) that are equal. */
static unsigned rows_equal(volatile uint8_t *a, volatile uint8_t *b)
{
	unsigned equal = 0;
	for (unsigned i = 0; i < LINE_BYTES; i++) equal += a[i] == b[i];
	return equal;
}

static void print_bytes(const char *what, volatile uint8_t *p, unsigned n)
{
	char text[3 * 16 + 1] = "";
	for (unsigned i = 0; i < n && i < 16; i++) snprintf(text + 3 * i, 4, " %02x", p[i]);
	printf("COACHZ camera: %s%s\n", what, text);
}

/* The colour bar: rows of one Bayer phase are identical down the frame, each line has several levels, the 14 bytes after each line
 * are the write master's padding, and the frame ends exactly at FRAME_BYTES. The first rows are compared at the packed and the
 * padded strides, so the log shows which one the hardware wrote. */
static void check_frame(unsigned b)
{
	volatile uint8_t *top = cam_view[b][0], *mid = cam_view[b][1], *last = cam_view[b][2];
	unsigned fill = 0, levels = 0;
	uint8_t seen[4] = {0};
	for (unsigned i = 0; i < LINE_BYTES; i++) {
		fill += mid[i] == FILL;
		if (i % 5 == 0) {
			uint8_t level = mid[i] >> 3;
			if (!(seen[level >> 3] & (1u << (level & 7)))) { seen[level >> 3] |= (uint8_t)(1u << (level & 7)); levels++; }
		}
	}
	printf("COACHZ camera: buffer %u: line %u bytes, stride %u; rows 0/2 equal in %u at stride %u and %u at stride %u; rows 2/%u equal in %u, rows %u/%u in %u; %u fill bytes, %u levels\n",
	    b, LINE_BYTES, FRAME_STRIDE, rows_equal(top, top + 2u * LINE_BYTES), LINE_BYTES, rows_equal(top, top + 2u * FRAME_STRIDE),
	    FRAME_STRIDE, cam->height / 2, rows_equal(top + 2u * FRAME_STRIDE, mid), cam->height / 2,
	    cam->height / 2 + 1, rows_equal(mid, mid + FRAME_STRIDE), fill, levels);
	char what[64];
	for (unsigned r = 0; r < 4; r++) {
		snprintf(what, sizeof(what), "buffer %u row %u:", b, r);
		print_bytes(what, top + r * FRAME_STRIDE, 16);
	}
	for (unsigned r = 0; r < 2; r++) {
		snprintf(what, sizeof(what), "buffer %u row %u bytes %u-%u:", b, r, LINE_BYTES - 2, LINE_BYTES + 13);
		print_bytes(what, top + r * FRAME_STRIDE + LINE_BYTES - 2, 16);
	}
	snprintf(what, sizeof(what), "buffer %u last row padding, then past the frame:", b);
	print_bytes(what, last + FRAME_STRIDE - 8, 16);
	/* The Bayer order on hardware: the high bytes of the 2x2 cell at the centre of each of the colour bar's eight bars, rows 0 and 1.
	 * In SGRBG the two greens (row 0 first, row 1 second) match in every bar, and a bar without red or blue zeroes row 0's second
	 * or row 1's first byte. */
	uint8_t cell[8][4];
	for (unsigned k = 0; k < 8; k++) {
		const unsigned x = ((cam->width / 8u) * k + cam->width / 16u) & ~1u, at = (x / 4u) * 5u + (x % 4u);
		cell[k][0] = top[at]; cell[k][1] = top[at + 1u];
		cell[k][2] = top[FRAME_STRIDE + at]; cell[k][3] = top[FRAME_STRIDE + at + 1u];
	}
	for (unsigned half = 0; half < 2; half++) {
		const uint8_t (*c)[4] = cell + 4u * half;
		printf("COACHZ camera: buffer %u bars %u-%u cells: %02x %02x/%02x %02x, %02x %02x/%02x %02x, %02x %02x/%02x %02x, %02x %02x/%02x %02x\n",
		    b, 4u * half, 4u * half + 3u, c[0][0], c[0][1], c[0][2], c[0][3], c[1][0], c[1][1], c[1][2], c[1][3], c[2][0], c[2][1],
		    c[2][2], c[2][3], c[3][0], c[3][1], c[3][2], c[3][3]);
	}
}

static void stream_and_count(void)
{
	unsigned sof = 0, eof = 0, done = 0, vsof = 0, overflow = 0, errors = 0;
	uint32_t csi2_any = 0, phy_any = 0;
	uint64_t start = mach_absolute_time(), now = start, limit = 0;
	nanoseconds_to_absolutetime(4000000000ull, &limit);
	while ((now = mach_absolute_time()) - start < limit) {
		uint32_t csi2 = rd(cam_csid, CSID_CSI2_IRQ_STATUS), rdi = rd(cam_csid, CSID_RDI_IRQ_STATUS(0));
		uint32_t top0 = rd(cam_vfe, VFE_IRQ_STATUS0), bus1 = rd(cam_vfe, BUS_IRQ_STATUS(1));
		if (csi2 || rdi) csid_clear(csi2, rdi);
		if (top0 || bus1) {
			wr(cam_vfe, BUS_IRQ_CLEAR(1), bus1);
			wr(cam_vfe, BUS_IRQ_GLOBAL_CLEAR, 1);
			wr(cam_vfe, VFE_IRQ_CLEAR0, top0);
			wr(cam_vfe, VFE_IRQ_GLOBAL_CLEAR, 1);
		}
		csi2_any |= csi2 & CSID_CSI2_ERRORS;
		errors += (csi2 & CSID_CSI2_ERRORS) != 0;
		overflow += (rdi & CSID_RDI_FIFO_OVERFLOW) != 0;
		eof += (rdi & CSID_RDI_EOF) != 0;
		if (rdi & CSID_RDI_SOF) sof++;
		if (top0 & VFE_IRQ0_RDI0_SOF) vsof++;
		if (bus1 & 1u) {
			done++;
			(void)ring_done();
		}
		delay(200);
	}
	phy_any = phy_status();
	printf("COACHZ camera: 4 s: CSID SOF %u EOF %u, IFE SOF %u, write master 0 done %u, RDI overflow %u, CSI2 errors %u (0x%x), PHY status 0x%x, WM0 status 0x%x\n",
	    sof, eof, vsof, done, overflow, errors, csi2_any, phy_any, rd(cam_vfe, WM0_STATUS0));
	check_frame(0);
	check_frame(1);
}

uint64_t coachz_camera_frame_pa(uint32_t index) { return RING_PA(index); }
/* 0 width, 1 height, 2 stride (MIPI RAW10 lines padded to 16 bytes), 3 frame bytes, 4 ring count, 5 Bayer order (0 RGGB, 1 GRBG,
 * 2 GBRG, 3 BGGR; the OV8856 is SGRBG10 by ov8856.c 0x3813, the OV5675 SBGGR10), 6 front (1) or rear (0). */
uint32_t coachz_camera_geometry(uint32_t which)
{
	switch (which) {
	case 0: return cam->width;
	case 1: return cam->height;
	case 2: return FRAME_STRIDE;
	case 3: return FRAME_BYTES;
	case 4: return RING;
	case 5: return cam->bayer;
	case 6: return cam == &cam_front;
	default: return 0;
	}
}

/* ---- power: the whole path comes up for a client and goes down when it leaves, as camx's ISP and sensor managers do per session.
 * The rear rails (GPIO7: pp1800_wf_cam, pp2800_wf_cam, which is also the VCM supply) are on only while a client streams. ---- */
enum cam_stage { NONE, VOTES, PLLS, TITAN, IFE0, SENSOR, PIPE, STREAM };
static enum cam_stage cam_reached;
static bool gcc_axi_found;

static void camera_down(void)
{
	const enum cam_stage reached = cam_reached;
	if (reached >= STREAM) (void)sensor_stream(false, 0);
	if (reached >= PIPE) {
		csid_stop();
		vfe_stop();
		ppi_stop();
		phy_stop();
	}
	if (reached >= SENSOR) {
		/* ov8856_power_off / ov5675_power_off: reset asserted, supplies off, clock off. */
		pin_set(&pins[PIN_RESET], pins[PIN_RESET].cfg, false);
		delay(100);
		pin_set(&pins[PIN_RAIL], pins[PIN_RAIL].cfg, false);
		(void)branch(cam_cc, cam->cbcr_mclk, false, "mclk");
	}
	if (reached >= IFE0) {
		(void)branch(cam_cc, CBCR_IFE0_CPHY_RX, false, "ife_0_cphy_rx");
		(void)branch(cam_cc, CBCR_IFE0_CSID, false, "ife_0_csid");
		(void)branch(cam_cc, CBCR_IFE0_AXI, false, "ife_0_axi");
		(void)branch(cam_cc, CBCR_IFE0, false, "ife_0");
		if (!gdsc(GDSC_IFE0, false, "ife_0_gdsc")) printf("COACHZ camera: ife_0_gdsc did not collapse\n");
	}
	if (reached >= TITAN) {
		(void)branch(cam_cc, cam->cbcr_phytimer, false, "csiphytimer");
		(void)branch(cam_cc, cam->cbcr_csiphy, false, "csiphy");
		(void)branch(cam_cc, CBCR_CCI0, false, "cci_0");
		(void)branch(cam_cc, CBCR_CAMNOC_AXI, false, "camnoc_axi");
		(void)branch(cam_cc, CBCR_CPAS_AHB, false, "cpas_ahb");
		(void)branch(cam_cc, CBCR_SOC_AHB, false, "soc_ahb");
		if (!gdsc(GDSC_TITAN, false, "titan_top_gdsc")) printf("COACHZ camera: titan_top_gdsc did not collapse\n");
	}
	/* CAMCC answers only with GCC's camera AHB branch on (set from PLLS on); an unclocked read stalls the bus. */
	const uint32_t titan = reached >= PLLS ? rd(cam_cc, GDSC_TITAN) : 0, ife0 = reached >= PLLS ? rd(cam_cc, GDSC_IFE0) : 0;
	if (reached >= PLLS) {
		pll_off(PLL3);
		pll_off(PLL0);
		if (!gcc_axi_found) (void)branch(cam_gcc, GCC_CAMERA_HF_AXI, false, "gcc_camera_hf_axi");
	}
	if (reached >= VOTES) {
		(void)coachz_camera_bus_vote(0, 0);
		(void)coachz_rpmh_cx_request(CX_CONSUMER_CAMERA, 0);
	}
	/* The rail and reset low as the regulator off and cam_sensor_rear_suspend (output-low) leave them; the clock and bus pins as
	 * they were before the first power-up. */
	for (unsigned i = 0; i < PIN_COUNT; i++) {
		if (i == PIN_RAIL || i == PIN_RESET) {
			pin_set(&pins[i], pins[i].cfg, false);
			continue;
		}
		wr(pins[i].base, 4, pins[i].saved_io);
		wr(pins[i].base, 0, pins[i].saved_cfg);
	}
	cam_reached = NONE;
	if (reached >= PLLS) printf("COACHZ camera: powered down; TITAN GDSC 0x%x IFE0 GDSC 0x%x, GPIO%u out %d\n", titan, ife0,
	    pins[PIN_RAIL].gpio, (rd(pins[PIN_RAIL].base, 4) & TLMM_OUT) != 0);
	else printf("COACHZ camera: stopped before the camera clocks\n");
}

/* The part both sensors share: votes, GCC branches, PLL0 and PLL3, the RCGs, TITAN and its branches with the CCI. */
static bool base_up(void)
{
	cam_reached = NONE;
	{
		int cx = coachz_rpmh_cx_request(CX_CONSUMER_CAMERA, CX_LEVEL_SVS), bus = coachz_camera_bus_vote(BUS_AVG_KBPS, BUS_PEAK_KBPS);
		printf("COACHZ camera: cx svs vote %d, MM0 vote %d\n", cx, bus);
		cam_reached = VOTES;
		if (cx || bus < 0) return false;
	}
	set_bits(cam_gcc, GCC_CAMERA_AHB, 1u, 1u);
	set_bits(cam_gcc, GCC_CAMERA_XO, 1u, 1u);
	if (!branch(cam_gcc, GCC_CAMERA_HF_AXI, true, "gcc_camera_hf_axi")) return false;
	/* camcc PLL0 (600 MHz; its even output feeds the PHY timer and slow AHB) and PLL3 (1080 MHz) as camcc-sc7180.c configures them. */
	cam_reached = PLLS;
	if (!pll_on(PLL0, 0x1f, 0x1u, "cam_cc_pll0") || !pll_on(PLL3, 0x38, 0, "cam_cc_pll3")) return false;
	/* ChromeOS rates: slow AHB 80 MHz, CCI 37.5 MHz, cphy_rx and CSID 270 MHz (svs), PHY timer 300 MHz, IFE 360 MHz (svs);
	 * MCLK 19.2 MHz from XO, the rate mainline's OV8856 tables are for. */
	if (!rcg(RCG_SLOW_AHB, SRC_PLL0_EVEN, 14, false, "slow_ahb") || !rcg(RCG_CCI0, SRC_PLL0_EVEN, 31, true, "cci_0") ||
	    !rcg(RCG_CPHY_RX, SRC_PLL3_MAIN, 7, false, "cphy_rx") || !rcg(cam->rcg_phytimer, SRC_PLL0_EVEN, 3, false, "csiphytimer") ||
	    !rcg(RCG_IFE0_CSID, SRC_PLL3_MAIN, 7, false, "ife_0_csid") || !rcg(RCG_IFE0, SRC_PLL3_MAIN, 5, false, "ife_0") ||
	    !rcg(cam->rcg_mclk, SRC_XO, 1, true, "mclk")) {
		return false;
	}
	cam_reached = TITAN;
	if (!gdsc(GDSC_TITAN, true, "titan_top_gdsc")) return false;
	return branch(cam_cc, CBCR_SOC_AHB, true, "soc_ahb") && branch(cam_cc, CBCR_CPAS_AHB, true, "cpas_ahb") &&
	    branch(cam_cc, CBCR_CAMNOC_AXI, true, "camnoc_axi") && branch(cam_cc, CBCR_CCI0, true, "cci_0") &&
	    branch(cam_cc, cam->cbcr_csiphy, true, "csiphy") && branch(cam_cc, cam->cbcr_phytimer, true, "csiphytimer");
}

/* Up to a streaming sensor: votes, PLLs, TITAN and IFE0 clocks, __ov8856_power_on, the CCI, identification, then CSIPHY, PPI, CSID
 * and VFE with the ring from `first`, and the sensor's stream with test pattern register value `pattern`. On failure it powers
 * down again. */
static bool camera_up(uint32_t first, uint8_t pattern)
{
	/* Reset asserted and the rail off before any clock runs. */
	pin_set(&pins[PIN_RESET], pins[PIN_RESET].cfg, false);
	pin_set(&pins[PIN_RAIL], pins[PIN_RAIL].cfg, false);
	if (!base_up()) goto fail;
	cam_reached = IFE0;
	if (!gdsc(GDSC_IFE0, true, "ife_0_gdsc")) goto fail;
	if (!branch(cam_cc, CBCR_IFE0, true, "ife_0") || !branch(cam_cc, CBCR_IFE0_AXI, true, "ife_0_axi") ||
	    !branch(cam_cc, CBCR_IFE0_CSID, true, "ife_0_csid") || !branch(cam_cc, CBCR_IFE0_CPHY_RX, true, "ife_0_cphy_rx")) {
		goto fail;
	}
	printf("COACHZ camera: CPAS 0x%x, CCI 0x%x, CSID 0x%x, VFE 0x%x, bus 0x%x\n", rd(cam_cpas, 0), rd(cam_cci, CCI_HW_VERSION),
	    rd(cam_csid, CSID_HW_VERSION), rd(cam_vfe, VFE_HW_VERSION), rd(cam_vfe, BUS_HW_VERSION));
	/* __ov8856_power_on / ov5675_power_on: clock, reset asserted, supplies (the rails' 1 ms ramp), the sensor's wait, reset
	 * released, its wait before the first transfer. */
	cam_reached = SENSOR;
	if (!branch(cam_cc, cam->cbcr_mclk, true, "mclk")) goto fail;
	pin_set(&pins[PIN_MCLK], pins[PIN_MCLK].cfg, false);
	delay(2000);
	pin_set(&pins[PIN_RAIL], pins[PIN_RAIL].cfg, true);
	delay(cam->rail_to_reset_us);
	pin_set(&pins[PIN_RESET], pins[PIN_RESET].cfg, true);
	delay(cam->reset_to_bus_us);
	/* Then the bus, as camx's camera_io_init follows cam_sensor_core_power_up: CCI pins active, controller reset and masks. */
	pin_set(&pins[PIN_SDA], pins[PIN_SDA].cfg, false);
	pin_set(&pins[PIN_SCL], pins[PIN_SCL].cfg, false);
	if (!cci_reset()) goto fail;
	if (!sensor_identify()) { printf("COACHZ camera: no %s on CCI0 master %u\n", cam->name, cam->cci); goto fail; }

	/* The receive side first, as the ISP manager starts CSIPHY, CSID and IFE before the sensor streams. */
	cam_reached = PIPE;
	phy_start();
	if (!ppi_start() || !csid_reset() || !vfe_reset()) goto fail;
	vfe_start();
	ring_start(first);
	csid_start();
	cam_reached = STREAM;
	if (!sensor_stream(true, pattern)) { printf("COACHZ camera: %s at 0x%02x did not start streaming\n", cam->name, cam_sensor_addr); goto fail; }
	return true;
fail:
	camera_down();
	return false;
}

/* ---- the front sensor's identity (/chosen/coachz-camera bit 0x100): chip ID reads only, nothing written to the sensor ----
 * Its model is in no public source: sc7180-trogdor-mipi-camera.dtsi names only qcom,cam-sensor on CCI0 master 0 (cci0_active: GPIO17
 * and GPIO18, cci_i2c, pull-up, 2 mA) with MCLK0 on GPIO13 (cam_sensor_mclk0_active) and CAM_RESET0 on GPIO30
 * (cam_sensor_front_active), and sc7180-trogdor.dtsi turns pp1800_uf_cam and pp2800_uf_cam on with GPIO6; its EEPROM at 0x50 is
 * never addressed. The node asks for MCLK0 at 24 MHz, which camcc-sc7180.c makes from the Agera PLL2; the probe runs it at 19.2 MHz
 * from XO, the same table's first entry, and leaves PLL2 to a front driver. Each address is read only with the register width of the
 * candidates that sit there, so no address byte can land as a data write; the candidates are the 5 MP sensors with a mainline or
 * ChromeOS driver, their addresses from the bindings' examples or a ChromeOS board, their ID registers and values from the drivers. */
#define CCI_FRONT       0u
static struct cam_pin front_pins[PIN_COUNT] = {
	[PIN_RAIL] = {.gpio = 6, .north = true, .cfg = TLMM_DRIVE(2) | TLMM_OE, .base = NULL, .saved_cfg = 0, .saved_io = 0},
	[PIN_RESET] = {.gpio = 30, .north = false, .cfg = TLMM_DRIVE(2) | TLMM_OE, .base = NULL, .saved_cfg = 0, .saved_io = 0},
	[PIN_MCLK] = {.gpio = 13, .north = false, .cfg = TLMM_FUNC(1) | TLMM_DRIVE(2), .base = NULL, .saved_cfg = 0, .saved_io = 0},
	[PIN_SDA] = {.gpio = 17, .north = false, .cfg = TLMM_FUNC(1) | TLMM_DRIVE(2) | TLMM_PULL_UP, .base = NULL, .saved_cfg = 0, .saved_io = 0},
	[PIN_SCL] = {.gpio = 18, .north = false, .cfg = TLMM_FUNC(1) | TLMM_DRIVE(2) | TLMM_PULL_UP, .base = NULL, .saved_cfg = 0, .saved_io = 0},
};
struct front_read { uint8_t addr, regbytes, len; uint16_t reg; const char *what; };
static const struct front_read front_reads[] = {
	/* ov5647, ov5648, ov5670, ov5675, ov5693 bindings and ChromeOS kodama's ov5695: 0x36; 0x300a read as ov5670/ov5675/ov5695 do
	 * (24 bits: 0x005670, 0x005675, 0x005695), its first two bytes as ov5647/ov5648/ov5693 do (0x5647, 0x5648, 0x5690). */
	{0x36, 2, 3, 0x300a, "OmniVision 0x300a"},
	{0x10, 2, 3, 0x300a, "OmniVision 0x300a at the other SID address"},
	/* ov5640 and ov5645 bindings: 0x3c; 0x5640 and 0x5645 at 0x300a. */
	{0x3c, 2, 3, 0x300a, "OmniVision 0x300a"},
	/* hi556.c: 0x0556 at 0x0f16 (16-bit). Its address comes only from ACPI; 0x20 is the Hynix hi846 binding's example. */
	{0x20, 2, 2, 0x0f16, "Hynix 0x0f16"},
	/* ChromeOS kodama's gc5035 at 0x37: 8-bit registers, 0x50 at 0xf0 and 0x35 at 0xf1 (gc5035.c, chromeos-5.10). */
	{0x37, 1, 1, 0x00f0, "GalaxyCore 0xf0"},
	{0x37, 1, 1, 0x00f1, "GalaxyCore 0xf1"},
};

static void front_probe(void)
{
	struct cam_pin *p = front_pins;
	uint8_t got[sizeof(front_reads) / sizeof(front_reads[0])][3];
	int err[sizeof(front_reads) / sizeof(front_reads[0])];
	const char *model = NULL;
	uint8_t at = 0;

	printf("COACHZ camera: front probe, read only; GPIO6 out %d before\n", (rd(p[PIN_RAIL].base, 4) & TLMM_OUT) != 0);
	pin_set(&p[PIN_RESET], p[PIN_RESET].cfg, false);
	pin_set(&p[PIN_RAIL], p[PIN_RAIL].cfg, false);
	if (!base_up() || !rcg(RCG_MCLK0, SRC_XO, 1, true, "mclk0") || !branch(cam_cc, CBCR_MCLK0, true, "mclk0")) {
		printf("COACHZ camera: front probe: clocks did not come up\n");
		goto down;
	}
	/* The rear sensor's power order: clock, supplies after 2 ms, reset released 1 ms later; then 30 ms, longer than any candidate
	 * driver waits after its reset (25 ms at most, ov5640 and ov5648); then the CCI pins and the controller. */
	pin_set(&p[PIN_MCLK], p[PIN_MCLK].cfg, false);
	delay(2000);
	pin_set(&p[PIN_RAIL], p[PIN_RAIL].cfg, true);
	delay(1000);
	pin_set(&p[PIN_RESET], p[PIN_RESET].cfg, true);
	delay(30000);
	pin_set(&p[PIN_SDA], p[PIN_SDA].cfg, false);
	pin_set(&p[PIN_SCL], p[PIN_SCL].cfg, false);
	if (!cci_reset()) goto off;
	for (unsigned i = 0; i < sizeof(front_reads) / sizeof(front_reads[0]); i++) {
		const struct front_read *r = &front_reads[i];
		got[i][0] = got[i][1] = got[i][2] = 0;
		err[i] = cci_read(CCI_FRONT, r->addr, r->reg, r->regbytes, got[i], r->len);
		if (err[i]) printf("COACHZ camera: front 0x%02x %s: error %d (CCI status 0x%x)\n", r->addr, r->what, err[i], cci_last_status);
		else printf("COACHZ camera: front 0x%02x %s: %02x %02x %02x (%u bytes)\n", r->addr, r->what, got[i][0], got[i][1], got[i][2], r->len);
		if (err[i] || model) continue;
		const uint32_t v24 = (uint32_t)got[i][0] << 16 | (uint32_t)got[i][1] << 8 | got[i][2], v16 = v24 >> 8;
		if (r->reg == 0x300a) {
			model = v24 == 0x005670u ? "ov5670" : v24 == 0x005675u ? "ov5675" : v24 == 0x005695u ? "ov5695" :
			    v16 == 0x5640u ? "ov5640" : v16 == 0x5645u ? "ov5645" : v16 == 0x5647u ? "ov5647" : v16 == 0x5648u ? "ov5648" :
			    v16 == 0x5690u ? "ov5693" : NULL;
		} else if (r->reg == 0x0f16 && v16 == 0x0556u) {
			model = "hi556";
		} else if (r->reg == 0x00f1 && i > 0 && !err[i - 1] && front_reads[i - 1].addr == r->addr && got[i - 1][0] == 0x50 && got[i][0] == 0x35) {
			model = "gc5035";
		}
		if (model) at = r->addr;
	}
	if (model) printf("COACHZ camera: front sensor is %s at 0x%02x\n", model, at);
	else printf("COACHZ camera: front sensor matched no candidate\n");
off:
	/* Powered off as the rear sensor is: reset asserted, supplies off, clock off; then every pin as it was. */
	pin_set(&p[PIN_RESET], p[PIN_RESET].cfg, false);
	delay(100);
	pin_set(&p[PIN_RAIL], p[PIN_RAIL].cfg, false);
	(void)branch(cam_cc, CBCR_MCLK0, false, "mclk0");
down:
	for (unsigned i = 0; i < PIN_COUNT; i++) {
		if (i == PIN_RAIL || i == PIN_RESET) {
			pin_set(&p[i], p[i].cfg, false);
			continue;
		}
		wr(p[i].base, 4, p[i].saved_io);
		wr(p[i].base, 0, p[i].saved_cfg);
	}
	camera_down();
	printf("COACHZ camera: front probe done; GPIO6 out %d\n", (rd(p[PIN_RAIL].base, 4) & TLMM_OUT) != 0);
}

/* What the client's copies saw (CoachzCamera.cpp): copies, their total and longest time, and copies the ring overtook. */
volatile uint32_t coachz_camera_copies, coachz_camera_copy_us_max, coachz_camera_overtaken;
volatile uint64_t coachz_camera_copy_us;

/* The service loop: the camera is powered down while no client streams. A client's start powers the whole path up (a few hundred
 * milliseconds; the viewer shows "Waiting for frames" meanwhile) and its stop or close powers it down again, which turns the rear
 * rails off. Each write-master done publishes the ring buffer it completed. */
__attribute__((noreturn)) static void service_loop(void)
{
	uint32_t applied = ~0u, complete = 0;
	bool streaming = false;
	coachz_camera_state = 1;
	coachz_camera_publish();
	printf("COACHZ camera: service ready, ring of %u frames at 0x%llx, powered down until a client streams\n", RING,
	    (unsigned long long)RING_PA(0));
	for (;;) {
		if (!streaming) {
			if (!coachz_camera_want) { delay(20000); continue; }
			const uint64_t t0 = mach_absolute_time();
			if (!camera_up(ring_next, 0)) {
				coachz_camera_state = 3;
				printf("COACHZ camera: power-up for a client failed; idle until it stops\n");
				while (coachz_camera_want) delay(20000);
				coachz_camera_state = 1;
				continue;
			}
			uint64_t ns = 0;
			absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
			streaming = true;
			applied = ~0u;
			complete = 0;
			coachz_camera_copies = coachz_camera_copy_us_max = coachz_camera_overtaken = 0;
			coachz_camera_copy_us = 0;
			coachz_camera_state = 2;
			printf("COACHZ camera: powered up for a client in %llu ms, streaming from ring %u\n", ns / 1000000ull, ring_q[0]);
		}
		if (!coachz_camera_want) {
			coachz_camera_state = 1;
			camera_down();
			streaming = false;
			const uint32_t copies = coachz_camera_copies;
			printf("COACHZ camera: client stopped after %u frames; %u copies mean %llu max %u us, %u overtaken\n", complete, copies,
			    copies ? (unsigned long long)(coachz_camera_copy_us / copies) : 0ull, coachz_camera_copy_us_max, coachz_camera_overtaken);
			continue;
		}
		if (applied != coachz_camera_settings) {
			applied = coachz_camera_settings;
			uint32_t e = coachz_camera_exposure, g = coachz_camera_gain;
			if (e < 6u) e = 6u;
			if (e > cam->vts - cam->exposure_margin) e = cam->vts - cam->exposure_margin;
			if (g < OV8856_ANAL_GAIN_MIN) g = OV8856_ANAL_GAIN_MIN;
			if (g > 2047u) g = 2047u;
			(void)sensor_write(OV8856_REG_EXPOSURE, e << cam->exposure_shift, 3);
			(void)sensor_write(OV8856_REG_ANALOG_GAIN, g, 2);
		}
		uint32_t csi2 = rd(cam_csid, CSID_CSI2_IRQ_STATUS), rdi = rd(cam_csid, CSID_RDI_IRQ_STATUS(0));
		uint32_t top0 = rd(cam_vfe, VFE_IRQ_STATUS0), bus1 = rd(cam_vfe, BUS_IRQ_STATUS(1));
		if (csi2 || rdi) csid_clear(csi2, rdi);
		if (top0 || bus1) {
			wr(cam_vfe, BUS_IRQ_CLEAR(1), bus1);
			wr(cam_vfe, BUS_IRQ_GLOBAL_CLEAR, 1);
			wr(cam_vfe, VFE_IRQ_CLEAR0, top0);
			wr(cam_vfe, VFE_IRQ_GLOBAL_CLEAR, 1);
		}
		if (bus1 & 1u) {
			coachz_camera_last = ring_done() % RING;
			complete++;
			__atomic_store_n(&coachz_camera_seq, coachz_camera_seq + 1u, __ATOMIC_RELEASE);
		}
		delay(2000);
	}
}

static void map_all(void)
{
	cam_gcc = (volatile uint8_t *)ml_io_map(GCC_PA + 0xb000u, 0x1000u) - 0xb000;
	cam_cc = (volatile uint8_t *)ml_io_map(CAMCC_PA, CAMCC_SIZE);
	cam_cpas = (volatile uint8_t *)ml_io_map(CPAS_PA, 0x1000u);
	cam_cci = (volatile uint8_t *)ml_io_map(CCI0_PA, 0x1000u);
	cam_phy = (volatile uint8_t *)ml_io_map(CSIPHY_PA(cam->phy), 0x1000u);
	cam_ppi = (volatile uint8_t *)ml_io_map(PPI_PAGE_PA, 0x1000u) + PPI_OFFSET(cam->phy);
	cam_csid = (volatile uint8_t *)ml_io_map(CSID0_PA, 0x1000u);
	cam_vfe = (volatile uint8_t *)ml_io_map(VFE0_PA, VFE0_SIZE);
	for (unsigned i = 0; i < PIN_COUNT; i++) {
		struct cam_pin *p = &cam_pins[i];
		p->base = (volatile uint8_t *)ml_io_map((p->north ? TLMM_NORTH_PA : TLMM_SOUTH_PA) + p->gpio * 0x1000u, 0x1000u);
		p->saved_cfg = rd(p->base, 0);
		p->saved_io = rd(p->base, 4);
	}
	for (unsigned i = 0; i < PIN_COUNT; i++) {
		struct cam_pin *p = &front_pins[i];
		p->base = (volatile uint8_t *)ml_io_map((p->north ? TLMM_NORTH_PA : TLMM_SOUTH_PA) + p->gpio * 0x1000u, 0x1000u);
		p->saved_cfg = rd(p->base, 0);
		p->saved_io = rd(p->base, 4);
	}
	/* Both camera enables as the firmware left them (GPIO6 UF_CAM_EN, GPIO7 WF_CAM_EN; sc7180-trogdor.dtsi): the board has no camera
	 * LED node, so a lit camera LED follows one of these rails. */
	printf("COACHZ camera: at start GPIO6 (UF_CAM_EN) cfg 0x%x out %d in %u, GPIO7 (WF_CAM_EN) cfg 0x%x out %d in %u\n",
	    front_pins[PIN_RAIL].saved_cfg, (front_pins[PIN_RAIL].saved_io & TLMM_OUT) != 0, front_pins[PIN_RAIL].saved_io & 1u,
	    cam_pins[PIN_RAIL].saved_cfg, (cam_pins[PIN_RAIL].saved_io & TLMM_OUT) != 0, cam_pins[PIN_RAIL].saved_io & 1u);
}

static void camera_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;

	if (cam_mode & 0x800u) {
		cam = &cam_front;
		pins = front_pins;
	}
	printf("COACHZ camera: this run drives the %s %s (CCI0 master %u, CSIPHY%u, %u lanes, %ux%u)\n", cam->where, cam->name, cam->cci,
	    cam->phy, cam->lanes, cam->width, cam->height);
	map_all();
	if (!map_views()) { printf("COACHZ camera: frame buffer views not mapped\n"); goto out; }
	gcc_axi_found = (rd(cam_gcc, GCC_CAMERA_HF_AXI) & 1u) != 0;
	if (cam_mode & 0x100u) front_probe();
	if (!(cam_mode & 0xffu)) goto out;
	/* The self-test: the colour bar for four seconds, checked, then everything down again. */
	if (!camera_up(0, OV8856_COLOUR_BAR)) goto out;
	if ((cam_mode & 0x200u) && cam == &cam_rear) {
		/* Only with bit 0x200 (off unless asked for). Read only: does anything answer at 0x0c on the rear bus? The rear VCM (actuator_rear in sc7180-trogdor-mipi-camera.dtsi,
		 * CCI0 master 1, supplied by pp2800_wf_cam) has no address or model in any public source; ChromeOS kukui puts a dongwoon,dw9768
		 * at 0x0c beside its OV8856 (mt8183-kukui-krane.dtsi), another board's module. A plain read writes no address byte. */
		uint8_t b[1] = {0};
		int e = cci_read(CCI_REAR, 0x0c, 0, 0, b, 1);
		printf("COACHZ camera: rear 0x0c plain read: %d, %02x (CCI status 0x%x)\n", e, b[0], cci_last_status);
	}
	stream_and_count();
	camera_down();
	if ((cam_mode & 0xffu) == 2) {
		service_loop();   /* does not return */
	}
out:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
coachz_camera_start(void)
{
	static bool started;
	DTEntry chosen = NULL;
	const void *value = NULL;
	unsigned int size = 0;
	thread_t thread;

	uint64_t now = 0, after = 0;
	/* Only once the system is up: root mounted and 120 s after boot, so the run never overlaps storage bring-up or the root mount. */
	nanoseconds_to_absolutetime(120ull * NSEC_PER_SEC, &after);
	now = mach_absolute_time();
	if (started || rootvnode == NULL || now < after) {
		return;
	}
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "coachz-camera", &value, &size) != kSuccess || size < 4 || !*(const uint32_t *)value) {
		return;
	}
	cam_mode = *(const uint32_t *)value;
	started = true;
	if (kernel_thread_start(camera_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
