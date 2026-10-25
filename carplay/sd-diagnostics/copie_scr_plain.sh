#!/bin/ksh
# Toolkit-encoded SD entry. Initial evidence uses pdksh builtins only.
SDPATH="${1%/}"
case "$SDPATH" in /*) ;; *) exit 2 ;; esac
case "$SDPATH" in /|*/../*|*/..|*/./*|*/.|*//*) exit 2 ;; esac
[ -d "$SDPATH" ] && [ ! -L "$SDPATH" ] || exit 2
[ -f "$SDPATH/MMI3G_CP_DIAG_CARD" ] && [ ! -L "$SDPATH/MMI3G_CP_DIAG_CARD" ] || exit 2
[ -f "$SDPATH/run.sh" ] && [ ! -L "$SDPATH/run.sh" ] || exit 2
STATUS="$SDPATH/MMI3G_DIAG_STATUS.txt"
BOOT="$SDPATH/MMI3G_DIAG_BOOT.txt"
for ENTRY_FILE in "$STATUS" "$BOOT"; do
    [ ! -L "$ENTRY_FILE" ] && { [ ! -e "$ENTRY_FILE" ] || [ -f "$ENTRY_FILE" ]; } || exit 2
done
entry_stop() {
    print -r -- "state=launcher_failed
entry_version=0.6
reason=$1" > "$STATUS" 2>/dev/null
    print -r -- "entry_failure=$1" >&2
    exit 3
}
# These are needed even before collection. Never silently continue without a
# sleep timer: otherwise a watchdog could kill jobs immediately.
for ENTRY_TOOL in mkdir rm sleep ksh; do
    command -v "$ENTRY_TOOL" >/dev/null 2>&1 || entry_stop "missing_utility_$ENTRY_TOOL"
done
PROBE_ATTEMPT=0
PROBE_CLEANUP=not_attempted
probe_write() {
    PROBE_ATTEMPT=$((PROBE_ATTEMPT + 1))
    WRITE_PROBE="$SDPATH/.mmi3g-diag-write-$$-$PROBE_ATTEMPT"
    # Successful mkdir gives exclusive ownership. Retries use new paths.
    mkdir "$WRITE_PROBE" 2>/dev/null || return 1
    print -r -- 'MMI3G diagnostics write check' > "$WRITE_PROBE/check.txt" 2>/dev/null
    WRITE_RESULT=$?
    PROBE_CLEANUP=failed_owned_probe_retained
    if [ ! -L "$WRITE_PROBE" ] && [ -d "$WRITE_PROBE" ]; then
        [ -L "$WRITE_PROBE/check.txt" ] || rm -f "$WRITE_PROBE/check.txt" 2>/dev/null
        PROBE_EMPTY=yes
        for PROBE_ITEM in "$WRITE_PROBE"/* "$WRITE_PROBE"/.[!.]* "$WRITE_PROBE"/..?*; do
            if [ -e "$PROBE_ITEM" ] || [ -L "$PROBE_ITEM" ]; then PROBE_EMPTY=no; break; fi
        done
        if [ "$PROBE_EMPTY" = yes ] && rm -R -f "$WRITE_PROBE" 2>/dev/null; then
            PROBE_CLEANUP=removed
        fi
    fi
    # Cleanup failure never changes the result of the actual file write.
    return "$WRITE_RESULT"
}
CP_SD_WRITE_MODE=already_writable
if ! probe_write; then
    case "$SDPATH" in
        /mnt/sdcard10t12|/mnt/sdcard20t12|/mnt/sdcard10t11|/mnt/sdcard20t11) ;;
        *) entry_stop write_failed_unapproved_mount ;;
    esac
    command -v mount >/dev/null 2>&1 || entry_stop missing_utility_mount
    mount -u -w "$SDPATH" >/dev/null 2>&1 &
    MOUNT_PID=$!
    (
        MOUNT_SLEEP_PID=
        trap '[ -z "$MOUNT_SLEEP_PID" ] || kill -15 "$MOUNT_SLEEP_PID" 2>/dev/null; exit 0' HUP INT TERM
        sleep 6 &
        MOUNT_SLEEP_PID=$!
        wait "$MOUNT_SLEEP_PID" 2>/dev/null
        kill -15 "$MOUNT_PID" 2>/dev/null
        sleep 1 &
        MOUNT_SLEEP_PID=$!
        wait "$MOUNT_SLEEP_PID" 2>/dev/null
        kill -9 "$MOUNT_PID" 2>/dev/null
    ) >/dev/null 2>&1 &
    MOUNT_GUARD_PID=$!
    wait "$MOUNT_PID" 2>/dev/null
    MOUNT_RESULT=$?
    kill -15 "$MOUNT_GUARD_PID" 2>/dev/null
    wait "$MOUNT_GUARD_PID" 2>/dev/null
    [ "$MOUNT_RESULT" -eq 0 ] || entry_stop "sd_remount_exit_$MOUNT_RESULT"
    probe_write || entry_stop sd_write_failed_after_remount
    CP_SD_WRITE_MODE=sd_remounted_readwrite
fi
# Append an entry trace and capture runner errors without printf/awk/date/UI.
exec >> "$BOOT" 2>&1 || entry_stop boot_log_open_failed
print -r -- "entry_version=0.6
entry_pid=$$
entry_stage=sd_write_confirmed
sd_write_mode=$CP_SD_WRITE_MODE
probe_attempts=$PROBE_ATTEMPT
probe_cleanup=$PROBE_CLEANUP"
print -r -- "state=launcher_started
entry_version=0.6
sd_write_mode=$CP_SD_WRITE_MODE" > "$STATUS" || entry_stop status_write_failed
for ENTRY_TOOL in print printf rmdir mkdir rm mv sleep ksh sync awk head cat cmp chmod; do
    if command -v "$ENTRY_TOOL" >/dev/null 2>&1; then print -r -- "utility_$ENTRY_TOOL=available"
    else print -r -- "utility_$ENTRY_TOOL=unavailable"; fi
done
print -r -- 'entry_stage=runner_exec'
export CP_SD_WRITE_MODE SDPATH
exec ksh "$SDPATH/run.sh" "$SDPATH"
entry_stop runner_exec_failed
