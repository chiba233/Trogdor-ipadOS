# Coachz eMMC and microSD (sdhci-msm): stage 1, read-only

Built into the prelinked kernel: `build-usb-stack.py` compiles `coachz_sdcc.c` with the kernel's C record into `libCoachzUsb.a` and `CoachzMmcBlock.cpp` with NativeRamBlock's own command into `libRamBlock.a`; `coachz-prelink-mmc.patch` calls `coachz_mmc_start()` from the sampler thread next to `coachz_wifi_start()`; `tools/xnu_screen.py` adds `coachz-emmc=1 coachz-sd=1`. The shared RPMh client of `coachz_rpmh_iface.h` is implemented in `coachz-prelink-wifi.patch` (cx aggregated over the SDHCI cores and the modem's proxy vote). On hardware the eMMC comes up at 52 MHz on 8 bits and its read-only block device is published; the empty microSD slot reports no card. Reading a card and the published partitions are not confirmed. The Chinese hand-off is [docs/plans/emmc.md](../../../../../docs/plans/emmc.md); the pinned sources are in [SOURCES.md](SOURCES.md).

The driver is a port of the mainline Linux driver for this exact SoC and board: `sdhci-msm` (sc7180 variant, v5 register map), the SDHCI core it uses, the MMC/SD core initialisation, `gcc-sc7180` clocks, `qcom-rpmh-regulator` supplies, `rpmhpd` CX levels and the trogdor/coachz device tree. Values and order come from Linux; the code is our own.

## Files

| File | Purpose |
|---|---|
| `coachz_mmc_proto.h` | Portable, no MMIO: SDHCI/msm/MMC/SD constants, response and card-register decoding (CID, CSD, EXT_CSD, SCR), command and transfer-mode encodings, the ADMA2 table, GCC RCG register values and floor selection, OCR and RPMh VRM millivolt selection, ARC corner selection, TLMM SDC pin states, and the msm power-interrupt decisions (decode, acknowledge, IO pad switch, which writes must wait). |
| `coachz_sdcc.c` | Kernel side: GCC branches and RCG, RPMh supplies through `coachz_rpmh_iface.h`, the two interrupts through `armcpu_spi_attach`, the SDHCI host operations with the msm write hooks, power up/off, the eMMC and SD initialisation, and block reads. |
| `CoachzMmcBlock.cpp` | One `IOBlockStorageDevice` per slot, write-protected unless the slot has a write window, attached under IOResources like `CoachzDataBlock`; `coachz_mmc_start()` starts one thread per enabled slot. |
| `coachz_rpmh_iface.h` | The contract of the shared RPMh client this driver calls. **Not implemented here** (see below). |

Host checks: `python3 -B -m unittest discover -s tests -p test_mmc.py -v` compiles and runs `tests/coachz_mmc_model_test.c` (ten groups, ASan/UBSan), type-checks `coachz_sdcc.c` against stub kernel headers with `-Wall -Wextra -Werror`, and confirms the active build does not reference it. `CoachzMmcBlock.cpp` needs IOKit headers and is not checked on the host.

## What stage 1 does

Per slot, behind `coachz-emmc=1` (sdhc_1, 0x7c4000) and `coachz-sd=1` (sdhc_2, 0x8804000):

1. `sdhci_msm_probe`: regulator registration (HPM mode for ldoa19/ldoa12 or ldoc9/ldoc6), the OPP boost (CX level for the clock, then the RCG), core then iface branch, `VENDOR_SPEC` POR, the CAPABILITIES0 fix-up on cores that need it, regulator capabilities and pad bits, the pending power interrupt handled, `PWRCTL_MASK`, the power interrupt attached (SPI 644 / 222).
2. `sdhci_setup_host` / `__sdhci_add_host`: RESET_ALL, capabilities, ADMA2 (64-bit, 12-byte descriptors), Auto-CMD12 (msm quirk) and Auto-CMD23, RESET_ALL, default interrupt enables, the host interrupt attached (SPI 641 / 204).
3. `mmc_rescan`: power up with vdd from the vmmc OCR mask (bit 17, 2.9–3.0 V), initial signal voltage (eMMC 1.8 V, SD 3.3 V), 400 kHz, CMD0, the SD probe, then MMC.
4. Every POWER_CONTROL / HOST_CONTROL2 / RESET_ALL write goes through the msm write hook and waits up to 5 s for the power interrupt, whose thread switches vmmc/vqmmc through RPMh and the SDC pins between their default and sleep states, then acknowledges in `PWRCTL_CTL`.
5. eMMC: CMD1 polling, CID, RCA 1, CSD, CMD7, EXT_CSD, HS_TIMING (52 MHz → the RCG's 50 MHz), BUS_WIDTH 8/4/1 with the EXT_CSD comparison. SD: CMD8, ACMD41 (CCS, 2T, XPC when the host allows >150 mA), CID, RCA, CSD, CMD7, SCR, CMD6 query and high speed, ACMD6 4-bit.
6. Reads: CMD17, or CMD18 with Auto-CMD23 (eMMC, and SD cards whose SCR has CMD23) or Auto-CMD12, into a fixed 64 KiB buffer; IOKit copies out.

### Stage limits (chosen, not discovered)

- Core clock only from XO and GPLL0 even, so at most 100 MHz. No HS200, HS400/HS400ES, DDR52, UHS-I, tuning, DLL/CDC calibration, CQE, ICE or SDIO probe. GPLL6 (192/384 MHz) and GPLL7 (202 MHz) are untouched.
- No interconnect (BCM) bandwidth vote; the CX vote goes through the shared aggregator.
- Writes need `coachz-emmc=2` or `coachz-sd=2`. On the eMMC they stay inside the GPT partition named `coachz-scratch` at its exact bounds, checked before any command; everything else is refused and counted. EXT_CSD is switched only for BUS_WIDTH and HS_TIMING, both lost at power-off. ERASE_GROUP_DEF, POWER_OFF_NOTIFICATION, HPI, CACHE_CTRL, CMDQ and POWER_CLASS are not switched, and a PART_CONFIG that selects a boot partition is reported and the card is not used, because rewriting PART_CONFIG rewrites its persistent boot bits. No write, erase, RPMB or boot partition access. eMMC writes need a fresh decision.
- SD: no hot-plug (card detect is read at start and before each read); SSR and the SD extension registers are not read.
- Data commands use the maximum hardware timeout count (0xF, sdhci-msm's `max_timeout_count`) instead of `mmc_set_data_timeout`; busy commands use their own timeout.

## Integration

- RPMh: `coachz_cmd_db_address`, `coachz_cmd_db_aux`, `coachz_rpmh_write` and `coachz_rpmh_cx_request` are in `coachz_wifi.c`, over its apps-RSC TCS and cmd-db lookup, serialized for every caller. cx is written as the highest corner over the consumers (SDCC1, SDCC2, the modem's proxy vote), so the modem's handover no longer drops it to 0.
- Interrupts: ArmCpu has 32 SPI slots (touch 1, Wi-Fi copy engines 12, these cores 4).
- Memory: the fixed pool `0x81100000 + 0x30000` lies between the USB pool (`0x81000000`, 1 MiB) and the speaker ring (`0x81200000`); no reserved-memory node covers it.

## Expected log lines

All start with `COACHZ mmc`:

- `<slot>: version … mci … caps …/… max current …, ocr 0x20000 caps_0 …, f_max 100000000, timeout clock … kHz` — the host was reached (clocks on, registers readable).
- eMMC: `MMC manfid … name … ext_csd rev …, N sectors (M MiB), card type …, part config …` then `ready at 52000000 Hz (rcg 50000000), 8-bit, high-speed timing, sector addressing`.
- SD: `SD manfid … name …, rocr …, csd v2, N sectors …, ready at 50000000 Hz (rcg 50000000), 4-bit, high-speed`, or `no card`.
- `<slot>: online after … ms, H host and P power interrupts (S spurious)`, then `COACHZ mmc slot N: block device published`.
- Failures name the step: `pwr_irq for req: (…) timed out`, `branch … stuck at off`, `rcg didn't update`, `ldoa19/ldoa12 not in cmd-db or mode vote failed`, `CMDn timed out in software`, `ADMA error …`, `PART_CONFIG … not switched`.

Acceptance needs the hardware: IOKit publishing the eMMC's GPT (LBA 1 `EFI PART`) and, with a card, the microSD's partitions, read-only, without disturbing the USB root disk.

## Next stage (after the stage-1 hardware result)

eMMC HS400ES at 384 MHz (GPLL6, CX nom, `sdhci_msm_hs400`/CM DLL SDC4 calibration, enhanced strobe), SD UHS-I (CMD11 voltage switch, GPLL7 202 MHz, tuning with CDR), interconnect votes, CQE, SD hot-plug from the GPIO69 interrupt (and the MPM regulator re-enable depthcharge's comment describes), then writes, which for the eMMC need a separate decision.
