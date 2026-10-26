#!/usr/bin/env python3
"""Sign the two diagnostic FITs and generate USB image files without device access."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
from disk import create_image

ROOT = Path(__file__).resolve().parents[1]
KEYS = ("kernel.keyblock", "kernel_data_key.vbprivk", "kernel_subkey.vbpubk")
MODES = (("reset-el2", 3), ("reset-el1", 10))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--futility", type=Path)
    p.add_argument("--key-dir", type=Path)
    p.add_argument("--output", type=Path, default=ROOT / "build/usb")
    a = p.parse_args()
    futility = str(a.futility.resolve()) if a.futility else shutil.which("futility")
    if not futility or not Path(futility).is_file():
        p.error("futility is required; Debian package: vboot-kernel-utils")
    candidates = [a.key_dir] if a.key_dir else [Path("/usr/share/vboot/devkeys"),
                   Path("/usr/share/vboot/devkeys-pkc"), ROOT / "upstream/vboot/tests/devkeys"]
    keys = next((d for d in candidates if all((d / key).is_file() for key in KEYS)), None)
    if keys is None:
        p.error("public developer test keys not found; specify --key-dir")
    out = a.output.resolve()
    if out.is_relative_to(Path("/dev")):
        p.error("output must be an ordinary directory outside /dev")
    if (out / "manifest.json").exists():
        p.error("manifest exists; choose a fresh --output directory")
    manifest = json.loads((ROOT / "build/manifest.json").read_text())
    report = {"schema": 1, "hardware_tested": False,
              "futility_sha256": hashlib.sha256(Path(futility).read_bytes()).hexdigest(),
              "build_manifest_sha256": hashlib.sha256((ROOT / "build/manifest.json").read_bytes()).hexdigest(),
              "artifacts": {}}

    for mode, _ in MODES:
        fit = ROOT / "build" / mode / "probe.itb"
        expected = manifest["artifacts"][mode + "/probe.itb"]["sha256"]
        if hashlib.sha256(fit.read_bytes()).hexdigest() != expected:
            p.error("FIT does not match build manifest: " + mode)
        for suffix in (".kpart", ".img"):
            if (out / (mode + suffix)).exists():
                p.error("output exists; choose a fresh --output directory")
    for mode, seconds in MODES:
        kpart, image = out / (mode + ".kpart"), out / (mode + ".img")
        subprocess.run([sys.executable, str(ROOT / "tools/sign.py"),
                        str(ROOT / "build" / mode / "probe.itb"),
                        "--futility", futility, "--key-dir", str(keys),
                        "--output", str(kpart)], check=True)
        create_image(kpart, image, mode + " " + str(seconds) + "s")
        for artifact in (kpart, image):
            report["artifacts"][artifact.name] = {
                "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(),
                "bytes": artifact.stat().st_size, "target_el": 2 if mode == "reset-el2" else 1,
                "delay_seconds": seconds}
        print(report["artifacts"][image.name]["sha256"], image.name, flush=True)
    with (out / "manifest.json").open("x") as dest:
        json.dump(report, dest, indent=2)
        dest.write("\n")
    print("Created image files only. Hardware boot and timing remain untested.")


if __name__ == "__main__":
    main()
