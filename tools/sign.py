#!/usr/bin/env python3
"""Create a developer-key vboot container as a regular file, never flash it."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from fdt import properties

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fit", type=Path)
    parser.add_argument("--futility", required=True, type=Path)
    parser.add_argument("--key-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cmdline", type=Path, help="explicit native Linux command line file")
    args = parser.parse_args()
    source, output = args.fit.resolve(), args.output.resolve()
    if not source.is_file():
        parser.error("FIT input must be a regular file")
    properties(source.read_bytes())
    if output == source or output.suffix != ".kpart" or output.is_relative_to(Path("/dev")):
        parser.error("output must be a separate .kpart file outside /dev")
    if output.exists():
        parser.error("output already exists; choose a new filename")
    for key in ("kernel.keyblock", "kernel_data_key.vbprivk", "kernel_subkey.vbpubk"):
        if not (args.key_dir / key).is_file():
            parser.error("missing developer key: " + key)
    cmdline = args.cmdline.read_bytes() if args.cmdline else b"trogdor_ipados.phase=0\n"
    if not cmdline.strip() or len(cmdline) > 4096 or b"\0" in cmdline:
        parser.error("invalid command line")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="vboot-", dir=output.parent) as scratch:
        scratch = Path(scratch)
        (scratch / "bootloader.bin").write_bytes(bytes(512))
        (scratch / "cmdline").write_bytes(cmdline)
        packed = scratch / "probe.kpart"
        subprocess.run([str(args.futility.resolve()), "vbutil_kernel", "--pack", str(packed),
                        "--keyblock", str((args.key_dir / "kernel.keyblock").resolve()),
                        "--signprivate", str((args.key_dir / "kernel_data_key.vbprivk").resolve()),
                        "--version", "1", "--config", str(scratch / "cmdline"),
                        "--bootloader", str(scratch / "bootloader.bin"),
                        "--vmlinuz", str(source), "--arch", "arm"], check=True)
        subprocess.run([str(args.futility.resolve()), "vbutil_kernel", "--verify", str(packed),
                        "--signpubkey", str((args.key_dir / "kernel_subkey.vbpubk").resolve())], check=True)
        # Exclusive creation also protects against another process creating this output in the meantime.
        with output.open("xb") as dest:
            dest.write(packed.read_bytes())
    print("Created and verified file:", output)
    print("Firmware policy and real boot have not been tested.")

if __name__ == "__main__":
    main()
