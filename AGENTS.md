# Trogdor-ipadOS

- Target: native iPadOS on HP Chromebook x2 11 / Google Coachz (SC7180).
- Read `docs/native-boot.md`, `docs/design.md`, and `docs/status.md` before changes.
- Preserve the factory Coreboot / TF-A / Depthcharge chain. The initial entry is a developer-mode signed FIT on removable media. A virtual machine is not the product target.
- Keep source observations, host verification, and hardware observations explicitly separate. Hardware attempts are recorded in `docs/hardware-tests.md`; no EL1 checkpoint has been confirmed.
- Do not assume Android ABL, a persistent splash framebuffer, EL1 entry, UFS storage, or an EFI interface.
- No automatic SPI, eMMC, partition, EC, or firmware writes. Image creation operates on ordinary files only.
- Do not publish firmware dumps, serials, calibration data, private guidance, Apple binaries, or root images.
- Keep upstream sources under ignored `upstream/`, pinned by `upstream.lock.json`. Preserve upstream licenses; do not copy large upstream trees into tracked source.
- Use `python3 tools/build.py` and `python3 -m unittest discover -s tests -v` for host verification. Never describe these checks as a real device boot.
