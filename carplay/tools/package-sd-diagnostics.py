#!/usr/bin/env python3
"""Build SD diagnostics with an optional owned GEM progress page."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import zipfile

ROOT = Path(__file__).resolve().parent.parent
VERSION = '0.6'
spec = importlib.util.spec_from_file_location(
    "toolkit_encoder", ROOT / "tools/vendor/toolkit/encoder.py"
)
encoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(encoder)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', nargs='?', type=Path, default=ROOT / ('artifacts/MMI3G-CarPlay-Diagnostics-' + VERSION + '.zip'))
    parser.add_argument('--enable-progress-ui', action='store_true', help='Enable exclusive creation of one GEM ESD page; never remount EFS')
    args = parser.parse_args()
    target = args.output.resolve()
    if target == Path('/Volumes') or Path('/Volumes') in target.parents:
        raise SystemExit('Refusing to build directly onto a mounted volume; build locally, then deploy separately')
    if target.exists():
        raise SystemExit("Refusing to replace existing archive: " + str(target))
    plain = (ROOT / "sd-diagnostics/copie_scr_plain.sh").read_bytes()
    payload = {
        "copie_scr.sh": encoder.MMI3GCipher().process(plain),
        "run.sh": (ROOT / "sd-diagnostics/run.sh").read_bytes(),
        "MMI3G_CP_DIAG_CARD": ('MMI3G CarPlay diagnostics ' + VERSION + '; not a CarPlay receiver\n').encode(),
        "MMI3G_DIAG_STATUS.txt": b'state=prepared_on_computer\ntarget_execution=unverified\n',
        "carplay-diagnostics/qnx-carplay-readonly.sh": (ROOT / "tools/qnx-carplay-readonly.sh").read_bytes(),
        "carplay-diagnostics/qnx-carplay-compat.sh": (ROOT / "tools/qnx-carplay-compat.sh").read_bytes(),
        "carplay-diagnostics/progress-view.sh": (ROOT / "sd-diagnostics/progress-view.sh").read_bytes(),
        "carplay-diagnostics/ui-page.sh": (ROOT / "sd-diagnostics/ui-page.sh").read_bytes(),
        "carplay-diagnostics/ui/README.md": (ROOT / "docs/SD_PROGRESS_UI.md").read_bytes(),
        "carplay-diagnostics/README.md": (ROOT / "docs/SD_DIAGNOSTICS.md").read_bytes(),
        "carplay-diagnostics/CARPLAY_REQUIREMENTS_MATRIX.md": (ROOT / "docs/CARPLAY_REQUIREMENTS_MATRIX.md").read_bytes(),
        "carplay-diagnostics/computer-tools/summarize-qnx-diagnostics.py": (ROOT / "tools/summarize-qnx-diagnostics.py").read_bytes(),
        "carplay-diagnostics/source/copie_scr_plain.sh": plain,
        "carplay-diagnostics/licenses/Toolkit-MIT.txt": (ROOT / "tools/vendor/toolkit/LICENSE").read_bytes(),
    }
    if args.enable_progress_ui:
        payload['carplay-diagnostics/ui/ENABLE'] = b'Enable one owned GEM progress page; never remount EFS.\n'
    manifest = {
        "package": "MMI3G-CarPlay-Diagnostics", "version": VERSION,
        "toolkit_reference": "e9ca3469835e3e1c78dabfb5cbe5676d2b752500",
        "target_validated": False,
        "writes": "SD logs/status; conditional SD-only writable remount; enabled UI creates one owned GEM ESD file only when current EFS mode permits",
        "progress_ui_enabled": args.enable_progress_ui,
        "progress_ui_target": "/mnt/efs-system/engdefs/MMI3GCarPlayDiag.esd",
        "progress_ui_activation": "Manual GEM: Main / CarPlayDiag / View collection progress",
        "efs_remount_permitted": False,
        "conditional_remount_paths": ["/mnt/sdcard10t12", "/mnt/sdcard20t12", "/mnt/sdcard10t11", "/mnt/sdcard20t11"],
        "receiver_included": False, "native_binary_included": False,
        "observation_window_seconds": 180, "inventory_budget_seconds": 240,
        "whole_run_budget_seconds": 300, "cleanup_grace_seconds": 8,
        "shell_output": "pdksh print with a literal %s formatter; no external printf/rmdir dependency",
        "startup_trace": "MMI3G_DIAG_BOOT.txt appended after SD write confirmation",
        "required_runner_utilities": ["mkdir", "rm", "mv", "sleep", "ksh"],
        "required_ui_utilities": ["cat", "chmod", "mkdir", "rm", "ls"],
        "metadata_filters": "pdksh builtins; no external awk/head or Toolkit pass-through shims",
        "sync_missing_behavior": "collectors still return logs; save confirmation remains unverified",
        "sha256": {name: hashlib.sha256(data).hexdigest() for name, data in payload.items()},
    }
    payload["carplay-diagnostics/manifest.json"] = json.dumps(manifest, indent=2).encode() + b"\n"
    target.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(target, "x", zipfile.ZIP_DEFLATED) as archive:
        for name, data in payload.items():
            info = zipfile.ZipInfo(name, (2026, 10, 9, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            mode = 0o100755 if name == 'carplay-diagnostics/progress-view.sh' else 0o100644
            info.external_attr = (mode << 16)
            archive.writestr(info, data)
    print(str(target.resolve()))
    print("sha256=" + hashlib.sha256(target.read_bytes()).hexdigest())

if __name__ == "__main__":
    main()
