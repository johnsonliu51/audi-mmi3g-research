#!/bin/ksh
# Bounded SD collectors; an explicit UI marker enables one owned GEM page.
export LC_ALL=C
ulimit -c 0 2>/dev/null || :
SDPATH="${1%/}"
case "$SDPATH" in /*) ;; *) exit 2 ;; esac
case "$SDPATH" in /|*/../*|*/..|*/./*|*/.) exit 2 ;; esac
[ -d "$SDPATH" ] && [ ! -L "$SDPATH" ] || exit 2
[ -f "$SDPATH/MMI3G_CP_DIAG_CARD" ] && [ ! -L "$SDPATH/MMI3G_CP_DIAG_CARD" ] || exit 2
[ ! -L "$SDPATH/MMI3G_DIAG_STATUS.txt" ] || exit 2
BASE="$SDPATH/carplay-diagnostics"
[ -d "$BASE" ] && [ ! -L "$BASE" ] || exit 2
COMPAT="$BASE/qnx-carplay-compat.sh"
[ -f "$COMPAT" ] && [ ! -L "$COMPAT" ] || {
    print -r -- 'state=runner_failed
reason=compat_absent_or_symlink' > "$SDPATH/MMI3G_DIAG_STATUS.txt"
    exit 2
}
. "$COMPAT" || exit 2
for RUNNER_TOOL in mkdir rm mv sleep ksh; do
    command -v "$RUNNER_TOOL" >/dev/null 2>&1 || {
        cp_printf 'state=runner_failed\nreason=missing_utility_%s\n' "$RUNNER_TOOL" > "$SDPATH/MMI3G_DIAG_STATUS.txt"
        cp_printf 'runner_failure=missing_utility_%s\n' "$RUNNER_TOOL" >&2
        exit 2
    }
done
SCRIPT="$BASE/qnx-carplay-readonly.sh"
[ -f "$SCRIPT" ] && [ ! -L "$SCRIPT" ] || exit 2
[ ! -L "$BASE/logs" ] || exit 2
[ -d "$BASE/logs" ] || mkdir "$BASE/logs" || exit 2
LOCK="$BASE/logs/.running"
# Atomic mkdir. Never delete another invocation's lock or overwrite old logs.
# Power loss may leave an SD-only lock that requires manual recovery.
if ! mkdir "$LOCK" 2>/dev/null; then
    cp_printf 'state=duplicate_or_stale_lock\nExisting logs preserved; no second collector started.\n' > "$SDPATH/MMI3G_DIAG_STATUS.txt"
    exit 0
fi
WORKER_PID=
OBSERVER_PID=
WATCHDOG_PID=
SYNC_PID=
SYNC_GUARD_PID=
UI_PID=
UI_RESULT=not_requested
OUT=
INVENTORY_RESULT=unknown
OBSERVATION_RESULT=unknown
cleanup() {
    CP_RUNNER_RESULT=$?
    # Complete child reclamation even if the launcher repeats a stop signal.
    trap '' HUP INT TERM
    trap - EXIT
    [ -z "$WATCHDOG_PID" ] || kill -15 "$WATCHDOG_PID" 2>/dev/null
    [ -z "$WATCHDOG_PID" ] || wait "$WATCHDOG_PID" 2>/dev/null
    [ -z "$SYNC_GUARD_PID" ] || kill -15 "$SYNC_GUARD_PID" 2>/dev/null
    [ -z "$SYNC_PID" ] || kill -9 "$SYNC_PID" 2>/dev/null
    [ -z "$SYNC_GUARD_PID" ] || wait "$SYNC_GUARD_PID" 2>/dev/null
    [ -z "$SYNC_PID" ] || wait "$SYNC_PID" 2>/dev/null
    [ -z "$UI_PID" ] || kill -15 "$UI_PID" 2>/dev/null
    CP_RECLAIM=
    if [ -n "$WORKER_PID" ] && kill -0 "$WORKER_PID" 2>/dev/null; then
        kill -15 "$WORKER_PID" 2>/dev/null
        CP_RECLAIM=yes
    fi
    if [ -n "$OBSERVER_PID" ] && kill -0 "$OBSERVER_PID" 2>/dev/null; then
        kill -15 "$OBSERVER_PID" 2>/dev/null
        CP_RECLAIM=yes
    fi
    if [ -n "$UI_PID" ] && kill -0 "$UI_PID" 2>/dev/null; then CP_RECLAIM=yes; fi
    # Pipeline producers may need their own 6+1-second timeout to finish.
    # Give their collector EXIT handlers time to drain before a forced stop.
    if [ "$CP_RECLAIM" = yes ]; then
        sleep 8 &
        CP_RECLAIM_SLEEP_PID=$!
        wait "$CP_RECLAIM_SLEEP_PID" 2>/dev/null
        CP_RECLAIM_SLEEP_PID=
    fi
    if [ -n "$WORKER_PID" ] && kill -0 "$WORKER_PID" 2>/dev/null; then
        kill -9 "$WORKER_PID" 2>/dev/null
    fi
    if [ -n "$OBSERVER_PID" ] && kill -0 "$OBSERVER_PID" 2>/dev/null; then
        kill -9 "$OBSERVER_PID" 2>/dev/null
    fi
    [ -z "$WORKER_PID" ] || wait "$WORKER_PID" 2>/dev/null
    [ -z "$OBSERVER_PID" ] || wait "$OBSERVER_PID" 2>/dev/null
    if [ -n "$UI_PID" ]; then
        kill -9 "$UI_PID" 2>/dev/null
        wait "$UI_PID" 2>/dev/null
    fi
    # Early termination is recorded as partial if SD I/O still works.
    if [ -n "$OUT" ] && [ ! -e "$OUT/completed.txt" ] && [ ! -L "$OUT/completed.txt" ]; then
        cp_printf 'runner_exit=%s\ninventory_exit=%s\nobservation_exit=%s\ncollection_state=partial_or_interrupted\nNo CarPlay session was attempted.\n' \
            "$CP_RUNNER_RESULT" "$INVENTORY_RESULT" "$OBSERVATION_RESULT" >| "$OUT/completed.pending.txt" && \
            mv "$OUT/completed.pending.txt" "$OUT/completed.txt"
        cp_printf 'state=partial_or_interrupted\nrun_directory=carplay-diagnostics/logs/run-%s\nrunner_exit=%s\n' \
            "$INDEX" "$CP_RUNNER_RESULT" >| "$SDPATH/MMI3G_DIAG_STATUS.txt"
    fi
    # Only this runner's successful atomic mkdir owns this exact lock.
    if [ ! -L "$LOCK" ] && [ -d "$LOCK" ]; then
        CP_LOCK_EMPTY=yes
        for CP_LOCK_ITEM in "$LOCK"/* "$LOCK"/.[!.]* "$LOCK"/..?*; do
            if [ -e "$CP_LOCK_ITEM" ] || [ -L "$CP_LOCK_ITEM" ]; then CP_LOCK_EMPTY=no; break; fi
        done
        if [ "$CP_LOCK_EMPTY" = yes ]; then
            rm -R -f "$LOCK" 2>/dev/null || cp_printf 'runner_cleanup=owned_lock_remove_failed\n' >&2
        else
            cp_printf 'runner_cleanup=unexpected_lock_content_retained\n' >&2
        fi
    fi
}
trap cleanup EXIT
trap 'exit 143' HUP INT TERM
INDEX=1
while [ -e "$BASE/logs/run-$INDEX" ] || [ -L "$BASE/logs/run-$INDEX" ]; do
    INDEX=$((INDEX + 1))
    [ "$INDEX" -le 9999 ] || exit 2
done
OUT="$BASE/logs/run-$INDEX"
mkdir "$OUT" || exit 2
set -C
cp_printf 'MMI3G CarPlay diagnostics; metadata only\nrunner_pid=%s\nsd_write_mode=%s\nwhole_run_budget_seconds=300\ncleanup_grace_seconds=8\ninventory_budget_seconds=240\nobservation_window_seconds=180\nNo CarPlay session attempted.\n' "$$" "${CP_SD_WRITE_MODE:-runner_direct}" > "$OUT/started.txt" || exit 2
cp_printf 'state=collection_started\nrun_directory=carplay-diagnostics/logs/run-%s\n' "$INDEX" >| "$SDPATH/MMI3G_DIAG_STATUS.txt" || exit 2
ksh "$SCRIPT" --observe "$OUT" > "$OUT/observation.txt" 2>&1 &
OBSERVER_PID=$!
ksh "$SCRIPT" inventory "$OUT" > "$OUT/inventory.txt" 2>&1 &
WORKER_PID=$!
RUNNER_PID=$$
(
    trap - EXIT
    GUARD_SLEEP_PID=
    trap '[ -z "$GUARD_SLEEP_PID" ] || kill -15 "$GUARD_SLEEP_PID" 2>/dev/null; exit 0' HUP INT TERM
    sleep 300 &
    GUARD_SLEEP_PID=$!
    wait "$GUARD_SLEEP_PID" 2>/dev/null
    GUARD_SLEEP_PID=
    # Signal our still-running parent, which tracks current children. Retaining
    # old collector PIDs in a long timer could target a later reused process ID.
    kill -15 "$RUNNER_PID" 2>/dev/null
    sleep 2 &
    GUARD_SLEEP_PID=$!
    wait "$GUARD_SLEEP_PID" 2>/dev/null
    GUARD_SLEEP_PID=
    kill -9 "$RUNNER_PID" 2>/dev/null
) >/dev/null 2>&1 &
WATCHDOG_PID=$!
# The UI installer is opt-in and adds one owned ESD file only. Collection
# remains independent; a UI failure is reported separately from log progress.
UI_ACTION=
if [ -f "$BASE/ui/REMOVE" ] && [ ! -L "$BASE/ui/REMOVE" ]; then UI_ACTION=remove
elif [ -f "$BASE/ui/ENABLE" ] && [ ! -L "$BASE/ui/ENABLE" ]; then UI_ACTION=install
fi
if [ -n "$UI_ACTION" ] && [ -f "$BASE/ui-page.sh" ] && [ ! -L "$BASE/ui-page.sh" ]; then
    ksh "$BASE/ui-page.sh" "$UI_ACTION" "$SDPATH" "$OUT" > "$OUT/ui-installation.txt" 2>&1 &
    UI_PID=$!
fi
wait "$WORKER_PID" 2>/dev/null
INVENTORY_RESULT=$?
WORKER_PID=
wait "$OBSERVER_PID" 2>/dev/null
OBSERVATION_RESULT=$?
OBSERVER_PID=
if [ -n "$UI_PID" ]; then
    wait "$UI_PID" 2>/dev/null
    UI_RESULT=$?
    UI_PID=
fi
cp_printf 'state=collectors_returned\nrun_directory=carplay-diagnostics/logs/run-%s\ninventory_exit=%s\nobservation_exit=%s\n' \
    "$INDEX" "$INVENTORY_RESULT" "$OBSERVATION_RESULT" >| "$SDPATH/MMI3G_DIAG_STATUS.txt" || exit 2
# Flush the collected data before publishing the completion marker. Remain
# under the whole-run watchdog; a missing/hung sync cannot block indefinitely.
SYNC_RESULT=127
if command -v sync >/dev/null 2>&1; then
    sync >/dev/null 2>&1 &
    SYNC_PID=$!
    (
        trap - EXIT
        SYNC_SLEEP_PID=
        trap '[ -z "$SYNC_SLEEP_PID" ] || kill -15 "$SYNC_SLEEP_PID" 2>/dev/null; exit 0' HUP INT TERM
        sleep 6 &
        SYNC_SLEEP_PID=$!
        wait "$SYNC_SLEEP_PID" 2>/dev/null
        kill -15 "$SYNC_PID" 2>/dev/null
        sleep 1 &
        SYNC_SLEEP_PID=$!
        wait "$SYNC_SLEEP_PID" 2>/dev/null
        kill -9 "$SYNC_PID" 2>/dev/null
    ) >/dev/null 2>&1 &
    SYNC_GUARD_PID=$!
    wait "$SYNC_PID" 2>/dev/null
    SYNC_RESULT=$?
    SYNC_PID=
    kill -15 "$SYNC_GUARD_PID" 2>/dev/null
    wait "$SYNC_GUARD_PID" 2>/dev/null
    SYNC_GUARD_PID=
fi
RUNNER_RESULT=0
if [ "$INVENTORY_RESULT" -ne 0 ] || [ "$OBSERVATION_RESULT" -ne 0 ] || [ "$SYNC_RESULT" -ne 0 ]; then RUNNER_RESULT=1; fi
cp_printf 'runner_exit=%s\ninventory_exit=%s\nobservation_exit=%s\nsync_exit=%s\nui_page_exit=%s\ncollection_state=collectors_returned\nCommand failures/timeouts are marked unknown in the logs.\nNo CarPlay session was attempted.\n' \
    "$RUNNER_RESULT" "$INVENTORY_RESULT" "$OBSERVATION_RESULT" "$SYNC_RESULT" "$UI_RESULT" > "$OUT/completed.pending.txt" || exit 2
mv "$OUT/completed.pending.txt" "$OUT/completed.txt" || exit 2
cp_printf 'state=collectors_returned\nrun_directory=carplay-diagnostics/logs/run-%s\nrunner_exit=%s\ninventory_exit=%s\nobservation_exit=%s\nsync_exit=%s\nui_page_exit=%s\n' \
    "$INDEX" "$RUNNER_RESULT" "$INVENTORY_RESULT" "$OBSERVATION_RESULT" "$SYNC_RESULT" "$UI_RESULT" >| "$SDPATH/MMI3G_DIAG_STATUS.txt" || exit 2
kill -15 "$WATCHDOG_PID" 2>/dev/null
wait "$WATCHDOG_PID" 2>/dev/null
WATCHDOG_PID=
exit "$RUNNER_RESULT"
