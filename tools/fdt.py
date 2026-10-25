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

def encode(root):
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
    struct_offset = 56
    string_offset = struct_offset + len(structure)
    header = struct.pack(">10I", 0xd00dfeed, string_offset + len(strings),
                         struct_offset, string_offset, reserve_offset,
                         17, 16, 0, len(strings), len(structure))
    return header + bytes(16) + structure + strings

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
