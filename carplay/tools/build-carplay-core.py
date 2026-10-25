#!/usr/bin/env python3
"""Build portable protocol objects/archive; optionally a target symbol probe.

Does not run tests, firmware, probe binaries, or write SD cards. Host products
are never accepted as SH/QNX artifacts. Requires an actual SDK for --target qnx.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCES = [
    'iap2_csm.c', 'ncm_ntb16.c', 'usbmux_codec.c', 'lockdown_channel.c',
    'avc_nal.c', 'usbmux_session.c', 'usbmux_host.c', 'iap2_link.c',
]

def sh_object(path):
    data = path.read_bytes()
    if len(data) < 52 or data[:6] != b'\x7fELF\x01\x01':
        raise ValueError('not ELF32 little-endian: ' + str(path))
    kind, machine = struct.unpack_from('<HH', data, 16)
    if kind != 1 or machine != 42:
        raise ValueError('not a SH relocatable object: ' + str(path))
    flags = struct.unpack_from('<I', data, 36)[0]
    if flags & 0x1f != 9:
        raise ValueError('object does not match reference SH4 CPU flags: ' + str(path))
    return {'machine': machine, 'elf_type': kind,
            'flags': hex(flags)}

def sh_executable(path):
    raw = path.read_bytes()
    if len(raw) < 52 or raw[:6] != b'\x7fELF\x01\x01':
        raise ValueError('executable is not ELF32 little-endian: ' + str(path))
    kind, machine = struct.unpack_from('<HH', raw, 16)
    entry, phoff = struct.unpack_from('<II', raw, 24)
    flags = struct.unpack_from('<I', raw, 36)[0]
    phsize, phcount = struct.unpack_from('<HH', raw, 42)
    if kind != 2 or machine != 42 or flags & 0x1f != 9:
        raise ValueError('executable does not match reference ET_EXEC SH4 identity: ' + str(path))
    if phsize != 32 or not phcount or phcount > 256 or phoff + phsize * phcount > len(raw):
        raise ValueError('invalid executable program table: ' + str(path))
    interpreters = []
    executable_entry = False
    for index in range(phcount):
        ptype, offset, address, _, size, memory_size, permissions, _ = struct.unpack_from('<8I', raw, phoff + phsize * index)
        if offset + size > len(raw) or (ptype == 1 and size > memory_size):
            raise ValueError('executable segment outside file: ' + str(path))
        if ptype == 3:
            interpreters.append(raw[offset:offset + size])
        if ptype == 1 and permissions & 1 and address <= entry < address + size:
            executable_entry = True
    if interpreters != [b'/usr/lib/ldqnx.so.2\0'] or not executable_entry:
        raise ValueError('executable PT_INTERP/entry does not match QNX reference: ' + str(path))
    return {'machine': machine, 'elf_type': kind, 'flags': hex(flags),
            'interpreter': '/usr/lib/ldqnx.so.2', 'entry': hex(entry)}

def resolved_tool(name):
    path = shutil.which(name)
    if not path:
        raise ValueError('tool unavailable: ' + name)
    return path

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('host', 'qnx'), default='host')
    parser.add_argument('--compiler', help='compiler executable, without embedded flags')
    parser.add_argument('--archiver', help='archive tool executable')
    parser.add_argument('--variant', help='explicit SDK qcc version/target, e.g. 3.3.5,gcc_ntoshle; inspect qcc -V first')
    parser.add_argument('--display-probe', action='store_true', help='QNX only: also compile GF display query using actual SDK headers')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    output = (args.output or Path('/private/tmp/mmi3g-carplay-build') / args.target).resolve()
    # Keep bulk build outputs outside the synced source checkout by default,
    # and reject an output nested under source to prevent accidental pollution.
    if output == ROOT or ROOT in output.parents:
        raise ValueError('build output must be outside the source checkout')
    qnx = args.target == 'qnx'
    if args.display_probe and not qnx:
        raise ValueError('--display-probe requires the actual QNX SDK; no host substitute is generated')
    if qnx and not args.variant:
        raise ValueError('QNX build requires --variant from the SDK qcc -V listing; compiler must support C99')
    compiler = resolved_tool(args.compiler or ('qcc' if qnx else 'cc'))
    if qnx and not args.archiver:
        raise ValueError('QNX build requires --archiver pointing to the matching SDK archive tool')
    archiver = resolved_tool(args.archiver or 'ar')
    sources = [ROOT / 'src/protocol' / name for name in SOURCES]
    missing = [str(p) for p in sources if not p.is_file()]
    if missing:
        raise ValueError('missing core source: ' + ', '.join(missing))
    output.mkdir(parents=True, exist_ok=True)
    manifest = {
        'target': args.target, 'compiler': compiler, 'archiver': archiver,
        'variant': args.variant if qnx else None,
        'host': platform.platform(), 'receiver_ready': False,
        'scope': 'protocol archive plus symbol-probe; not a CarPlay receiver',
        'runtime_validated': False, 'tests_run': False, 'objects': [],
        'calling_abi_validated': False,
    }
    tracked = sources + sorted((ROOT / 'src/protocol').glob('*.h')) + [
        ROOT / 'src/qnx_symbol_probe.c', Path(__file__).resolve(),
    ]
    if args.display_probe:
        tracked.append(ROOT / 'src/qnx_display_probe.c')
    # macOS cloud placeholders can block read()/compiler indefinitely. Request
    # downloads explicitly, then rerun after files have local contents.
    placeholders = [str(p) for p in tracked
                    if getattr(p.stat(), 'st_flags', 0) & 0x40000000]
    if placeholders:
        raise ValueError('iCloud source files not downloaded: ' + ', '.join(placeholders))
    manifest['source_sha256'] = {
        str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked
    }
    flags = ['-V' + args.variant] if qnx else []
    flags += ['-std=c99', '-Wall', '-Wextra', '-Werror', '-O2']
    with tempfile.TemporaryDirectory(prefix='core-build-', dir=output) as directory:
        stage = Path(directory)
        objects = []
        for source in sources:
            obj = stage / (source.stem + '.o')
            subprocess.run([compiler, *flags, '-I' + str(ROOT / 'src/protocol'),
                            '-c', str(source), '-o', str(obj)], check=True)
            item = {'source': str(source.relative_to(ROOT)),
                    'sha256': hashlib.sha256(source.read_bytes()).hexdigest()}
            if qnx:
                item['elf'] = sh_object(obj)
            manifest['objects'].append(item)
            objects.append(obj)
        archive = stage / 'libmmi_carplay_core.a'
        subprocess.run([archiver, 'rcs', str(archive), *map(str, objects)], check=True)
        probe = stage / ('qnx-symbol-probe' if qnx else 'symbol-probe-host')
        # QNX dlopen/dlsym are supplied by libc; Linux requires libdl on older hosts.
        libraries = ['-ldl'] if not qnx and platform.system() == 'Linux' else []
        subprocess.run([compiler, *flags, str(ROOT / 'src/qnx_symbol_probe.c'),
                        '-o', str(probe), *libraries], check=True)
        if qnx:
            manifest['executable_elf'] = {probe.name: sh_executable(probe)}
        products = [archive, probe]
        if args.display_probe:
            display_probe = stage / 'qnx-display-probe'
            subprocess.run([compiler, *flags, str(ROOT / 'src/qnx_display_probe.c'),
                            '-o', str(display_probe), '-lgf'], check=True)
            manifest['executable_elf'][display_probe.name] = sh_executable(display_probe)
            products.append(display_probe)
        if not qnx:
            inspector = stage / 'csm-inspect-host'
            subprocess.run([compiler, *flags, str(ROOT / 'src/protocol/iap2_csm.c'),
                            str(ROOT / 'src/protocol/csm_inspect.c'), '-o', str(inspector)], check=True)
            products.append(inspector)
        manifest['products'] = {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in products
        }
        for product in products:
            os.replace(product, output / product.name)
    (output / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(output)
    print('Build complete; no CarPlay session or device test performed.')

if __name__ == '__main__':
    try:
        main()
    except (ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
