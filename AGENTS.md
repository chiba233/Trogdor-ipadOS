# Trogdor-ipadOS

- Target: native iPadOS on HP Chromebook x2 11 / Google Coachz (SC7180).
- Read `docs/native-boot.md`, `docs/design.md`, and `docs/status.md` before changes.
- Preserve the factory Coreboot / TF-A / Depthcharge chain. The initial entry is a developer-mode signed FIT on removable media. A virtual machine is not the product target.
- Keep source observations, host verification, and hardware observations explicitly separate. Hardware attempts are recorded in `docs/hardware-tests.md`; the Linux log receiver has captured entry EL2, probe EL1, stage=3, and handoff EL2 directly on hardware; the source-XNU pre-MMU entry checkpoint has also executed at EL1, verified boot_args, and returned to Linux. This does not establish MMU/C initialization, a full XNU boot, or an iPadOS boot.
- Do not assume Android ABL, a persistent splash framebuffer, EL1 entry, UFS storage, or an EFI interface.
- No automatic SPI, eMMC, partition, EC, or firmware writes. Image creation operates on ordinary files only.
- Do not publish firmware dumps, serials, calibration data, private guidance, Apple binaries, or root images.
- Keep upstream sources under ignored `upstream/`, pinned by `upstream.lock.json`. Preserve upstream licenses; do not copy large upstream trees into tracked source.
- Use `python3 tools/build.py` and `python3 -m unittest discover -s tests -v` for host verification. Never describe these checks as a real device boot.
