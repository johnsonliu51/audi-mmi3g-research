#!/usr/bin/env python3
"""Start/stop the existing task-local QNX cross-build VM, not a car emulator.
Requires the VM assets prepared under Library/Application Support/MMI3G-CarPlay.
Exposes SSH only on 127.0.0.1:22293. Never executes SH target binaries.
"""
import argparse, gzip, os, signal, stat, subprocess
from pathlib import Path
ROOT=Path.home()/'Library/Application Support/MMI3G-CarPlay'
VM=ROOT/'linux-build-vm'

def entry(name,data,mode,ino):
    name=name.encode()+b'\0'
    values=[ino,mode,0,0,1,0,len(data),0,0,0,0,len(name),0]
    h=b'070701'+b''.join(('%08x'%v).encode() for v in values)
    a=h+name;a+=b'\0'*((-len(a))%4)
    return a+data+b'\0'*((-len(data))%4)

def start():
    pidfile=VM/'pid'
    if pidfile.exists():
        try:os.kill(int(pidfile.read_text()),0)
        except ProcessLookupError:pass
        else:raise SystemExit('Recorded VM PID still exists; inspect before starting another VM')
    for n in ['linux','initrd.gz','build.qcow2','extra-modules']:
        if not (VM/n).exists():raise SystemExit('Missing VM asset: '+n)
    script=b"""#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
depmod -a
modprobe virtio_pci
modprobe virtio_blk
modprobe ext4
modprobe virtio_net
mkdir -p /target
mount /dev/vda1 /target || exec /bin/sh
mount --bind /dev /target/dev
mount --bind /proc /target/proc
mount --bind /sys /target/sys
mount -t tmpfs tmpfs /target/run
mkdir -p /target/run/sshd
ip link set eth0 up
ip addr add 10.0.2.15/24 dev eth0
ip route add default via 10.0.2.2
exec chroot /target /usr/sbin/sshd -D -e -o PermitRootLogin=prohibit-password
"""
    records=[];ino=1
    for p in [VM/'extra-modules/lib', *sorted((VM/'extra-modules/lib').rglob('*'))]:
        if p.is_symlink():raise SystemExit('Unexpected module symlink')
        mode=(stat.S_IFDIR|0o755) if p.is_dir() else (stat.S_IFREG|0o644)
        data=b'' if p.is_dir() else p.read_bytes()
        records.append(entry(str(p.relative_to(VM/'extra-modules')),data,mode,ino));ino+=1
    records.append(entry('mmi-init',script,stat.S_IFREG|0o755,ino));ino+=1
    records.append(entry('TRAILER!!!',b'',0,ino))
    initrd=VM/'build-initrd.gz'
    initrd.write_bytes((VM/'initrd.gz').read_bytes()+gzip.compress(b''.join(records),mtime=0))
    args=['qemu-system-x86_64','-machine','q35','-accel','tcg','-cpu','max','-m','1536','-smp','2','-display','none','-serial','file:'+str(VM/'build-console.log'),'-monitor','none','-kernel',str(VM/'linux'),'-initrd',str(initrd),'-append','console=ttyS0,115200 rdinit=/mmi-init','-drive','file='+str(VM/'build.qcow2')+',if=virtio,format=qcow2','-netdev','user,id=n1,hostfwd=tcp:127.0.0.1:22293-:22','-device','virtio-net-pci,netdev=n1']
    with (VM/'build-qemu.log').open('ab') as log:p=subprocess.Popen(args,stdout=log,stderr=log,start_new_session=True)
    pidfile.write_text(str(p.pid));print('Started build VM',p.pid)

def stop():
    pid=int((VM/'pid').read_text())
    identity=subprocess.run(['ps','-p',str(pid),'-o','command='],capture_output=True,text=True,check=True).stdout
    if 'qemu-system-x86_64' not in identity or str(VM/'build.qcow2') not in identity:
        raise SystemExit('Recorded PID does not identify this build VM')
    args=['ssh','-o','BatchMode=yes','-o','ConnectTimeout=5','-o','UserKnownHostsFile='+str(VM/'known_hosts'),'-i',str(VM/'id_ed25519'),'-p','22293','root@127.0.0.1','sync']
    subprocess.run(args,check=True,timeout=15)
    os.kill(pid,signal.SIGTERM);print('Flushed and stopped VM',pid)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['start','stop']);a=p.parse_args()
    start() if a.action=='start' else stop()
