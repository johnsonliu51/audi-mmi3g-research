#!/usr/bin/env python3
"""Deploy a reviewed diagnostics archive onto a backed-up macOS SD volume.

Only the package entries are replaced. Original conflicting entries move to a
disabled folder on the same card. A verified full backup is required first.
"""
import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import zipfile


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--volume', required=True, type=Path)
    parser.add_argument('--backup', required=True, type=Path)
    parser.add_argument('--package', required=True, type=Path)
    args = parser.parse_args()
    volume = args.volume.resolve()
    backup = args.backup.resolve()
    if backup == volume or volume in backup.parents:
        raise ValueError('Backup must be outside the SD card')
    if volume.parent != Path('/Volumes') or args.volume.is_symlink():
        raise ValueError('Expected a direct /Volumes mount, not a symlink')
    info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', str(volume)]))
    old_info = plistlib.loads((backup / 'volume-info.plist').read_bytes())
    if (info.get('Internal') or not info.get('WritableVolume') or
            info.get('FilesystemType') != 'msdos' or
            info.get('MountPoint') != str(volume) or
            not info.get('VolumeUUID') or info['VolumeUUID'] != old_info.get('VolumeUUID')):
        raise ValueError('Volume identity, FAT format, or write access mismatch')
    if not (backup / 'BACKUP_VERIFIED.txt').is_file():
        raise ValueError('Missing full-backup verification marker')
    original = json.loads((backup / 'original-files-sha256.json').read_text())
    for name, entry in original.items():
        saved = backup / 'card-original' / name
        if not saved.is_file() or saved.is_symlink() or file_sha(saved) != entry['sha256']:
            raise ValueError('Backup content verification failed')
        # The OS may update its own indexing/event files after backup.
        if name.split('/')[0] in ('.fseventsd', '.Spotlight-V100', '.Trashes'):
            continue
        current = volume / name
        if not current.is_file() or current.is_symlink() or file_sha(current) != entry['sha256']:
            raise ValueError('Original card changed since backup; refusing deployment')
    with zipfile.ZipFile(args.package) as archive:
        payload = {}
        for item in archive.infolist():
            name = item.filename
            path = Path(name)
            if path.is_absolute() or '..' in path.parts or item.is_dir() or name in payload:
                raise ValueError('Unsafe or duplicate archive path')
            if item.file_size > 1024 * 1024 or len(payload) >= 32:
                raise ValueError('Unexpected package size')
            payload[name] = archive.read(item)
    manifest_name = 'carplay-diagnostics/manifest.json'
    manifest = json.loads(payload[manifest_name])
    expected = set(manifest['sha256']) | {manifest_name}
    if (set(payload) != expected or manifest.get('package') != 'MMI3G-CarPlay-Diagnostics' or
            manifest.get('receiver_included') is not False or
            manifest.get('native_binary_included') is not False):
        raise ValueError('Unexpected package manifest')
    for name, expected_sha in manifest['sha256'].items():
        if sha(payload[name]) != expected_sha:
            raise ValueError('Package payload hash mismatch')
    roots = sorted({Path(name).parts[0] for name in payload})
    if set(roots) != {'copie_scr.sh', 'run.sh', 'MMI3G_CP_DIAG_CARD',
                      'MMI3G_DIAG_STATUS.txt', 'carplay-diagnostics'}:
        raise ValueError('Unexpected package root entries')
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    stage = volume / ('.mmi3g-diag-stage-' + stamp)
    disabled = volume / ('toolkit-original-disabled-' + stamp)
    if stage.exists() or disabled.exists():
        raise ValueError('Deployment staging name already exists')
    if info.get('FreeSpace', 0) < sum(map(len, payload.values())) + 16 * 1024 * 1024:
        raise ValueError('Not enough free SD space')
    moved = []
    automatic_relocations = {}
    installed = []
    stage.mkdir()
    try:
        for name, data in payload.items():
            destination = stage / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            with destination.open('xb') as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            if file_sha(destination) != sha(data):
                raise ValueError('Staged SD readback mismatch')
        disabled.mkdir()
        # Disable the old automatic entry first; install the new one last.
        for name in ['copie_scr.sh'] + [root for root in roots if root != 'copie_scr.sh']:
            current = volume / name
            apple_double = volume / ('._' + name)
            had_apple_double = apple_double.exists()
            if current.exists() or current.is_symlink():
                if current.is_symlink():
                    raise ValueError('Refusing to move an unexpected symlink')
                if current.is_dir():
                    # Keep its parent unchanged so the FAT '..' entry stays
                    # correct. Its disabled root name cannot be an entry point.
                    saved = volume / (name + '.disabled-' + stamp)
                else:
                    saved = disabled / (name + '.disabled')
                current.rename(saved)
                moved.append((saved, current))
                # macOS FAT rename may move the associated AppleDouble file
                # itself. Track its actual destination for hash verification;
                # renaming the original back also restores this companion.
                auto_saved = saved.parent / ('._' + saved.name)
                if had_apple_double and not apple_double.exists() and auto_saved.is_file():
                    automatic_relocations[apple_double.name] = auto_saved
            if apple_double.exists():
                saved = disabled / (apple_double.name + '.disabled')
                apple_double.rename(saved)
                moved.append((saved, apple_double))
        for name in [root for root in roots if root != 'copie_scr.sh'] + ['copie_scr.sh']:
            destination = volume / name
            installed.append(destination)
            if (stage / name).is_dir():
                # Create at the final parent. A cross-parent FAT directory
                # rename on macOS may leave a stale '..' start cluster.
                shutil.copytree(stage / name, destination, copy_function=shutil.copyfile)
                shutil.rmtree(stage / name)
            else:
                (stage / name).rename(destination)
        for name, data in payload.items():
            if file_sha(volume / name) != sha(data):
                raise ValueError('Deployed SD readback mismatch')
        (volume / 'carplay-diagnostics/logs').mkdir()
        # Verify every preserved original user file at its new or old position.
        relocations = {str(old.relative_to(volume)): saved for saved, old in moved}
        relocations.update(automatic_relocations)
        for name, entry in original.items():
            if name.split('/')[0] in ('.fseventsd', '.Spotlight-V100', '.Trashes'):
                continue
            root = Path(name).parts[0]
            if root in relocations and len(Path(name).parts) > 1:
                saved = relocations[root].joinpath(*Path(name).parts[1:])
            else:
                saved = relocations.get(name, volume / name)
            if file_sha(saved) != entry['sha256']:
                raise ValueError('Preserved original file readback mismatch')
        result = {'version': manifest['version'], 'volume_uuid': info['VolumeUUID'],
                  'device': info['DeviceIdentifier'], 'backup': str(backup),
                  'disabled_original_entries': str(disabled),
                  'original_entry_locations': {name: str(path) for name, path in relocations.items()},
                  'package_sha256': file_sha(args.package),
                  'payload_files_verified': len(payload),
                  'all_original_user_files_preserved': True,
                  'target_execution_verified': False}
        (backup / 'deployment-receipt.json').write_text(json.dumps(result, indent=2) + '\n')
        stage.rmdir()
        subprocess.run(['sync'], check=True)
        print(json.dumps(result, ensure_ascii=False, indent=2))
    except BaseException:
        # Roll back only this script's entries, never delete unrelated files.
        for destination in reversed(installed):
            if destination.is_dir():
                shutil.rmtree(destination)
            elif destination.exists():
                destination.unlink()
        for saved, old in reversed(moved):
            if saved.exists():
                saved.rename(old)
        if disabled.exists():
            try:
                disabled.rmdir()
            except OSError:
                pass
        raise
    finally:
        if stage.exists():
            shutil.rmtree(stage)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        raise SystemExit(str(error))
