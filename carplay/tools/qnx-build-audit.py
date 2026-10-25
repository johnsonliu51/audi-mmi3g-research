#!/usr/bin/env python3
"""Read-only inventory of QNX build inputs and extracted target ELF files.

No compiler, binary, license activation, or vehicle operation is executed.
This reports missing inputs; it does not certify runtime compatibility.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
from pathlib import Path


def elf_identity(path):
    data = path.read_bytes()
    if len(data) < 52 or data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("not ELF32 little-endian")
    h = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    if h[9] != 32 or h[10] > 256 or h[5] + h[9] * h[10] > len(data):
        raise ValueError("invalid program table")
    interpreter = None
    for i in range(h[10]):
        seg = struct.unpack_from("<IIIIIIII", data, h[5] + i * h[9])
        if seg[1] + seg[4] > len(data):
            raise ValueError("segment outside file")
        if seg[0] == 3:
            interpreter = data[seg[1]:seg[1] + seg[4]].rstrip(b"\0").decode("ascii")
    return {
        "path": str(path), "sha256": hashlib.sha256(data).hexdigest(),
        "bytes": len(data), "class": 32, "byte_order": "little",
        "machine": h[2], "type": h[1], "flags": hex(h[7]),
        "osabi": data[7], "abi_version": data[8], "interpreter": interpreter,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware-root", required=True, type=Path)
    parser.add_argument("--sdk-root", type=Path,
                        help="optional locally supplied, authorized QNX SDK root")
    parser.add_argument("--candidate", type=Path,
                        help="optional QNX executable to compare with reference identity")
    args = parser.parse_args()
    root = args.firmware_root.resolve()
    if not root.is_dir():
        parser.error("firmware root is not a directory")
    files = [p for p in root.rglob("*") if p.is_file()]
    names = {}
    for p in files:
        names.setdefault(p.name, []).append(p)
    report = {
        "status": "inventory_only_not_runtime_validation",
        "firmware_root": str(root),
        "host_tools": {n: shutil.which(n) for n in ("qcc", "QCC", "ntosh-gcc")},
        "environment": {n: os.environ.get(n) for n in
                        ("QNX_HOST", "QNX_TARGET", "QCC_CONF_PATH")},
        "reference_elf": [], "firmware_build_inputs": {}, "missing_inputs": [],
    }
    for name in ("ksh", "io-media-nvidia", "libc.so.2", "libgf.so.1",
                 "libasound.so.2", "libusbdi.so.2", "libmmedia.so.1"):
        matches = names.get(name, [])
        for p in matches:
            try:
                report["reference_elf"].append(elf_identity(p))
            except (ValueError, OSError, UnicodeError, struct.error) as error:
                report["reference_elf"].append({"path": str(p), "error": str(error)})
    for name in ("qcc", "ld", "crt1.o", "crti.o", "crtn.o", "libc.a",
                 "usbdi.h", "gf.h", "asoundlib.h"):
        report["firmware_build_inputs"][name] = [str(p) for p in names.get(name, [])]
    if not any(report["host_tools"].values()):
        report["missing_inputs"].append("QNX compiler driver not found on host PATH")
    if args.sdk_root:
        sdk = args.sdk_root.resolve()
        report["sdk_root"] = str(sdk)
        if not sdk.is_dir():
            report["missing_inputs"].append("supplied SDK root does not exist")
        else:
            found = {}
            for p in sdk.rglob("*"):
                if p.is_file() and p.name in report["firmware_build_inputs"]:
                    found.setdefault(p.name, []).append(str(p))
            report["sdk_build_inputs"] = found
            for name in ("qcc", "crt1.o", "usbdi.h", "gf.h", "asoundlib.h"):
                if name not in found:
                    report["missing_inputs"].append("SDK lacks indexed input: " + name)
    else:
        report["missing_inputs"].append("no locally supplied authorized QNX SDK root")
    if args.candidate:
        try:
            candidate = elf_identity(args.candidate.resolve())
            candidate["basic_reference_identity_matches"] = (
                candidate["machine"] == 42 and candidate["flags"] == "0x9"
                and candidate["type"] == 2
                and candidate["interpreter"] == "/usr/lib/ldqnx.so.2")
            candidate["validation_limit"] = (
                "matching identity does not verify QNX API ABI, relocations, dependencies, or execution")
            report["candidate"] = candidate
        except (ValueError, OSError, UnicodeError, struct.error) as error:
            report["candidate"] = {"path": str(args.candidate), "error": str(error)}
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
