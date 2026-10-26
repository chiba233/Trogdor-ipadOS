#!/usr/bin/env python3
"""Create a matched EL1 control by replacing only the final reset SMC with NOP."""
import argparse
import hashlib
import json
from pathlib import Path

from artifacts import extract_image, fit, inspect_image, inspect_scalar_code
from build import elf_symbols
from fdt import properties

ROOT = Path(__file__).resolve().parents[1]
SMC = bytes.fromhex("030000d4")
NOP = bytes.fromhex("1f2003d5")
# Exact instruction sequences from the pinned scalar Zig 0.15.2 build.
# PSCI_VERSION: x0=0x84000000, x1..x3=0, CNTFRQ read, SMC.
VERSION = bytes.fromhex("0080b052 e1031faa e2031faa e3031faa 15e03bd5 030000d4")
# SYSTEM_RESET: x0=0x84000009, barrier, x1..x3=0, SMC.
RESET = bytes.fromhex("20018052 9f3f03d5 e1031faa 0080b072 e2031faa e3031faa 030000d4")


def inhibit_reset(image, address, size):
    """Fail closed on an unfamiliar checkpoint; preserve all other Image bytes."""
    inspect_image(image)
    if address < 64 or address % 4 or size % 4 or size <= 0 or address + size > len(image):
        raise ValueError("invalid checkpoint function range")
    code = image[address:address + size]
    calls = [p for p in range(0, len(code), 4) if code[p:p + 4] == SMC]
    version, reset = code.find(VERSION), code.find(RESET)
    if (version < 0 or reset < 0 or version % 4 or reset % 4 or
            code.count(VERSION) != 1 or code.count(RESET) != 1 or
            calls != [version + len(VERSION) - 4, reset + len(RESET) - 4] or
            version + len(VERSION) > reset):
        raise ValueError("unexpected PSCI instruction sequences; no patch created")
    at = address + reset + len(RESET) - 4
    control = image[:at] + NOP + image[at + 4:]
    assert len(control) == len(image)
    return control, at


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/control-el1")
    args = parser.parse_args()
    out = args.output.resolve()
    if out.is_relative_to(Path("/dev")) or out.exists():
        parser.error("output must be a new ordinary directory outside /dev")
    build = ROOT / "build"
    raw_manifest = (build / "manifest.json").read_bytes()
    manifest = json.loads(raw_manifest)
    if (manifest["toolchain"]["zig"] != "0.15.2" or
            manifest["reset-el1"]["reset_target_el"] != 1 or
            manifest["reset-el1"]["reset_delay_seconds"] != 10 or
            manifest["reset-el1"]["uart_mmio_enabled"]):
        parser.error("expected the pinned scalar 10-second EL1 checkpoint")
    sources = {}
    for name in ("probe.elf", "Image", "probe.itb"):
        key = "reset-el1/" + name
        data = (build / key).read_bytes()
        if sha(data) != manifest["artifacts"][key]["sha256"]:
            parser.error("source does not match build manifest: " + key)
        sources[name] = data
    scalar = inspect_scalar_code(sources["probe.elf"])
    if extract_image(sources["probe.elf"]) != sources["Image"]:
        parser.error("ELF and Image differ")
    original = properties(sources["probe.itb"])
    if original[("/images/kernel", "data")] != sources["Image"]:
        parser.error("FIT and Image differ")
    checkpoint = elf_symbols(sources["probe.elf"])["checkpoint_run"]
    control, at = inhibit_reset(sources["Image"], checkpoint["address"], checkpoint["size"])
    payload = fit(control, original[("/images/fdt", "data")])
    packed = properties(payload)
    changed = {key for key in original.keys() | packed.keys() if original.get(key) != packed.get(key)}
    if changed != {("/images/kernel", "data"), ("/images/kernel/hash-1", "value")}:
        parser.error("unexpected FIT property changes")
    report = {"schema": 1, "hardware_tested": False,
              "source_build_manifest_sha256": sha(raw_manifest),
              "source_sha256": {name: sha(data) for name, data in sources.items()},
              "source_scalar_code": scalar, "target_el": 1, "delay_seconds": 10,
              "reset_inhibited": True,
              "patch": {"image_offset": at, "before_hex": SMC.hex(), "after_hex": NOP.hex()},
              "artifacts": {name: {"sha256": sha(data), "bytes": len(data)}
                            for name, data in (("Image", control), ("probe.itb", payload))}}
    out.mkdir(parents=True)
    for name, data in (("Image", control), ("probe.itb", payload)):
        with (out / name).open("xb") as dest:
            dest.write(data)
    with (out / "manifest.json").open("x") as dest:
        json.dump(report, dest, indent=2)
        dest.write("\n")
    print(f"Created matched control: final SMC at Image +{at:#x} replaced with NOP")
    print("Other Image bytes and non-kernel FIT properties preserved. Hardware remains untested.")


if __name__ == "__main__":
    main()
