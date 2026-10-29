#!/usr/bin/env python3
"""Build native probes and their unsigned FIT containers."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from artifacts import diagnostic_dtb, extract_image, fit, inspect_image, inspect_scalar_code
from fdt import properties
from relocation_audit import audit_sources

ROOT = Path(__file__).resolve().parents[1]
CPU = "generic+el3+strict_align-fp_armv8-neon"

def elf_symbols(data):
    if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 18)[0] != 183:
        raise ValueError("expected little-endian AArch64 ELF")
    offset = struct.unpack_from("<Q", data, 40)[0]
    stride, count = struct.unpack_from("<HH", data, 58)
    sections = [struct.unpack_from("<II4QII2Q", data, offset + i * stride) for i in range(count)]
    symbols = {}
    for section in sections:
        if section[1] in (4, 9) and section[5]:
            raise ValueError("final ELF has relocations")
        if section[1] != 2:
            continue
        strings = sections[section[6]]
        table = data[strings[4]:strings[4] + strings[5]]
        for at in range(section[4], section[4] + section[5], section[9]):
            name, info, _, index, value, size = struct.unpack_from("<IBBHQQ", data, at)
            if not name:
                continue
            name = table[name:table.index(b"\0", name)].decode()
            if index == 0:
                raise ValueError("unresolved symbol: " + name)
            symbols[name] = {"address": value, "size": size}
    return symbols

def main():
    candidates = [os.environ.get("ZIG"), str(ROOT / ".tools/ziglang/ziglang/zig"), shutil.which("zig")]
    zig = next((str(Path(p).resolve()) for p in candidates if p and Path(p).is_file()), None)
    if not zig:
        raise SystemExit("Install Zig 0.15.2 under .tools/ziglang or set ZIG; see README.")
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT / ".tools/zig-cache"),
               ZIG_LOCAL_CACHE_DIR=str(ROOT / ".tools/zig-local-cache"))
    version = subprocess.check_output([zig, "version"], env=env, text=True).strip()
    if version != "0.15.2":
        raise SystemExit("Expected pinned Zig 0.15.2, found " + version)
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    dtb = diagnostic_dtb()
    (build / "coachz-probe.dtb").write_bytes(dtb)
    manifest = {"schema": 1, "toolchain": {"zig": version, "cpu": CPU}, "hardware_tested": False, "artifacts": {}}
    for mode, uart, reset_el in (("memory", 0, 0), ("uart", 1, 0),
                                 ("reset-el2", 0, 2), ("reset-el1", 0, 1)):
        out = build / mode
        out.mkdir(exist_ok=True)
        elf, image = out / "probe.elf", out / "Image"
        command = [zig, "cc", "-target", "aarch64-freestanding-none", "-mcpu=" + CPU,
                   "-mgeneral-regs-only", "-ffreestanding", "-fno-builtin", "-fno-pic", "-fno-pie",
                   "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
                   "-Wall", "-Wextra", "-Werror", "-Os", "-g", "-nostdlib", "-static",
                   "-DPROBE_UART=" + str(uart), "-DPROBE_RESET_EL=" + str(reset_el), "-Wl,--build-id=none", "-Wl,--no-undefined",
                   "-Wl,-z,max-page-size=4096", "-Wl,-T," + str(ROOT / "boot/link.ld"),
                   str(ROOT / "boot/entry.S"), str(ROOT / "boot/probe.c"), str(ROOT / "boot/fdt.c"),
                   str(ROOT / "boot/checkpoint.c"),
                   "-o", str(elf)]
        relocations=audit_sources(zig,CPU,[Path(arg) for arg in command if arg.endswith('.c')],env,
            out/'relocation-audit',('PROBE_UART='+str(uart),'PROBE_RESET_EL='+str(reset_el)))
        subprocess.run(command, env=env, check=True)
        image.write_bytes(extract_image(elf.read_bytes()))
        data = image.read_bytes()
        header = inspect_image(data)
        scalar = inspect_scalar_code(elf.read_bytes())
        symbols = elf_symbols(elf.read_bytes())
        if symbols["_start"]["address"] != 0 or header["memory_size"] != symbols["_image_end"]["address"]:
            raise ValueError("ELF/Image layout mismatch")
        (out / "probe.map").write_text("# Symbol map generated from the linked ELF\n" + "".join(f"{v['address']:016x} {v['size']:08x} {k}\n" for k, v in sorted(symbols.items(), key=lambda pair: pair[1]['address'])))
        payload = fit(data, dtb)
        p = properties(payload)
        if p[("/images/kernel", "data")] != data or p[("/images/fdt", "data")] != dtb:
            raise ValueError("FIT payload mismatch")
        (out / "probe.itb").write_bytes(payload)
        (out / "symbols.json").write_text(json.dumps(symbols, indent=2) + "\n")
        for path in (image, elf, out / "probe.itb", out / "symbols.json"):
            manifest["artifacts"][str(path.relative_to(build))] = {
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "bytes": path.stat().st_size}
        manifest[mode] = {"image": header, "c_relocations": relocations, "probe_log": symbols["probe_log"],
                          "scalar_code": scalar, "uart_mmio_enabled": bool(uart), "reset_target_el": reset_el,
                          "reset_delay_seconds": {0: 0, 1: 10, 2: 3}[reset_el]}
        print(mode + ": Image/FIT built; header and ELF symbols verified; hardware not tested")
    (build / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

if __name__ == "__main__":
    main()
