#!/bin/ksh
# QNX SH runtime candidate launcher, not an installer or a CarPlay receiver.
# Invoke explicitly: ksh /mnt/sdcard10t12/native-runtime/start.sh /mnt/sdcard10t12
sdroot=$1
case "$sdroot" in
    /mnt/sdcard*) ;;
    *) echo 'ERROR: explicit MMI SD mount path required' >&2; exit 64 ;;
esac
if [ ! -d "$sdroot/native-runtime" ]; then
    echo 'ERROR: native-runtime directory missing' >&2
    exit 66
fi
# This record precedes any native code. Never remount internal filesystems.
if ! echo 'phase=shell-entered version=0.2' >> "$sdroot/MMI_NATIVE_BOOT.txt"; then
    echo 'ERROR: SD log is not writable; native program not started' >&2
    exit 73
fi
logroot="$sdroot/native-runtime/logs"
if ! mkdir -p "$logroot"; then
    echo 'phase=log-directory-failed' >> "$sdroot/MMI_NATIVE_BOOT.txt"
    exit 73
fi
index=0
while [ -d "$logroot/run-$$-$index" ]; do
    index=$((index + 1))
done
logdir="$logroot/run-$$-$index"
if ! mkdir "$logdir"; then
    echo 'phase=run-directory-failed' >> "$sdroot/MMI_NATIVE_BOOT.txt"
    exit 73
fi
status="$logdir/status.txt"
echo 'phase=shell-ready' > "$status"
echo "logs=$logdir" >> "$sdroot/MMI_NATIVE_BOOT.txt"
program="$sdroot/native-runtime/mmi-runtime-check"
if [ ! -f "$program" ]; then
    echo 'phase=missing-native-program' >> "$status"
    exit 66
fi
echo 'phase=native-start-requested' >> "$status"
"$program" > "$logdir/stdout.txt" 2> "$logdir/stderr.txt"
result=$?
echo "native_exit=$result" >> "$status"
if [ "$result" -eq 0 ]; then
    # Success here means this small diagnostic exited, not CarPlay success.
    echo 'phase=native-returned-zero' >> "$status"
else
    echo 'phase=native-failed-see-stderr' >> "$status"
fi
echo 'carplay_session=not_attempted' >> "$status"
echo 'phase=wrapper-finished' >> "$status"
sync
exit "$result"
