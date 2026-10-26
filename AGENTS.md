# Trogdor-ipadOS

- Target: native iPadOS on HP Chromebook x2 11 / Google Coachz (SC7180).
- Read `docs/native-boot.md`, `docs/design.md`, and `docs/status.md` before changes.
- Preserve the factory Coreboot / TF-A / Depthcharge chain. The initial entry is a developer-mode signed FIT on removable media. A virtual machine is not the product target.
- Keep source observations, host verification, and hardware observations explicitly separate. Hardware attempts are recorded in `docs/hardware-tests.md`; the Linux log receiver has captured entry EL2, probe EL1, stage=3, and handoff EL2 directly on hardware; the source-XNU pre-MMU entry checkpoint has also executed at EL1, verified boot_args, and returned to Linux. That earlier checkpoint alone does not establish later initialization or a full kernel/OS boot.
- The separate MMU-on checkpoint has now passed on hardware: complete table-pool, EL1 register and AT translation checks succeeded before restoring state and returning to Linux. See `docs/xnu-mmu-checkpoint.md`. The later high-VA milestone is recorded separately; C initialization, ADT consumption and a full XNU/iPadOS boot remain unverified.
- The high-VA/C-entry stack-read checkpoint has passed on hardware: original RET reached the real arm_init KVA, its original frame adjustment ran, diagnostic stack read and all register/table/AT checks passed, and Linux received the log. See `docs/xnu-highva-checkpoint.md`; this does not execute the C initialization body or validate stack writes.
- Do not assume Android ABL, a persistent splash framebuffer, EL1 entry, UFS storage, or an EFI interface.
- No automatic SPI, eMMC, partition, EC, or firmware writes. Image creation operates on ordinary files only.
- Do not publish firmware dumps, serials, calibration data, private guidance, Apple binaries, or root images.
- Keep upstream sources under ignored `upstream/`, pinned by `upstream.lock.json`. Preserve upstream licenses; do not copy large upstream trees into tracked source.
- Use `python3 tools/build.py` and `python3 -m unittest discover -s tests -v` for host verification. Never describe these checks as a real device boot.
