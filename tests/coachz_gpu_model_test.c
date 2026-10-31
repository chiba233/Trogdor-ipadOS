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
/* Modes 5 and 6: the SMMU's registers and a real walk of the tables, the GMU's other registers, the PDC and the pool. */
static uint32_t smmu[GPU_SMMU_SIZE / 4], gmu_regs[GPU_GMU_SIZE / 4], pdc[GPU_PDC_SIZE / 4], pdc_seq[GPU_PDC_SEQ_SIZE / 4];
static uint64_t pool[GMU_POOL_BYTES / 8];
static int gmu_live, fw_silent, wrong_walk, released_early, notes;
/* Mode 7: CMD DB, and the firmware: it reads the queue table and answers every message after checking it. */
static uint32_t cmd_db[GPU_CMD_DB_SIZE / 4], hfi_ids[8], hfi_count, hfi_seq_seen, gx_allowed, no_boot_ack, refuse_message, table_good;
/* Mode 8: the GPU's registers and a CP that walks the ring and the indirect buffer through bank 0. */
static uint32_t core[GPU_CORE_SIZE / 4], core_live, core_cp_on, me_init_seen, sw_resets, dcvs_value, cp_silent, cp_ops[12], cp_op_count, sqe_base_ok, usf_when_started;
static uint32_t perf_top_vote, perf_top_khz, perf_counts, perf_cx_khz, bw_first_ddr, bw_cnoc_second, init_debug, init_boot, gpu_set_seen;
#define MODEL_BANK (0x8000u + GMU_BANK * 0x1000u)

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
	if (block == GPU_CORE) {
		if (!core_live || !gpu_set_seen || offset >= GPU_CORE_SIZE || offset % 4) { faults++; return 0xffffffffu; }
		return core[offset / 4];
	}
	if (block == GPU_CMD_DB) {
		if (offset >= GPU_CMD_DB_SIZE || offset % 4 || !gx_allowed) { faults++; return 0; }
		return cmd_db[offset / 4];
	}
	if (block == GPU_SMMU) {
		/* CX is its power domain; memnoc (bus) and cfg-ahb (iface) its clocks. */
		if (!ahb_on() || !cx_on() || !(gcc[GCC_GPU_MEMNOC_GFX / 4] & CBCR_ENABLE) || offset >= GPU_SMMU_SIZE || offset % 4) {
			faults++;
			return 0xffffffffu;
		}
		smmu_reads++;
		if (gmu_live) return smmu[offset / 4];
		if (offset == SMMU_IDR0) return 0x48016605u;   /* 5 stream match groups */
		if (offset == SMMU_IDR1) return smmu_idr1;
		if (offset >= SMMU_S2CR && offset < SMMU_S2CR + 4 * 5) return 0x000200ffu;
		return offset == 0 ? 0x00200626u : 0u;
	}
	if (block == GPU_GMU) {
		if (!ahb_on() || !cx_on() || (cc[GPUCC_CX_GMU / 4] & CBCR_CLK_OFF)) { faults++; return 0xffffffffu; }
		if (gx_allowed && !(sysreset & 1u) && offset >= GMU_DCVS_ACK_OPTION && offset <= GMU_DCVS_RETURN) return gmu_regs[offset / 4];
		if (offset >= GMU_CM3_ITCM && offset < GMU_CM3_ITCM + GMU_TCM_BYTES) return tcm[(offset - GMU_CM3_ITCM) / 4];
		if (gmu_live && offset != GMU_CM3_SYSRESET) return offset == GMU_CM3_CFG && !gmu_regs[offset / 4] ? 0x52u : gmu_regs[offset / 4];
		return offset == GMU_CM3_SYSRESET ? sysreset : offset == GMU_CM3_CFG ? 0x52u : 0u;
	}
	if (block == GPU_CC && offset == GPUCC_CX_GDS_HW_CTRL) tick();
	if (block == GPU_CC && offset == GPUCC_GMU_RCG_CMD && pending_rcg && !--pending_rcg) cc[offset / 4] &= ~RCG_UPDATE;
	return *slot(block, offset);
}

/* The stage-1 walk the bank's registers describe: the PAR, with F set for anything the tables do not map. */
static uint64_t walk_bank(unsigned bank, uint64_t va) {
	const uint32_t base = 0x8000u + bank * 0x1000u;
	const uint64_t ttbr = (uint64_t)smmu[(base + SMMU_CB_TTBR0) / 4 + 1] << 32 | smmu[(base + SMMU_CB_TTBR0) / 4];
	if (!(smmu[(base + SMMU_CB_SCTLR) / 4] & 1u) || !(smmu[(0x1000u + SMMU_CBA2R + 4u * bank) / 4] & SMMU_CBA2R_VA64) ||
	    smmu[(base + SMMU_CB_TCR) / 4] != SMMU_TCR_32BIT || (smmu[(0x1000u + 4u * bank) / 4] >> 16 & 3u) != 1u) {
		faults++;
		return 1;
	}
	uint64_t table = ttbr & 0x0000fffffffff000ull;
	for (unsigned level = 1; level <= 3; level++) {
		if (table < GMU_POOL_PA || table >= GMU_POOL_PA + GMU_POOL_BYTES) { faults++; return 1; }
		const unsigned index = (unsigned)(va >> (39 - 9 * level)) & (level == 1 ? 3u : 511u);
		const uint64_t entry = pool[(table - GMU_POOL_PA) / 8 + index];
		if ((entry & 3u) != 3u) return 1;
		table = entry & 0x0000fffffffff000ull;
		if (level == 3 && !(entry & (1u << 10))) return 1;   /* no access flag */
	}
	return wrong_walk ? table + 0x1000u : table;
}

/* A memory access as stream 0 makes it: anything the bank does not map is a fault of the run. */
static uint32_t *cp_memory(uint64_t va) {
	const uint64_t par = walk_bank(0, va & ~0xfffull);
	if (par & 1u) { faults++; return NULL; }
	return (uint32_t *)((char *)pool + (par - GMU_POOL_PA) + (va & 0xfffu));
}

/* One packet at address, whichever buffer it is in: the dwords it used. */
static uint32_t cp_packet(uint64_t address) {
	const uint32_t *word = cp_memory(address);
	if (!word) return 1;
	const uint32_t header = *word, count = header & 0x3fffu, opcode = header >> 16 & 0x7fu;
	if (header >> 28 != 7u || (header >> 15 & 1u) != pm4_parity(count) || (header >> 23 & 1u) != pm4_parity(opcode)) { faults++; return 1; }
	if (cp_op_count < 12) cp_ops[cp_op_count++] = opcode;
	if (!me_init_seen && opcode != PM4_ME_INIT) faults++;
	const uint32_t *args[4] = {NULL, NULL, NULL, NULL};
	for (uint32_t i = 0; i < count && i < 4; i++) args[i] = cp_memory(address + 4 * (i + 1));
	if (opcode == PM4_ME_INIT) {
		if (count != 8 || !args[0] || *args[0] != 0x2fu || !args[1] || *args[1] != 3u || !args[2] || *args[2] != 0x20000000u) faults++;
		me_init_seen = 1;
	} else if (opcode == PM4_WHERE_AM_I) {
		uint32_t *to = cp_memory((uint64_t)*args[1] << 32 | *args[0]);
		if (count != 2 || !to) faults++; else *to = core[CORE(CP_RB_WPTR) / 4];
	} else if (opcode == PM4_MEM_WRITE) {
		uint32_t *to = cp_memory((uint64_t)*args[1] << 32 | *args[0]);
		if (count != 3 || !to) faults++; else if (!cp_silent) *to = *args[2];
	} else if (opcode == PM4_EVENT_WRITE) {
		uint32_t *to = cp_memory((uint64_t)*args[2] << 32 | *args[1]);
		if (count != 4 || *args[0] != PM4_CACHE_FLUSH_TS || !to) faults++; else *to = *args[3];
	} else if (opcode == PM4_INDIRECT_BUFFER) {
		if (count != 3) { faults++; return 1 + count; }
		const uint64_t ib = (uint64_t)*args[1] << 32 | *args[0];
		for (uint32_t at = 0; at < *args[2];) at += cp_packet(ib + 4 * at);
	} else {
		faults++;
	}
	return 1 + count;
}

static void cp_run(void) {
	if (!core_cp_on) return;
	const uint64_t base = (uint64_t)core[CORE(CP_RB_BASE) / 4 + 1] << 32 | core[CORE(CP_RB_BASE) / 4];
	uint32_t rptr = core[CORE(CP_RB_RPTR) / 4];
	while (rptr != core[CORE(CP_RB_WPTR) / 4]) rptr = (rptr + cp_packet(base + 4 * rptr)) % GPU_RING_WORDS;
	core[CORE(CP_RB_RPTR) / 4] = rptr;
}

static void model_core_write(uint32_t offset, uint32_t value) {
	if (!core_live || !gpu_set_seen || offset >= GPU_CORE_SIZE || offset % 4) { faults++; return; }
	core[offset / 4] = value;
	if (offset == CORE(GBIF_HALT)) core[CORE(GBIF_HALT_ACK) / 4] = value;
	if (offset == CORE(RBBM_GBIF_HALT)) core[CORE(RBBM_GBIF_HALT_ACK) / 4] = value;
	if (offset == CORE(RBBM_SW_RESET_CMD) && value) sw_resets++;
	if (offset == CORE(CP_SQE_CNTL)) {
		if (value) {
			/* The firmware and the ring must be in place, the bank live, and an unmatched stream must fault. */
			const uint64_t sqe = (uint64_t)core[CORE(CP_SQE_INSTR_BASE) / 4 + 1] << 32 | core[CORE(CP_SQE_INSTR_BASE) / 4];
			const uint32_t *first = cp_memory(sqe);
			sqe_base_ok = first && *first == 0x00000207u;
			if (core[CORE(CP_RB_CNTL) / 4] != CORE_RB_CNTL || !core[CORE(CP_RB_BASE) / 4] || core[CORE(RBBM_CLOCK_CNTL) / 4] != CORE_CLOCK_CNTL_ON) faults++;
			usf_when_started = smmu[SMMU_SCR0 / 4] & SMMU_SCR0_USFCFG;
		}
		core_cp_on = value;
	}
	if (offset == CORE(CP_RB_WPTR)) cp_run();
}

static uint64_t model_read64(void *cookie, unsigned block, uint32_t offset) {
	if (offset % 8) { faults++; return 0; }
	return (uint64_t)model_read(cookie, block, offset + 4) << 32 | model_read(cookie, block, offset);
}

static void model_write64(void *cookie, unsigned block, uint32_t offset, uint64_t value) {
	(void)cookie;
	writes++;
	if (offset % 8) { faults++; return; }   /* an unaligned store to Device memory aborts: this panicked the hardware */
	if (block == GPU_CORE) {
		model_core_write(offset, (uint32_t)value);
		model_core_write(offset + 4, (uint32_t)(value >> 32));
		return;
	}
	if (block != GPU_SMMU || !gmu_live) { faults++; return; }
	if (offset == 0x8000u + GPU_BANK * 0x1000u + SMMU_CB_ATS1PR) {
		const uint64_t par = walk_bank(GPU_BANK, value);
		smmu[(0x8000u + GPU_BANK * 0x1000u + SMMU_CB_PAR) / 4] = (uint32_t)par;
		smmu[(0x8000u + GPU_BANK * 0x1000u + SMMU_CB_PAR) / 4 + 1] = (uint32_t)(par >> 32);
		return;
	}
	if (offset == MODEL_BANK + SMMU_CB_ATS1PR) {
		const uint64_t par = walk_bank(GMU_BANK, value);
		smmu[(MODEL_BANK + SMMU_CB_PAR) / 4] = (uint32_t)par;
		smmu[(MODEL_BANK + SMMU_CB_PAR) / 4 + 1] = (uint32_t)(par >> 32);
		return;
	}
	smmu[offset / 4] = (uint32_t)value;
	smmu[offset / 4 + 1] = (uint32_t)(value >> 32);
}

static void model_note(void *cookie, const struct coachz_gpu_report *r) { (void)cookie; (void)r; notes++; }

static uint32_t mem32(uint32_t at) { return ((uint32_t *)pool)[at / 4]; }
static void mem32_set(uint32_t at, uint32_t value) { ((uint32_t *)pool)[at / 4] = value; }

/* The firmware's view of the table: everything Linux writes, at the addresses the bank translates. */
static void firmware_reads_table(void) {
	const uint32_t q0 = GMU_POOL_HFI + 24u, q1 = q0 + 48u;
	table_good = gmu_regs[GMU_HFI_QTBL_ADDR / 4] == GMU_IOVA_HFI && !mem32(GMU_POOL_HFI) && mem32(GMU_POOL_HFI + 4) == 120 && mem32(GMU_POOL_HFI + 8) == 6 &&
	    mem32(GMU_POOL_HFI + 12) == 12 && mem32(GMU_POOL_HFI + 16) == 2 && mem32(GMU_POOL_HFI + 20) == 2 && mem32(q0) == 1 && mem32(q0 + 4) == GMU_IOVA_HFI + 0x1000u &&
	    mem32(q0 + 8) == 0xa00u && mem32(q0 + 12) == 1024 && mem32(q0 + 32) == 1 && mem32(q1 + 4) == GMU_IOVA_HFI + 0x2000u && mem32(q1 + 8) == 0xa04u && mem32(q1 + 12) == 1024;
	if (!table_good) faults++;
}

/* One command out of the queue, checked; its acknowledgement into the response queue, with a firmware error message first once. */
static void firmware_takes_message(void) {
	const uint32_t q0 = GMU_POOL_HFI + 24u, q1 = q0 + 48u, data = GMU_POOL_HFI + 0x1000u, rsp = GMU_POOL_HFI + 0x2000u;
	uint32_t read = mem32(q0 + 40), out = mem32(q1 + 44);
	if (read == mem32(q0 + 44)) { faults++; return; }
	const uint32_t header = mem32(data + read * 4), id = header & 0xffu, dwords = header >> 8 & 0xffu;
	const uint32_t expected = id == HFI_MSG_INIT ? 5u : id == HFI_MSG_FW_VERSION ? 2u : id == HFI_MSG_PERF_TABLE ? HFI_PERF_DWORDS : id == HFI_MSG_BW_TABLE ? HFI_BW_DWORDS : 1u;
	if (dwords != expected || header >> 16 & 0xfu || (header >> 20) != hfi_seq_seen + 1) faults++;
	hfi_seq_seen = header >> 20;
	if (hfi_count < 8) hfi_ids[hfi_count++] = id;
	if (id == HFI_MSG_INIT) { init_debug = mem32(data + (read + 2) * 4); init_boot = mem32(data + (read + 4) * 4); }
	if (id == HFI_MSG_PERF_TABLE) {
		perf_counts = mem32(data + (read + 1) * 4) << 8 | mem32(data + (read + 2) * 4);
		perf_top_vote = mem32(data + (read + 3 + 2 * 7) * 4);
		perf_top_khz = mem32(data + (read + 4 + 2 * 7) * 4);
		perf_cx_khz = mem32(data + (read + 36 + 2) * 4);
	}
	if (id == HFI_MSG_BW_TABLE) { bw_first_ddr = mem32(data + (read + 24) * 4); bw_cnoc_second = mem32(data + (read + 18) * 4); }
	mem32_set(q0 + 40, (read + dwords) % 1024);
	const uint32_t words[4] = {126u | 4u << 8 | (hfi_count << 20), header, id == refuse_message ? 7u : 0u, id == HFI_MSG_FW_VERSION ? 0x10a20000u : 0u};
	if (id == HFI_MSG_INIT) {   /* an unrelated firmware error comes first and must be skipped */
		const uint32_t error[2] = {HFI_MSG_ERROR | 2u << 8, 0x55u};
		for (unsigned i = 0; i < 2; i++) { mem32_set(rsp + out * 4, error[i]); out = (out + 1) % 1024; }
	}
	for (unsigned i = 0; i < 4; i++) { mem32_set(rsp + out * 4, words[i]); out = (out + 1) % 1024; }
	mem32_set(q1 + 44, out);
	gmu_regs[GMU_GMU2HOST_INTR_INFO / 4] |= GMU_INTR_MSGQ;
}

static void db_put16(uint32_t at, uint32_t value) {
	cmd_db[at / 4] = (cmd_db[at / 4] & ~(0xffffu << ((at & 2u) * 8u))) | value << ((at & 2u) * 8u);
}

/* One slave with three rail tables, in Linux cmd-db.c's layout. */
static void db_build(void) {
	static const uint16_t gfx[] = {0, 16, 48, 64, 128, 192, 256, 320, 384, 416}, mx[] = {16, 48, 128, 192, 256, 320, 384, 416}, cx[] = {0, 16, 48, 64, 128, 192, 256, 320, 384};
	static const struct { uint32_t name0, name1; const uint16_t *levels; uint32_t count; } tables[3] = {{CMD_DB_GFX_LVL, gfx, 10}, {CMD_DB_MX_LVL, mx, 8}, {CMD_DB_CX_LVL, cx, 9}};
	const uint32_t rsc = 8u, headers = CMD_DB_DATA + 0x10u, data = CMD_DB_DATA + 0x100u;
	cmd_db[1] = CMD_DB_MAGIC;
	db_put16(rsc, 3); db_put16(rsc + 2, 0x10); db_put16(rsc + 4, 0x100); db_put16(rsc + 6, 3);
	uint32_t used = 0;
	for (unsigned i = 0; i < 3; i++) {
		const uint32_t entry = headers + i * CMD_DB_ENTRY;
		cmd_db[entry / 4] = tables[i].name0;
		cmd_db[entry / 4 + 1] = tables[i].name1;
		db_put16(entry + 20, tables[i].count * 2);
		db_put16(entry + 22, used);
		for (uint32_t j = 0; j < tables[i].count; j++) db_put16(data + used + j * 2, tables[i].levels[j]);
		used += tables[i].count * 2;
	}
}

static void branch(uint32_t *reg, uint32_t value, int voted) {
	*reg = value;
	if (voted) return;
	if (value & CBCR_ENABLE) *reg &= ~CBCR_CLK_OFF; else *reg |= CBCR_CLK_OFF;
}

static void model_write(void *cookie, unsigned block, uint32_t offset, uint32_t value) {
	(void)cookie;
	writes++;
	if (block == GPU_SMMU) {   /* the inventory never writes the SMMU */
		if (!gmu_live) { faults++; return; }
		smmu[offset / 4] = value;
		return;
	}
	if (block == GPU_PDC || block == GPU_PDC_SEQ) {
		if (!gmu_live) faults++; else if (block == GPU_PDC) pdc[offset / 4] = value; else pdc_seq[offset / 4] = value;
		return;
	}
	if (block == GPU_CMD_DB) { faults++; return; }   /* read only */
	if (block == GPU_CORE) { model_core_write(offset, value); return; }
	if (block == GPU_GMU && gmu_live && (offset < GMU_CM3_ITCM || offset >= GMU_CM3_ITCM + GMU_TCM_BYTES || (gx_allowed && !(sysreset & 1u) && ((offset >= GMU_BOOT_SLUMBER_OPTION && offset <= GMU_MX_VOTE_IDX) || (offset >= GMU_DCVS_ACK_OPTION && offset <= GMU_DCVS_RETURN))))) {
		if (!cx_on() || (cc[GPUCC_CX_GMU / 4] & CBCR_CLK_OFF)) faults++;
		gmu_regs[offset / 4] = value;
		if (offset == GMU_HOST2GMU_INTR_SET) {
			if (sysreset & 1u) faults++;   /* a stopped firmware cannot answer */
			if (value == 1u << GMU_OOB_BOOT_SET) {
				if (!no_boot_ack) {
					gmu_regs[GMU_GMU2HOST_INTR_INFO / 4] |= 1u << GMU_OOB_BOOT_ACK;
					cc[GPUCC_GX_GDSCR / 4] &= ~GDSC_SW_COLLAPSE;
				}
			} else if (value == 1u << GMU_OOB_GPU_SET) {
				gmu_regs[GMU_GMU2HOST_INTR_INFO / 4] |= 1u << GMU_OOB_GPU_ACK;
				gpu_set_seen = 1;
			} else if (value == 1u) {
				firmware_takes_message();
			} else if (value == 1u << GMU_OOB_DCVS_SET) {
				dcvs_value = gmu_regs[GMU_DCVS_PERF_SETTING / 4];
				gmu_regs[GMU_GMU2HOST_INTR_INFO / 4] |= 1u << GMU_OOB_DCVS_ACK;
			} else if (value != 1u << GMU_OOB_GPU_ACK && value != 1u << GMU_OOB_DCVS_ACK) {
				faults++;
			}
		}
		if (offset == GMU_GMU2HOST_INTR_CLR) gmu_regs[GMU_GMU2HOST_INTR_INFO / 4] &= ~value;
		if (offset == GMU_HFI_CTRL_INIT) { firmware_reads_table(); gmu_regs[GMU_HFI_CTRL_STATUS / 4] |= 1u; }
		if (offset == GMU_GX_SPTPRAC_POWER_CONTROL) gmu_regs[GMU_SPTPRAC_PWR_CLK_STATUS / 4] = value == 0x778000u ? 0x28u : 0x04u;
		if (offset == GMU_RSCC_CONTROL_REQ) {
			if (value & 2u) gmu_regs[GMU_RSCC_CONTROL_ACK / 4] |= 2u;
			if (value & 1u) gmu_regs[RSCC_RSC_STATUS0_DRV0 / 4] |= RSC_STATUS0_POWERED_OFF;
		}
		if (offset == GMU_CM3_SYSRESET) {
			sysreset = value;
			if (!value) {
				/* CM3 reads its memory through the bank, so it must already be live. */
				if ((smmu[SMMU_SCR0 / 4] & SMMU_SCR0_CLIENTPD) || !(smmu[(SMMU_SMR + 4u * GMU_GROUP) / 4] & SMMU_SMR_VALID)) released_early = 1;
				if (!fw_silent) gmu_regs[GMU_CM3_FW_INIT_RESULT / 4] = 0xbabefaceu;
			}
		}
		return;
	}
	if (block == GPU_GMU) {
		/* Only the TCM, only with CM3 in reset, CX powered and the GMU clocked. */
		if (offset < GMU_CM3_ITCM || offset >= GMU_CM3_ITCM + GMU_TCM_BYTES || offset % 4 || !(sysreset & 1) || !cx_on() ||
		    (cc[GPUCC_CX_GMU / 4] & CBCR_CLK_OFF)) { faults++; return; }
		if ((offset - GMU_CM3_ITCM) / 4 != stuck_word) tcm[(offset - GMU_CM3_ITCM) / 4] = value;
		return;
	}
	if (block == GPU_CC && offset == GPUCC_GX_GDSCR) {
		/* Only mode 7's shutdown, and only collapse and retention. */
		if (!gx_allowed || (value ^ cc[GPUCC_GX_GDSCR / 4]) & ~(GDSC_SW_COLLAPSE | 1u << 11)) faults++; else cc[GPUCC_GX_GDSCR / 4] = value;
		return;
	}
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
	gmu_live = fw_silent = wrong_walk = released_early = notes = 0;
	memset(smmu, 0, sizeof(smmu));
	memset(gmu_regs, 0, sizeof(gmu_regs));
	memset(pdc, 0, sizeof(pdc));
	memset(pdc_seq, 0, sizeof(pdc_seq));
	memset(pool, 0xa5, sizeof(pool));
	memset(cmd_db, 0, sizeof(cmd_db));
	memset(core, 0, sizeof(core));
	memset(cp_ops, 0, sizeof(cp_ops));
	core_live = core_cp_on = me_init_seen = sw_resets = dcvs_value = cp_silent = cp_op_count = sqe_base_ok = usf_when_started = 0;
	memset(hfi_ids, 0, sizeof(hfi_ids));
	hfi_count = hfi_seq_seen = gx_allowed = no_boot_ack = refuse_message = table_good = 0;
	perf_top_vote = perf_top_khz = perf_counts = perf_cx_khz = bw_first_ddr = bw_cnoc_second = init_debug = init_boot = gpu_set_seen = 0;
	refuse_message = ~0u;
	db_build();
	gmu_regs[RSCC(0x346u) / 4] = gmu_regs[RSCC(0x3eeu) / 4] = gmu_regs[RSCC(0x496u) / 4] = gmu_regs[RSCC(0x53eu) / 4] = 1;
	/* What the hardware reported (docs/hardware-tests.md): bypass, six groups, eight banks, every bank's CBAR at its reset value. */
	smmu[SMMU_SCR0 / 4] = 1;
	smmu[SMMU_IDR0 / 4] = 0x78016606u;
	smmu[SMMU_IDR1 / 4] = 0x20008008u;
	for (unsigned i = 0; i < 8; i++) smmu[0x1000u / 4 + i] = 0x00010000u;
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
	static uint32_t sqe_file[34188 / 4];
	sqe_file[0] = 0x4f525000u;   /* the file's first dword: the CP is given what follows it */
	sqe_file[1] = 0x00000207u;
	for (unsigned i = 2; i < 34188 / 4; i++) sqe_file[i] = i * 2654435761u;
	const struct coachz_gpu_io io = {NULL, model_read, model_write, model_delay, model_read64, model_write64, pool, model_note, sqe_file, sizeof(sqe_file)};
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

	reset();
	gmu_live = 1;
	memcpy(g0, gcc, sizeof(g0)); memcpy(c0, cc, sizeof(c0));
	coachz_gpu_run(&io, 5, NULL, 0, &r);
	CHECK("the bank translates what the tables map", r.stage == 7 && r.restored && !r.error && faults == 0 && r.ats_total == GMU_ATS_CHECKS &&
	    r.ats_right == GMU_ATS_CHECKS && r.scr0_on == (SMMU_SCR0_ENABLE_BITS) && r.gmu_step == 1 && notes == 1);
	CHECK("mode 5 stops before the RSC", !r.own_rsc && !pdc[PDC_ENABLE / 4] && sysreset == 1);
	CHECK("the SMMU is back to bypass", smmu[SMMU_SCR0 / 4] == 1 && !smmu[(SMMU_SMR + 4u * GMU_GROUP) / 4] && !smmu[(MODEL_BANK + SMMU_CB_SCTLR) / 4] &&
	    smmu[(0x1000u + 4u * GMU_BANK) / 4] == 0x00010000u && !smmu[(MODEL_BANK + SMMU_CB_TTBR0) / 4] && r.unmap_clean);
	CHECK("mode 5 window restored", same_except_rcg(g0, c0) && !cx_on());

	reset();
	gmu_live = 1;
	wrong_walk = 1;
	coachz_gpu_run(&io, 6, image, sizeof(image), &r);
	CHECK("a wrong translation stops everything after it", r.error && r.stage == 6 && r.restored && faults == 0 && !r.own_rsc &&
	    !pdc[PDC_ENABLE / 4] && r.ats_wrong_va == GMU_IOVA_DUMMY && sysreset == 1 && smmu[SMMU_SCR0 / 4] == 1);

	reset();
	gmu_live = 1;
	smmu[SMMU_SCR0 / 4] = 0;
	coachz_gpu_run(&io, 5, NULL, 0, &r);
	CHECK("an enabled SMMU is left alone", r.error && r.stage == 6 && r.restored && faults == 0 && !r.own_smmu && !r.ats_total);

	reset();
	gmu_live = 1;
	smmu[SMMU_SMR / 4 + 3] = SMMU_SMR_VALID | 9u;
	coachz_gpu_run(&io, 5, NULL, 0, &r);
	CHECK("a used stream match is left alone", r.error && !r.own_smmu && faults == 0 && smmu[SMMU_SMR / 4 + 3] == (SMMU_SMR_VALID | 9u));

	image[(GMU_FW_VERSION_AT - GMU_CM3_ITCM) / 4] = 0x20010004u;
	reset();
	gmu_live = 1;
	memcpy(g0, gcc, sizeof(g0)); memcpy(c0, cc, sizeof(c0));
	coachz_gpu_run(&io, 6, image, sizeof(image), &r);
	CHECK("the firmware starts", r.stage == 7 && r.restored && !r.error && faults == 0 && !released_early && r.fw_init == 0xbabefaceu && r.fw_init_polls);
	CHECK("the firmware got its addresses", gmu_regs[GMU_HFI_QTBL_ADDR / 4] == GMU_IOVA_HFI && gmu_regs[GMU_HFI_QTBL_INFO / 4] == 1 &&
	    gmu_regs[GMU_PWR_COL_CP_MSG / 4] == (GMU_IOVA_LOG | 3u) && gmu_regs[GMU_HFI_SFR_ADDR / 4] == GMU_CHIP_ID && gmu_regs[GMU_AHB_FENCE_RANGE_0 / 4] == 0x802800a0u);
	CHECK("CM3 is back in reset", sysreset == 1 && (r.sysreset_stopped & 1u) && r.tcm_words == 8192 && !r.tcm_mismatches && !gmu_regs[GMU_GENERAL_7 / 4] && !gmu_regs[GMU_RPMH_CTRL / 4]);
	CHECK("the RSC and PDC were programmed", gmu_regs[RSCC_SEQ_MEM_0_DRV0 / 4] == 0xa7a506a0u && pdc_seq[0] == 0xfebea1e1u && pdc[PDC_ENABLE / 4] == 0x80000001u &&
	    pdc[PDC_TCS3_CMD0 / 4 + 4 + 2] == 2u && pdc[PDC_TCS3_CMD0 / 4 + 8 + 1] == PDC_ADDR_GFX && r.rsc_stop_polls && r.rsc_start_polls && r.rsc_stop_polls_end);
	CHECK("notes before each step", notes == 4);
	CHECK("mode 6 window restored", same_except_rcg(g0, c0) && !cx_on() && smmu[SMMU_SCR0 / 4] == 1);

	reset();
	gmu_live = 1;
	fw_silent = 1;
	coachz_gpu_run(&io, 6, image, sizeof(image), &r);
	CHECK("a silent firmware is reported and stopped", r.error && r.stage == 6 && r.restored && faults == 0 && !r.fw_init_polls && sysreset == 1 && smmu[SMMU_SCR0 / 4] == 1);

	reset();
	coachz_gpu_run(&io, 6, NULL, 0, &r);
	CHECK("mode 6 needs the firmware", r.error && r.stage == 0 && writes == 0 && faults == 0);

	reset();
	gmu_live = 1;
	gx_allowed = 1;
	memcpy(g0, gcc, sizeof(g0)); memcpy(c0, cc, sizeof(c0));
	coachz_gpu_run(&io, 7, image, sizeof(image), &r);
	CHECK("GX, HFI and GPU_SET complete", r.stage == 7 && r.restored && !r.error && faults == 0 && r.oob_boot_polls && r.sptprac_polls && r.hfi_start_polls && r.oob_gpu_polls && gpu_set_seen);
	CHECK("the top votes from CMD DB", gmu_regs[GMU_GX_VOTE_IDX / 4] == 8 && gmu_regs[GMU_MX_VOTE_IDX / 4] == 6 && r.gx_vote_top == (384u << 16 | 6u << 8 | 8u) && r.db_counts[0] == 10 && r.db_counts[1] == 8 && r.db_counts[2] == 9);
	CHECK("Linux's message order", hfi_count == 5 && hfi_ids[0] == HFI_MSG_INIT && hfi_ids[1] == HFI_MSG_FW_VERSION && hfi_ids[2] == HFI_MSG_PERF_TABLE && hfi_ids[3] == HFI_MSG_BW_TABLE && hfi_ids[4] == HFI_MSG_TEST && r.hfi_done == 5);
	CHECK("the messages' contents", table_good && init_debug == GMU_IOVA_DEBUG && init_boot == GMU_BOOT_COLD && perf_counts == (8u << 8 | 2u) && perf_top_vote == r.gx_vote_top && perf_top_khz == 800000 && perf_cx_khz == 200000 &&
	    bw_first_ddr == 0x50000u && bw_cnoc_second == 0x60000001u && r.hfi_version == 0x10a20000u && r.hfi_fw_error == 0x55u);
	CHECK("all off again", sysreset == 1 && r.tcs_idle == 4 && r.sptprac_off_polls && cc[GPUCC_GX_GDSCR / 4] == c0[GPUCC_GX_GDSCR / 4] && same_except_rcg(g0, c0) && !cx_on() && smmu[SMMU_SCR0 / 4] == 1);

	reset();
	gmu_live = 1;
	gx_allowed = 1;
	no_boot_ack = 1;
	coachz_gpu_run(&io, 7, image, sizeof(image), &r);
	CHECK("no boot OOB ack stops HFI and still shuts down", r.error && r.stage == 6 && r.restored && faults == 0 && !r.oob_boot_polls && !hfi_count && sysreset == 1 && !cx_on());

	reset();
	gmu_live = 1;
	gx_allowed = 1;
	refuse_message = HFI_MSG_PERF_TABLE;
	coachz_gpu_run(&io, 7, image, sizeof(image), &r);
	CHECK("a refused message stops there", r.error && r.restored && faults == 0 && r.hfi_done == 2 && r.hfi_status == 7 && hfi_count == 3 && !gpu_set_seen && sysreset == 1);

	reset();
	gmu_live = 1;
	gx_allowed = 1;
	core_live = 1;
	memcpy(g0, gcc, sizeof(g0)); memcpy(c0, cc, sizeof(c0));
	coachz_gpu_run(&io, 8, image, sizeof(image), &r);
	CHECK("the CP runs the commands", r.stage == 7 && r.restored && !r.error && faults == 0 && r.core_registers_done && r.core_init_polls && r.core_idle_polls && r.core_ring_polls && r.core_ib_polls && r.core_fence_polls);
	CHECK("the results are in memory", mem32(GPU_RESULT_RING) == GPU_RING_VALUE && mem32(GPU_RESULT_IB) == GPU_IB_VALUE && mem32(GPU_FENCE) == GPU_FENCE_VALUE && r.core_shadow == r.core_wptr);
	CHECK("the firmware went in without its first dword", sqe_base_ok && mem32(GPU_SQE) == 0x207u && r.sqe_version == 0x207u);
	CHECK("the order the CP saw", cp_op_count == 8 && cp_ops[0] == PM4_ME_INIT && cp_ops[1] == PM4_WHERE_AM_I && cp_ops[2] == PM4_MEM_WRITE && cp_ops[3] == PM4_WHERE_AM_I && cp_ops[4] == PM4_INDIRECT_BUFFER && cp_ops[5] == PM4_MEM_WRITE && cp_ops[6] == PM4_EVENT_WRITE && cp_ops[7] == PM4_WHERE_AM_I);
	CHECK("the lowest frequency, the tables and the trust", dcvs_value == (3u << 28 | 1u) && core[CORE(0x0b0u) / 4] == 0x02222222u && core[CORE(CP_PROTECT + 31u) / 4] == (1u << 31 | 0x11c00u) && core[CORE(CP_PROTECT_CNTL) / 4] == 0xbu && core[CORE(0x8e08u) / 4] == 2u && usf_when_started);
	CHECK("the CP is stopped and everything off", !core_cp_on && sw_resets == 1 && !core[CORE(GBIF_HALT) / 4] && !core[CORE(RBBM_SW_RESET_CMD) / 4] && r.core_halt_polls[0] && r.core_halt_polls[1] && r.core_halt_polls[2] && sysreset == 1 && !cx_on() && same_except_rcg(g0, c0) && smmu[SMMU_SCR0 / 4] == 1 && !smmu[(SMMU_SMR + 4u * GPU_GROUP) / 4] && !smmu[(0x8000u + SMMU_CB_SCTLR) / 4]);

	reset();
	gmu_live = 1;
	gx_allowed = 1;
	core_live = 1;
	cp_silent = 1;
	coachz_gpu_run(&io, 8, image, sizeof(image), &r);
	CHECK("a write that never lands stops and shuts down", r.error && r.stage == 6 && r.restored && faults == 0 && !r.core_ring_polls && r.core_init_polls && !core_cp_on && sw_resets == 1 && sysreset == 1 && !cx_on());

	reset();
	gmu_live = 1;
	gx_allowed = 1;
	core_live = 1;
	sqe_file[1] = 0x00000100u;
	coachz_gpu_run(&io, 8, image, sizeof(image), &r);
	sqe_file[1] = 0x00000207u;
	CHECK("an old SQE firmware never starts", r.error && r.restored && faults == 0 && !r.own_core && !core_cp_on && sysreset == 1);

	reset();
	{
		const struct coachz_gpu_io no_sqe = {NULL, model_read, model_write, model_delay, model_read64, model_write64, pool, model_note, NULL, 0};
		coachz_gpu_run(&no_sqe, 8, image, sizeof(image), &r);
	}
	CHECK("mode 8 needs the SQE firmware", r.error && r.stage == 0 && writes == 0 && faults == 0);

	reset();
	gmu_live = 1;
	gx_allowed = 1;
	cmd_db[1] = 0;
	coachz_gpu_run(&io, 7, image, sizeof(image), &r);
	CHECK("a bad CMD DB votes nothing", r.error && r.restored && faults == 0 && !r.own_gx && !gmu_regs[GMU_GX_VOTE_IDX / 4] && sysreset == 1);

	if (!failed) printf("PASS\n");
	return failed;
}
