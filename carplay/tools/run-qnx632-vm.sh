#!/bin/sh
set -eu

VM_DIR=${QNX632_VM_DIR:-/private/tmp/qnx632-legacy-host}
SHARE_DIR=${QNX632_SHARE_DIR:-/private/tmp/qnx632-share}
DISK_IMAGE="$VM_DIR/qnx632-ubuntu704.qcow2"
BOOT_ISO=${QNX632_BOOT_ISO:-}
QEMU_BIN=${QEMU_SYSTEM_I386:-qemu-system-i386}
QEMU_IMG=${QEMU_IMG:-qemu-img}

if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
	printf '%s\n' "Missing QEMU executable: $QEMU_BIN" >&2
	exit 1
fi
if ! command -v "$QEMU_IMG" >/dev/null 2>&1; then
	printf '%s\n' "Missing QEMU image utility: $QEMU_IMG" >&2
	exit 1
fi

mkdir -p "$VM_DIR" "$SHARE_DIR"
if [ ! -f "$DISK_IMAGE" ]; then
	"$QEMU_IMG" create -f qcow2 "$DISK_IMAGE" 8G
fi

set -- \
	-machine pc,accel=tcg \
	-m 1536 \
	-smp 1 \
	-drive "if=ide,index=0,media=disk,format=qcow2,file=$DISK_IMAGE" \
	-drive "if=ide,index=1,media=disk,format=raw,file=fat:rw:$SHARE_DIR" \
	-display cocoa \
	-vga std \
	-usb \
	-device usb-tablet \
	-nic none \
	-rtc base=localtime

if [ -n "$BOOT_ISO" ]; then
	if [ ! -f "$BOOT_ISO" ]; then
		printf '%s\n' "Boot ISO not found: $BOOT_ISO" >&2
		exit 1
	fi
	set -- "$@" -drive "if=ide,index=2,media=cdrom,readonly=on,file=$BOOT_ISO" -boot order=d,menu=on
else
	set -- "$@" -boot order=c
fi

printf '%s\n' "Starting isolated x86 QEMU guest. Networking is disabled." \
	"VM disk: $DISK_IMAGE" \
	"Host share: $SHARE_DIR (FAT, read/write)"
exec "$QEMU_BIN" "$@"
