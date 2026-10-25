# Upstream references

Original project source is BSD-2-Clause. Upstream repositories are not relicensed
by this project. Their tracked revisions and origins are in upstream.lock.json.

- ChromiumOS Coreboot, Depthcharge and Velvet tools: firmware and packaging
  interface references; retain their respective GPL and per-file notices.
- UniXNU DarwinBoot: BSD-2-Clause-Patent; reference for the future XNU handoff.
- ios-sweet: reference for direct native ARM64 shim loading; consult upstream
  per-file licensing before importing any implementation.
- Zig 0.15.2: MIT toolchain distributed by ziglang on PyPI, installed separately
  in the ignored .tools directory.
- ARM64 Image and FDT interfaces follow their published formats.

No upstream source trees, toolchain binaries, firmware dumps, developer keys,
Apple firmware, kernelcache, or system images are included in tracked files.
