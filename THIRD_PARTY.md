# Upstream references

Original project source is BSD-2-Clause. Upstream repositories are not relicensed
by this project. Their tracked revisions and origins are in upstream.lock.json.

- ChromiumOS Coreboot, Depthcharge, vboot and Velvet tools: firmware and packaging
  interface references; retain their respective GPL and per-file notices.
- UniXNU DarwinBoot: BSD-2-Clause-Patent; reference for the future XNU handoff.
- ios-sweet (MIT, Copyright (c) 2026 dipa-studio): `platform/coachz-screen/patches/xnu/coachz-screen-console.patch` adapts its early framebuffer console, RAM disk root changes and built-in AMFI interface; `tools/coachz_init.S` and `tools/xnu_init.py` adapt its minimal init and static Mach-O packer. Also a reference for direct native ARM64 shim loading; consult upstream
  per-file licensing before importing any implementation.
- Zig 0.15.2: MIT toolchain distributed by ziglang on PyPI, installed separately
  in the ignored .tools directory.
- ARM64 Image and FDT interfaces follow their published formats.

No upstream source trees, toolchain binaries, firmware dumps, developer keys,
Apple firmware, kernelcache, or system images are included in tracked files.

- UniXNU ae687009: imported at runtime from the separately fetched, pinned
  checkout for source build orchestration and ADT generation; its licenses
  and component licenses remain in that checkout. No implementation is vendored.
- Apple XNU 10063.141.1: separately fetched Apple open-source reference, subject
  to its original license notices; the ARM64 boot header is used for an optional
  compile-time ABI test. Source kernel products remain in ignored build output.
- Optional project-local CMake 3.31.6 (BSD-3-Clause) and Ninja 1.11.1.4
  (Apache-2.0 for Ninja) are build dependencies, never bundled in tracked files.
