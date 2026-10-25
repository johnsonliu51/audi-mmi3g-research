#!/usr/bin/env python3
"""Passive SH4/QNX ELF and dependency audit. Never loads or executes a binary.

Symbol-name resolution is a necessary condition, not a calling-ABI or runtime
proof. Library directories must come from one coherent reference extraction;
ambiguous differing files are rejected instead of choosing one silently.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('elf_inventory', HERE / 'elf32-interface-report.py')
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)
EXPECTED_INTERPRETER = '/usr/lib/ldqnx.so.2'


def read_elf(path):
    report = inventory.inspect(path)
    raw = path.read_bytes()
    report['sha256'] = hashlib.sha256(raw).hexdigest()
    report['flags'] = struct.unpack_from('<I', raw, 36)[0]
    report['interpreter'] = None
    report['entry_in_executable_segment'] = False
    entry = struct.unpack_from('<I', raw, 24)[0]
    phoff = struct.unpack_from('<I', raw, 28)[0]
    phsize, phcount = struct.unpack_from('<HH', raw, 42)
    for index in range(phcount):
        kind, offset, address, _, size, memory_size, permissions, _ = struct.unpack_from('<8I', raw, phoff + index * phsize)
        if kind == 1:
            if size > memory_size or address + memory_size > 0x100000000:
                raise ValueError('PT_LOAD file/memory range is invalid')
            if permissions & 1 and address <= entry < address + size:
                report['entry_in_executable_segment'] = True
        if kind == 3:
            value = raw[offset:offset + size]
            if not value or value[-1:] != b'\0' or b'\0' in value[:-1]:
                raise ValueError('invalid PT_INTERP')
            if report['interpreter'] is not None:
                raise ValueError('multiple PT_INTERP segments')
            report['interpreter'] = value[:-1].decode('ascii')
    # ELF version tables require an additional version-aware audit. The target
    # libraries we studied use SYSV hash; do not silently flatten versioned ABI.
    report['symbol_versions_unchecked'] = False
    for index in range(phcount):
        kind, offset, _, _, size, _, _, _ = struct.unpack_from('<8I', raw, phoff + index * phsize)
        if kind != 2:
            continue
        for at in range(offset, offset + size - 7, 8):
            tag, _ = struct.unpack_from('<II', raw, at)
            if tag == 0:
                break
            if tag in (0x6ffffff0, 0x6ffffffc, 0x6ffffffe):
                report['symbol_versions_unchecked'] = True
    return report


def audit(candidate, directories):
    result = {'candidate': str(candidate), 'runtime_validated': False,
              'scope': 'ELF identity and unversioned symbol names only',
              'issues': [], 'modules': [], 'unresolved_imports': [],
              'optional_weak_imports': [], 'ambiguous_libraries': []}
    try:
        initial = read_elf(candidate)
    except (OSError, ValueError, UnicodeError, struct.error) as error:
        result['issues'].append('candidate ELF could not be parsed: ' + str(error))
        result['static_conditions_satisfied'] = False
        return result
    if initial['type'] != 2:
        result['issues'].append('candidate must match the reference ET_EXEC model; PIE/shared objects need separate review')
    if initial['interpreter'] != EXPECTED_INTERPRETER:
        result['issues'].append('candidate PT_INTERP does not match ' + EXPECTED_INTERPRETER)
    if not initial['entry_in_executable_segment']:
        result['issues'].append('candidate entry is not in a file-backed executable PT_LOAD')
    result['required_loader'] = EXPECTED_INTERPRETER

    candidates = {}
    for directory in directories:
        if not directory.is_dir():
            result['issues'].append('library directory unavailable: ' + str(directory))
            continue
        entries = sorted(directory.iterdir())
        if len(entries) > 8192:
            result['issues'].append('library directory exceeds bounded entry count: ' + str(directory))
            continue
        for path in entries:
            if path.is_file() and ('.so' in path.name or path.name.startswith('ldqnx')):
                candidates.setdefault(path.name, []).append(path)

    queue = [initial]
    visited = {str(candidate.resolve())}
    missing = set()
    while queue:
        current = queue.pop(0)
        result['modules'].append(current)
        if len(result['modules']) > 256:
            result['issues'].append('dependency graph exceeds 256 modules')
            break
        if current['machine'] != 42 or current['flags'] & 0x1f != 9:
            result['issues'].append('module is not reference SH4 little-endian: ' + current['file'])
        if current['symbol_versions_unchecked']:
            result['issues'].append('symbol versions require additional review: ' + current['file'])
        # Section tables are sometimes stripped or defective in QNX images.
        # inspect() uses validated program headers; keep section issues as
        # evidence, rather than treating every stripped section as fatal.
        dependencies = list(current['dependencies'])
        if current is initial:
            dependencies.append(Path(EXPECTED_INTERPRETER).name)
        for name in dependencies:
            if '/' in name or name in ('.', '..'):
                result['issues'].append('dependency is not a bare SONAME: ' + name)
                continue
            options = candidates.get(name, [])
            if not options:
                missing.add(name)
                continue
            by_hash = {}
            for path in options:
                try:
                    digest = hashlib.sha256(path.read_bytes()).hexdigest()
                    by_hash.setdefault(digest, path)
                except OSError as error:
                    result['issues'].append('cannot read dependency: ' + str(error))
            if len(by_hash) != 1:
                result['ambiguous_libraries'].append({'name': name, 'paths': [str(p) for p in options]})
                continue
            path = next(iter(by_hash.values()))
            canonical = str(path.resolve())
            if canonical in visited:
                continue
            visited.add(canonical)
            try:
                library = read_elf(path)
                if library['type'] != 3:
                    result['issues'].append('dependency is not ET_DYN: ' + str(path))
                queue.append(library)
            except (OSError, ValueError, UnicodeError, struct.error) as error:
                result['issues'].append('cannot parse dependency ' + str(path) + ': ' + str(error))

    result['missing_libraries'] = sorted(missing)
    exported = {item['name'] for module in result['modules'] for item in module['exports']
                if item['visibility'] in (0, 3)}
    for module in result['modules']:
        for symbol in module['imports']:
            if symbol['name'] in exported:
                continue
            item = {'module': module['file'], 'symbol': symbol['name']}
            bucket = 'optional_weak_imports' if symbol['binding'] == 2 else 'unresolved_imports'
            result[bucket].append(item)
    result['static_conditions_satisfied'] = not (result['issues'] or missing or
        result['ambiguous_libraries'] or result['unresolved_imports'])
    result['limitations'] = [
        'No executable, shared library, constructor or device API was invoked.',
        'Names do not prove prototypes, struct layouts, relocation support, TLS or constructor compatibility.',
        'Reference firmware libraries do not prove the actual P0710 runtime is identical.',
        'A satisfied report is not permission to deploy and is not a CarPlay acceptance record.',
    ]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--library-dir', action='append', type=Path, required=True,
                        help='exact nonrecursive library directory; repeat for one coherent extraction')
    parser.add_argument('--output', type=Path, help='new JSON file; refuses to overwrite')
    args = parser.parse_args()
    report = audit(args.candidate, args.library_dir)
    output = json.dumps(report, indent=2) + '\n'
    if args.output:
        with args.output.open('x') as stream:
            stream.write(output)
        print(args.output.resolve())
    else:
        print(output, end='')
    return 0 if report['static_conditions_satisfied'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
