#!/usr/bin/env python3
"""Read-only ELF32 little-endian segment/dynamic-symbol inventory.

Reject invalid file ranges instead of treating carved bytes as valid symbols.
Does not execute or load the target binary.
"""
import argparse
import json
import struct
from pathlib import Path

def inspect(path):
    data = path.read_bytes()
    def unpack(fmt, off):
        size = struct.calcsize(fmt)
        if off < 0 or off + size > len(data):
            raise ValueError("structure outside file at " + str(off))
        return struct.unpack_from(fmt, data, off)
    header = unpack('<16sHHIIIIIHHHHHH', 0)
    if header[0][:6] != b'\x7fELF\x01\x01':
        raise ValueError('requires ELF32 little-endian')
    _, kind, machine, _, entry, phoff, shoff, _, _, phsize, phcount, shsize, shcount, _ = header
    if phsize != 32 or phcount > 256:
        raise ValueError('unsupported program table')
    segments = [unpack('<IIIIIIII', phoff + i * phsize) for i in range(phcount)]
    for seg in segments:
        if seg[1] + seg[4] > len(data):
            raise ValueError('segment outside file')
    issues = []
    if shcount:
        if shsize != 40 or shoff + shsize * shcount > len(data):
            issues.append('invalid section table range')
        else:
            for i in range(shcount):
                section = unpack('<IIIIIIIIII', shoff + i * shsize)
                if section[1] != 8 and section[4] + section[5] > len(data):
                    issues.append('section %d extends beyond file' % i)
    def offset(address, count=1):
        for seg in segments:
            if seg[0] == 1 and seg[2] <= address and address + count <= seg[2] + seg[4]:
                return seg[1] + address - seg[2]
        raise ValueError('unmapped dynamic address ' + hex(address))
    tags = {}
    needed = []
    for seg in segments:
        if seg[0] != 2:
            continue
        ended = False
        for off in range(seg[1], seg[1] + seg[4] - 7, 8):
            tag, value = unpack('<II', off)
            if tag == 0:
                ended = True
                break
            if tag == 1:
                needed.append(value)
            else:
                tags[tag] = value
        if not ended:
            raise ValueError('unterminated dynamic table')
    result = dict(file=str(path), bytes=len(data), machine=machine,
                  type=kind, entry=hex(entry), issues=issues,
                  dependencies=[], exports=[], imports=[])
    if not tags:
        return result
    if not all(tag in tags for tag in (4, 5, 6, 10, 11)) or tags[11] != 16:
        raise ValueError('requires SYSV hash, strtab, symtab, strsz, ELF32 syment')
    string_offset = offset(tags[5], tags[10])
    strings = data[string_offset:string_offset + tags[10]]
    def string(index):
        if index >= len(strings):
            raise ValueError('string index outside table')
        end = strings.find(b'\0', index)
        if end < 0:
            raise ValueError('unterminated string')
        return strings[index:end].decode('utf-8', errors='replace')
    hash_offset = offset(tags[4], 8)
    buckets, count = unpack('<II', hash_offset)
    if count > 100000 or buckets > 100000:
        raise ValueError('implausible hash table')
    offset(tags[4], 8 + 4 * (buckets + count))
    symbol_offset = offset(tags[6], count * 16)
    result['dependencies'] = [string(i) for i in needed]
    for i in range(count):
        name, value, size, info, other, section = unpack('<IIIBBH', symbol_offset + i * 16)
        if not name or info >> 4 not in (1, 2):
            continue
        item = dict(name=string(name), value=hex(value), size=size,
                    binding=info >> 4, symbol_type=info & 15, visibility=other & 3)
        result['imports' if section == 0 else 'exports'].append(item)
    return result

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('files', nargs='+', type=Path)
    args = parser.parse_args()
    reports = []
    for path in args.files:
        try:
            reports.append(inspect(path))
        except (OSError, ValueError, struct.error) as error:
            reports.append(dict(file=str(path), error=str(error)))
    print(json.dumps(reports, indent=2))

if __name__ == '__main__':
    main()
