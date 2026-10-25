#!/bin/ksh
# Minimal SD-only entry. No green-menu installation or internal remount.
SDPATH=$1
case "$SDPATH" in
    /mnt/sdcard10t12|/mnt/sdcard20t12|/mnt/sdcard10t11|/mnt/sdcard20t11) ;;
    *) exit 64 ;;
esac
# Match the original Toolkit SD remount; never remount EFS or firmware.
mount -u "$SDPATH" >/dev/null 2>&1
if ! echo 'phase=encoded-entry-called version=0.1' >> "$SDPATH/MMI_NATIVE_BOOT.txt"; then
    exit 73
fi
if [ ! -f "$SDPATH/native-runtime/start.sh" ]; then
    echo 'phase=missing-wrapper' >> "$SDPATH/MMI_NATIVE_BOOT.txt"
    exit 66
fi
exec ksh "$SDPATH/native-runtime/start.sh" "$SDPATH"
