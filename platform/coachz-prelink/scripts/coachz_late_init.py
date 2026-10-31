# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Defer our own Mach-O class initializer until the original superclass is loaded."""
import struct


def stage_initializer(raw, name):
    data = bytearray(raw)
    if len(data) < 32 or struct.unpack_from('<4I', data) != (0xfeedfacf, 0x100000c, 0, 1):
        raise ValueError('expected our ARM64 Mach-O object')
    count, size = struct.unpack_from('<II', data, 16)
    if size > len(data) - 32:
        raise ValueError('load commands exceed object')
    cursor, changed, symtab = 32, 0, None
    for _ in range(count):
        if cursor + 8 > 32 + size:
            raise ValueError('truncated load command')
        command, length = struct.unpack_from('<II', data, cursor)
        if length < 8 or length > 32 + size - cursor:
            raise ValueError('invalid load command length')
        if command == 0x19:
            if length < 72:
                raise ValueError('truncated segment')
            sections = struct.unpack_from('<I', data, cursor + 64)[0]
            if sections > (length - 72) // 80:
                raise ValueError('section headers exceed segment')
            for i in range(sections):
                at = cursor + 72 + i * 80
                section = bytes(data[at:at + 16]).rstrip(b'\0')
                flags = struct.unpack_from('<I', data, at + 64)[0]
                if section == b'__mod_init_func':
                    if flags & 0xff != 9:
                        raise ValueError('unexpected initializer section type')
                    data[at:at + 16] = b'__coachz_init'.ljust(16, b'\0')
                    struct.pack_into('<I', data, at + 64, flags & ~0xff)
                    changed += 1
        elif command == 2:
            if symtab is not None or length != 24:
                raise ValueError('unexpected symbol command')
            symtab = struct.unpack_from('<4I', data, cursor + 8)
        cursor += length
    if cursor != 32 + size or changed != 1 or symtab is None:
        raise ValueError('expected one initializer and a complete symbol table')
    symbols, number, strings, string_bytes = symtab
    if symbols > len(data) or number > (len(data) - symbols) // 16 or strings > len(data) or string_bytes > len(data) - strings:
        raise ValueError('symbol table exceeds object')
    exported = 0
    for i in range(number):
        at = symbols + i * 16
        index, kind, section = struct.unpack_from('<IBB', data, at)
        if index >= string_bytes:
            raise ValueError('symbol string exceeds table')
        end = data.find(0, strings + index, strings + string_bytes)
        if end < 0:
            raise ValueError('unterminated symbol')
        if data[strings + index:end] == name.encode():
            if kind & 0xe0 or kind & 0xe != 0xe or not section:
                raise ValueError('initializer must be an existing defined function')
            data[at + 4] = kind | 1
            exported += 1
    if exported != 1:
        raise ValueError('initializer function was not uniquely found')
    return bytes(data)
