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
