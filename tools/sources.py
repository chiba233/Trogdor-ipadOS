#!/usr/bin/env python3
"""Fetch or verify pinned upstream references without building upstream code."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SPARSE = {
    "coreboot": ["src/mainboard/google/trogdor", "src/soc/qualcomm/sc7180",
                 "src/arch/arm64", "payloads/libpayload/arch/arm64",
                 "src/include", "src/lib"],
}
def run(*args, cwd=None):
    subprocess.run(["git", *args], cwd=cwd, check=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("fetch", "verify"))
    parser.add_argument("sources", nargs="*")
    args = parser.parse_args()
    lock = json.loads((ROOT / "upstream.lock.json").read_text())["sources"]
    selected = args.sources or ["coreboot", "depthcharge"]
    for name in selected:
        if name not in lock:
            parser.error("unknown source: " + name)
        source = lock[name]
        path = ROOT / "upstream" / name
        if args.action == "fetch":
            path.parent.mkdir(exist_ok=True)
            if not path.exists():
                run("clone", "--filter=blob:none", "--no-checkout", source["url"], str(path))
            run("fetch", "--depth=1", "origin", source["commit"], cwd=path)
            if name in SPARSE:
                run("sparse-checkout", "set", *SPARSE[name], cwd=path)
            run("checkout", "--detach", source["commit"], cwd=path)
        head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=path, text=True).strip()
        changes = subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=no"],
                                          cwd=path, text=True)
        if head != source["commit"] or changes:
            raise SystemExit(name + ": revision mismatch or tracked source changes")
        print(name + ": " + head)

if __name__ == "__main__":
    main()
