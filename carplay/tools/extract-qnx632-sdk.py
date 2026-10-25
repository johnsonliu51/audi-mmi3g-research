#!/usr/bin/env python3
"""Extract selected SDK payloads from a supplied Momentics 6.3.2 installer JAR.
No installer execution, activation, license modification or system-wide writes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zipfile
import zlib

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('jar',type=Path)
    ap.add_argument('destination',type=Path)
    a=ap.parse_args()
    root=a.destination.resolve()
    root.mkdir(parents=True,exist_ok=True)
    rows=[]
    with zipfile.ZipFile(a.jar) as z:
        names=set(z.namelist())
        indexes=[n for n in names if n.endswith('/index')]
        if len(indexes)!=1:raise ValueError('Expected one payload index')
        index=indexes[0];prefix=index.rsplit('/',1)[0]+'/md5/'
        raw=z.read(index)
        version,count=struct.unpack_from('>II',raw)
        if version!=1000 or not 1<=count<=100:raise ValueError('Unsupported index')
        position=8
        for _ in range(count):
            size=struct.unpack_from('>I',raw,position)[0];position+=4
            block=zlib.decompress(raw[position:position+size]);position+=size
            for m in re.finditer(rb'(?:host/linux/x86|target/qnx6)/[ -~]+',block):
                start=m.start();path=m.group().decode('ascii')
                if start<18 or struct.unpack_from('>H',block,start-2)[0]!=len(m.group()):continue
                if not path.startswith(('host/linux/x86/usr/','host/linux/x86/etc/qcc/',
                                        'target/qnx6/usr/include/','target/qnx6/shle/')):continue
                if '..' in Path(path).parts:raise ValueError('Unsafe payload path')
                digest=block[start-18:start-2].hex();name=prefix+digest
                if name not in names:continue
                data=z.read(name)
                if not data:continue  # directory records share the empty MD5
                if hashlib.md5(data).hexdigest()!=digest:data=zlib.decompress(data)
                if hashlib.md5(data).hexdigest()!=digest:raise ValueError('Payload digest mismatch: '+path)
                dest=root/path
                dest.parent.mkdir(parents=True,exist_ok=True)
                dest.write_bytes(data)
                if path.startswith('host/') and data.startswith((b'\x7fELF',b'#!')):dest.chmod(0o755)
                rows.append({'path':path,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
        if position!=len(raw):raise ValueError('Trailing index bytes')
    (root/'extraction-manifest.json').write_text(json.dumps(rows,indent=2)+'\n')
    print('Extracted',len(rows),'files into',root)

if __name__=='__main__':main()
