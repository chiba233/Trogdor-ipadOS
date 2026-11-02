/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Coachz sdhc_1 (eMMC) and sdhc_2 (microSD) host, stage 1: read-only, interrupt driven, ADMA2, legacy/high-speed
 * timings. Not in the kernel build. A port of the mainline sequence (Linux 7.4-rc1, SOURCES.md):
 *
 *   sdhci_msm_probe      OPP boost vote, core then iface clock, vendor POR, capability fix-up, regulator capabilities,
 *                        pending power interrupt handled, PWRCTL mask, power interrupt attached
 *   sdhci_add_host       caps read after RESET_ALL, DMA/Auto-CMD flags, RESET_ALL again, default interrupts, hc_irq
 *   mmc_rescan           power up (vdd from the vmmc OCR mask, initial signal voltage, 10 ms, f_init 400 kHz, 10 ms),
 *                        CMD0, SD probe (CMD8 / ACMD41), then MMC; power off when neither attaches
 *   sdhci-msm power irq  every POWER_CONTROL / HOST_CONTROL2 / RESET_ALL write that changes the bus or IO level waits
 *                        for PWRCTL, whose handler switches vmmc/vqmmc through RPMh and the SDC pins, then acknowledges
 *
 * Limits, chosen and not discovered: below HS400 the core clock stays on sources already running (XO, GPLL0 even, so at most
 * 100 MHz); no HS200/DDR52/UHS-I, tuning, CQE, ICE or SDIO probe; no interconnect bandwidth vote; no erase, cache, RPMB or
 * boot-partition access. EXT_CSD is changed only where the bus needs it (BUS_WIDTH, HS_TIMING), both lost at power-off; a
 * PART_CONFIG that selects a boot partition is reported and left alone, so the card is not used.
 *
 * Boot arguments: coachz-emmc=1 reads at high speed, =2 also writes inside the window, =3 also takes HS400 with enhanced strobe, as
 * Debian does on this card (mmc_select_hs400es, GPLL6 at 384 MHz, CX nominal). coachz-sd=1 reads a card, =2 also writes to it. Without
 * them nothing is mapped.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include "coachz_mmc_proto.h"
#include "coachz_rpmh_iface.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
extern int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);   /* ArmCpu.cpp, IOReturn */

int coachz_sdcc_prepare(void);
int coachz_sdcc_open(int slot, uint64_t *blocks);
int coachz_sdcc_read(int slot, uint64_t block, uint32_t count, void **data);
uint32_t coachz_sdcc_max_blocks(void);
int coachz_sdcc_removable(int slot);
int coachz_sdcc_enabled(int slot);
int coachz_sdcc_write(int slot, uint64_t block, uint32_t count, const void *data);
int coachz_sdcc_writable(int slot);
void coachz_sdcc_write_selftest(int slot);

/* ------------------------------------------------------------------ board facts (sc7180.dtsi, sc7180-trogdor.dtsi) */
#define GCC_WINDOW_PA    0x00112000u   /* SDCC1 0x112000-0x112fff and SDCC2 0x114000-0x114fff, GCC offsets minus 0x12000 */
#define GCC_WINDOW_BYTES 0x3000u
#define GCC_OFF(o)       ((o) - 0x12000u)
#define TLMM_SOUTH_PA    0x03d00000u
#define TLMM_WEST_PA     0x03500000u
#define GICD_PA          0x17a00000u
#define GICD_ISENABLER(n) (0x100u + ((n) / 32u) * 4u)
#define GICD_ICENABLER(n) (0x180u + ((n) / 32u) * 4u)
/* A fixed pool below 4 GiB outside the XNU window, as the USB driver's (0x81000000, 1 MiB) and below the speaker ring
   (0x81200000). The apps SMMU is in bypass after firmware (docs/usb-read.md), and SDCC1/2 sit behind it (SID 0x60/0x80). */
#define POOL_PA          0x81100000ul
#define POOL_BYTES       0x100000ul    /* up to the speaker ring */
#define SLOT_POOL        0x80000ul
#define ADMA_OFF         0x0000ul
#define ADMA_BYTES       0x1000ul
#define SCRATCH_OFF      0x1000ul
#define XFER_OFF         0x2000ul
#define MAX_BLOCKS       960u          /* 480 KiB a request: at HS400 most of a request's time is then the transfer, not its commands */
#define TEST_BLOCKS_AT   128u          /* what the write test moves at a time */

enum { PM_UNDEFINED, PM_OFF, PM_UP, PM_ON };

struct sdcc_cmd { unsigned opcode; uint32_t arg; unsigned flags; uint32_t resp[4]; unsigned busy_timeout_ms; int error; };
struct sdcc_data { unsigned blksz, blocks; uint64_t pa; int read; const struct sdcc_cmd *sbc; struct sdcc_cmd *stop; int error; };

struct sdcc_vreg {
    struct cm_vreg v;
    uint32_t address;
    int known;            /* rpmh vreg->enabled != -EINVAL */
    int enabled;
    uint32_t mv, cached_mv;
    int mode_set;
};

struct sdcc {
    /* board */
    const char *name;
    int sdcc1;
    uint32_t hc_pa, hc_intid, pwr_intid;
    uint32_t gcc_ahb, gcc_apps, gcc_rcg;
    const struct cm_freq *freqs;
    size_t nfreqs;
    const int8_t *parents;
    uint32_t sdc_ctl_pa;
    const struct cm_pin *pins_on, *pins_off;
    size_t npins;
    int removable;
    unsigned dt_bus_width;      /* 8 or 4 */
    int dt_caps_180;            /* SDHCI_SIGNALING_180 from the DT/host caps, see sdhci_setup_host */
    unsigned cx_consumer;
    struct sdcc_vreg vmmc, vqmmc;
    /* mappings */
    vm_offset_t hc, sdc, cd, pool;
    uint64_t pool_pa;
    /* sdhci_host / sdhci_msm_host / mmc_host */
    uint16_t version;
    uint32_t caps, caps1, mci_version, caps0, max_current;
    uint32_t ocr_avail, f_max, f_min, timeout_khz;
    int adma64, auto12, auto23, signaling_330, signaling_180, cdclp533;
    int gpll6, enhanced_strobe, calibration_done, calibration_err, hs400_wanted;
    uint32_t clk_rate;
    struct { unsigned clock, power_mode, vdd, bus_width, timing, signal, drv_type; } ios;
    unsigned host_clock, host_timing;
    uint8_t host_pwr;
    int reinit_uhs;
    uint32_t curr_pwr_state, curr_io_level;
    int regulator_enabled;      /* mmc->regulator_enabled (vmmc) */
    int vqmmc_enabled;          /* msm_host->vqmmc_enabled */
    uint32_t rcg_hz;
    unsigned cx_level;
    /* interrupts */
    uint32_t hc_status;         /* accumulated INT_STATUS of the current request */
    int pwr_pending, pwr_flag;
    uint32_t hc_irqs, pwr_irqs, spurious;
    /* card */
    int card_registered, card_mmc, card_readonly;
    uint32_t rca, raw_cid[4], raw_csd[4], raw_scr[2];
    struct cm_csd csd;
    struct cm_ext_csd ext;
    struct cm_scr scr;
    uint32_t hs_max_dtr;
    int blockaddr, cmd23;
    uint64_t sectors;
    int opened;
    /* The only blocks coachz_sdcc_write takes: none until a slot's window is set. */
    uint64_t write_start, write_blocks;
    uint32_t writes_refused;
    const struct sdcc_cmd *stop_cmd;   /* the stop command being sent: it does not wait for DATA_INHIBIT */
};

static const struct cm_vreg vreg_ldoa19 = { "ldoa19", 1504000, 8000, 256, 2960000, 2960000 };   /* pmic5_pldo, mcp_vcc */
static const struct cm_vreg vreg_ldoa12 = { "ldoa12", 1504000, 8000, 63, 1800000, 1800000 };    /* pmic5_pldo_lv, mcp_vccq */
static const struct cm_vreg vreg_ldoc9 = { "ldoc9", 1504000, 8000, 256, 2952000, 2952000 };     /* pmic5_pldo, pp2950_l9c */
static const struct cm_vreg vreg_ldoc6 = { "ldoc6", 1504000, 8000, 256, 1800000, 2952000 };     /* pmic5_pldo, ppvar_l6c */

static struct sdcc slots[2] = {
    { .name = "emmc", .sdcc1 = 1, .hc_pa = 0x007c4000u, .hc_intid = 32u + 641u, .pwr_intid = 32u + 644u,
      .gcc_ahb = 0x12008u, .gcc_apps = 0x1200cu, .gcc_rcg = 0x12028u,
      .freqs = cm_sdcc1_freqs, .nfreqs = sizeof(cm_sdcc1_freqs) / sizeof(cm_sdcc1_freqs[0]), .parents = cm_sdcc1_parent_cfg,
      .sdc_ctl_pa = TLMM_SOUTH_PA + 0x7a000u, .pins_on = cm_sdc1_on, .pins_off = cm_sdc1_off, .npins = 4,
      .removable = 0, .dt_bus_width = 8, .dt_caps_180 = 1, .cx_consumer = COACHZ_CX_SDCC1 },
    { .name = "sd", .sdcc1 = 0, .hc_pa = 0x08804000u, .hc_intid = 32u + 204u, .pwr_intid = 32u + 222u,
      .gcc_ahb = 0x14008u, .gcc_apps = 0x14004u, .gcc_rcg = 0x1400cu,
      .freqs = cm_sdcc2_freqs, .nfreqs = sizeof(cm_sdcc2_freqs) / sizeof(cm_sdcc2_freqs[0]), .parents = cm_sdcc2_parent_cfg,
      .sdc_ctl_pa = TLMM_SOUTH_PA + 0x7b000u, .pins_on = cm_sdc2_on, .pins_off = cm_sdc2_off, .npins = 3,
      .removable = 1, .dt_bus_width = 4, .dt_caps_180 = 0, .cx_consumer = COACHZ_CX_SDCC2 },
};
static vm_offset_t gcc, gicd, pool;

/* ------------------------------------------------------------------ MMIO, time */
static inline uint32_t rd32(vm_offset_t b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline uint16_t rd16(vm_offset_t b, uint32_t o) { return *(volatile uint16_t *)(b + o); }
static inline uint8_t rd8(vm_offset_t b, uint32_t o) { return *(volatile uint8_t *)(b + o); }
static inline void wr32(vm_offset_t b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void wr16(vm_offset_t b, uint32_t o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }
static inline void wr8(vm_offset_t b, uint32_t o, uint8_t v) { *(volatile uint8_t *)(b + o) = v; }
static inline void mb(void) { __asm__ volatile ("dsb sy" ::: "memory"); }

static uint64_t now_us(void)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000u;
}
/* A short delay spins on the counter: a blocking delay costs a timer interrupt and a wake (coachz_usb.c). */
static void spin_us(unsigned us)
{
    const uint64_t end = now_us() + us;
    while (now_us() < end) __asm__ volatile ("yield");
}
static void sleep_ms(unsigned ms)
{
    uint64_t deadline;
    static int chan;
    clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
    (void)assert_wait_deadline(&chan, THREAD_UNINT, deadline);
    thread_block(THREAD_CONTINUE_NULL);
}
/* usleep_range in the busy polls: spin below a millisecond, block above. */
static void sleep_us(unsigned us) { if (us < 1000u) spin_us(us); else sleep_ms((us + 999u) / 1000u); }

/* Waits until *flag is set (it is cleared by the taker) or the deadline passes; the pattern of the touch driver's
   irq_wait, so a wake between the check and the block is not lost. */
static int wait_flag(int *flag, void *chan, unsigned ms)
{
    if (__atomic_load_n(flag, __ATOMIC_ACQUIRE)) return 1;
    uint64_t deadline;
    clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
    (void)assert_wait_deadline(chan, THREAD_UNINT, deadline);
    if (__atomic_load_n(flag, __ATOMIC_ACQUIRE)) thread_wakeup(chan);
    thread_block(THREAD_CONTINUE_NULL);
    return __atomic_load_n(flag, __ATOMIC_ACQUIRE) != 0;
}

/* ------------------------------------------------------------------ clocks (clk-branch.c, clk-rcg2.c) */
static int branch_enable(uint32_t cbcr)
{
    wr32(gcc, GCC_OFF(cbcr), rd32(gcc, GCC_OFF(cbcr)) | CM_CBCR_ENABLE);
    for (unsigned i = 0; i < CM_BRANCH_POLLS; i++) {
        if (cm_branch_on(rd32(gcc, GCC_OFF(cbcr)))) return 1;
        spin_us(1);
    }
    printf("COACHZ mmc: branch 0x%x status stuck at off (0x%x)\n", cbcr, rd32(gcc, GCC_OFF(cbcr)));
    return 0;
}

static int rcg_set(struct sdcc *s, uint32_t rate)
{
    const struct cm_freq *f = cm_rcg_floor(s->freqs, s->nfreqs, rate, s->gpll6 ? CM_HS400_SOURCES : CM_STAGE1_SOURCES);
    struct cm_rcg_regs r;
    if (!f || !cm_rcg_regs(f, s->parents, 8, 5, &r)) return 0;
    if (s->rcg_hz == f->hz) return 1;
    const uint32_t base = GCC_OFF(s->gcc_rcg);
    if (r.mnd) {
        wr32(gcc, base + CM_RCG_M, (rd32(gcc, base + CM_RCG_M) & ~r.mnd_mask) | r.m);
        wr32(gcc, base + CM_RCG_N, (rd32(gcc, base + CM_RCG_N) & ~r.mnd_mask) | r.n);
        wr32(gcc, base + CM_RCG_D, (rd32(gcc, base + CM_RCG_D) & ~r.mnd_mask) | r.d);
    }
    wr32(gcc, base + CM_RCG_CFG, (rd32(gcc, base + CM_RCG_CFG) & ~r.cfg_mask) | r.cfg);
    wr32(gcc, base, rd32(gcc, base) | CM_RCG_CMD_UPDATE);
    for (unsigned i = 0; i < CM_RCG_UPDATE_POLLS; i++) {
        if (!(rd32(gcc, base) & CM_RCG_CMD_UPDATE)) { s->rcg_hz = f->hz; return 1; }
        spin_us(1);
    }
    printf("COACHZ mmc %s: rcg didn't update its configuration\n", s->name);
    return 0;
}

/* dev_pm_opp_set_rate: the rate the RCG can make (floor), CX for its OPP first when going up, then the clock. */
static uint32_t opp_set_rate(struct sdcc *s, uint32_t target)
{
    const uint32_t sources = s->gpll6 ? CM_HS400_SOURCES : CM_STAGE1_SOURCES;
    const struct cm_freq *f = cm_rcg_floor(s->freqs, s->nfreqs, target, sources);
    if (!f) f = cm_rcg_floor(s->freqs, s->nfreqs, 0xffffffffu, sources);
    const unsigned level = cm_sdcc_opp_level(s->sdcc1, f->hz);
    if (level > s->cx_level) { if (coachz_rpmh_cx_request(s->cx_consumer, level)) printf("COACHZ mmc %s: cx vote failed\n", s->name); }
    if (!rcg_set(s, f->hz)) return 0;
    if (level < s->cx_level) (void)coachz_rpmh_cx_request(s->cx_consumer, level);
    s->cx_level = level;
    return f->hz;
}

/* ------------------------------------------------------------------ supplies (qcom-rpmh-regulator.c, mmc regulator.c) */
static int vreg_send(struct sdcc_vreg *r, uint32_t reg, uint32_t data, int wait)
{
    const uint32_t address = r->address + reg;
    return coachz_rpmh_write(&address, &data, 1, wait) == 0;
}
/* regulator registration: regulator-initial-mode = HPM (rpmh_regulator_vrm_set_mode, acknowledged). */
static int vreg_register(struct sdcc_vreg *r)
{
    if (!(r->address = coachz_cmd_db_address(r->v.resource))) return 0;
    r->mode_set = vreg_send(r, CM_VRM_MODE, CM_PMIC5_LDO_MODE_HPM, 1);
    return r->mode_set;
}
/* rpmh_regulator_vrm_set_voltage_sel: cached until the first enable/disable, acknowledged when raised. */
static int vreg_set_mv(struct sdcc_vreg *r, uint32_t mv)
{
    if (!mv) return 0;
    if (!r->known) { r->cached_mv = mv; return 1; }
    if (mv == r->mv) return 1;
    if (!vreg_send(r, CM_VRM_VOLTAGE, mv, mv > r->mv)) return 0;
    r->mv = mv;
    return 1;
}
/* rpmh_regulator_set_enable_state: the cached voltage first (acknowledged), then the enable (acknowledged when on). */
static int vreg_enable(struct sdcc_vreg *r, int on)
{
    if (!r->known && r->cached_mv) {
        if (!vreg_send(r, CM_VRM_VOLTAGE, r->cached_mv, 1)) return 0;
        r->mv = r->cached_mv;
    }
    if (!vreg_send(r, CM_VRM_ENABLE, on ? 1u : 0u, on)) return 0;
    r->known = 1;
    r->enabled = on;
    return 1;
}

/* sdhci_msm_set_vmmc -> mmc_regulator_set_ocr(mmc, vmmc, ios.vdd) */
static int set_vmmc(struct sdcc *s)
{
    if (s->ios.vdd) {
        int min_uv, max_uv;
        cm_ocrbit_to_vdd((int)s->ios.vdd, &min_uv, &max_uv);
        if (!vreg_set_mv(&s->vmmc, cm_vreg_mv(&s->vmmc.v, min_uv, max_uv))) return 0;
        if (!s->regulator_enabled) {
            if (!vreg_enable(&s->vmmc, 1)) return 0;
            s->regulator_enabled = 1;
        }
    } else if (s->regulator_enabled) {
        if (!vreg_enable(&s->vmmc, 0)) return 0;
        s->regulator_enabled = 0;
    }
    return 1;
}
/* sdhci_msm_set_vqmmc / msm_toggle_vqmmc */
static int set_vqmmc(struct sdcc *s, int level)
{
    if (s->ios.power_mode == PM_UNDEFINED) return 1;
    const int always_on = !s->removable && s->card_registered && s->card_mmc;
    if (always_on) return 1;   /* msm_config_vqmmc_mode: a load change only; no allow-set-load in the device tree */
    if (s->vqmmc_enabled == level) return 1;
    if (level) {
        int signal = CM_SIGNAL_330;
        if (s->caps0 & CM_CORE_3_0V_SUPPORT) signal = CM_SIGNAL_330;
        else if (s->caps0 & CM_CORE_1_8V_SUPPORT) signal = CM_SIGNAL_180;
        if ((s->caps0 & CM_CORE_VOLT_SUPPORT) && !vreg_set_mv(&s->vqmmc, cm_vqmmc_mv(&s->vqmmc.v, signal, (int)s->ios.vdd)))
            return 0;
        if (!vreg_enable(&s->vqmmc, 1)) return 0;
    } else if (!vreg_enable(&s->vqmmc, 0)) {
        return 0;
    }
    s->vqmmc_enabled = level;
    return 1;
}
/* sdhci_msm_set_pincfg: the default or sleep state (msm_config_group_set per group, pinctrl-sc7180.c). */
static int set_pincfg(struct sdcc *s, int level)
{
    wr32(s->sdc, 0, cm_pins_apply(rd32(s->sdc, 0), level ? s->pins_on : s->pins_off, s->npins));
    if (s->cd) wr32(s->cd, 0, cm_pin_apply(rd32(s->cd, 0), &cm_sd_cd_pin));
    return 1;
}
/* cd-gpios = <&tlmm 69 GPIO_ACTIVE_LOW>: the io register's input bit. */
static int card_present(struct sdcc *s)
{
    if (!s->removable) return 1;
    return !(rd32(s->cd, 4) & 1u);
}

/* ------------------------------------------------------------------ the power interrupt (sdhci_msm_handle_pwr_irq) */
static void handle_pwr_irq(struct sdcc *s)
{
    uint32_t status = rd32(s->hc, CM_MSM_PWRCTL_STATUS) & CM_PWRCTL_INT_MASK;
    wr32(s->hc, CM_MSM_PWRCTL_CLEAR, status);
    for (int retry = 10; status & rd32(s->hc, CM_MSM_PWRCTL_STATUS); retry--) {
        if (!retry) {
            printf("COACHZ mmc %s: timed out clearing (0x%x) pwrctl status, status 0x%x mask 0x%x ctl 0x%x\n", s->name, status,
                rd32(s->hc, CM_MSM_PWRCTL_STATUS), rd32(s->hc, CM_MSM_PWRCTL_MASK), rd32(s->hc, CM_MSM_PWRCTL_CTL));
            break;
        }
        wr32(s->hc, CM_MSM_PWRCTL_CLEAR, status);
        spin_us(10);
    }
    const struct cm_pwr_request r = cm_msm_pwr_decode(status, s->card_registered, card_present(s));
    if (r.card_gone) { wr32(s->hc, CM_MSM_PWRCTL_CTL, cm_msm_pwr_ack(&r, 0, 1)); return; }
    int bus_ok = 1, io_ok = 1;
    if (r.pwr_state) {
        const int on = (r.pwr_state & CM_REQ_BUS_ON) != 0;
        bus_ok = set_vmmc(s) && set_vqmmc(s, on) && set_pincfg(s, on);
    }
    if (r.set_vqmmc)
        io_ok = vreg_set_mv(&s->vqmmc, cm_vqmmc_mv(&s->vqmmc.v, (int)s->ios.signal, (int)s->ios.vdd));
    wr32(s->hc, CM_MSM_PWRCTL_CTL, cm_msm_pwr_ack(&r, bus_ok, io_ok));
    const uint32_t spec = rd32(s->hc, CM_MSM_VENDOR_SPEC), next = cm_msm_io_pad_spec(spec, s->caps0, r.io_level);
    if (spec != next) wr32(s->hc, CM_MSM_VENDOR_SPEC, next);
    if (r.pwr_state) s->curr_pwr_state = r.pwr_state;
    if (r.io_level) s->curr_io_level = r.io_level;
    s->pwr_irqs++;
    if (!bus_ok || !io_ok)
        printf("COACHZ mmc %s: pwr irq 0x%x handled with failure (bus %d io %d)\n", s->name, status, bus_ok, io_ok);
}

/* The threaded half of the one-shot power interrupt: handled, the waiter woken, then the line unmasked. */
static void pwr_thread(void *arg, wait_result_t result __unused)
{
    struct sdcc *s = arg;
    for (;;) {
        while (!wait_flag(&s->pwr_pending, &s->pwr_pending, 60000)) { }
        __atomic_store_n(&s->pwr_pending, 0, __ATOMIC_RELEASE);
        handle_pwr_irq(s);
        __atomic_store_n(&s->pwr_flag, 1, __ATOMIC_RELEASE);
        thread_wakeup(&s->pwr_flag);
        wr32(gicd, GICD_ISENABLER(s->pwr_intid), 1u << (s->pwr_intid % 32u));
    }
}
static void pwr_isr(void *arg)
{
    struct sdcc *s = arg;
    wr32(gicd, GICD_ICENABLER(s->pwr_intid), 1u << (s->pwr_intid % 32u));
    __atomic_store_n(&s->pwr_pending, 1, __ATOMIC_RELEASE);
    thread_wakeup(&s->pwr_pending);
}

/* sdhci_irq: read, clear what was read, and hand the bits to the waiting request. */
static void hc_isr(void *arg)
{
    struct sdcc *s = arg;
    const uint32_t status = rd32(s->hc, CM_SDHCI_INT_STATUS);
    if (!status || status == 0xffffffffu) { s->spurious++; return; }
    wr32(s->hc, CM_SDHCI_INT_STATUS, status);
    __atomic_fetch_or(&s->hc_status, status, __ATOMIC_ACQ_REL);
    s->hc_irqs++;
    thread_wakeup(&s->hc_status);
}

static void msm_write8(struct sdcc *s, uint32_t reg, uint8_t value);

/* sdhci_msm_check_power_status */
static void check_power_status(struct sdcc *s, uint32_t req)
{
    if (cm_msm_should_wait(req, s->host_pwr, s->curr_pwr_state, s->curr_io_level) &&
        !wait_flag(&s->pwr_flag, &s->pwr_flag, CM_MSM_PWR_IRQ_TIMEOUT_MS))
        printf("COACHZ mmc %s: pwr_irq for req: (%u) timed out\n", s->name, req);
    if ((req & CM_REQ_BUS_ON) && s->card_registered && !card_present(s)) {
        msm_write8(s, CM_SDHCI_POWER_CONTROL, 0);   /* sdhci_writeb goes through the same hook */
        s->host_pwr = 0;
    }
}
/* sdhci_msm_writeb / sdhci_msm_writew */
static void msm_write8(struct sdcc *s, uint32_t reg, uint8_t value)
{
    const uint32_t req = cm_msm_write_request(reg, value, s->host_pwr);
    if (req) { __atomic_store_n(&s->pwr_flag, 0, __ATOMIC_RELEASE); mb(); }
    wr8(s->hc, reg, value);
    if (req) check_power_status(s, req);
}
static void msm_write16(struct sdcc *s, uint32_t reg, uint16_t value)
{
    const uint32_t req = cm_msm_write_request(reg, value, s->host_pwr);
    if (req) { __atomic_store_n(&s->pwr_flag, 0, __ATOMIC_RELEASE); mb(); }
    wr16(s->hc, reg, value);
    if (req) check_power_status(s, req);
}

/* ------------------------------------------------------------------ sdhci host operations */
static void sdhci_reset(struct sdcc *s, uint8_t mask)
{
    msm_write8(s, CM_SDHCI_SOFTWARE_RESET, mask);
    if (mask & CM_RESET_ALL) s->host_clock = 0;
    const uint64_t end = now_us() + 100000u;
    while (rd8(s->hc, CM_SDHCI_SOFTWARE_RESET) & mask) {
        if (now_us() > end) { printf("COACHZ mmc %s: reset 0x%x never completed\n", s->name, mask); return; }
        spin_us(10);
    }
}

static void enable_clk(struct sdcc *s, uint16_t clk)
{
    clk |= CM_CLOCK_INT_EN;
    wr16(s->hc, CM_SDHCI_CLOCK_CONTROL, clk);
    const uint64_t end = now_us() + 150000u;
    while (!((clk = rd16(s->hc, CM_SDHCI_CLOCK_CONTROL)) & CM_CLOCK_INT_STABLE)) {
        if (now_us() > end) { printf("COACHZ mmc %s: internal clock never stabilised\n", s->name); return; }
        spin_us(10);
    }
    wr16(s->hc, CM_SDHCI_CLOCK_CONTROL, clk | CM_CLOCK_CARD_EN);
}

/* msm_cm_dll_set_freq: the MCLK range of host->clock, 112 MHz and up in steps. */
static void dll_set_freq(struct sdcc *s)
{
    static const uint32_t top[] = {112000000u, 125000000u, 137000000u, 150000000u, 162000000u, 175000000u, 187000000u, 200000000u};
    uint32_t mclk = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (s->host_clock <= top[i]) { mclk = i; break; }
    }
    wr32(s->hc, CM_MSM_DLL_CONFIG, (rd32(s->hc, CM_MSM_DLL_CONFIG) & ~CM_MSM_CMUX_SHIFT_PHASE_MASK) | mclk << CM_MSM_CMUX_SHIFT_PHASE_SHIFT);
}

/* msm_init_cm_dll for this controller (MCI version 0x72: 14lpp DLL reset, tassadar DLL), no qcom,dll-config in its device tree. */
static int dll_init(struct sdcc *s)
{
    wr32(s->hc, CM_MSM_VENDOR_SPEC, rd32(s->hc, CM_MSM_VENDOR_SPEC) & ~CM_MSM_CLK_PWRSAVE);
    wr32(s->hc, CM_MSM_DLL_CONFIG, rd32(s->hc, CM_MSM_DLL_CONFIG) & ~CM_MSM_CK_OUT_EN);
    wr32(s->hc, CM_MSM_DLL_CONFIG_2, rd32(s->hc, CM_MSM_DLL_CONFIG_2) | CM_MSM_DLL_CLOCK_DISABLE);
    wr32(s->hc, CM_MSM_DLL_CONFIG, rd32(s->hc, CM_MSM_DLL_CONFIG) | CM_MSM_DLL_RST);
    wr32(s->hc, CM_MSM_DLL_CONFIG, rd32(s->hc, CM_MSM_DLL_CONFIG) | CM_MSM_DLL_PDN);
    dll_set_freq(s);
    uint32_t config = rd32(s->hc, CM_MSM_DLL_CONFIG_2);
    const uint64_t cycles = (uint64_t)s->host_clock * (config & CM_MSM_FLL_CYCLE_CNT ? 8u : 4u);
    const uint32_t mclk = (uint32_t)((cycles + 19200000u / 2u) / 19200000u);   /* DIV_ROUND_CLOSEST_ULL by XO */
    wr32(s->hc, CM_MSM_DLL_CONFIG_2, (config & ~(0xffu << 10)) | mclk << 10);
    spin_us(5);
    wr32(s->hc, CM_MSM_DLL_CONFIG, rd32(s->hc, CM_MSM_DLL_CONFIG) & ~CM_MSM_DLL_RST);
    wr32(s->hc, CM_MSM_DLL_CONFIG, rd32(s->hc, CM_MSM_DLL_CONFIG) & ~CM_MSM_DLL_PDN);
    dll_set_freq(s);
    wr32(s->hc, CM_MSM_DLL_CONFIG_2, rd32(s->hc, CM_MSM_DLL_CONFIG_2) & ~CM_MSM_DLL_CLOCK_DISABLE);
    wr32(s->hc, CM_MSM_DLL_USR_CTL, CM_MSM_DLL_USR_CTL_VALUE);
    wr32(s->hc, CM_MSM_DLL_CONFIG_3, (rd32(s->hc, CM_MSM_DLL_CONFIG_3) & ~0xffu) | (s->clk_rate < 150000000u ? CM_MSM_DLL_CONFIG_3_LOW : CM_MSM_DLL_CONFIG_3_HIGH));
    wr32(s->hc, CM_MSM_DLL_CONFIG, rd32(s->hc, CM_MSM_DLL_CONFIG) | CM_MSM_DLL_EN);
    wr32(s->hc, CM_MSM_DLL_CONFIG, rd32(s->hc, CM_MSM_DLL_CONFIG) | CM_MSM_CK_OUT_EN);
    for (unsigned waited = 0; !(rd32(s->hc, CM_MSM_DLL_STATUS) & CM_MSM_DLL_LOCK); waited++) {
        if (waited >= 50) { printf("COACHZ mmc %s: DLL failed to lock (0x%x)\n", s->name, rd32(s->hc, CM_MSM_DLL_STATUS)); return -ETIMEDOUT; }
        spin_us(1);
    }
    return 0;
}

/* sdhci_msm_hs400_dll_calibration with enhanced strobe: no tuning phase, then sdhci_msm_cm_dll_sdc4_calibration. */
static int dll_calibrate(struct sdcc *s)
{
    const int err = dll_init(s);
    if (err) return err;
    wr32(s->hc, CM_MSM_DDR_CONFIG, CM_MSM_DDR_CONFIG_POR);
    wr32(s->hc, CM_MSM_DDR_200_CFG, rd32(s->hc, CM_MSM_DDR_200_CFG) | CM_MSM_CMDIN_RCLK_EN);
    wr32(s->hc, CM_MSM_DLL_CONFIG_2, rd32(s->hc, CM_MSM_DLL_CONFIG_2) | CM_MSM_DDR_CAL_EN);
    for (unsigned waited = 0; !(rd32(s->hc, CM_MSM_DLL_STATUS) & CM_MSM_DDR_DLL_LOCK); waited += 10) {
        if (waited >= 1000) { printf("COACHZ mmc %s: CM_DLL_SDC4 calibration not completed (0x%x)\n", s->name, rd32(s->hc, CM_MSM_DLL_STATUS)); return -ETIMEDOUT; }
        spin_us(10);
    }
    mb();
    return 0;
}

/* sdhci_msm_set_clock: hc_select_mode (msm_hc_select_hs400 or default), the GCC rate for the bus mode, then the SDHCI clock without a divider. */
static void msm_set_clock(struct sdcc *s, unsigned clock)
{
    if (clock) {
        const int hs400 = s->ios.timing == CM_TIMING_MMC_HS400;
        if (hs400) {
            uint32_t spec = (rd32(s->hc, CM_MSM_VENDOR_SPEC) & ~CM_MSM_HC_MCLK_SEL_MASK) | CM_MSM_HC_MCLK_SEL_HS400;
            wr32(s->hc, CM_MSM_VENDOR_SPEC, spec);
            if (s->enhanced_strobe && !s->calibration_done) wr32(s->hc, CM_MSM_VENDOR_SPEC, spec | CM_MSM_HC_SELECT_IN_HS400 | CM_MSM_HC_SELECT_IN_EN);
        } else {
            if (!s->cdclp533) wr32(s->hc, CM_MSM_VENDOR_SPEC3, rd32(s->hc, CM_MSM_VENDOR_SPEC3) & ~CM_MSM_PWRSAVE_DLL);
            uint32_t spec = rd32(s->hc, CM_MSM_VENDOR_SPEC);
            spec = (spec & ~CM_MSM_HC_MCLK_SEL_MASK) | CM_MSM_HC_MCLK_SEL_DFLT;
            wr32(s->hc, CM_MSM_VENDOR_SPEC, spec);
            wr32(s->hc, CM_MSM_VENDOR_SPEC, rd32(s->hc, CM_MSM_VENDOR_SPEC) & ~(CM_MSM_HC_SELECT_IN_EN | CM_MSM_HC_SELECT_IN_MASK));
        }
        mb();
        /* msm_get_clock_mult_for_bus_mode: HS400 is DDR off a clock of twice the bus rate. */
        const uint32_t rate = opp_set_rate(s, s->ios.clock * (hs400 ? 2u : 1u));
        if (!rate) printf("COACHZ mmc %s: failed to set clock at rate %u\n", s->name, s->ios.clock);
        s->clk_rate = s->ios.clock * (hs400 ? 2u : 1u);
    }
    wr16(s->hc, CM_SDHCI_CLOCK_CONTROL, 0);
    if (clock) enable_clk(s, rd16(s->hc, CM_SDHCI_CLOCK_CONTROL));
}

/* sdhci_msm_set_uhs_signaling for the stage-1 timings, including the DLL reset below 100 MHz. */
static void msm_set_uhs(struct sdcc *s)
{
    uint16_t ctrl2 = cm_msm_uhs_ctrl2(rd16(s->hc, CM_SDHCI_HOST_CONTROL2), s->ios.timing);
    if (s->host_clock <= CM_MSM_CORE_FREQ_100MHZ) {
        /* Below 100 MHz the feedback clock is used, not the DLL, whatever the timing. */
        ctrl2 &= (uint16_t)~CM_CTRL2_UHS_MASK;
        wr32(s->hc, CM_MSM_DLL_CONFIG, rd32(s->hc, CM_MSM_DLL_CONFIG) | CM_MSM_DLL_RST);
        wr32(s->hc, CM_MSM_DLL_CONFIG, rd32(s->hc, CM_MSM_DLL_CONFIG) | CM_MSM_DLL_PDN);
        s->calibration_done = 0;
    }
    msm_write16(s, CM_SDHCI_HOST_CONTROL2, ctrl2);
    /* sdhci_msm_hs400 */
    if (s->ios.timing == CM_TIMING_MMC_HS400 && s->host_clock > CM_MSM_CORE_FREQ_100MHZ && s->enhanced_strobe && !s->calibration_done) {
        s->calibration_err = dll_calibrate(s);
        s->calibration_done = !s->calibration_err;
        if (s->calibration_err) printf("COACHZ mmc %s: failed to calibrate DLL for HS400 (%d)\n", s->name, s->calibration_err);
    }
}

/* sdhci_set_power_noreg with SDHCI_QUIRK_SINGLE_POWER_WRITE */
static void set_power(struct sdcc *s)
{
    const uint8_t pwr = s->ios.power_mode != PM_OFF ? cm_sdhci_vdd((int)s->ios.vdd) : 0;
    if (s->ios.power_mode != PM_OFF && !pwr) printf("COACHZ mmc %s: invalid vdd %u\n", s->name, s->ios.vdd);
    if (s->host_pwr == pwr) return;
    s->host_pwr = pwr;
    msm_write8(s, CM_SDHCI_POWER_CONTROL, pwr ? (uint8_t)(pwr | CM_POWER_ON) : 0);
}

static void set_bus_width(struct sdcc *s)
{
    uint8_t ctrl = rd8(s->hc, CM_SDHCI_HOST_CONTROL);
    if (s->ios.bus_width == CM_BUS_WIDTH_8) {
        ctrl = (uint8_t)((ctrl & ~CM_CTRL_4BITBUS) | CM_CTRL_8BITBUS);
    } else {
        if (s->dt_bus_width == 8) ctrl &= (uint8_t)~CM_CTRL_8BITBUS;
        if (s->ios.bus_width == CM_BUS_WIDTH_4) ctrl |= CM_CTRL_4BITBUS;
        else ctrl &= (uint8_t)~CM_CTRL_4BITBUS;
    }
    wr8(s->hc, CM_SDHCI_HOST_CONTROL, ctrl);
}

/* sdhci_set_ios */
static void set_ios(struct sdcc *s)
{
    const int reinit_uhs = s->reinit_uhs;
    s->reinit_uhs = 0;
    if (s->ios.power_mode == PM_UNDEFINED) return;
    const int turning_on_clk = s->ios.clock != s->host_clock && s->ios.clock && !s->host_clock;
    if (!s->ios.clock || s->ios.clock != s->host_clock) {
        msm_set_clock(s, s->ios.clock);
        s->host_clock = s->ios.clock;
    }
    set_power(s);
    set_bus_width(s);
    if (!reinit_uhs && turning_on_clk && s->host_timing == s->ios.timing && s->version >= CM_SPEC_300) return;
    uint8_t ctrl = rd8(s->hc, CM_SDHCI_HOST_CONTROL);
    if (s->ios.timing == CM_TIMING_SD_HS || s->ios.timing == CM_TIMING_MMC_HS || s->ios.timing == CM_TIMING_MMC_HS400) ctrl |= CM_CTRL_HISPD;
    else ctrl &= (uint8_t)~CM_CTRL_HISPD;
    if (s->version >= CM_SPEC_300) {
        uint16_t clk = rd16(s->hc, CM_SDHCI_CLOCK_CONTROL);
        if (clk & CM_CLOCK_CARD_EN) wr16(s->hc, CM_SDHCI_CLOCK_CONTROL, clk & (uint16_t)~CM_CLOCK_CARD_EN);
        wr8(s->hc, CM_SDHCI_HOST_CONTROL, ctrl);
        /* preset values are broken (SDHCI_QUIRK2_PRESET_VALUE_BROKEN): driver type B */
        msm_write16(s, CM_SDHCI_HOST_CONTROL2, rd16(s->hc, CM_SDHCI_HOST_CONTROL2) & (uint16_t)~CM_CTRL2_DRV_TYPE_MASK);
        msm_set_uhs(s);
        s->host_timing = s->ios.timing;
        msm_set_clock(s, s->host_clock);
    } else {
        wr8(s->hc, CM_SDHCI_HOST_CONTROL, ctrl);
    }
}

/* sdhci_msm_start_signal_voltage_switch */
static int signal_voltage_switch(struct sdcc *s, unsigned signal)
{
    if (s->version < CM_SPEC_300) return 0;
    uint16_t ctrl = rd16(s->hc, CM_SDHCI_HOST_CONTROL2);
    if (signal == CM_SIGNAL_330) { if (!s->signaling_330) return -EINVAL; ctrl &= (uint16_t)~CM_CTRL2_VDD_180; }
    else { if (!s->signaling_180) return -EINVAL; ctrl |= CM_CTRL2_VDD_180; }
    msm_write16(s, CM_SDHCI_HOST_CONTROL2, ctrl);
    sleep_ms(5);
    const uint16_t want = ctrl & CM_CTRL2_VDD_180;
    if ((rd16(s->hc, CM_SDHCI_HOST_CONTROL2) & CM_CTRL2_VDD_180) == want) return 0;
    printf("COACHZ mmc %s: regulator output did not become stable\n", s->name);
    return -EAGAIN;
}
/* mmc_set_signal_voltage */
static int set_signal_voltage(struct sdcc *s, unsigned signal)
{
    const unsigned old = s->ios.signal;
    s->ios.signal = signal;
    const int err = signal_voltage_switch(s, signal);
    if (err) s->ios.signal = old;
    return err;
}

/* ------------------------------------------------------------------ requests (sdhci_send_command, sdhci_irq) */
static int wait_int(struct sdcc *s, uint32_t any, unsigned ms, uint32_t *status)
{
    const uint64_t began = now_us(), end = began + (uint64_t)ms * 1000u;
    /* A response or a short transfer ends within this; blocking at once costs a wakeup that took longer than the transfer (1.1 ms per
       64 KiB at HS400, of which the bus took 0.16). Longer waits still sleep. */
    while (now_us() - began < 300u) {
        const uint32_t early = __atomic_load_n(&s->hc_status, __ATOMIC_ACQUIRE);
        if (early & any) { *status = early; return 1; }
        __asm__ volatile ("yield");
    }
    for (;;) {
        uint32_t st = __atomic_load_n(&s->hc_status, __ATOMIC_ACQUIRE);
        if (st & any) { *status = st; return 1; }
        if (now_us() >= end) { *status = st; return 0; }
        uint64_t deadline;
        clock_interval_to_deadline(10, NSEC_PER_MSEC, &deadline);
        (void)assert_wait_deadline(&s->hc_status, THREAD_UNINT, deadline);
        if (__atomic_load_n(&s->hc_status, __ATOMIC_ACQUIRE) & any) thread_wakeup(&s->hc_status);
        thread_block(THREAD_CONTINUE_NULL);
    }
}
static void consume(struct sdcc *s, uint32_t bits) { __atomic_fetch_and(&s->hc_status, ~bits, __ATOMIC_ACQ_REL); }

static int issue(struct sdcc *s, struct sdcc_cmd *cmd, const struct sdcc_data *data)
{
    const int line = cm_data_line_cmd(cmd->flags, data != NULL);
    uint32_t mask = CM_PS_CMD_INHIBIT | (line ? CM_PS_DATA_INHIBIT : 0);
    if (cmd == s->stop_cmd) mask &= ~CM_PS_DATA_INHIBIT;
    for (unsigned tries = 0; rd32(s->hc, CM_SDHCI_PRESENT_STATE) & mask; tries++) {
        if (tries >= 10) { printf("COACHZ mmc %s: controller never released inhibit bit(s)\n", s->name); return -EIO; }
        sleep_ms(1);
    }
    if (line) {
        /* __sdhci_set_timeout: busy commands from their timeout, data at the hardware maximum. */
        const uint64_t target = data ? 0 : (uint64_t)cmd->busy_timeout_ms * 1000u;
        wr8(s->hc, CM_SDHCI_TIMEOUT_CONTROL, (uint8_t)cm_timeout_count(s->timeout_khz, target, CM_MSM_MAX_TIMEOUT_COUNT));
    }
    if (data) {
        uint8_t *table = (uint8_t *)(s->pool + ADMA_OFF);
        if (!cm_adma_table(table, ADMA_BYTES, data->pa, data->blksz * data->blocks)) return -EINVAL;
        mb();
        const uint64_t table_pa = s->pool_pa + ADMA_OFF;
        wr32(s->hc, CM_SDHCI_ADMA_ADDRESS, (uint32_t)table_pa);
        if (s->adma64) wr32(s->hc, CM_SDHCI_ADMA_ADDRESS_HI, (uint32_t)(table_pa >> 32));
        uint8_t ctrl = rd8(s->hc, CM_SDHCI_HOST_CONTROL) & (uint8_t)~CM_CTRL_DMA_MASK;
        ctrl |= s->adma64 ? CM_CTRL_ADMA64 : CM_CTRL_ADMA32;
        wr8(s->hc, CM_SDHCI_HOST_CONTROL, ctrl);
        wr16(s->hc, CM_SDHCI_BLOCK_SIZE, cm_block_size(data->blksz));
        wr16(s->hc, CM_SDHCI_BLOCK_COUNT, (uint16_t)data->blocks);
    }
    wr32(s->hc, CM_SDHCI_ARGUMENT, cmd->arg);
    if (!data) {
        const uint16_t mode = rd16(s->hc, CM_SDHCI_TRANSFER_MODE);
        wr16(s->hc, CM_SDHCI_TRANSFER_MODE, mode & (uint16_t)~(CM_TRNS_AUTO_CMD12 | CM_TRNS_AUTO_CMD23));
    } else {
        const int multi = cmd->opcode == CM_MMC_READ_MULTIPLE_BLOCK || data->blocks > 1;
        const int a23 = data->sbc && s->auto23, a12 = !data->sbc && s->auto12 && multi;
        if (multi && a23) wr32(s->hc, CM_SDHCI_DMA_ADDRESS, data->sbc->arg);   /* ARGUMENT2 */
        wr16(s->hc, CM_SDHCI_TRANSFER_MODE, cm_transfer_mode(cmd->opcode, data->blocks, data->read, 1, a12, a23));
    }
    wr16(s->hc, CM_SDHCI_COMMAND, cm_sdhci_command(cmd->opcode, cmd->flags, data != NULL));
    return 0;
}

/* One request: the command, its busy end or its data, and the reset of the command and data circuits after an error
   (sdhci_request_done). Returns the command error, else the data error. */
static int request(struct sdcc *s, struct sdcc_cmd *cmd, struct sdcc_data *data)
{
    cmd->error = 0;
    if (data) data->error = 0;
    __atomic_store_n(&s->hc_status, 0, __ATOMIC_RELEASE);
    int err = issue(s, cmd, data);
    if (err) { cmd->error = err; return err; }
    uint32_t st;
    const unsigned limit_ms = cmd->busy_timeout_ms > 9000u ? cmd->busy_timeout_ms + 1000u : 10000u;
    if (!wait_int(s, CM_INT_RESPONSE | CM_INT_CMD_ERRORS | CM_INT_AUTO_CMD_ERR, limit_ms, &st)) {
        printf("COACHZ mmc %s: CMD%u timed out in software, status 0x%x\n", s->name, cmd->opcode, st);
        cmd->error = -ETIMEDOUT;
    } else if ((st & CM_INT_AUTO_CMD_ERR) && data && !data->sbc && s->auto12) {
        data->error = (rd16(s->hc, CM_SDHCI_AUTO_CMD_STATUS) & CM_AUTO_CMD_TIMEOUT) ? -ETIMEDOUT : -EILSEQ;
    } else if ((st & CM_INT_AUTO_CMD_ERR) && data && data->sbc && s->auto23) {
        /* an Auto-CMD23 failure is the set-block-count command's error, which ends the request */
        cmd->error = (rd16(s->hc, CM_SDHCI_AUTO_CMD_STATUS) & CM_AUTO_CMD_TIMEOUT) ? -ETIMEDOUT : -EILSEQ;
    } else if (st & CM_INT_CMD_ERRORS) {
        if (data && (st & (CM_INT_CRC | CM_INT_TIMEOUT)) == CM_INT_CRC) data->error = -EILSEQ;
        else cmd->error = (st & CM_INT_TIMEOUT) ? -ETIMEDOUT : -EILSEQ;
    } else {
        consume(s, CM_INT_RESPONSE);
        if (cmd->flags & CM_RSP_136) {
            uint32_t regs[4];
            for (unsigned i = 0; i < 4; i++) regs[i] = rd32(s->hc, CM_SDHCI_RESPONSE + 4u * i);
            cm_rsp136(regs, cmd->resp);
        } else if (cmd->flags & CM_RSP_PRESENT) {
            cmd->resp[0] = rd32(s->hc, CM_SDHCI_RESPONSE);
        }
        if (data || (cmd->flags & CM_RSP_BUSY)) {
            /* Data, or the busy end of an R1B command, both signalled by DATA_END. */
            if (!wait_int(s, CM_INT_DATA_END | CM_INT_DATA_ERRORS | CM_INT_AUTO_CMD_ERR, limit_ms, &st)) {
                if (data) data->error = -ETIMEDOUT; else cmd->error = -ETIMEDOUT;
            } else if (st & CM_INT_DATA_ERRORS) {
                const int e = (st & CM_INT_DATA_TIMEOUT) ? -ETIMEDOUT : (st & CM_INT_ADMA_ERROR) ? -EIO : -EILSEQ;
                if (st & CM_INT_ADMA_ERROR)
                    printf("COACHZ mmc %s: ADMA error 0x%x, adma error 0x%x\n", s->name, st, rd32(s->hc, CM_SDHCI_ADMA_ERROR));
                if (data) data->error = e; else cmd->error = e;
            } else if (st & CM_INT_AUTO_CMD_ERR) {
                if (data) data->error = (rd16(s->hc, CM_SDHCI_AUTO_CMD_STATUS) & CM_AUTO_CMD_TIMEOUT) ? -ETIMEDOUT : -EILSEQ;
            }
        }
    }
    if (cmd->error || (data && data->error)) {
        sdhci_reset(s, CM_RESET_CMD);
        sdhci_reset(s, CM_RESET_DATA);
        /* sdhci_finish_data: a failed multi-block transfer still gets its stop command */
        if (data && data->stop && data->error) {
            struct sdcc_cmd *stop = data->stop;
            stop->resp[0] = 0;
            s->stop_cmd = stop;
            (void)request(s, stop, NULL);
            s->stop_cmd = NULL;
        }
    }
    return cmd->error ? cmd->error : data ? data->error : 0;
}

/* mmc_wait_for_cmd: retries on error */
static int send_cmd(struct sdcc *s, struct sdcc_cmd *cmd, unsigned retries)
{
    int err;
    do { err = request(s, cmd, NULL); } while (err && retries-- > 0);
    return err;
}
static int send_data(struct sdcc *s, unsigned opcode, uint32_t arg, void *buffer, unsigned len)
{
    /* mmc_send_adtc_data into the slot's scratch block */
    struct sdcc_cmd cmd = { opcode, arg, CM_RSP_R1, {0}, 0, 0 };
    struct sdcc_data data = { len, 1, s->pool_pa + SCRATCH_OFF, 1, NULL, NULL, 0 };
    const int err = request(s, &cmd, &data);
    if (!err) __builtin_memcpy(buffer, (const void *)(s->pool + SCRATCH_OFF), len);
    return err;
}
/* mmc_app_cmd + mmc_wait_for_app_cmd */
static int app_cmd(struct sdcc *s, int with_card)
{
    struct sdcc_cmd cmd = { CM_MMC_APP_CMD, with_card ? s->rca << 16 : 0, CM_RSP_R1, {0}, 0, 0 };
    const int err = send_cmd(s, &cmd, 0);
    if (err) return err;
    return (cmd.resp[0] & CM_R1_APP_CMD) ? 0 : -EOPNOTSUPP;
}
static int send_app_cmd(struct sdcc *s, int with_card, struct sdcc_cmd *cmd)
{
    int err = -EIO;
    for (unsigned i = 0; i <= CM_MMC_CMD_RETRIES; i++) {
        if ((err = app_cmd(s, with_card))) continue;
        cmd->resp[0] = cmd->resp[1] = cmd->resp[2] = cmd->resp[3] = 0;
        if (!(err = request(s, cmd, NULL))) break;
    }
    return err;
}

/* ------------------------------------------------------------------ power (core.c) */
static void set_initial_state(struct sdcc *s)
{
    s->ios.bus_width = CM_BUS_WIDTH_1;
    s->ios.timing = CM_TIMING_LEGACY;
    s->ios.drv_type = 0;
    set_ios(s);
}
static void power_up(struct sdcc *s, uint32_t ocr)
{
    if (s->ios.power_mode == PM_ON) return;
    s->ios.vdd = (unsigned)(cm_fls(ocr) - 1);
    s->ios.power_mode = PM_UP;
    set_initial_state(s);
    if (set_signal_voltage(s, CM_SIGNAL_330) && set_signal_voltage(s, CM_SIGNAL_180))
        printf("COACHZ mmc %s: no initial signal voltage\n", s->name);
    sleep_ms(10);   /* ios.power_delay_ms */
    s->ios.clock = s->f_min > CM_MSM_MIN_CLOCK ? s->f_min : CM_MSM_MIN_CLOCK;   /* f_init: max(400 kHz, f_min) */
    s->ios.power_mode = PM_ON;
    set_ios(s);
    sleep_ms(10);
}
static void power_off(struct sdcc *s)
{
    if (s->ios.power_mode == PM_OFF) return;
    s->ios.clock = 0;
    s->ios.vdd = 0;
    s->ios.power_mode = PM_OFF;
    set_initial_state(s);
    spin_us(1000);   /* ios.power_off_delay_us */
}
static void set_clock(struct sdcc *s, unsigned hz) { s->ios.clock = hz > s->f_max ? s->f_max : hz; set_ios(s); }
static void set_timing(struct sdcc *s, unsigned timing) { s->ios.timing = timing; set_ios(s); }
static void set_width(struct sdcc *s, unsigned width) { s->ios.bus_width = width; set_ios(s); }

static int go_idle(struct sdcc *s)
{
    sleep_ms(1);
    struct sdcc_cmd cmd = { CM_MMC_GO_IDLE_STATE, 0, CM_RSP_NONE, {0}, 0, 0 };
    const int err = request(s, &cmd, NULL);
    sleep_ms(1);
    return err;
}

/* __mmc_poll_for_busy around one probe command: period doubling up to 32 ms. */
static int poll_ready(struct sdcc *s, int app, struct sdcc_cmd *cmd, uint32_t ocr, unsigned period_us, unsigned timeout_ms)
{
    const uint64_t end = now_us() + (uint64_t)timeout_ms * 1000u + 1000u;
    unsigned delay = period_us ? period_us : 32u;
    for (;;) {
        const int expired = now_us() > end;
        const int err = app ? send_app_cmd(s, 0, cmd) : send_cmd(s, cmd, 0);
        if (err) return err;
        int busy;
        if (app) busy = ocr != 0 && !(cmd->resp[0] & CM_MMC_CARD_BUSY);
        else {
            busy = !(cmd->resp[0] & CM_MMC_CARD_BUSY);
            if (busy && !ocr) cmd->arg = cmd->resp[0] | (1u << 30);
        }
        if (!busy) return 0;
        if (expired) { printf("COACHZ mmc %s: card stuck being busy\n", s->name); return -ETIMEDOUT; }
        sleep_us(delay);
        if (delay < 32768u) delay *= 2u;
    }
}

/* ------------------------------------------------------------------ MMC (mmc.c, mmc_ops.c) */
static int mmc_send_status(struct sdcc *s, uint32_t *status)
{
    struct sdcc_cmd cmd = { CM_MMC_SEND_STATUS, s->rca << 16, CM_RSP_R1, {0}, 0, 0 };
    const int err = send_cmd(s, &cmd, CM_MMC_CMD_RETRIES);
    if (!err) *status = cmd.resp[0];
    return err;
}
/* __mmc_switch with MMC_CAP_WAIT_WHILE_BUSY | MMC_CAP_NEED_RSP_BUSY: R1B, no CMD13 polling, then the switch status. */
static int mmc_switch(struct sdcc *s, unsigned index, unsigned value, unsigned timeout_ms, unsigned timing)
{
    const unsigned old_timing = s->ios.timing;
    struct sdcc_cmd cmd = { CM_MMC_SWITCH, cm_mmc_switch_arg(index, value), CM_RSP_R1B, {0}, timeout_ms ? timeout_ms : s->ext.generic_cmd6_ms, 0 };
    int err = send_cmd(s, &cmd, CM_MMC_CMD_RETRIES);
    if (err) return err;
    if (timing) set_timing(s, timing);
    uint32_t status;
    err = mmc_send_status(s, &status);
    if (!err) {
        if (status & CM_R1_STATUS_MASK) printf("COACHZ mmc %s: unexpected status 0x%x after switch\n", s->name, status);
        if (status & CM_R1_SWITCH_ERROR) err = -EBADMSG;
    }
    if (err && timing) set_timing(s, old_timing);
    return err;
}

/* clk_alpha_pll_fixed_fabia, in FSM mode: vote for GPLL6 (enable_reg 0x52010 bit 6, gcc-sc7180.c) and wait for its active flag. */
static int gpll6_enable(void)
{
    const vm_offset_t vote = ml_io_map(0x152000u, 0x1000);
    if (!vote) return 0;
    const uint32_t mode = rd32(gcc, GCC_OFF(0x13000u));
    if (!(mode & (1u << 20))) {   /* PLL_VOTE_FSM_ENA */
        printf("COACHZ mmc: GPLL6 mode 0x%x is not in FSM mode, not used\n", mode);
        return 0;
    }
    wr32(vote, 0x10, rd32(vote, 0x10) | 1u << 6);
    for (unsigned waited = 0; waited < 200; waited++) {
        if (rd32(gcc, GCC_OFF(0x13000u)) & (1u << 31)) return 1;   /* PLL_ACTIVE_FLAG */
        spin_us(5);
    }
    printf("COACHZ mmc: GPLL6 did not become active (mode 0x%x)\n", rd32(gcc, GCC_OFF(0x13000u)));
    return 0;
}

/* mmc_select_hs400es from 8-bit high-speed, then mmc_set_bus_speed: no tuning, the card clocks data out on its strobe. The switches
   only change BUS_WIDTH and HS_TIMING, which are lost at power-off and reset. */
static int mmc_select_hs400es(struct sdcc *s)
{
    if (!(s->ext.card_type & CM_EXT_CSD_CARD_TYPE_HS400_1_8V) || !s->ext.strobe_support || !s->signaling_180 ||
        s->ios.bus_width != CM_BUS_WIDTH_8 || s->ios.timing != CM_TIMING_MMC_HS) {
        printf("COACHZ mmc %s: HS400 not possible: card type 0x%x, strobe %u, 1.8 V %d, width %u, timing %u\n", s->name, s->ext.card_type,
            s->ext.strobe_support, s->signaling_180, s->ios.bus_width, s->ios.timing);
        return 0;
    }
    if (!gpll6_enable()) return 0;
    s->gpll6 = 1;
    s->f_max = cm_rcg_max(s->freqs, s->nfreqs, CM_HS400_SOURCES);   /* sdhci_get_max_clock: the fastest the RCG can now make */
    int err = set_signal_voltage(s, CM_SIGNAL_180);
    if (!err) err = mmc_switch(s, CM_EXT_CSD_BUS_WIDTH, CM_EXT_CSD_DDR_BUS_WIDTH_8 | CM_EXT_CSD_BUS_WIDTH_STROBE, s->ext.generic_cmd6_ms, 0);
    if (!err) err = mmc_switch(s, CM_EXT_CSD_HS_TIMING, CM_EXT_CSD_TIMING_HS400, s->ext.generic_cmd6_ms, CM_TIMING_MMC_HS400);
    if (err) {
        printf("COACHZ mmc %s: switch to HS400 failed (%d)\n", s->name, err);
        return err;
    }
    s->enhanced_strobe = 1;
    set_clock(s, CM_MMC_HS200_MAX_DTR);
    if (s->calibration_err) return s->calibration_err;
    /* The switch status at the new rate: the first command through the calibrated DLL. */
    uint32_t status = 0;
    err = mmc_send_status(s, &status);
    printf("COACHZ mmc %s: HS400 enhanced strobe at %u Hz (rcg %u), DLL status 0x%x, card status 0x%x (%d)\n", s->name, s->ios.clock, s->rcg_hz,
        rd32(s->hc, CM_MSM_DLL_STATUS), status, err);
    return err;
}

static int mmc_attach(struct sdcc *s)
{
    unsigned emmc_mode = 0;
    s->hs400_wanted = s->sdcc1 && PE_parse_boot_argn("coachz-emmc", &emmc_mode, sizeof(emmc_mode)) && emmc_mode == 3;
    struct sdcc_cmd op = { CM_MMC_SEND_OP_COND, 0, CM_RSP_R3, {0}, 0, 0 };
    set_ios(s);   /* mmc_set_bus_mode(OPENDRAIN): SDHCI has no open-drain control; the call still runs set_ios */
    int err = poll_ready(s, 0, &op, 0, CM_MMC_OP_COND_PERIOD_US, CM_MMC_OP_COND_TIMEOUT_MS);
    if (err) return err;
    const uint32_t rocr = cm_select_voltage(op.resp[0], s->ocr_avail);
    if (!rocr) { printf("COACHZ mmc %s: no support for card's volts (ocr 0x%x)\n", s->name, op.resp[0]); return -EINVAL; }

    /* mmc_init_card */
    set_ios(s);
    go_idle(s);
    struct sdcc_cmd init = { CM_MMC_SEND_OP_COND, rocr | (1u << 30), CM_RSP_R3, {0}, 0, 0 };
    if ((err = poll_ready(s, 0, &init, rocr | (1u << 30), CM_MMC_OP_COND_PERIOD_US, CM_MMC_OP_COND_TIMEOUT_MS))) return err;
    const uint32_t card_ocr = init.resp[0];
    struct sdcc_cmd cid = { CM_MMC_ALL_SEND_CID, 0, CM_RSP_R2, {0}, 0, 0 };
    if ((err = send_cmd(s, &cid, CM_MMC_CMD_RETRIES))) return err;
    __builtin_memcpy(s->raw_cid, cid.resp, sizeof(s->raw_cid));
    s->rca = 1;
    s->card_mmc = 1;
    struct sdcc_cmd rca = { CM_MMC_SET_RELATIVE_ADDR, s->rca << 16, CM_RSP_R1, {0}, 0, 0 };
    if ((err = send_cmd(s, &rca, CM_MMC_CMD_RETRIES))) return err;
    set_ios(s);   /* PUSHPULL */
    struct sdcc_cmd csd = { CM_MMC_SEND_CSD, s->rca << 16, CM_RSP_R2, {0}, 0, 0 };
    if ((err = send_cmd(s, &csd, CM_MMC_CMD_RETRIES))) return err;
    __builtin_memcpy(s->raw_csd, csd.resp, sizeof(s->raw_csd));
    if (!cm_mmc_decode_csd(s->raw_csd, &s->csd)) return -EINVAL;
    struct cm_cid id;
    if (!cm_mmc_decode_cid(s->raw_cid, s->csd.mmca_vsn, &id)) return -EINVAL;
    struct sdcc_cmd sel = { CM_MMC_SELECT_CARD, s->rca << 16, CM_RSP_R1, {0}, 0, 0 };
    if ((err = send_cmd(s, &sel, CM_MMC_CMD_RETRIES))) return err;

    static uint8_t ext_1bit[2][512];
    uint8_t *ext = ext_1bit[s->sdcc1 ? 0 : 1];
    if (s->csd.mmca_vsn < 4) return -EOPNOTSUPP;   /* mmc_card_can_ext_csd: no EXT_CSD, no sector count */
    if ((err = send_data(s, CM_MMC_SEND_EXT_CSD, 0, ext, 512))) return err;
    cm_ext_csd_parse(ext, &s->ext);
    s->blockaddr = s->ext.blockaddr || (card_ocr & (1u << 30));
    s->sectors = s->ext.sectors;
    printf("COACHZ mmc %s: MMC manfid 0x%02x oem 0x%04x name %s prv 0x%02x %u/%u, ocr 0x%x, ext_csd rev %u, %llu sectors (%llu MiB), "
        "card type 0x%x, part config 0x%x, boot %u x 128 KiB, rpmb %u x 128 KiB\n", s->name, id.manfid, id.oemid, id.name, id.prv,
        id.month, id.year, card_ocr, s->ext.rev, (unsigned long long)s->sectors, (unsigned long long)(s->sectors / 2048u),
        s->ext.card_type, s->ext.part_config, s->ext.boot_mult, s->ext.rpmb_mult);
    if (s->ext.data_sector_size != 512) { printf("COACHZ mmc %s: 4 KiB native sectors, not used in stage 1\n", s->name); return -EOPNOTSUPP; }
    /* "Ensure eMMC user default partition is enabled": rewriting PART_CONFIG also rewrites its persistent boot bits,
       which stage 1 does not do; a card left on a boot partition is reported and not used. */
    if (s->ext.part_config & CM_EXT_CSD_PART_CONFIG_ACC_MASK) {
        printf("COACHZ mmc %s: PART_CONFIG 0x%x selects partition %u, not switched (read-only stage)\n", s->name,
            s->ext.part_config, s->ext.part_config & CM_EXT_CSD_PART_CONFIG_ACC_MASK);
        return -EOPNOTSUPP;
    }

    /* mmc_select_timing: HS only in stage 1, then mmc_set_bus_speed */
    /* mmc_blk's capacity: EXT_CSD sectors for a sector-addressed card, else the CSD */
    if (!s->blockaddr) s->sectors = s->csd.capacity << (s->csd.read_blkbits - 9u);
    s->hs_max_dtr = cm_mmc_hs_max_dtr(s->ext.card_type, (s->caps & CM_CAP_CAN_DO_HISPD) != 0);
    if (s->hs_max_dtr) {
        err = mmc_switch(s, CM_EXT_CSD_HS_TIMING, CM_EXT_CSD_TIMING_HS, s->ext.generic_cmd6_ms, CM_TIMING_MMC_HS);
        if (err && err != -EBADMSG) return err;
        if (err) printf("COACHZ mmc %s: switch to high-speed failed, err:%d\n", s->name, err);
    }
    set_clock(s, s->ios.timing == CM_TIMING_MMC_HS ? s->hs_max_dtr : s->csd.max_dtr);

    /* mmc_select_bus_width with mmc_compare_ext_csds */
    static const unsigned bits[] = { CM_EXT_CSD_BUS_WIDTH_8, CM_EXT_CSD_BUS_WIDTH_4, CM_EXT_CSD_BUS_WIDTH_1 };
    static const unsigned widths[] = { CM_BUS_WIDTH_8, CM_BUS_WIDTH_4, CM_BUS_WIDTH_1 };
    uint8_t again[512];
    for (unsigned i = s->dt_bus_width == 8 ? 0u : 1u; i < 3; i++) {
        if (mmc_switch(s, CM_EXT_CSD_BUS_WIDTH, bits[i], s->ext.generic_cmd6_ms, 0)) continue;
        set_width(s, widths[i]);
        if (widths[i] == CM_BUS_WIDTH_1) break;
        if (!send_data(s, CM_MMC_SEND_EXT_CSD, 0, again, 512) && cm_ext_csd_same(ext, again)) break;
        printf("COACHZ mmc %s: switch to bus width %u failed\n", s->name, 1u << widths[i]);
    }
    s->cmd23 = 1;   /* mmc_card_mmc: MMC_BLK_CMD23 */
    if (s->hs400_wanted && (err = mmc_select_hs400es(s))) return err;
    printf("COACHZ mmc %s: ready at %u Hz (rcg %u), %u-bit, %s timing, %s addressing\n", s->name, s->ios.clock, s->rcg_hz,
        s->ios.bus_width == CM_BUS_WIDTH_8 ? 8u : s->ios.bus_width == CM_BUS_WIDTH_4 ? 4u : 1u,
        s->ios.timing == CM_TIMING_MMC_HS400 ? "HS400" : s->ios.timing == CM_TIMING_MMC_HS ? "high-speed" : "legacy", s->blockaddr ? "sector" : "byte");
    return 0;
}

/* ------------------------------------------------------------------ SD (sd.c, sd_ops.c) */
static int sd_attach(struct sdcc *s)
{
    struct sdcc_cmd query = { CM_SD_APP_OP_COND, 0, CM_RSP_R3, {0}, 0, 0 };
    int err = poll_ready(s, 1, &query, 0, CM_SD_APP_OP_COND_PERIOD_US, CM_SD_APP_OP_COND_TIMEOUT_MS);
    if (err) return err;
    const uint32_t rocr_avail = cm_select_voltage(query.resp[0] & ~0x7fffu, s->ocr_avail);
    if (!rocr_avail) return -EINVAL;

    /* mmc_sd_get_cid, without S18R: stage 1 offers no UHS-I mode */
    uint32_t ocr = rocr_avail;
    go_idle(s);
    struct sdcc_cmd ifc = { CM_SD_SEND_IF_COND, cm_sd_if_cond_arg(ocr), CM_RSP_R7, {0}, 0, 0 };
    if (!send_cmd(s, &ifc, 0) && (ifc.resp[0] & 0xffu) == 0xaau) ocr |= CM_SD_OCR_CCS | CM_SD_OCR_2T;
    /* sd_get_host_max_current: max_current_300 exists only with SDHCI_CAN_VDD_300 */
    if ((s->caps & CM_CAP_CAN_VDD_300) && cm_sd_max_current_ma(s->max_current, (int)s->ios.vdd) > 150u) ocr |= CM_SD_OCR_XPC;
    struct sdcc_cmd op = { CM_SD_APP_OP_COND, ocr, CM_RSP_R3, {0}, 0, 0 };
    if ((err = poll_ready(s, 1, &op, ocr, CM_SD_APP_OP_COND_PERIOD_US, CM_SD_APP_OP_COND_TIMEOUT_MS))) return err;
    const uint32_t rocr = op.resp[0];
    struct sdcc_cmd cid = { CM_MMC_ALL_SEND_CID, 0, CM_RSP_R2, {0}, 0, 0 };
    if ((err = send_cmd(s, &cid, CM_MMC_CMD_RETRIES))) return err;
    __builtin_memcpy(s->raw_cid, cid.resp, sizeof(s->raw_cid));
    s->card_mmc = 0;

    struct sdcc_cmd rca = { CM_SD_SEND_RELATIVE_ADDR, 0, CM_RSP_R6, {0}, 0, 0 };
    if ((err = send_cmd(s, &rca, CM_MMC_CMD_RETRIES))) return err;
    s->rca = rca.resp[0] >> 16;
    struct sdcc_cmd csd = { CM_MMC_SEND_CSD, s->rca << 16, CM_RSP_R2, {0}, 0, 0 };
    if ((err = send_cmd(s, &csd, CM_MMC_CMD_RETRIES))) return err;
    __builtin_memcpy(s->raw_csd, csd.resp, sizeof(s->raw_csd));
    if (!cm_sd_decode_csd(s->raw_csd, &s->csd)) return -EINVAL;
    struct cm_cid id;
    cm_sd_decode_cid(s->raw_cid, &id);
    struct sdcc_cmd sel = { CM_MMC_SELECT_CARD, s->rca << 16, CM_RSP_R1, {0}, 0, 0 };
    if ((err = send_cmd(s, &sel, CM_MMC_CMD_RETRIES))) return err;

    /* mmc_sd_setup_card: SCR (ACMD51, two big-endian words), the switch capabilities (CMD6 mode 0) */
    if ((err = app_cmd(s, 1))) return err;
    uint8_t scr[8];
    if ((err = send_data(s, CM_SD_APP_SEND_SCR, 0, scr, 8))) return err;
    s->raw_scr[0] = (uint32_t)scr[0] << 24 | (uint32_t)scr[1] << 16 | (uint32_t)scr[2] << 8 | scr[3];
    s->raw_scr[1] = (uint32_t)scr[4] << 24 | (uint32_t)scr[5] << 16 | (uint32_t)scr[6] << 8 | scr[7];
    if (!cm_sd_decode_scr(s->raw_scr, &s->scr)) return -EINVAL;
    s->hs_max_dtr = 0;
    uint8_t status[64];
    if (s->scr.sda_vsn >= 1 && (s->csd.cmdclass & CM_CCC_SWITCH)) {
        err = send_data(s, CM_SD_SWITCH, cm_sd_switch_arg(0, 0, 0), status, 64);
        if (err && err != -EINVAL && err != -ENOSYS && err != -EFAULT) return err;   /* mmc_read_switch */
        if (err) printf("COACHZ mmc %s: problem reading Bus Speed modes\n", s->name);
        if (!err && (status[13] & (1u << CM_SD_HIGH_SPEED_BUS_SPEED))) s->hs_max_dtr = CM_SD_HIGH_SPEED_MAX_DTR;
    }
    s->card_readonly = s->csd.readonly;

    /* mmc_sd_switch_hs, the bus speed, the 4-bit bus */
    if (s->hs_max_dtr && (s->caps & CM_CAP_CAN_DO_HISPD)) {
        if ((err = send_data(s, CM_SD_SWITCH, cm_sd_switch_arg(1, 0, CM_SD_HIGH_SPEED_BUS_SPEED), status, 64))) return err;
        if ((status[16] & 0xfu) == CM_SD_HIGH_SPEED_BUS_SPEED) set_timing(s, CM_TIMING_SD_HS);
        else printf("COACHZ mmc %s: problem switching card into high-speed mode\n", s->name);
    }
    set_clock(s, s->ios.timing == CM_TIMING_SD_HS ? s->hs_max_dtr : s->csd.max_dtr);
    if (s->scr.bus_widths & CM_SD_SCR_BUS_WIDTH_4) {
        struct sdcc_cmd bw = { CM_SD_APP_SET_BUS_WIDTH, CM_SD_BUS_WIDTH_4, CM_RSP_R1, {0}, 0, 0 };
        if ((err = send_app_cmd(s, 1, &bw))) return err;
        set_width(s, CM_BUS_WIDTH_4);
    }
    s->blockaddr = s->csd.blockaddr;
    s->sectors = s->csd.blockaddr ? s->csd.capacity : (s->csd.capacity << s->csd.read_blkbits) / 512u;
    s->cmd23 = (s->scr.cmds & CM_SD_SCR_CMD23) != 0;
    printf("COACHZ mmc %s: SD manfid 0x%02x oem 0x%04x name %s %u/%u, rocr 0x%x, csd v%u, %llu sectors (%llu MiB), scr v%u cmds 0x%x, "
        "%s, ready at %u Hz (rcg %u), %u-bit, %s\n", s->name, id.manfid, id.oemid, id.name, id.month, id.year, rocr,
        s->csd.structure + 1u, (unsigned long long)s->sectors, (unsigned long long)(s->sectors / 2048u), s->scr.sda_vsn,
        s->scr.cmds, s->card_readonly ? "write protected" : "writable", s->ios.clock, s->rcg_hz,
        s->ios.bus_width == CM_BUS_WIDTH_4 ? 4u : 1u, s->ios.timing == CM_TIMING_SD_HS ? "high-speed" : "default speed");
    return 0;
}

/* ------------------------------------------------------------------ probe (sdhci_msm_probe, sdhci_add_host) */
static int probe(struct sdcc *s)
{
    const int which = s->sdcc1 ? 0 : 1;
    s->hc = ml_io_map(s->hc_pa, 0x1000);
    s->sdc = ml_io_map(s->sdc_ctl_pa, 0x1000);
    s->cd = s->removable ? ml_io_map(TLMM_WEST_PA + 0x1000u * 69u, 0x1000) : 0;
    s->pool = pool + SLOT_POOL * (vm_offset_t)which;
    s->pool_pa = POOL_PA + SLOT_POOL * (uint64_t)which;
    if (!s->hc || !s->sdc || (s->removable && !s->cd)) { printf("COACHZ mmc %s: map failed\n", s->name); return -ENOMEM; }

    s->vmmc.v = s->sdcc1 ? vreg_ldoa19 : vreg_ldoc9;
    s->vqmmc.v = s->sdcc1 ? vreg_ldoa12 : vreg_ldoc6;
    if (!vreg_register(&s->vmmc) || !vreg_register(&s->vqmmc)) {
        printf("COACHZ mmc %s: %s/%s not in cmd-db or mode vote failed\n", s->name, s->vmmc.v.resource, s->vqmmc.v.resource);
        return -ENODEV;
    }
    s->ios.power_mode = PM_UNDEFINED;
    s->f_max = cm_rcg_max(s->freqs, s->nfreqs, CM_STAGE1_SOURCES);   /* sdhci_msm_get_max_clock within stage 1 */
    s->f_min = CM_MSM_MIN_CLOCK;
    if (!opp_set_rate(s, 0xffffffffu)) printf("COACHZ mmc %s: core clock boost failed\n", s->name);
    if (!branch_enable(s->gcc_apps) || !branch_enable(s->gcc_ahb)) return -EIO;   /* bulk order: core, iface */

    wr32(s->hc, CM_MSM_VENDOR_SPEC, CM_MSM_VENDOR_SPEC_POR);
    const uint16_t host_version = rd16(s->hc, CM_SDHCI_HOST_VERSION);
    s->mci_version = rd32(s->hc, CM_MSM_MCI_VERSION);
    s->cdclp533 = cm_msm_uses_cdclp533(s->mci_version);
    if (cm_msm_needs_caps0(s->mci_version))
        wr32(s->hc, CM_MSM_VENDOR_CAPS0, rd32(s->hc, CM_SDHCI_CAPABILITIES) | CM_CAP_CAN_VDD_300 | CM_CAP_CAN_DO_8BIT);

    /* sdhci_msm_register_vreg: OCR from vmmc's voltages, caps_0 from vqmmc's */
    s->ocr_avail = 0;
    for (uint32_t i = 0; i < s->vmmc.v.n_voltages; i++) {
        const uint32_t uv = s->vmmc.v.range_min_uv + i * s->vmmc.v.step_uv;
        if (uv >= s->vmmc.v.dt_min_uv && uv <= s->vmmc.v.dt_max_uv) s->ocr_avail |= cm_vddrange_to_ocrmask((int)(uv / 1000u), (int)(uv / 1000u));
    }
    s->caps0 = 0;
    if (cm_vreg_supports(&s->vqmmc.v, 1700000, 1950000)) s->caps0 |= CM_CORE_1_8V_SUPPORT;
    if (cm_vreg_supports(&s->vqmmc.v, 2700000, 3600000)) s->caps0 |= CM_CORE_3_0V_SUPPORT;
    wr32(s->hc, CM_MSM_VENDOR_SPEC, cm_msm_regulator_caps_spec(rd32(s->hc, CM_MSM_VENDOR_SPEC), s->caps0, s->curr_io_level));

    handle_pwr_irq(s);   /* acknowledge what is pending before the interrupt is enabled */
    mb();
    thread_t thread;
    if (kernel_thread_start(pwr_thread, s, &thread) != KERN_SUCCESS) return -ENOMEM;
    thread_deallocate(thread);
    wr32(s->hc, CM_MSM_PWRCTL_MASK, CM_PWRCTL_INT_MASK);
    uint32_t readback = 0;
    uint64_t router = 0;
    if (armcpu_spi_attach(s->pwr_intid, false, pwr_isr, s, &readback, &router)) {
        printf("COACHZ mmc %s: pwr_irq %u not attached\n", s->name, s->pwr_intid);
        return -EIO;
    }

    /* sdhci_setup_host */
    sdhci_reset(s, CM_RESET_ALL);
    s->version = rd16(s->hc, CM_SDHCI_HOST_VERSION) & 0xffu;
    s->caps = rd32(s->hc, CM_SDHCI_CAPABILITIES);
    s->caps1 = s->version >= CM_SPEC_300 ? rd32(s->hc, CM_SDHCI_CAPABILITIES_1) : 0;
    s->max_current = rd32(s->hc, CM_SDHCI_MAX_CURRENT);
    const int adma = s->version >= CM_SPEC_200 && (s->caps & CM_CAP_CAN_DO_ADMA2);
    s->adma64 = (s->caps & CM_CAP_CAN_64BIT) != 0;
    s->auto12 = 1;   /* SDHCI_QUIRK_MULTIBLOCK_READ_ACMD12 */
    s->auto23 = s->version >= CM_SPEC_300 && adma;
    s->timeout_khz = cm_timeout_clk_khz(s->caps);
    s->signaling_330 = cm_vreg_supports(&s->vqmmc.v, 2700000, 3600000);
    s->signaling_180 = s->dt_caps_180 || (s->caps1 & 0x7u);   /* DT 1.8 V DDR/HS200/HS400, or UHS in caps1 */
    printf("COACHZ mmc %s: version 0x%x mci 0x%x caps 0x%x/0x%x max current 0x%x, ocr 0x%x caps_0 0x%x, f_max %u, timeout clock %u kHz\n",
        s->name, host_version, s->mci_version, s->caps, s->caps1, s->max_current, s->ocr_avail, s->caps0, s->f_max, s->timeout_khz);
    if (!adma || !s->timeout_khz) { printf("COACHZ mmc %s: no ADMA2 or no timeout clock, stage 1 stops\n", s->name); return -EOPNOTSUPP; }

    /* __sdhci_add_host: sdhci_init(host, 0), the host interrupt */
    sdhci_reset(s, CM_RESET_ALL);
    wr32(s->hc, CM_SDHCI_INT_ENABLE, CM_INT_DEFAULT);
    wr32(s->hc, CM_SDHCI_SIGNAL_ENABLE, CM_INT_DEFAULT);
    if (armcpu_spi_attach(s->hc_intid, false, hc_isr, s, &readback, &router)) {
        printf("COACHZ mmc %s: hc_irq %u not attached\n", s->name, s->hc_intid);
        return -EIO;
    }
    return 0;
}

/* mmc_rescan / mmc_rescan_try_freq (the SDIO probe is not sent: neither slot carries an SDIO function) */
static int rescan(struct sdcc *s)
{
    if (s->removable && !card_present(s)) { printf("COACHZ mmc %s: no card\n", s->name); power_off(s); return -ENODEV; }
    power_up(s, s->ocr_avail);
    go_idle(s);
    struct sdcc_cmd ifc = { CM_SD_SEND_IF_COND, cm_sd_if_cond_arg(s->ocr_avail), CM_RSP_R7, {0}, 0, 0 };
    (void)send_cmd(s, &ifc, 0);   /* mmc_send_if_cond_pcie: no SD Express here, errors ignored */
    if (!sd_attach(s) || !mmc_attach(s)) { s->card_registered = 1; return 0; }
    printf("COACHZ mmc %s: no card attached\n", s->name);
    power_off(s);
    return -EIO;
}

/* ------------------------------------------------------------------ the block interface used by CoachzMmcBlock.cpp */
int coachz_sdcc_enabled(int slot)
{
    unsigned value = 0;
    return (slot == 0 && PE_parse_boot_argn("coachz-emmc", &value, sizeof(value)) && (value >= 1 && value <= 3)) ||
           (slot == 1 && PE_parse_boot_argn("coachz-sd", &value, sizeof(value)) && (value == 1 || value == 2));
}
/* 2 asks for writes: to the whole card in the microSD slot, on the eMMC only inside the window find_write_window validates. */
static int write_requested(int slot)
{
    unsigned value = 0;
    return PE_parse_boot_argn(slot == 0 ? "coachz-emmc" : "coachz-sd", &value, sizeof(value)) && value >= 2;
}
int coachz_sdcc_removable(int slot) { return slot == 1; }
uint32_t coachz_sdcc_max_blocks(void) { return MAX_BLOCKS; }

/* The mappings both slots share, made once before either slot's thread starts. */
int coachz_sdcc_prepare(void)
{
    if (!gcc) gcc = ml_io_map(GCC_WINDOW_PA, GCC_WINDOW_BYTES);
    if (!gicd) gicd = ml_io_map(GICD_PA, 0x1000);
    if (!pool && (pool = ml_io_map_wcomb(POOL_PA, POOL_BYTES))) __builtin_memset((void *)pool, 0, POOL_BYTES);
    if (!gcc || !gicd || !pool) { printf("COACHZ mmc: shared maps failed\n"); return 0; }
    return 1;
}

/*
 * The eMMC holds this machine's Debian, the way back to it, so the driver writes only the partitions the owner agreed to: each
 * named here, at exactly the place it was carved. coachz-scratch for the write test; with the system on this disk
 * (coachz-root-emmc=1) also the log ring and the Data volume. Anything else about the table and that window stays closed.
 */
static const struct { const char *name; uint64_t first, blocks; int root_only; } emmc_windows[] = {
    {"coachz-scratch", 242171904ull, 2097152ull, 0},
    {"coachz-log", 126803968ull, 524288ull, 1},
    {"coachz-data", 127328256ull, 114843648ull, 1},
};
#define EMMC_WINDOWS (sizeof(emmc_windows) / sizeof(emmc_windows[0]))
static uint64_t window_first[EMMC_WINDOWS], window_blocks[EMMC_WINDOWS];

int coachz_emmc_root(void)
{
    static int root = -1;
    if (root < 0) {
        unsigned value = 0;
        root = PE_parse_boot_argn("coachz-root-emmc", &value, sizeof(value)) && value == 1;
    }
    return root;
}

static void find_write_window(struct sdcc *s, int slot)
{
    void *data;
    if (!coachz_sdcc_read(slot, 1, 1, &data)) return;
    uint8_t header[512];
    __builtin_memcpy(header, data, sizeof(header));
    uint64_t entries_lba;
    uint32_t entries, entry_size;
    __builtin_memcpy(&entries_lba, header + 72, 8);
    __builtin_memcpy(&entries, header + 80, 4);
    __builtin_memcpy(&entry_size, header + 84, 4);
    if (__builtin_memcmp(header, "EFI PART", 8) || entry_size != 128 || entries_lba < 2 || entries > 128) {
        printf("COACHZ mmc %s: no GPT header I understand, writes stay closed\n", s->name);
        return;
    }
    for (uint32_t i = 0; i < entries; i++) {
        if (!coachz_sdcc_read(slot, entries_lba + i / 4u, 1, &data)) return;
        uint8_t entry[128];
        __builtin_memcpy(entry, (const uint8_t *)data + (i % 4u) * 128u, sizeof(entry));
        for (unsigned w = 0; w < EMMC_WINDOWS; w++) {
            const char *name = emmc_windows[w].name;
            int same = 1;
            for (unsigned k = 0; k <= __builtin_strlen(name); k++)
                if (entry[56 + 2 * k] != (uint8_t)name[k] || entry[57 + 2 * k]) same = 0;
            if (!same) continue;
            uint64_t first, last;
            __builtin_memcpy(&first, entry + 32, 8);
            __builtin_memcpy(&last, entry + 40, 8);
            if (first != emmc_windows[w].first || last - first + 1u != emmc_windows[w].blocks || last >= s->sectors) {
                printf("COACHZ mmc %s: %s at %llu..%llu is not where it was carved, that window stays closed\n", s->name, name,
                    (unsigned long long)first, (unsigned long long)last);
            } else if (!emmc_windows[w].root_only || coachz_emmc_root()) {
                window_first[w] = first;
                window_blocks[w] = emmc_windows[w].blocks;
            }
        }
    }
    s->write_start = window_first[0];
    s->write_blocks = window_blocks[0];
    for (unsigned w = 0; w < EMMC_WINDOWS; w++)
        printf("COACHZ mmc %s: window %s %llu blocks from %llu\n", s->name, emmc_windows[w].name,
            (unsigned long long)window_blocks[w], (unsigned long long)window_first[w]);
}

/* A window find_write_window validated, by its GPT name: 0 until the eMMC has opened. */
int coachz_sdcc_window(const char *name, uint64_t *first, uint64_t *blocks)
{
    for (unsigned w = 0; w < EMMC_WINDOWS; w++) {
        if (__builtin_strcmp(name, emmc_windows[w].name) || !window_blocks[w]) continue;
        *first = window_first[w];
        *blocks = window_blocks[w];
        return 1;
    }
    return 0;
}

/* Whether a write of these blocks is inside a window: the eMMC's named ones, or the whole of a removable card. */
int coachz_sdcc_write_allowed(int slot, uint64_t block, uint32_t count)
{
    if (slot < 0 || slot > 1) return 0;
    const struct sdcc *s = &slots[slot];
    if (!s->opened) return 0;
    if (!s->removable) {
        for (unsigned w = 0; w < EMMC_WINDOWS; w++)
            if (window_blocks[w] && block >= window_first[w] && block - window_first[w] < window_blocks[w] &&
                count <= window_blocks[w] - (block - window_first[w])) return 1;
        return 0;
    }
    return s->write_blocks && block >= s->write_start && block - s->write_start < s->write_blocks &&
        count <= s->write_blocks - (block - s->write_start);
}

int coachz_sdcc_open(int slot, uint64_t *blocks)
{
    if (slot < 0 || slot > 1 || !coachz_sdcc_enabled(slot) || !gcc || !gicd || !pool) return 0;
    struct sdcc *s = &slots[slot];
    const uint64_t start = now_us();
    int err = probe(s);
    if (!err) err = rescan(s);
    printf("COACHZ mmc %s: %s after %llu ms, %u host and %u power interrupts (%u spurious)\n", s->name, err ? "failed" : "online",
        (now_us() - start) / 1000u, s->hc_irqs, s->pwr_irqs, s->spurious);
    if (err || !s->sectors) return 0;
    s->opened = 1;
    if (write_requested(slot)) {
        if (s->removable) {
            if (!s->card_readonly) { s->write_start = 0; s->write_blocks = s->sectors; }
        } else {
            find_write_window(s, slot);
        }
        printf("COACHZ mmc %s: writes %s, %llu blocks from %llu\n", s->name, s->write_blocks ? "open" : "closed",
            (unsigned long long)s->write_blocks, (unsigned long long)s->write_start);
    }
    *blocks = s->sectors;
    return 1;
}

/* mmc_blk_rw_rq_prep for reads: CMD17, or CMD18 with CMD23 (Auto-CMD23 when the host has it) or Auto-CMD12. */
int coachz_sdcc_read(int slot, uint64_t block, uint32_t count, void **data)
{
    if (slot < 0 || slot > 1 || !data || !count || count > MAX_BLOCKS) return 0;
    struct sdcc *s = &slots[slot];
    if (!s->opened || block >= s->sectors || count > s->sectors - block) return 0;
    if (s->removable && !card_present(s)) return 0;
    const uint32_t arg = s->blockaddr ? (uint32_t)block : (uint32_t)(block * 512u);
    struct sdcc_cmd cmd = { count > 1 ? CM_MMC_READ_MULTIPLE_BLOCK : CM_MMC_READ_SINGLE_BLOCK, arg, CM_RSP_R1, {0}, 0, 0 };
    struct sdcc_cmd sbc = { CM_MMC_SET_BLOCK_COUNT, count, CM_RSP_R1, {0}, 0, 0 };
    struct sdcc_cmd stop = { CM_MMC_STOP_TRANSMISSION, 0, CM_RSP_R1B, {0}, 0, 0 };
    struct sdcc_data d = { 512, count, s->pool_pa + XFER_OFF, 1, NULL, count > 1 ? &stop : NULL, 0 };
    if (count > 1 && s->cmd23) {
        d.sbc = &sbc;
        if (!s->auto23 && send_cmd(s, &sbc, 0)) return 0;   /* sdhci_request: CMD23 first when there is no Auto-CMD23 */
    }
    int err = request(s, &cmd, &d);
    if (!err && count > 1 && !d.sbc && !s->auto12) err = request(s, &stop, NULL);
    if (err || (cmd.resp[0] & CM_R1_CMD_ERRORS)) {
        printf("COACHZ mmc %s: read %llu+%u failed: err %d status 0x%x\n", s->name, (unsigned long long)block, count, err, cmd.resp[0]);
        return 0;
    }
    *data = (void *)(s->pool + XFER_OFF);
    return 1;
}

int coachz_sdcc_writable(int slot)
{
    if (slot < 0 || slot > 1 || !slots[slot].opened) return 0;
    if (slots[slot].write_blocks) return 1;
    return slot == 0 && coachz_emmc_root() && window_blocks[2] != 0;
}

/* mmc_blk_card_busy: CMD13 until the card is ready for data and back in the transfer state. */
static int card_ready(struct sdcc *s, unsigned timeout_ms, uint32_t *seen)
{
    const uint64_t end = now_us() + (uint64_t)timeout_ms * 1000u;
    unsigned wait = 32u;
    for (;;) {
        uint32_t status = 0;
        const int err = mmc_send_status(s, &status);
        if (err) return err;
        *seen |= status & CM_R1_CMD_ERRORS;
        if ((status & CM_R1_READY_FOR_DATA) && CM_R1_STATE(status) == CM_R1_STATE_TRAN) return 0;
        if (now_us() > end) return -ETIMEDOUT;
        sleep_us(wait);
        if (wait < 4096u) wait *= 2u;
    }
}

/* mmc_blk_rw_rq_prep for writes: CMD24, or CMD25 with CMD23 (Auto-CMD23 when the host has it) or Auto-CMD12; done when CMD13 says so.
 * The eMMC's cache is off (CACHE_CTRL is never set), so a finished write is on flash. Refuses anything outside the slot's window. */
int coachz_sdcc_write(int slot, uint64_t block, uint32_t count, const void *source)
{
    if (slot < 0 || slot > 1 || !source || !count || count > MAX_BLOCKS) return 0;
    struct sdcc *s = &slots[slot];
    if (!coachz_sdcc_write_allowed(slot, block, count)) {
        s->writes_refused++;
        return 0;
    }
    if (s->removable && !card_present(s)) return 0;
    __builtin_memcpy((void *)(s->pool + XFER_OFF), source, count * 512u);
    mb();
    const uint32_t arg = s->blockaddr ? (uint32_t)block : (uint32_t)(block * 512u);
    struct sdcc_cmd cmd = { count > 1 ? CM_MMC_WRITE_MULTIPLE_BLOCK : CM_MMC_WRITE_BLOCK, arg, CM_RSP_R1, {0}, 0, 0 };
    struct sdcc_cmd sbc = { CM_MMC_SET_BLOCK_COUNT, count, CM_RSP_R1, {0}, 0, 0 };
    struct sdcc_cmd stop = { CM_MMC_STOP_TRANSMISSION, 0, CM_RSP_R1B, {0}, 0, 0 };
    struct sdcc_data d = { 512, count, s->pool_pa + XFER_OFF, 0, NULL, count > 1 ? &stop : NULL, 0 };
    if (count > 1 && s->cmd23) {
        d.sbc = &sbc;
        if (!s->auto23 && send_cmd(s, &sbc, 0)) return 0;
    }
    int err = request(s, &cmd, &d);
    if (!err && count > 1 && !d.sbc && !s->auto12) err = request(s, &stop, NULL);
    uint32_t seen = 0;
    if (!err) err = card_ready(s, 60000u, &seen);
    if (err || (cmd.resp[0] & CM_R1_CMD_ERRORS) || seen) {
        printf("COACHZ mmc %s: write %llu+%u failed: err %d status 0x%x then 0x%x\n", s->name, (unsigned long long)block, count, err,
            cmd.resp[0], seen);
        return 0;
    }
    return 1;
}

/* A block's test image: fixed by its LBA, so a later boot, or Linux, can check it independently of this run. */
static void test_block(uint64_t block, uint32_t *words)
{
    for (uint32_t i = 0; i < 128u; i++) words[i] = (uint32_t)block * 2654435761u ^ i * 0x9e3779b1u ^ 0xc0ac4a12u;
}

/*
 * coachz-emmc=2: the write path on the last MiB of the window, on the eMMC only (a card's contents are the owner's). First whether the
 * previous boot's image survived the reset, then the refusals at both ends of the window, single-block then multi-block writes, and a
 * read-back. The image stays in place.
 */
#define TEST_BLOCKS 2048u
void coachz_sdcc_write_selftest(int slot)
{
    if (!write_requested(slot) || !coachz_sdcc_writable(slot)) return;
    struct sdcc *s = &slots[slot];
    /* With the system here the writes just outside the scratch window would land in the Data volume, which ends where it starts. */
    if (slot == 0 && coachz_emmc_root()) return;
    const uint64_t start = s->write_start + s->write_blocks - TEST_BLOCKS;
    static uint32_t want[128], chunk[128 * TEST_BLOCKS_AT];
    void *data;
    uint32_t survived = 0, mismatches = 0, refused = 0, single = 0, multi = 0;
    for (uint32_t b = 0; b < TEST_BLOCKS; b += TEST_BLOCKS_AT) {
        if (!coachz_sdcc_read(slot, start + b, TEST_BLOCKS_AT, &data)) { printf("COACHZ mmc %s: test read failed\n", s->name); return; }
        __builtin_memcpy(chunk, data, sizeof(chunk));
        for (uint32_t k = 0; k < TEST_BLOCKS_AT; k++) {
            test_block(start + b + k, want);
            if (!__builtin_memcmp(want, &chunk[128u * k], sizeof(want))) survived++;
        }
    }
    /* Each of these must come back refused, before any command reaches the card. */
    test_block(0, want);
    refused += !coachz_sdcc_write(slot, s->write_start - 1u, 1, want);
    refused += !coachz_sdcc_write(slot, s->write_start - 1u, 2, want);
    refused += !coachz_sdcc_write(slot, s->write_start + s->write_blocks, 1, want);
    refused += !coachz_sdcc_write(slot, s->write_start + s->write_blocks - 1u, 2, want);
    const unsigned must_refuse = s->write_start ? 5u : 4u;   /* a whole card has no first block to keep */
    if (s->write_start) refused += !coachz_sdcc_write(slot, 0, 1, want);
    if (refused != must_refuse) { printf("COACHZ mmc %s: window let a write through (%u of %u refused), test stops\n", s->name, refused, must_refuse); return; }
    const uint64_t began = now_us();
    uint32_t b = 0;
    for (; b < 8u; b++) {
        test_block(start + b, want);
        if (!coachz_sdcc_write(slot, start + b, 1, want)) goto failed;
        single++;
    }
    for (; b < TEST_BLOCKS; ) {
        const uint32_t n = TEST_BLOCKS - b < TEST_BLOCKS_AT ? TEST_BLOCKS - b : TEST_BLOCKS_AT;
        for (uint32_t k = 0; k < n; k++) test_block(start + b + k, &chunk[128u * k]);
        if (!coachz_sdcc_write(slot, start + b, n, chunk)) goto failed;
        multi += n;
        b += n;
    }
    const uint64_t wrote = now_us();
    for (b = 0; b < TEST_BLOCKS; ) {
        const uint32_t n = TEST_BLOCKS - b < TEST_BLOCKS_AT ? TEST_BLOCKS - b : TEST_BLOCKS_AT;
        if (!coachz_sdcc_read(slot, start + b, n, &data)) goto failed;
        __builtin_memcpy(chunk, data, n * 512u);
        for (uint32_t k = 0; k < n; k++) {
            test_block(start + b + k, want);
            if (__builtin_memcmp(want, &chunk[128u * k], sizeof(want))) mismatches++;
        }
        b += n;
    }
    const uint64_t read_back = now_us();
    printf("COACHZ mmc %s: write test at %llu: previous image %u of %u, refused 5 of 5 outside the window, %u single and %u multi blocks written, "
        "read back %u mismatches; write %llu KB/s, read %llu KB/s\n", s->name, (unsigned long long)start, survived, TEST_BLOCKS, single, multi,
        mismatches, (unsigned long long)(TEST_BLOCKS * 512ull * 1000u / (wrote - began ? wrote - began : 1u)),
        (unsigned long long)(TEST_BLOCKS * 512ull * 1000u / (read_back - wrote ? read_back - wrote : 1u)));
    /* A longer sequential read inside the window, without copying the buffer: the bus and the driver alone. */
    const uint64_t blocks = 64ull * 2048u;
    const uint64_t read_began = now_us();
    for (uint64_t at = 0; at + MAX_BLOCKS <= blocks; at += MAX_BLOCKS) {
        if (!coachz_sdcc_read(slot, start - blocks + at, MAX_BLOCKS, &data)) {
            printf("COACHZ mmc %s: read test stopped at block %llu\n", s->name, (unsigned long long)at);
            return;
        }
    }
    const uint64_t read_took = now_us() - read_began;
    printf("COACHZ mmc %s: read 64 MiB in %llu ms, %llu KB/s\n", s->name, (unsigned long long)(read_took / 1000u),
        (unsigned long long)(blocks * 512ull * 1000u / (read_took ? read_took : 1u)));
    return;
failed:
    printf("COACHZ mmc %s: write test stopped at block %u (%u single, %u multi written)\n", s->name, b, single, multi);
}
