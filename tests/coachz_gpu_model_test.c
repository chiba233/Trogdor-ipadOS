/* SPDX-License-Identifier: BSD-2-Clause */
/* A register model of the SC7180 GCC/GPUCC/GMU subset for coachz_gpu_seq.h: a run must never touch GPUCC with its AHB branch
 * stopped, read the GMU without CX power and its clock, write the GMU or GX, or change bits it does not own. */
#include <stdio.h>
#include <string.h>
#include "coachz_gpu_seq.h"

static uint32_t gcc[0x80000 / 4], cc[0x9000 / 4];
static int faults, writes, pending_cx, pending_rcg;
static int cx_never_on;
static uint32_t tcm[GMU_TCM_BYTES / 4], sysreset, stuck_word = ~0u;
static uint32_t smmu_idr1, smmu_reads;

static uint32_t *slot(unsigned block, uint32_t offset) {
	return block == GPU_GCC ? &gcc[offset / 4] : &cc[offset / 4];
}

static int ahb_on(void) { return !(gcc[GCC_GPU_CFG_AHB / 4] & CBCR_CLK_OFF); }
static int cx_on(void) { return (cc[GPUCC_CX_GDS_HW_CTRL / 4] & GDSC_PWR_ON) != 0; }

/* The GDS controller follows SW_COLLAPSE a few microseconds later. */
static void tick(void) {
	if (pending_cx && !--pending_cx) {
		if (cc[GPUCC_CX_GDSCR / 4] & GDSC_SW_COLLAPSE) cc[GPUCC_CX_GDS_HW_CTRL / 4] &= ~GDSC_PWR_ON;
		else if (!cx_never_on) cc[GPUCC_CX_GDS_HW_CTRL / 4] |= GDSC_PWR_ON;
	}
}

static uint32_t model_read(void *cookie, unsigned block, uint32_t offset) {
	(void)cookie;
	if (block == GPU_CC && !ahb_on()) { faults++; return 0xffffffffu; }
	if (block == GPU_SMMU) {
		/* CX is its power domain; memnoc (bus) and cfg-ahb (iface) its clocks. */
		if (!ahb_on() || !cx_on() || !(gcc[GCC_GPU_MEMNOC_GFX / 4] & CBCR_ENABLE) || offset >= GPU_SMMU_SIZE || offset % 4) {
			faults++;
			return 0xffffffffu;
		}
		smmu_reads++;
		if (offset == SMMU_IDR0) return 0x48016605u;   /* 5 stream match groups */
		if (offset == SMMU_IDR1) return smmu_idr1;
		if (offset >= SMMU_S2CR && offset < SMMU_S2CR + 4 * 5) return 0x000200ffu;
		return offset == 0 ? 0x00200626u : 0u;
	}
	if (block == GPU_GMU) {
		if (!ahb_on() || !cx_on() || (cc[GPUCC_CX_GMU / 4] & CBCR_CLK_OFF)) { faults++; return 0xffffffffu; }
		if (offset >= GMU_CM3_ITCM && offset < GMU_CM3_ITCM + GMU_TCM_BYTES) return tcm[(offset - GMU_CM3_ITCM) / 4];
		return offset == GMU_CM3_SYSRESET ? sysreset : offset == GMU_CM3_CFG ? 0x52u : 0u;
	}
	if (block == GPU_CC && offset == GPUCC_CX_GDS_HW_CTRL) tick();
	if (block == GPU_CC && offset == GPUCC_GMU_RCG_CMD && pending_rcg && !--pending_rcg) cc[offset / 4] &= ~RCG_UPDATE;
	return *slot(block, offset);
}

static void branch(uint32_t *reg, uint32_t value, int voted) {
	*reg = value;
	if (voted) return;
	if (value & CBCR_ENABLE) *reg &= ~CBCR_CLK_OFF; else *reg |= CBCR_CLK_OFF;
}

static void model_write(void *cookie, unsigned block, uint32_t offset, uint32_t value) {
	(void)cookie;
	writes++;
	if (block == GPU_SMMU) { faults++; return; }   /* the inventory never writes the SMMU */
	if (block == GPU_GMU) {
		/* Only the TCM, only with CM3 in reset, CX powered and the GMU clocked. */
		if (offset < GMU_CM3_ITCM || offset >= GMU_CM3_ITCM + GMU_TCM_BYTES || offset % 4 || !(sysreset & 1) || !cx_on() ||
		    (cc[GPUCC_CX_GMU / 4] & CBCR_CLK_OFF)) { faults++; return; }
		if ((offset - GMU_CM3_ITCM) / 4 != stuck_word) tcm[(offset - GMU_CM3_ITCM) / 4] = value;
		return;
	}
	if (block == GPU_CC && offset == GPUCC_GX_GDSCR) { faults++; return; }
	if (block == GPU_CC && !ahb_on()) { faults++; return; }
	uint32_t *reg = slot(block, offset);
	if (block == GPU_GCC && offset == GCC_BRANCH_VOTE && ((value ^ *reg) & ~GPU_GPLL0_DIV_GATE)) faults++;
	if (block == GPU_GCC && offset == GCC_GPU_CFG_AHB) { branch(reg, value, 0); return; }
	if (block == GPU_GCC && (offset == GCC_GPU_MEMNOC_GFX || offset == GCC_DDRSS_GPU_AXI)) {
		*reg = value;   /* voted: the halt bit follows the vote */
		if (value & CBCR_ENABLE) *reg &= ~CBCR_CLK_OFF; else *reg |= CBCR_CLK_OFF;
		return;
	}
	if (block == GPU_CC && offset == GPUCC_CX_GDSCR) { *reg = value; pending_cx = 3; return; }
	if (block == GPU_CC && offset == GPUCC_GMU_RCG_CMD) { *reg = value; if (value & RCG_UPDATE) pending_rcg = 2; return; }
	if (block == GPU_CC && (offset == GPUCC_CX_GMU || offset == GPUCC_CXO)) {
		if ((value & CBCR_ENABLE) && !cx_on() && offset == GPUCC_CX_GMU) faults++;   /* a CX branch needs CX power */
		branch(reg, value, 0);
		return;
	}
	*reg = value;
}

static void model_delay(void *cookie, unsigned us) { (void)cookie; while (us--) tick(); }

static void reset(void) {
	memset(gcc, 0, sizeof(gcc));
	memset(cc, 0, sizeof(cc));
	faults = writes = pending_cx = pending_rcg = cx_never_on = 0;
	memset(tcm, 0, sizeof(tcm));
	sysreset = 1;
	stuck_word = ~0u;
	smmu_idr1 = 0x20008005u;   /* 4 KiB pages, 8 global pages, 5 context banks */
	smmu_reads = 0;
	/* As ios-extra read the A618 before any change, and the SC7180 boot values Linux assumes. */
	gcc[GCC_GPLL0_MODE / 4] = 0xd0100005u;
	gcc[GCC_GPLL0_VOTE / 4] = 1;
	gcc[GCC_BRANCH_VOTE / 4] = 0x01241d7bu & ~GPU_GPLL0_DIV_GATE;
	gcc[GCC_GPU_CFG_AHB / 4] = 0x00000001u;
	gcc[GCC_GPU_MEMNOC_GFX / 4] = 0x80000220u;
	gcc[GCC_DDRSS_GPU_AXI / 4] = 0x80000000u;
	cc[GPUCC_CX_GDSCR / 4] = 0x00222001u;
	cc[GPUCC_GX_GDSCR / 4] = 0x00222001u;
	cc[GPUCC_GMU_RCG_CMD / 4] = 0x80000000u;
	cc[GPUCC_CX_GMU / 4] = 0x80004220u;
	cc[GPUCC_CXO / 4] = 0x00000001u;
}

static int same_except_rcg(const uint32_t *g, const uint32_t *c) {
	uint32_t c0[0x9000 / 4];
	memcpy(c0, c, sizeof(c0));
	c0[GPUCC_GMU_RCG_CFG / 4] = cc[GPUCC_GMU_RCG_CFG / 4];
	if (memcmp(g, gcc, sizeof(gcc))) return 0;
	for (unsigned i = 0; i < 0x9000 / 4; i++) {
		uint32_t mask = i == GPUCC_CX_GMU / 4 ? ~CBCR_WAKE_SLEEP : ~0u;   /* the recommended wake/sleep fields stay */
		if (i != GPUCC_GMU_RCG_CFG / 4 && ((c0[i] ^ cc[i]) & mask)) return 0;
	}
	return 1;
}

int main(void) {
	const struct coachz_gpu_io io = {NULL, model_read, model_write, model_delay};
	struct coachz_gpu_report r;
	static uint32_t g0[0x80000 / 4], c0[0x9000 / 4];
	int failed = 0;
#define CHECK(name, cond) do { if (!(cond)) { printf("FAIL %s (stage %u error %s faults %d)\n", name, r.stage, r.error ? r.error : "-", faults); failed = 1; } } while (0)

	reset();
	coachz_gpu_run(&io, 1, NULL, 0, &r);
	CHECK("inventory writes nothing", writes == 0 && faults == 0 && !r.error && r.cc_before[0] == 0x00222001u);

	reset();
	gcc[GCC_GPU_CFG_AHB / 4] = 0x80000000u;
	coachz_gpu_run(&io, 1, NULL, 0, &r);
	CHECK("inventory skips GPUCC without AHB", writes == 0 && faults == 0 && r.error);

	reset();
	memcpy(g0, gcc, sizeof(g0)); memcpy(c0, cc, sizeof(c0));
	coachz_gpu_run(&io, 2, NULL, 0, &r);
	CHECK("window completes", r.stage == 7 && r.restored && !r.error && faults == 0);
	CHECK("GMU read under power", r.gmu[0] == 1 && r.gmu[3] == 0x52u);
	CHECK("CX and clock were on", (r.cc_on[1] & GDSC_PWR_ON) && !(r.cc_on[5] & CBCR_CLK_OFF) && (r.cc_on[4] & RCG_SRC_HID_MASK) == RCG_GMU_200MHZ);
	CHECK("everything else restored", same_except_rcg(g0, c0) && !cx_on());

	reset();
	gcc[GCC_GPU_CFG_AHB / 4] = 0x80000000u;
	memcpy(g0, gcc, sizeof(g0)); memcpy(c0, cc, sizeof(c0));
	coachz_gpu_run(&io, 2, NULL, 0, &r);
	CHECK("AHB owned and returned", r.stage == 7 && r.restored && r.own_ahb && faults == 0 && same_except_rcg(g0, c0));

	reset();
	cx_never_on = 1;
	memcpy(g0, gcc, sizeof(g0)); memcpy(c0, cc, sizeof(c0));
	coachz_gpu_run(&io, 2, NULL, 0, &r);
	CHECK("CX timeout rolls back", r.error && r.stage == 2 && faults == 0 && same_except_rcg(g0, c0));

	reset();
	cc[GPUCC_CX_GMU / 4] = 0x00004ff1u;
	coachz_gpu_run(&io, 2, NULL, 0, &r);
	CHECK("foreign cx-gmu refused", r.error && writes == 0 && faults == 0);

	reset();
	gcc[GCC_GPLL0_MODE / 4] = 0;
	coachz_gpu_run(&io, 2, NULL, 0, &r);
	CHECK("unlocked GPLL0 refused", r.error && writes == 0 && faults == 0);

	static uint32_t image[GMU_TCM_BYTES / 4];
	for (unsigned i = 0; i < GMU_TCM_BYTES / 4; i++) image[i] = i * 2654435761u;

	reset();
	memcpy(g0, gcc, sizeof(g0)); memcpy(c0, cc, sizeof(c0));
	coachz_gpu_run(&io, 3, image, sizeof(image), &r);
	CHECK("firmware written and read back", r.stage == 7 && r.restored && !r.error && faults == 0 && r.tcm_words == 8192 &&
	    !r.tcm_mismatches && r.fw_hash == r.tcm_hash && !memcmp(tcm, image, sizeof(image)) && r.sysreset_after == 1);
	CHECK("firmware window restored", same_except_rcg(g0, c0) && !cx_on());

	reset();
	sysreset = 0;
	memcpy(g0, gcc, sizeof(g0)); memcpy(c0, cc, sizeof(c0));
	coachz_gpu_run(&io, 3, image, sizeof(image), &r);
	CHECK("running CM3 is not written", r.error && r.stage == 6 && r.restored && faults == 0 && !r.tcm_words && same_except_rcg(g0, c0));

	reset();
	coachz_gpu_run(&io, 3, NULL, 0, &r);
	CHECK("no firmware, nothing touched", r.error && r.stage == 0 && writes == 0 && faults == 0);
	coachz_gpu_run(&io, 3, image, 0x8004, &r);
	CHECK("oversized firmware refused", r.error && writes == 0 && faults == 0);

	reset();
	stuck_word = 4100;
	coachz_gpu_run(&io, 3, image, sizeof(image), &r);
	CHECK("a bad TCM word is reported", r.error && r.tcm_mismatches == 1 && r.tcm_first_mismatch == 4100 && r.restored && faults == 0);

	reset();
	memcpy(g0, gcc, sizeof(g0)); memcpy(c0, cc, sizeof(c0));
	coachz_gpu_run(&io, 4, NULL, 0, &r);
	CHECK("SMMU inventory", r.stage == 7 && r.restored && !r.error && faults == 0 && r.smmu[0] == 0x00200626u &&
	    r.smmu_groups == 5 && r.smmu_banks == 5 && r.smmu_page == 0x1000 && r.smmu_bank_base == 0x8000 &&
	    r.s2cr[4] == 0x000200ffu && smmu_reads == 10 + 2 * 5 + 5 * 5);
	CHECK("SMMU inventory restored", same_except_rcg(g0, c0) && !cx_on() && !r.tcm_words);

	reset();
	smmu_idr1 = 0xa0008005u;   /* 64 KiB pages: the described layout would not fit the 64 KiB window */
	coachz_gpu_run(&io, 4, NULL, 0, &r);
	CHECK("an impossible SMMU layout stops the reads", r.error && r.stage == 6 && r.restored && faults == 0 && smmu_reads == 10 &&
	    !r.smmu_banks);

	reset();
	coachz_gpu_run(&io, 2, NULL, 0, &r);
	CHECK("mode 2 leaves the SMMU alone", r.stage == 7 && smmu_reads == 0 && faults == 0);

	if (!failed) printf("PASS\n");
	return failed;
}
