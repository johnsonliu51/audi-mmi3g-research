#!/usr/bin/env python3
"""Disassemble a file-backed SH4 virtual address range without section headers.

QNX firmware ELF files can omit .text/.dynamic section names. Mapping a range
through PT_LOAD avoids guessing where the code lies. This only reads firmware;
temporary bytes are created outside the workspace and removed on completion.
"""
import argparse
import struct
import subprocess
import tempfile
from pathlib import Path


def integer(value):
    return int(value, 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("--start", required=True, type=integer)
    parser.add_argument("--bytes", type=integer, default=256, dest="count")
    parser.add_argument("--objdump", required=True, type=Path)
    args = parser.parse_args()
    if args.start < 0 or args.start % 2 or not 0 < args.count <= 1024 * 1024:
        parser.error("start must be nonnegative/even; bytes must be 1..1048576")
    data = args.file.read_bytes()
    if len(data) < 52 or data[:6] != b"\x7fELF\x01\x01":
        parser.error("requires ELF32 little-endian")
    h = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    if h[2] != 42 or h[9] != 32 or h[10] > 256:
        parser.error("requires EM_SH and ordinary ELF32 program headers")
    if h[5] + h[9] * h[10] > len(data):
        parser.error("program table outside file")
    fragment = None
    for i in range(h[10]):
        seg = struct.unpack_from("<IIIIIIII", data, h[5] + i * h[9])
        if seg[0] != 1 or not seg[2] <= args.start:
            continue
        end = args.start + args.count
        if end <= seg[2] + seg[4]:
            off = seg[1] + args.start - seg[2]
            if off + args.count > len(data):
                parser.error("selected segment extends outside file")
            fragment = data[off:off + args.count]
            break
    if fragment is None:
        parser.error("range is not wholly inside a file-backed PT_LOAD segment")
    with tempfile.TemporaryDirectory(prefix="mmi3g-sh-disasm-", dir="/private/tmp") as tmp:
        raw = Path(tmp) / "code.bin"
        raw.write_bytes(fragment)
        completed = subprocess.run([
            str(args.objdump.resolve()), "-D", "-b", "binary", "-m", "sh4", "-EL",
            "--adjust-vma=" + hex(args.start), str(raw)], check=False)
        raise SystemExit(completed.returncode)


if __name__ == "__main__":
    main()
