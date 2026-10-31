#!/usr/bin/env python3
"""Build a native EL1 probe -> EL2 Linux log receiver from an existing container."""
import argparse
import hashlib
import json
import lzma
import os
from pathlib import Path
import struct
import subprocess
import sys
import zlib

from artifacts import extract_image, inspect_image, inspect_scalar_code
from build import CPU, elf_symbols
from disk import create_image
import usb_stack
from relocation_audit import audit_sources
from fdt import Node, decode, properties, string

ROOT = Path(__file__).resolve().parents[1]
LOG_PROPERTY = "trogdor-ipados,probe-log"
LOG_BYTES = 8192
MAX_LINUX = 128 * 1024 * 1024


def sha(data):
    return hashlib.sha256(data).hexdigest()


def child(node, name):
    matches = [c for c in node.children if c.name == name]
    if len(matches) != 1:
        raise ValueError("missing/duplicate FIT node: " + name)
    return matches[0]


def cstring(data):
    if not data.endswith(b"\0") or b"\0" in data[:-1]:
        raise ValueError("expected a single FIT string")
    return data[:-1].decode("ascii")


def digest(data, algo):
    if algo == "crc32":
        return struct.pack(">I", zlib.crc32(data))
    if algo in ("sha1", "sha256"):
        return hashlib.new(algo, data).digest()
    raise ValueError("unsupported FIT hash: " + algo)


def hashes(node, update=False):
    for h in node.children:
        if h.name.startswith("signature"):
            raise ValueError("signed FIT nodes are not supported")
        algo = cstring(h.props["algo"])
        value = digest(node.props["data"], algo)
        if update:
            h.props["value"] = value
        elif h.props.get("value") != value:
            raise ValueError("source FIT hash mismatch: " + node.name)


def linux_image(data, compression):
    if compression == "lzma":
        decoder = lzma.LZMADecompressor()
        data = decoder.decompress(data, max_length=MAX_LINUX + 1)
        if not decoder.eof or decoder.unused_data:
            raise ValueError("oversized or noncanonical compressed Linux Image")
    elif compression != "none":
        raise ValueError("unsupported Linux compression")
    if not 64 <= len(data) <= MAX_LINUX:
        raise ValueError("invalid Linux Image file size")
    offset, memory, flags = struct.unpack_from("<3Q", data, 8)
    if (struct.unpack_from("<I", data, 56)[0] != 0x644d5241 or offset != 0 or
            not len(data) <= memory <= MAX_LINUX or flags & 1 or not flags & 8):
        raise ValueError("requires relocatable little-endian ARM64 Image with text_offset=0")
    return data, memory


def add_log(dtb):
    tree = decode(dtb)
    chosen = [c for c in tree.root.children if c.name == "chosen"]
    if not chosen:
        chosen = [Node("chosen")]
        tree.root.children.extend(chosen)
    if LOG_PROPERTY in chosen[0].props:
        raise ValueError("source DTB already has a probe log")
    chosen[0].props[LOG_PROPERTY] = bytes(LOG_BYTES)
    result = tree.encode()
    if len(result) > 2 * 1024 * 1024:
        raise ValueError("DTB exceeds ARM64 boot limit")
    old, new = properties(dtb), properties(result)
    expected = dict(old)
    expected[("/chosen", LOG_PROPERTY)] = bytes(LOG_BYTES)
    if new != expected:
        raise ValueError("unexpected DTB property changes")
    return result


def compile_wrapper(out, kernel, memory, checkpoint=None, discovery=False, pattern=False, backlight=False, screen=False, usb_read=False):
    if discovery and checkpoint is not None and not screen: raise ValueError("display discovery and XNU are separate images")
    zig = Path(os.environ.get("ZIG", ROOT / ".tools/ziglang/ziglang/zig")).resolve()
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT / ".tools/zig-cache"),
               ZIG_LOCAL_CACHE_DIR=str(ROOT / ".tools/zig-local-cache"))
    if subprocess.check_output([str(zig), "version"], env=env, text=True).strip() != "0.15.2":
        raise ValueError("requires pinned Zig 0.15.2")
    payload = out / "linux.Image"
    payload.write_bytes(kernel)
    quoted = str(payload).replace("\\", "\\\\").replace('"', '\\"')
    assembly = out / "payload.S"
    assembly.write_text(('.global XNU_IMAGE_LIMIT\n.set XNU_IMAGE_LIMIT, ' + str(512 << 20) + '\n' if screen else '') +
                        '.global LINUX_MEMORY_SIZE\n.set LINUX_MEMORY_SIZE, ' + str(memory) + '\n.section .linux, "a"\n.incbin "' + quoted + '"\n')
    if checkpoint is not None:
        if len(checkpoint)<1152 or checkpoint[:8]!=b'TRXNUC01' or int.from_bytes(checkpoint[8:12],'little') not in (1,2,3,4,5,6,7,8,9,10,11,12):
            raise ValueError("invalid source-XNU checkpoint payload header")
        payload_path = out / "xnu-checkpoint.bin"
        payload_path.write_bytes(checkpoint)
        quoted_xnu = str(payload_path).replace("\\", "\\\\").replace('"', '\\"')
        with assembly.open('a') as stream:
            stream.write('// payload sha256 ' + sha(checkpoint) + ' (the compiler cache does not track .incbin)\n.section .xnu_payload, "a"\n.balign 16\n.global _xnu_payload_start, _xnu_payload_end\n_xnu_payload_start:\n.incbin "' + quoted_xnu + '"\n_xnu_payload_end:\n')
    elf = out / "probe.elf"
    usb_objects, usb_relocations = usb_stack.build(zig, CPU, env, out) if usb_read else ((), {})
    command = [str(zig), "cc", "-target", "aarch64-freestanding-none", "-mcpu=" + CPU,
               "-mgeneral-regs-only", "-ffreestanding", "-fno-builtin", "-fno-pic", "-fno-pie",
               "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
               "-Wall", "-Wextra", "-Werror", "-Os", "-g", "-nostdlib", "-static",
               "-DPROBE_UART=0", "-DPROBE_RESET_EL=0", "-DPROBE_LINUX_CHAIN=1",
               "-DPROBE_XNU_CHECKPOINT=" + str(int(checkpoint is not None)),
               "-DPROBE_DISPLAY_DISCOVERY=" + str(int(discovery)),
               "-DPROBE_DISPLAY_PATTERN=" + str(int(pattern)),
               "-DPROBE_DISPLAY_BACKLIGHT=" + str(int(backlight)),
               "-DPROBE_XNU_SCREEN=" + str(int(screen)),
               "-DPROBE_USB_READ=" + str(int(usb_read)),
               "-Wl,--build-id=none", "-Wl,--no-undefined", "-Wl,-z,max-page-size=4096",
               "-Wl,-T," + str(ROOT / ("boot/xnu-chain.ld" if checkpoint is not None else "boot/chain.ld")),
               *[str(ROOT / "boot" / name) for name in ("entry.S", "probe.c", "fdt.c", "checkpoint.c")],
               *([str(ROOT / "boot" / name) for name in
                  ("xnu_checkpoint.c", "xnu_load.c", "xnu_memory.c", "xnu_tables.c", "xnu_cinit.c", "xnu_cpu.c", "xnu_platform.c", "xnu_pe.c", "xnu_diagnostics.c", "xnu_topology.c")] if checkpoint is not None else []),
               *([str(ROOT / "boot" / name) for name in ("coreboot.c","display_state.c","display.c",*(() if screen else ("xnu_memory.c",)))] if discovery else []),
               *([str(ROOT / "boot/xnu_screen.c")] if screen else []),
               *([str(ROOT / "boot/display_ec.c")] if backlight else []),
               *([str(ROOT / "boot/xnu_memory.c")] if usb_read else []),
               *[str(obj) for obj in usb_objects],
               str(assembly), "-o", str(elf)]
    relocations=audit_sources(zig,CPU,[Path(arg) for arg in command if arg.endswith('.c')],env,
        out/'relocation-audit',('PROBE_UART=0','PROBE_RESET_EL=0','PROBE_LINUX_CHAIN=1',
        'PROBE_XNU_CHECKPOINT='+str(int(checkpoint is not None)), 'PROBE_USB_READ='+str(int(usb_read)),'PROBE_DISPLAY_DISCOVERY='+str(int(discovery)),'PROBE_DISPLAY_PATTERN='+str(int(pattern)),'PROBE_DISPLAY_BACKLIGHT='+str(int(backlight)),'PROBE_XNU_SCREEN='+str(int(screen))))
    subprocess.run(command, env=env, check=True)
    linked = elf.read_bytes()
    image = extract_image(linked)
    header, scalar, symbols = inspect_image(image), inspect_scalar_code(linked), elf_symbols(linked)
    start = symbols["_linux_entry"]["address"]
    expected_memory = start + memory
    extra = {}
    if checkpoint is not None:
        if checkpoint not in image:
            raise ValueError("Image does not contain the requested XNU payload")
        arena = symbols["_xnu_window"]["address"]
        expected_memory = arena + 24 * 1024 * 1024
        if arena % 0x200000 or arena < start + memory:
            raise ValueError("XNU arena overlaps Linux memory")
        extra = {"xnu_checkpoint_version": int.from_bytes(checkpoint[8:12],"little"),
                 "xnu_payload_sha256": sha(checkpoint), "xnu_window_offset": arena, "xnu_window_bytes": 24 * 1024 * 1024}
    if (start % 0x200000 or start < symbols["_stack_top"]["address"] or
            header["memory_size"] != expected_memory or
            image[start:start + len(kernel)] != kernel or len(image) != start + len(kernel)):
        raise ValueError("Linux wrapper memory/payload layout mismatch")
    (out / "Image").write_bytes(image)
    return image, {"header": header, "scalar_code": scalar, "c_relocations": {**relocations, **usb_relocations}, "linux_offset": start, **({"display_discovery_schema":4 if backlight else 3 if pattern else 2} if discovery else {}), **extra}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, required=True, help="existing signed native Linux container")
    p.add_argument("--futility", type=Path, required=True)
    p.add_argument("--key-dir", type=Path, required=True)
    p.add_argument("--xnu-checkpoint", type=Path, help="instrumented source-XNU checkpoint payload")
    p.add_argument("--display-discovery", action="store_true", help="separate read-only display metadata/MMIO discovery, without XNU")
    p.add_argument("--display-pattern", action="store_true", help="discovery plus the first scanout restart; writes display registers and the framebuffer, no EC or backlight")
    p.add_argument("--display-backlight", action="store_true", help="display pattern plus EC backlight; sends runtime EC commands, never writes EC firmware")
    p.add_argument("--xnu-screen", type=Path, help="screen-output XNU payload: keeps the display on and never returns to Linux")
    p.add_argument("--xnu-root-partition", type=Path, action="append", default=[],
                   help="with --xnu-screen: APFS container for a GPT partition after the kernel; repeat for more, in order")
    p.add_argument("--xnu-kernel-only", action="store_true",
                   help="with --xnu-root-partition: an image of only the GPT and kernel partition, for a medium that has the containers")
    p.add_argument("--usb-read", action="store_true", help="read three known sectors of the USB medium through the libpayload stack, then Linux; issues no writes")
    p.add_argument("--output", type=Path, default=ROOT / "build/linux-log")
    a = p.parse_args()
    if sum(map(bool, (a.display_discovery, a.display_pattern, a.display_backlight))) > 1: p.error("choose one display mode")
    if a.xnu_screen and (a.display_discovery or a.display_pattern or a.display_backlight or a.xnu_checkpoint): p.error("xnu screen is its own image")
    if a.usb_read and (a.display_discovery or a.display_pattern or a.display_backlight or a.xnu_checkpoint or a.xnu_screen): p.error("USB read is its own image")
    if a.xnu_root_partition and not a.xnu_screen: p.error("a root partition needs --xnu-screen")
    if a.xnu_kernel_only and not a.xnu_root_partition: p.error("a kernel-only image needs its root partitions")
    a.screen = bool(a.xnu_screen)
    a.display_backlight = a.display_backlight or a.screen
    a.pattern = a.display_pattern or a.display_backlight
    a.display_discovery = a.display_discovery or a.pattern
    if a.display_discovery and a.xnu_checkpoint: p.error("display discovery and XNU require separate images")
    a.xnu_checkpoint = a.xnu_checkpoint or a.xnu_screen
    out, source, futility = a.output.resolve(), a.source.resolve(), str(a.futility.resolve())
    if out.exists() or out.is_relative_to(Path("/dev")) or not source.is_file():
        p.error("source must be a file and output a new ordinary directory outside /dev")
    subprocess.run([futility, "vbutil_kernel", "--verify", str(source), "--signpubkey",
                    str((a.key_dir / "kernel_subkey.vbpubk").resolve())], check=True,
                   stdout=subprocess.DEVNULL)
    cmdline = subprocess.check_output([futility, "dump_kernel_config", str(source)])
    if not cmdline.strip() or len(cmdline) > 4096 or b"\0" in cmdline:
        p.error("invalid source command line")
    out.mkdir(parents=True)
    extracted = out / "source.itb"
    subprocess.run([futility, "vbutil_kernel", "--get-vmlinuz", str(source),
                    "--vmlinuz-out", str(extracted)], check=True)
    raw = extracted.read_bytes()
    total = struct.unpack_from(">I", raw, 4)[0]
    original = raw[:total]
    tree = decode(original)
    images, configs = child(tree.root, "images"), child(tree.root, "configurations")
    kernels = {cstring(c.props["kernel"]) for c in configs.children}
    if len(kernels) != 1:
        p.error("requires configurations sharing one native kernel")
    for node in images.children:
        hashes(node)
    kernel_node = child(images, kernels.pop())
    kernel, memory = linux_image(kernel_node.props["data"], cstring(kernel_node.props["compression"]))
    wrapper, layout = compile_wrapper(out, kernel, memory,
        a.xnu_checkpoint.read_bytes() if a.xnu_checkpoint else None, discovery=a.display_discovery, pattern=a.pattern, backlight=a.display_backlight, screen=a.screen, usb_read=a.usb_read)
    kernel_node.props["data"], kernel_node.props["compression"] = wrapper, string("none")
    hashes(kernel_node, True)
    count = 0
    for node in images.children:
        if node.props.get("type") == string("flat_dt"):
            if node.props.get("compression") != string("none"):
                p.error("requires uncompressed source DTBs")
            node.props["data"] = add_log(node.props["data"])
            hashes(node, True)
            count += 1
    if not count:
        p.error("no complete DTB found")
    packed = tree.encode()
    before, after = properties(original), properties(packed)
    allowed = {(path, key) for path, key in after
               if path.startswith("/images/" + kernel_node.name + "/") and key == "value"}
    allowed |= {("/images/" + kernel_node.name, key) for key in ("data", "compression")}
    for node in images.children:
        if node.props.get("type") == string("flat_dt"):
            allowed.add(("/images/" + node.name, "data"))
            allowed |= {(path, key) for path, key in after
                        if path.startswith("/images/" + node.name + "/") and key == "value"}
    changed = {key for key in before.keys() | after.keys() if before.get(key) != after.get(key)}
    if not changed <= allowed:
        p.error("unexpected FIT configuration/ramdisk/metadata change")
    fit_path, cmd_path = out / "probe.itb", out / "cmdline.txt"
    fit_path.write_bytes(packed)
    cmd_path.write_bytes(cmdline)
    kpart, image = out / "linux-log.kpart", out / "linux-log.img"
    subprocess.run([sys.executable, str(ROOT / "tools/sign.py"), str(fit_path),
                    "--futility", futility, "--key-dir", str(a.key_dir.resolve()),
                    "--cmdline", str(cmd_path), "--output", str(kpart)], check=True,
                   stdout=subprocess.DEVNULL)
    if subprocess.check_output([futility, "dump_kernel_config", str(kpart)]) != cmdline:
        p.error("signed command line differs from baseline")
    # The screen image never returns to Linux, and an unexplained panic can need a plain retry.
    # A prelinked kernel or a RAM disk root does not fit the usual 96 MiB partition; smaller images keep their exact layout.
    disk_mib, kernel_mib = next(sizes for sizes in ((128, 96), (256, 224), (512, 480)) if kpart.stat().st_size <= sizes[1] << 20)
    root_mib = sum(-(-root.stat().st_size // (1 << 20)) for root in a.xnu_root_partition)
    if root_mib:
        disk_mib = kernel_mib + root_mib + 4
    create_image(kpart, image, "EL1 probe Linux log", disk_mib=disk_mib, kernel_mib=kernel_mib, tries=15 if a.screen or a.usb_read else 1,
                 roots=a.xnu_root_partition, kernel_only=a.xnu_kernel_only)
    code_files = ("boot/entry.S", "boot/probe.c", "boot/fdt.c", "boot/checkpoint.c", "boot/chain.ld",
                  "tools/linux_log.py", "tools/fdt.py", "tools/build.py", "tools/artifacts.py",
                  "tools/sign.py", "tools/disk.py", "tools/read_probe.py", "tools/relocation_audit.py")
    if a.xnu_checkpoint:
        code_files += ("boot/xnu_checkpoint.h", "boot/xnu_checkpoint.c", "boot/xnu_load.c",
                       "boot/xnu_memory.c", "boot/xnu_tables.c", "boot/xnu_cinit.c", "boot/xnu_cpu.c", "boot/xnu_platform.c", "boot/xnu_pe.c", "boot/xnu_diagnostics.c", "boot/xnu_topology.c", "boot/xnu-chain.ld", "tools/xnu_checkpoint.py", "tools/xnu_macho.py", "tools/xnu_cinit.py", "tools/xnu_cpu.py", "tools/xnu_platform.py", "tools/xnu_pe.py", "tools/xnu_soc.py", "tools/xnu_diagnostics.py", "tools/xnu_topology.py")
    if a.display_discovery:
        code_files += ("boot/coreboot.h", "boot/coreboot.c", "boot/display.h", "boot/display_state.c", "boot/display.c", "boot/xnu_memory.c", "boot/xnu_checkpoint.h", "tools/read_display.py")
    if a.display_backlight:
        code_files += ("boot/display_ec.c",)
    if a.screen:
        code_files += ("boot/xnu_screen.c", "tools/xnu_screen.py", "tools/xnu_init.py", "tools/coachz_init.S")
    if a.usb_read:
        code_files += ("boot/usb_read.c", "tools/usb_stack.py", "boot/usb_compat/libpayload.h")
    report = {"schema": 1, "display_discovery": a.display_discovery, "display_pattern": a.pattern, "display_backlight": a.display_backlight, "xnu_screen": a.screen,
              "xnu_root_partitions": [{"name": root.name, "bytes": root.stat().st_size} for root in a.xnu_root_partition],
              "xnu_kernel_only": a.xnu_kernel_only, "xnu_checkpoint": bool(a.xnu_checkpoint), "hardware_boot_tested": False,
              "toolchain": {"zig": "0.15.2", "cpu": CPU},
              "code_sha256": {name: sha((ROOT / name).read_bytes()) for name in code_files},
              "source_sha256": sha(source.read_bytes()),
              "linux_sha256": sha(kernel), "cmdline_sha256": sha(cmdline), "dtb_count": count,
              "log_property": LOG_PROPERTY, "log_bytes": LOG_BYTES, "layout": layout,
              "artifacts": {f.name: {"sha256": sha(f.read_bytes()), "bytes": f.stat().st_size}
                            for f in (out / "probe.elf", out / "Image", fit_path, kpart, image)}}
    (out / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Built native probe/Linux log image; source command line, DTBs and initrd preserved.")
    print(report["artifacts"][image.name]["sha256"], image.name)
    print("No device was written; hardware boot remains untested.")


if __name__ == "__main__":
    main()
