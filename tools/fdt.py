"""Small deterministic FDT v17 writer for the diagnostic DTB and FIT container."""
import struct
from dataclasses import dataclass, field

def u32(value):
    return struct.pack(">I", value)

def string(value):
    return value.encode("ascii") + b"\0"

@dataclass
class Node:
    name: str
    props: dict = field(default_factory=dict)
    children: list = field(default_factory=list)

def encode(root, reservations=(), boot_cpuid_phys=0, last_comp_version=16):
    strings = bytearray()
    offsets = {}
    structure = bytearray()

    def word(value):
        structure.extend(u32(value))

    def align():
        structure.extend(b"\0" * (-len(structure) % 4))

    def visit(node):
        word(1)
        structure.extend(string(node.name))
        align()
        for name, data in node.props.items():
            if name not in offsets:
                offsets[name] = len(strings)
                strings.extend(string(name))
            word(3)
            word(len(data))
            word(offsets[name])
            structure.extend(data)
            align()
        for child in node.children:
            visit(child)
        word(2)

    visit(root)
    word(9)
    reserve_offset = 40
    reserve = bytearray()
    for address, size in reservations:
        if not (0 <= address < 1 << 64 and 0 <= size < 1 << 64) or not (address or size):
            raise ValueError("invalid reservation entry")
        reserve.extend(struct.pack(">QQ", address, size))
    reserve.extend(bytes(16))
    struct_offset = reserve_offset + len(reserve)
    string_offset = struct_offset + len(structure)
    header = struct.pack(">10I", 0xd00dfeed, string_offset + len(strings),
                         struct_offset, string_offset, reserve_offset,
                         17, last_comp_version, boot_cpuid_phys, len(strings), len(structure))
    return header + reserve + structure + strings

def properties(blob):
    """Decode generated containers for independent artifact verification."""
    if len(blob) < 40:
        raise ValueError("truncated FDT")
    magic, total, pos, strings, _, version, _, _, names_size, struct_size = struct.unpack_from(">10I", blob)
    if magic != 0xd00dfeed or version != 17 or total != len(blob):
        raise ValueError("invalid FDT header")
    end = pos + struct_size
    if end > len(blob) or strings + names_size > len(blob):
        raise ValueError("invalid FDT bounds")
    stack, out = [], {}
    while pos + 4 <= end:
        token = struct.unpack_from(">I", blob, pos)[0]
        pos += 4
        if token == 1:
            stop = blob.index(b"\0", pos, end)
            stack.append(blob[pos:stop].decode("ascii"))
            pos = (stop + 4) & ~3
        elif token == 2:
            if not stack:
                raise ValueError("unbalanced tree")
            stack.pop()
        elif token == 3:
            if not stack or pos + 8 > end:
                raise ValueError("property outside node")
            size, name = struct.unpack_from(">II", blob, pos)
            pos += 8
            if pos + size > end or name >= names_size:
                raise ValueError("invalid property")
            stop = blob.index(b"\0", strings + name, strings + names_size)
            key = blob[strings + name:stop].decode("ascii")
            path = "/" + "/".join(stack[1:])
            out[(path, key)] = blob[pos:pos + size]
            pos = (pos + size + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            if stack or pos != end:
                raise ValueError("invalid end")
            return out
        else:
            raise ValueError("invalid token")
    raise ValueError("missing end")


@dataclass
class DecodedFdt:
    root: Node
    reservations: tuple
    boot_cpuid_phys: int
    last_comp_version: int

    def encode(self):
        return encode(self.root, self.reservations, self.boot_cpuid_phys,
                      self.last_comp_version)


def decode(blob):
    """Decode canonical v17 FIT/DTBs without dropping empty nodes or reservations."""
    if len(blob) < 56:
        raise ValueError("truncated FDT")
    magic, total, pos, names_at, reserve_at, version, last, cpu, names_size, size = struct.unpack_from(">10I", blob)
    end = pos + size
    if (magic != 0xd00dfeed or total != len(blob) or version != 17 or
            not 1 <= last <= version or reserve_at < 40 or reserve_at % 8 or
            pos % 4 or pos < reserve_at + 16 or end > names_at or
            names_at + names_size > total):
        raise ValueError("invalid canonical FDT layout")
    reservations = []
    at = reserve_at
    while at + 16 <= pos:
        address, length = struct.unpack_from(">QQ", blob, at)
        at += 16
        if not (address or length):
            break
        if address + length > 1 << 64:
            raise ValueError("reservation wraps address space")
        reservations.append((address, length))
    else:
        raise ValueError("unterminated reservations")
    stack, root = [], None
    while pos + 4 <= end:
        token = struct.unpack_from(">I", blob, pos)[0]
        pos += 4
        if token == 1:
            stop = blob.index(b"\0", pos, end)
            name = blob[pos:stop].decode("ascii")
            pos = (stop + 4) & ~3
            if "/" in name or len(stack) >= 64:
                raise ValueError("invalid node name or depth")
            node = Node(name)
            if stack:
                if any(child.name == name for child in stack[-1].children):
                    raise ValueError("duplicate node")
                stack[-1].children.append(node)
            elif root is not None or name:
                raise ValueError("invalid root")
            else:
                root = node
            stack.append(node)
        elif token == 2:
            if not stack:
                raise ValueError("unbalanced tree")
            stack.pop()
        elif token == 3:
            if not stack or pos + 8 > end:
                raise ValueError("property outside node")
            length, name_at = struct.unpack_from(">II", blob, pos)
            pos += 8
            if pos + length > end or name_at >= names_size:
                raise ValueError("invalid property bounds")
            stop = blob.index(b"\0", names_at + name_at, names_at + names_size)
            name = blob[names_at + name_at:stop].decode("ascii")
            if not name or "/" in name or name in stack[-1].props:
                raise ValueError("invalid or duplicate property")
            stack[-1].props[name] = blob[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            if stack or root is None or pos != end:
                raise ValueError("invalid end")
            return DecodedFdt(root, tuple(reservations), cpu, last)
        else:
            raise ValueError("invalid token")
    raise ValueError("missing end")
