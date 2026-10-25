#!/usr/bin/env python3
"""Package the SD-only native runtime experiment; never deploys or runs it."""
from pathlib import Path
import hashlib
import json
import zipfile
ROOT=Path(__file__).resolve().parent.parent

def encode(data):
    # Toolkit-compatible PRNG XOR format, documented by dspl1236/MMI3G-Toolkit
    # e9ca3469835e3e1c78dabfb5cbe5676d2b752500/builder/encoder.py.
    seed=0x1be3ac
    def step():
        nonlocal seed
        value=seed & 0xffffffff
        rotated=((seed>>1)|(seed<<31)) & 0xffffffff
        added=(rotated+((rotated>>16)&255)) & 0xffffffff
        seed=(added-(((added>>8)&255)<<16)) & 0xffffffff
        return value
    step()
    return bytes(x^(step()&255) for x in data)

def main():
    with zipfile.ZipFile(ROOT/'artifacts/MMI3G-QNX-SH-Build-0.2.zip') as z:
        binary=z.read('mmi-runtime-check')
        audit=z.read('reference-audit.json')
        manifest=json.loads(z.read('build-manifest.json'))
        license_text=z.read('source/src/protocol/licenses/DiPlay-LICENSE')
        sources={n:z.read(n) for n in z.namelist() if n.startswith('source/')}
    if hashlib.sha256(binary).hexdigest()!=manifest['products']['mmi-runtime-check']:
        raise ValueError('Native binary does not match build manifest')
    plain=(ROOT/'sd-native-runtime/copie_scr_plain.sh').read_bytes()
    files={'copie_scr.sh':encode(plain),'native-runtime/copie_scr_plain.sh':plain,
           'native-runtime/start.sh':(ROOT/'sd-native-runtime/start.sh').read_bytes(),
           'native-runtime/mmi-runtime-check':binary,'native-runtime/reference-audit.json':audit,
           'native-runtime/build-manifest.json':(json.dumps(manifest,indent=2)+'\n').encode(),
           'native-runtime/COPYING':license_text}
    files.update({'native-runtime/'+n:b for n,b in sources.items()})
    readme='''MMI3G Native Runtime Experiment 0.1 -- NOT CARPLAY

Experimental SD boot + native loader check. P0710 runtime is NOT validated.
No green-menu item is created. No display or audio is taken over.
No USB device or input stream is opened. No firmware/EFS files are written.

This package REPLACES the SD root copie_scr.sh entry. Before copying it,
back up the CURRENT card entry and check SD health/contents. Keep every
other Toolkit file intact. To undo, restore that exact backed-up entry.
Do not restore an old backup blindly over a newer card configuration.

After an actual run, inspect MMI_NATIVE_BOOT.txt and
native-runtime/logs/run-*/status.txt, stdout.txt and stderr.txt.
encoded-entry-called = entry executed; shell-entered = wrapper entered;
native-start-requested = launch attempted; native-returned-zero = diagnostic
exited successfully. None of these means a CarPlay session worked.

No boot marker does NOT identify the cause: launch, mount/write or entry
conditions may have failed. Do not wait indefinitely for a menu or popup.

Encoding format reference: dspl1236/MMI3G-Toolkit commit
 e9ca3469835e3e1c78dabfb5cbe5676d2b752500, builder/encoder.py.
'''
    files['README-FIRST.txt']=readme.encode()
    files['native-runtime/package-manifest.json']=(json.dumps({'version':'0.1',
        'type':'experimental-native-runtime-diagnostic','target_validated':False,
        'vehicle_tested':False,'carplay_receiver_ready':False,
        'native_binary_sha256':hashlib.sha256(binary).hexdigest(),
        'files_sha256':{n:hashlib.sha256(b).hexdigest() for n,b in files.items()}},indent=2)+'\n').encode()
    dest=ROOT/'artifacts/MMI3G-Native-Runtime-Experiment-0.1.zip'
    with zipfile.ZipFile(dest,'w',zipfile.ZIP_DEFLATED) as z:
        for n,b in files.items():
            info=zipfile.ZipInfo(n);info.compress_type=zipfile.ZIP_DEFLATED
            info.external_attr=(0o100755 if n.endswith('.sh') or n.endswith('/mmi-runtime-check') else 0o100644)<<16
            z.writestr(info,b)
    print(dest)
    print(hashlib.sha256(dest.read_bytes()).hexdigest())

if __name__=='__main__':main()
