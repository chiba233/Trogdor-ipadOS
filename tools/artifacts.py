"""Image/FIT construction and checks; no device I/O."""
import hashlib
import struct
from fdt import Node, encode, properties, string, u32

def inspect_image(image):
    if len(image) < 64:
        raise ValueError("Image shorter than 64-byte header")
    code, code1, offset, size, flags, r2, r3, r4, magic, pe = struct.unpack_from("<II6QII", image)
    if magic != 0x644d5241 or code >> 26 != 0b000101:
        raise ValueError("invalid ARM64 Image entry")
    if offset or code1 or r2 or r3 or r4 or pe or flags != 0xa:
        raise ValueError("unexpected Image ABI")
    if not len(image) <= size <= 512 * 1024 * 1024:
        raise ValueError("invalid Image memory size")
    target = (code & 0x3ffffff) << 2
    if target >= len(image):
        raise ValueError("entry branch outside Image")
    return {"file_size": len(image), "memory_size": size, "text_offset": offset,
            "flags": flags, "branch_target": target}

def diagnostic_dtb():
    return encode(Node("", {
        "model": string("Coachz phase-0 probe (FIT supplied identity)"),
        "compatible": string("google,coachz") + string("qcom,sc7180"),
        "#address-cells": u32(2), "#size-cells": u32(2),
    }, [Node("chosen"), Node("psci", {
        "compatible": string("arm,psci-0.2"), "method": string("smc"),
    })]))

def fit(image, dtb):
    inspect_image(image)
    properties(dtb)
    def hashed(name, data, kind):
        p = {"description": string(name), "data": data, "type": string(kind),
             "arch": string("arm64"), "compression": string("none")}
        if kind == "kernel":
            p["os"] = string("linux")  # Depthcharge ARM64 Image ABI, not a Linux payload.
        return Node(name, p, [Node("hash-1", {
            "algo": string("sha256"), "value": hashlib.sha256(data).digest(),
        })])
    return encode(Node("", {
        "description": string("Coachz native boot diagnostic; no XNU"),
        "#address-cells": u32(1),
    }, [
        Node("images", children=[hashed("kernel", image, "kernel"),
                                 hashed("fdt", dtb, "flat_dt")]),
        Node("configurations", {"default": string("conf-coachz")}, [
            Node("conf-coachz", {
                "description": string("Google Coachz"),
                "compatible": string("google,coachz"),
                "kernel": string("kernel"), "fdt": string("fdt"),
            })
        ])
    ]))

def extract_image(data):
    """Extract file-backed PT_LOAD segments at their linked physical addresses."""
    if data[:6] != b"\x7fELF\x02\x01" or len(data) < 64:
        raise ValueError("expected ELF64 little-endian")
    phoff = struct.unpack_from("<Q", data, 32)[0]
    stride, count = struct.unpack_from("<HH", data, 54)
    if stride < 56 or phoff + stride * count > len(data):
        raise ValueError("invalid program headers")
    loads = []
    for i in range(count):
        kind, _, offset, address, physical, size, memory, _ = struct.unpack_from("<II6Q", data, phoff + i * stride)
        if kind != 1 or not size:
            continue
        if physical != address or size > memory or offset + size > len(data):
            raise ValueError("invalid load segment")
        loads.append((address, offset, size))
    loads.sort()
    if not loads or loads[0][0] != 0:
        raise ValueError("Image must link at address zero")
    end = max(address + size for address, _, size in loads)
    if end > 512 * 1024 * 1024:
        raise ValueError("load segments too large")
    image = bytearray(end)
    previous = 0
    for address, offset, size in loads:
        if address < previous:
            raise ValueError("overlapping load segments")
        image[address:address + size] = data[offset:offset + size]
        previous = address + size
    return bytes(image)


def inspect_scalar_code(data):
    """Reject FP/SIMD/SVE instruction classes in ELF AArch64 $x ranges.

    Mapping symbols distinguish executable instructions from the Image header
    and other embedded data. This is an early-boot ISA check, not a disassembler.
    """
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 18)[0] != 183:
        raise ValueError("expected little-endian AArch64 ELF")
    offset = struct.unpack_from("<Q", data, 40)[0]
    stride, count = struct.unpack_from("<HH", data, 58)
    if stride < 64 or not count or offset + stride * count > len(data):
        raise ValueError("invalid ELF sections")
    sections = [struct.unpack_from("<II4QII2Q", data, offset + i * stride) for i in range(count)]
    markers = {}
    for section in sections:
        if section[1] != 2:
            continue
        if section[6] >= count or section[9] != 24 or section[5] % 24:
            raise ValueError("invalid ELF symbol table")
        strings = sections[section[6]]
        if strings[4] + strings[5] > len(data) or section[4] + section[5] > len(data):
            raise ValueError("ELF symbol table out of bounds")
        names = data[strings[4]:strings[4] + strings[5]]
        for at in range(section[4], section[4] + section[5], 24):
            name, _, _, index, address, _ = struct.unpack_from("<IBBHQQ", data, at)
            if name >= len(names):
                raise ValueError("ELF symbol name out of bounds")
            end = names.find(b"\0", name)
            if end < 0:
                raise ValueError("unterminated ELF symbol name")
            symbol = names[name:end]
            if symbol in (b"$x", b"$d") or symbol.startswith((b"$x.", b"$d.")):
                values = markers.setdefault(index, {})
                kind = symbol[:2]
                if address in values and values[address] != kind:
                    raise ValueError("conflicting AArch64 mapping symbols")
                values[address] = kind
    instructions, ranges = 0, 0
    for index, section in enumerate(sections):
        if not (section[2] & 4) or not section[5]:
            continue
        start, size, file_at = section[3], section[5], section[4]
        if section[1] == 8 or file_at + size > len(data):
            raise ValueError("invalid executable section")
        points = sorted(markers.get(index, {}).items())
        if not points or points[0][0] != start or points[-1][0] > start + size:
            raise ValueError("executable section lacks complete AArch64 mapping")
        points.append((start + size, b"$d"))
        for (begin, kind), (end, _) in zip(points, points[1:]):
            if kind != b"$x":
                continue
            if begin % 4 or (end - begin) % 4:
                raise ValueError("misaligned AArch64 code range")
            ranges += 1
            for address in range(begin, end, 4):
                word = struct.unpack_from("<I", data, file_at + address - start)[0]
                major = (word >> 25) & 15
                # Base-A64 SIMD/FP processing and vector load/store classes;
                # SVE also has no place in a general-register-only boot probe.
                if major in (2, 7, 15) or (major in (4, 6, 12, 14) and word & (1 << 26)):
                    raise ValueError(f"non-scalar AArch64 instruction at {address:#x}: {word:#010x}")
                instructions += 1
    if not instructions:
        raise ValueError("no AArch64 instructions checked")
    return {"instructions": instructions, "ranges": ranges}
