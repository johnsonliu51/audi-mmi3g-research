#!/bin/ksh
# Metadata only. Caller redirects stdout to SD; no IPC/device streams, auth
# stores, interface claims, driver changes, DSP reloads or firmware writes.
export LC_ALL=C
CP_SCRIPT_DIR="${0%/*}"
[ "$CP_SCRIPT_DIR" != "$0" ] || CP_SCRIPT_DIR=.
CP_COMPAT="$CP_SCRIPT_DIR/qnx-carplay-compat.sh"
[ -f "$CP_COMPAT" ] && [ ! -L "$CP_COMPAT" ] || {
    print -r -- 'collector_failure=compat_absent_or_symlink' >&2
    exit 2
}
. "$CP_COMPAT" || exit 2
for CP_REQUIRED_TOOL in sleep; do
    command -v "$CP_REQUIRED_TOOL" >/dev/null 2>&1 || {
        cp_printf 'collector_failure=missing_utility_%s\n' "$CP_REQUIRED_TOOL" >&2
        exit 2
    }
done
SECONDS=0
CP_TASK_PID=
CP_WATCHDOG_PID=
CP_LIMIT=240
CP_MODE="${1:-standalone}"
case "$CP_MODE" in standalone|inventory) ;; --observe) CP_LIMIT=180 ;; *) exit 2 ;; esac
CP_PROGRESS_DIR=
CP_PROGRESS_ROLE=inventory
[ "$CP_MODE" != '--observe' ] || CP_PROGRESS_ROLE=observer
CP_PROGRESS_PHASE=starting
CP_PROGRESS_STAGE=none
CP_PROGRESS_SEQ=0
CP_PROGRESS_PROCESSED=0
CP_PROGRESS_EXIT=unknown
CP_PROGRESS_NATURAL_RETURN=0
CP_COMPLETED_SAMPLES=0
CP_SAMPLE=0
CP_USB_QUERY_EXIT=unknown
CP_USB_QUERY_SAMPLE=unknown
CP_USB_RETURNED_COUNT=0
CP_USB_ZERO_COUNT=0
CP_USB_FAILED_COUNT=0
CP_USB_UNKNOWN_COUNT=0
ulimit -f 4096 2>/dev/null || :
ulimit -c 0 2>/dev/null || :
cleanup() {
    CP_FINAL_EXIT=$?
    trap '' HUP INT TERM
    trap - EXIT
    [ -z "$CP_WATCHDOG_PID" ] || kill -15 "$CP_WATCHDOG_PID" 2>/dev/null
    [ -z "$CP_TASK_PID" ] || kill -9 "$CP_TASK_PID" 2>/dev/null
    # A bounded producer on the left of a pipeline may run in a ksh subshell.
    # Its own timer survives until that producer finishes (at most 6+1 seconds).
    # Let those jobs drain; the runner allows 8 seconds before a final KILL.
    wait 2>/dev/null
    if [ "$CP_PROGRESS_NATURAL_RETURN" -ne 1 ]; then
        CP_PROGRESS_PHASE=interrupted
        CP_PROGRESS_EXIT="$CP_FINAL_EXIT"
        progress_publish
    fi
}
trap cleanup EXIT
trap 'exit 143' HUP INT TERM
section() { cp_printf '\n=== %s ===\n' "$1"; }
unknown() { cp_printf '[unknown: %s]\n' "$1"; }
has() { command -v "$1" >/dev/null 2>&1; }
# Optional second argument: an existing run directory. All snapshots remain
# separate from stdout; a progress failure never stops the diagnostic itself.
progress_init() {
    [ -n "$1" ] || return 0
    CP_PROGRESS_CANDIDATE="${1%/}"
    case "$CP_PROGRESS_CANDIDATE" in /|''|*/../*|*/..|*/./*|*/.|*//*)
        unknown 'progress_directory_invalid; stdout_collection_continues'; return 0 ;;
        /*) ;; *) unknown 'progress_directory_not_absolute; stdout_collection_continues'; return 0 ;;
    esac
    [ "${#CP_PROGRESS_CANDIDATE}" -le 512 ] || {
        unknown 'progress_directory_too_long; stdout_collection_continues'; return 0;
    }
    CP_PROGRESS_REST="${CP_PROGRESS_CANDIDATE#/}"
    CP_PROGRESS_PREFIX=
    while [ -n "$CP_PROGRESS_REST" ]; do
        CP_PROGRESS_PART="${CP_PROGRESS_REST%%/*}"
        CP_PROGRESS_PREFIX="$CP_PROGRESS_PREFIX/$CP_PROGRESS_PART"
        if [ -L "$CP_PROGRESS_PREFIX" ] || [ ! -d "$CP_PROGRESS_PREFIX" ]; then
            unknown 'progress_directory_absent_or_symlink; stdout_collection_continues'; return 0
        fi
        case "$CP_PROGRESS_REST" in */*) CP_PROGRESS_REST="${CP_PROGRESS_REST#*/}" ;; *) CP_PROGRESS_REST= ;; esac
    done
    if ! has mv; then
        unknown 'progress_mv_unavailable; stdout_collection_continues'; return 0
    fi
    CP_PROGRESS_DIR="$CP_PROGRESS_CANDIDATE"
    CP_PROGRESS_RUN_ID="${CP_PROGRESS_DIR##*/}"
    case "$CP_PROGRESS_RUN_ID" in *[!A-Za-z0-9_-]*|'') CP_PROGRESS_RUN_ID=unknown ;; esac
    [ "${#CP_PROGRESS_RUN_ID}" -le 64 ] || CP_PROGRESS_RUN_ID=unknown
    CP_PROGRESS_TOKEN="$$-${RANDOM:-0}"
    if [ "$CP_PROGRESS_ROLE" = inventory ]; then
        CP_PROGRESS_FILE="$CP_PROGRESS_DIR/inventory.progress"
        CP_USB_RESULT_FILE="$CP_PROGRESS_DIR/inventory.usb-result"
    else
        CP_PROGRESS_FILE="$CP_PROGRESS_DIR/observation.progress"
        CP_USB_RESULT_FILE="$CP_PROGRESS_DIR/observation.usb-result"
    fi
    progress_publish
}
progress_safe_file() {
    [ ! -L "$1" ] && { [ ! -e "$1" ] || [ -f "$1" ]; }
}
progress_publish() {
    [ -n "$CP_PROGRESS_DIR" ] || return 0
    CP_PROGRESS_PENDING="$CP_PROGRESS_FILE.pending"
    if ! progress_safe_file "$CP_PROGRESS_FILE" || ! progress_safe_file "$CP_PROGRESS_PENDING"; then
        unknown 'progress_snapshot_path_unsafe; stdout_collection_continues'; return 0
    fi
    CP_PROGRESS_SEQ=$((CP_PROGRESS_SEQ + 1))
    if {
        cp_printf 'schema=1\nrun_id=%s\nrole=%s\nphase=%s\nstage=%s\nseq=%s\nelapsed_seconds=%s\nexit_code=%s\n' \
            "$CP_PROGRESS_RUN_ID" "$CP_PROGRESS_ROLE" "$CP_PROGRESS_PHASE" "$CP_PROGRESS_STAGE" \
            "$CP_PROGRESS_SEQ" "$SECONDS" "$CP_PROGRESS_EXIT"
        if [ "$CP_PROGRESS_ROLE" = inventory ]; then
            cp_printf 'processed=%s\ntotal=14\n' "$CP_PROGRESS_PROCESSED"
        else
            CP_USB_SUCCESS_DISPLAY=unknown
            CP_USB_FAILURE_DISPLAY=unknown
            if [ "$CP_USB_RETURNED_COUNT" -gt 0 ]; then
                CP_USB_SUCCESS_DISPLAY="$CP_USB_ZERO_COUNT"
                CP_USB_FAILURE_DISPLAY="$CP_USB_FAILED_COUNT"
            fi
            cp_printf 'completed_samples=%s\ncurrent_sample=%s\nusb_query_sample=%s\nusb_query_exit=%s\nreturned_usb_queries=%s\nsuccessful_usb_queries=%s\nfailed_usb_queries=%s\nunknown_usb_queries=%s\n' \
                "$CP_COMPLETED_SAMPLES" "$CP_SAMPLE" "$CP_USB_QUERY_SAMPLE" "$CP_USB_QUERY_EXIT" "$CP_USB_RETURNED_COUNT" \
                "$CP_USB_SUCCESS_DISPLAY" "$CP_USB_FAILURE_DISPLAY" "$CP_USB_UNKNOWN_COUNT"
        fi
        cp_printf 'quality=unassessed\ncarplay_status=untested\ncommit=1\n'
    } > "$CP_PROGRESS_PENDING" && mv "$CP_PROGRESS_PENDING" "$CP_PROGRESS_FILE"; then
        :
    else
        unknown 'progress_snapshot_write_failed; stdout_collection_continues'
    fi
    return 0
}
inventory_stage_begin() {
    [ "$CP_PROGRESS_ROLE" = inventory ] || return 0
    CP_PROGRESS_PHASE=collecting
    CP_PROGRESS_STAGE="$1"
    progress_publish
}
inventory_stage_end() {
    [ "$CP_PROGRESS_ROLE" = inventory ] || return 0
    CP_PROGRESS_PROCESSED=$((CP_PROGRESS_PROCESSED + 1))
    [ -z "$CP_PROGRESS_DIR" ] || cp_printf 'progress_section_returned=%s processed=%s\n' "$1" "$CP_PROGRESS_PROCESSED"
    progress_publish
}
progress_usb_result() {
    # Called only by bounded's USB producer. A pipeline may fork it; do not
    # modify or publish the parent observer's counters from this subshell.
    [ -n "$CP_PROGRESS_DIR" ] || return 0
    CP_USB_PENDING="$CP_USB_RESULT_FILE.pending"
    if ! progress_safe_file "$CP_USB_RESULT_FILE" || ! progress_safe_file "$CP_USB_PENDING"; then
        unknown 'progress_usb_result_path_unsafe'; return 0
    fi
    if cp_printf 'schema=1\ntoken=%s\nsample=%s\nusb_query_exit=%s\ncommit=1\n' \
        "$CP_PROGRESS_TOKEN" "$CP_SAMPLE" "$1" > "$CP_USB_PENDING" && \
        mv "$CP_USB_PENDING" "$CP_USB_RESULT_FILE"; then
        :
    else
        unknown 'progress_usb_result_write_failed'
    fi
    return 0
}
progress_read_usb_result() {
    CP_USB_QUERY_EXIT=unknown
    if [ -n "$CP_PROGRESS_DIR" ] && [ -f "$CP_USB_RESULT_FILE" ] && [ ! -L "$CP_USB_RESULT_FILE" ]; then
        CP_USB_SCHEMA=
        CP_USB_TOKEN=
        CP_USB_SAMPLE=
        CP_USB_RC=
        CP_USB_COMMIT=
        CP_USB_FIELDS=0
        CP_USB_BAD=0
        while IFS='=' read -r CP_USB_KEY CP_USB_VALUE; do
            CP_USB_FIELDS=$((CP_USB_FIELDS + 1))
            [ "$CP_USB_FIELDS" -le 8 ] || { CP_USB_BAD=1; break; }
            [ "${#CP_USB_VALUE}" -le 64 ] || { CP_USB_BAD=1; break; }
            case "$CP_USB_KEY" in
                schema) [ -z "$CP_USB_SCHEMA" ] || CP_USB_BAD=1; CP_USB_SCHEMA="$CP_USB_VALUE" ;;
                token) [ -z "$CP_USB_TOKEN" ] || CP_USB_BAD=1; CP_USB_TOKEN="$CP_USB_VALUE" ;;
                sample) [ -z "$CP_USB_SAMPLE" ] || CP_USB_BAD=1; CP_USB_SAMPLE="$CP_USB_VALUE" ;;
                usb_query_exit) [ -z "$CP_USB_RC" ] || CP_USB_BAD=1; CP_USB_RC="$CP_USB_VALUE" ;;
                commit) [ -z "$CP_USB_COMMIT" ] || CP_USB_BAD=1; CP_USB_COMMIT="$CP_USB_VALUE" ;;
                *) CP_USB_BAD=1 ;;
            esac
        done < "$CP_USB_RESULT_FILE"
        case "$CP_USB_RC" in ''|*[!0-9]*) CP_USB_BAD=1 ;; esac
        if [ "$CP_USB_BAD" -eq 0 ] && [ "$CP_USB_SCHEMA" = 1 ] && [ "$CP_USB_COMMIT" = 1 ] && \
            [ "$CP_USB_TOKEN" = "$CP_PROGRESS_TOKEN" ] && [ "$CP_USB_SAMPLE" = "$CP_SAMPLE" ]; then
            CP_USB_QUERY_EXIT="$CP_USB_RC"
        fi
    fi
    if [ "$CP_USB_QUERY_EXIT" = unknown ]; then
        CP_USB_UNKNOWN_COUNT=$((CP_USB_UNKNOWN_COUNT + 1))
    else
        CP_USB_RETURNED_COUNT=$((CP_USB_RETURNED_COUNT + 1))
        if [ "$CP_USB_QUERY_EXIT" -eq 0 ]; then CP_USB_ZERO_COUNT=$((CP_USB_ZERO_COUNT + 1))
        else CP_USB_FAILED_COUNT=$((CP_USB_FAILED_COUNT + 1)); fi
    fi
}
progress_finish() {
    CP_PROGRESS_PHASE=returned
    CP_PROGRESS_STAGE=end
    CP_PROGRESS_EXIT="$1"
    CP_PROGRESS_NATURAL_RETURN=1
    progress_publish
}
bounded() {
    CP_REMAINING=$((CP_LIMIT - SECONDS))
    if [ "$CP_REMAINING" -le 0 ]; then
        unknown "collection_budget_expired; skipped=$1"
        return 124
    fi
    CP_COMMAND_BUDGET=6
    [ "$CP_REMAINING" -ge 6 ] || CP_COMMAND_BUDGET="$CP_REMAINING"
    "$@" &
    CP_TASK_PID=$!
    (
        trap - EXIT
        CP_SLEEP_PID=
        trap '[ -z "$CP_SLEEP_PID" ] || kill -15 "$CP_SLEEP_PID" 2>/dev/null; exit 0' HUP INT TERM
        sleep "$CP_COMMAND_BUDGET" &
        CP_SLEEP_PID=$!
        wait "$CP_SLEEP_PID" 2>/dev/null
        CP_SLEEP_PID=
        kill -15 "$CP_TASK_PID" 2>/dev/null
        sleep 1 &
        CP_SLEEP_PID=$!
        wait "$CP_SLEEP_PID" 2>/dev/null
        CP_SLEEP_PID=
        kill -9 "$CP_TASK_PID" 2>/dev/null
    ) >/dev/null 2>&1 &
    CP_WATCHDOG_PID=$!
    wait "$CP_TASK_PID" 2>/dev/null
    CP_RESULT=$?
    CP_TASK_PID=
    kill -15 "$CP_WATCHDOG_PID" 2>/dev/null
    wait "$CP_WATCHDOG_PID" 2>/dev/null
    CP_WATCHDOG_PID=
    if [ "$1" = usb ]; then
        cp_printf 'usb_query_exit=%s\n' "$CP_RESULT"
        progress_usb_result "$CP_RESULT"
    fi
    if [ "$CP_RESULT" -ne 0 ]; then
        unknown "command_incomplete rc=$CP_RESULT utility=$1"
    fi
    return "$CP_RESULT"
}
progress_init "${2:-}"
# These filters use only pdksh builtins: no awk/head, no evaluation of input,
# and no pass-through compatibility shim. A producer's pipe is always drained.
# read obtains one complete line; lines longer than 4096 bytes are withheld
# before case conversion or field parsing. Printed fields have smaller caps.
clip_metadata_line() {
    typeset CP_CLIP_Q32 CP_CLIP_MASK
    CP_CLIP_Q32='????????????????????????????????'
    CP_CLIPPED="$1"
    [ "${#CP_CLIPPED}" -gt "$2" ] || return 0
    case "$2" in
        160) CP_CLIP_MASK="$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32" ;;
        200) CP_CLIP_MASK="$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32????????" ;;
        240) CP_CLIP_MASK="$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32????????????????" ;;
        320) CP_CLIP_MASK="$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32$CP_CLIP_Q32" ;;
        *) return 2 ;;
    esac
    CP_CLIPPED="${CP_CLIPPED%"${CP_CLIPPED#$CP_CLIP_MASK}"}"
}
trim_metadata_field() {
    # Space and literal tab only; preserve every other byte.
    CP_TRIMMED="${1#"${1%%[! 	]*}"}"
    CP_TRIMMED="${CP_TRIMMED%"${CP_TRIMMED##*[! 	]}"}"
}
sensitive_metadata_line() {
    typeset -l CP_SENSITIVE
    CP_SENSITIVE="$1"
    case "$CP_SENSITIVE" in
        *password*|*passphrase*|*private*key*|*certificate*|*pair*record*|*udid*|*serial*|*container*id*|*systembuid*|*hostid*|*imei*|*token*|*secret*|*psk*) return 0 ;;
        *) return 1 ;;
    esac
}
limited_metadata() {
    typeset CP_LINE CP_CLIPPED CP_LINES
    typeset -l CP_LOWER
    CP_LINES=0
    while IFS= read -r CP_LINE || [ -n "$CP_LINE" ]; do
        CP_LINES=$((CP_LINES + 1))
        [ "$CP_LINES" -le 200 ] || continue
        if [ "${#CP_LINE}" -gt 4096 ]; then unknown 'metadata_line_too_long; content_withheld'; continue; fi
        if sensitive_metadata_line "$CP_LINE"; then print -r -- '[redacted: sensitive metadata field]'
        else clip_metadata_line "$CP_LINE" 320; print -r -- "$CP_CLIPPED"; fi
    done
    [ "$CP_LINES" -le 200 ] || print -r -- '[metadata line cap reached]'
}
first_config_lines() {
    # Caller has resolved a fixed configuration allowlist. No device streams.
    typeset CP_LINE CP_LINES
    [ -f "$1" ] && [ ! -L "$1" ] || { unknown 'config_source_no_longer_regular'; return 1; }
    CP_LINES=0
    while IFS= read -r CP_LINE || [ -n "$CP_LINE" ]; do
        CP_LINES=$((CP_LINES + 1))
        if [ "${#CP_LINE}" -le 4096 ]; then print -r -- "$CP_LINE"
        else unknown 'metadata_line_too_long; content_withheld'; fi
        [ "$CP_LINES" -lt 200 ] || break
    done < "$1"
    [ "$CP_LINES" -lt 200 ] || print -r -- '[config line limit reached; remainder not read]'
}
identity_metadata() {
    typeset CP_LINE
    case "$1" in /dev/shmem/sw_trainname.txt|/dev/shmem/MUVersion.txt) ;; *) return 2 ;; esac
    [ -f "$1" ] && [ ! -L "$1" ] || { unknown 'identity_source_no_longer_regular'; return 1; }
    CP_LINE=
    IFS= read -r CP_LINE < "$1"
    if [ "${#CP_LINE}" -le 512 ] && [ -n "$CP_LINE" ]; then print -r -- "$CP_LINE"
    else unknown "identity_empty_or_line_too_long=$1"; fi
}
config_link_target() {
    typeset CP_LINE CP_LINK_FOUND
    CP_LINK_FOUND=0
    while IFS= read -r CP_LINE || [ -n "$CP_LINE" ]; do
        [ "${#CP_LINE}" -le 1024 ] || continue
        case "$CP_LINE" in *' -> '*)
            CP_LINK_FOUND=$((CP_LINK_FOUND + 1))
            [ "$CP_LINK_FOUND" -eq 1 ] && print -r -- "${CP_LINE#* -> }" ;;
        esac
    done
}
trusted_config_path() {
    # Fixed allowlist callers only. Normal firmware links point into mounted
    # IFS/EFS images; refuse arbitrary destinations and multi-hop links.
    if [ -f "$1" ] && [ ! -L "$1" ]; then cp_printf '%s\n' "$1"; return 0; fi
    [ -L "$1" ] && has ls || return 1
    CP_CONFIG_TARGET=$(bounded ls -ld "$1" 2>/dev/null | config_link_target)
    [ "${#CP_CONFIG_TARGET}" -le 512 ] || return 1
    case "$CP_CONFIG_TARGET" in /mnt/ifs-root/*|/mnt/efs-system/*) ;; *) return 1 ;; esac
    case "$CP_CONFIG_TARGET" in */../*|*/..|*/./*|*/.) return 1 ;; esac
    [ -f "$CP_CONFIG_TARGET" ] && [ ! -L "$CP_CONFIG_TARGET" ] || return 1
    cp_printf '%s\n' "$CP_CONFIG_TARGET"
}
usb_metadata() {
    typeset CP_LINE CP_CLIPPED CP_TRIMMED CP_KEY CP_VALUE CP_ADDR CP_N CP_DEVICES CP_ALLOW CP_REST CP_ID
    typeset -l CP_LOWER
    CP_N=0
    CP_DEVICES=0
    while IFS= read -r CP_LINE || [ -n "$CP_LINE" ]; do
        if [ "${#CP_LINE}" -gt 4096 ]; then
            CP_N=$((CP_N + 1)); [ "$CP_N" -gt 320 ] || unknown 'USB_metadata_line_too_long; content_withheld'; continue
        fi
        if sensitive_metadata_line "$CP_LINE"; then
            CP_N=$((CP_N + 1)); [ "$CP_N" -gt 320 ] || print -r -- '[redacted: USB device identity]'; continue
        fi
        case "$CP_LINE" in usb_query_exit=*)
            CP_VALUE="${CP_LINE#usb_query_exit=}"
            case "$CP_VALUE" in unknown) print -r -- 'usb_query_exit=unknown' ;;
                ''|*[!0-9]*) unknown 'USB_query_result_malformed; content_withheld' ;;
                *) if [ "${#CP_VALUE}" -le 3 ]; then cp_printf 'usb_query_exit=%s\n' "$CP_VALUE"
                   else unknown 'USB_query_result_malformed; content_withheld'; fi ;;
            esac
            continue ;;
            '[unknown:'*)
                CP_N=$((CP_N + 1)); [ "$CP_N" -gt 320 ] || { clip_metadata_line "$CP_LINE" 200; print -r -- "$CP_CLIPPED"; }; continue ;;
        esac
        CP_LOWER="$CP_LINE"
        trim_metadata_field "$CP_LOWER"
        CP_ALLOW=0
        case "$CP_TRIMMED" in
            'usb ('*|'control,'*) CP_ALLOW=1 ;;
            'usb '[0-9]*' ('*)
                CP_REST="${CP_TRIMMED#usb }"; CP_ID="${CP_REST%% *}"
                case "$CP_ID" in ''|*[!0-9]*) ;; *) CP_ALLOW=1 ;; esac ;;
            *:*)
                CP_KEY="${CP_TRIMMED%%:*}"
                CP_VALUE="${CP_TRIMMED#*:}"
                trim_metadata_field "$CP_KEY"; CP_KEY="$CP_TRIMMED"
                case "$CP_KEY" in
                    'device address')
                        trim_metadata_field "$CP_VALUE"; CP_VALUE="$CP_TRIMMED"
                        # Count exact address records, never upstream addresses,
                        # error text, headings containing the words, or junk.
                        case "$CP_VALUE" in ''|*[!0-9]*) ;; *)
                            CP_ADDR="$CP_VALUE"
                            while [ "${#CP_ADDR}" -gt 1 ]; do case "$CP_ADDR" in 0*) CP_ADDR="${CP_ADDR#0}" ;; *) break ;; esac; done
                            if [ "${#CP_VALUE}" -le 3 ] && [ "$CP_ADDR" -le 127 ]; then CP_DEVICES=$((CP_DEVICES + 1)); CP_ALLOW=1; fi ;;
                        esac ;;
                    'upstream host controller'|'upstream device address'|'upstream port'|'upstream port speed'|port|'port speed'|vendor|product|'device release'|'usb spec release'|class|subclass|protocol|'max packetsize0'|languages|configuration|configurations|attributes|'max power'|interface|interfaces|endpoint|endpoints|'max packet size'|interval) CP_ALLOW=1 ;;
                esac ;;
        esac
        [ "$CP_ALLOW" -eq 1 ] || continue
        CP_N=$((CP_N + 1))
        [ "$CP_N" -le 320 ] || continue
        clip_metadata_line "$CP_LINE" 200; print -r -- "$CP_CLIPPED"
    done
    cp_printf 'usb_device_addresses_observed=%s\n' "$CP_DEVICES"
    [ "$CP_N" -le 320 ] || print -r -- '[USB metadata line cap reached]'
}
process_metadata() {
    typeset CP_LINE CP_CLIPPED CP_TRIMMED CP_PID CP_SEEN CP_N CP_FIRST
    CP_N=0; CP_FIRST=1; CP_SEEN='|'
    while IFS= read -r CP_LINE || [ -n "$CP_LINE" ]; do
        if [ "${#CP_LINE}" -gt 4096 ]; then CP_N=$((CP_N + 1)); [ "$CP_N" -gt 160 ] || unknown 'process_metadata_line_too_long; content_withheld'; continue; fi
        if sensitive_metadata_line "$CP_LINE"; then CP_N=$((CP_N + 1)); [ "$CP_N" -gt 160 ] || print -r -- '[redacted: sensitive metadata field]'; continue; fi
        case "$CP_LINE" in '[unknown:'*) CP_N=$((CP_N + 1)); [ "$CP_N" -gt 160 ] || { clip_metadata_line "$CP_LINE" 160; print -r -- "$CP_CLIPPED"; }; continue ;; esac
        if [ "$CP_FIRST" -eq 1 ]; then CP_FIRST=0; clip_metadata_line "$CP_LINE" 160; print -r -- "$CP_CLIPPED"; continue; fi
        trim_metadata_field "$CP_LINE"; CP_PID="${CP_TRIMMED%%[ 	]*}"
        case "$CP_PID" in ''|*[!0-9]*) continue ;; esac
        [ "${#CP_PID}" -le 10 ] || continue
        case "$CP_SEEN" in *"|$CP_PID|"*) continue ;; esac
        CP_N=$((CP_N + 1)); [ "$CP_N" -le 160 ] || continue
        CP_SEEN="$CP_SEEN$CP_PID|"
        clip_metadata_line "$CP_LINE" 160; print -r -- "$CP_CLIPPED"
    done
    [ "$CP_N" -le 160 ] || print -r -- '[process line cap reached]'
}
network_metadata() {
    typeset CP_LINE CP_CLIPPED CP_TRIMMED CP_KEY CP_N
    typeset -l CP_LOWER
    CP_N=0
    while IFS= read -r CP_LINE || [ -n "$CP_LINE" ]; do
        if [ "${#CP_LINE}" -gt 4096 ]; then CP_N=$((CP_N + 1)); [ "$CP_N" -gt 100 ] || unknown 'network_metadata_line_too_long; content_withheld'; continue; fi
        if sensitive_metadata_line "$CP_LINE"; then CP_N=$((CP_N + 1)); [ "$CP_N" -gt 100 ] || print -r -- '[redacted: sensitive metadata field]'; continue; fi
        CP_LOWER="$CP_LINE"
        case "$CP_LOWER" in *ether*|*lladdr*|*hwaddr*|*'link address'*|*ssid*|*bssid*) continue ;; esac
        CP_N=$((CP_N + 1)); [ "$CP_N" -le 100 ] || continue
        trim_metadata_field "$CP_LOWER"; CP_KEY="${CP_TRIMMED%%:*}"
        trim_metadata_field "$CP_KEY"
        if [ "$CP_TRIMMED" = address ]; then print -r -- '[redacted: link-layer address]'
        else clip_metadata_line "$CP_LINE" 200; print -r -- "$CP_CLIPPED"; fi
    done
    [ "$CP_N" -le 100 ] || print -r -- '[network line cap reached]'
}
library_metadata() {
    typeset CP_LINE CP_CLIPPED CP_N
    typeset -l CP_LOWER
    CP_N=0
    while IFS= read -r CP_LINE || [ -n "$CP_LINE" ]; do
        if [ "${#CP_LINE}" -gt 4096 ]; then CP_N=$((CP_N + 1)); [ "$CP_N" -gt 160 ] || unknown 'library_metadata_line_too_long; content_withheld'; continue; fi
        if sensitive_metadata_line "$CP_LINE"; then CP_N=$((CP_N + 1)); [ "$CP_N" -gt 160 ] || print -r -- '[redacted: sensitive metadata field]'; continue; fi
        case "$CP_LINE" in '[unknown:'*) CP_N=$((CP_N + 1)); [ "$CP_N" -gt 160 ] || { clip_metadata_line "$CP_LINE" 240; print -r -- "$CP_CLIPPED"; }; continue ;; esac
        CP_LOWER="$CP_LINE"
        case "$CP_LOWER" in *usb*|*h264*|*avc*|*video*|*codec*|*audio*|*asound*|*photon*|*screen*|*gf*|*gem*|*nvidia*|*nvom*|*ncm*|*cdc*|*ipod*|*ami*|*media*|*display*|*layer*|*input*|*dsi*|*crypto*|*ssl*|*ldqnx*|*libc.*)
            CP_N=$((CP_N + 1)); [ "$CP_N" -le 160 ] || continue
            clip_metadata_line "$CP_LINE" 240; print -r -- "$CP_CLIPPED" ;;
        esac
    done
    [ "$CP_N" -le 160 ] || print -r -- '[library line cap reached]'
}
api_string_metadata() {
    typeset CP_LINE CP_CLIPPED CP_N
    CP_N=0
    while IFS= read -r CP_LINE || [ -n "$CP_LINE" ]; do
        if [ "${#CP_LINE}" -gt 4096 ]; then CP_N=$((CP_N + 1)); [ "$CP_N" -gt 120 ] || unknown 'API_string_line_too_long; content_withheld'; continue; fi
        if sensitive_metadata_line "$CP_LINE"; then CP_N=$((CP_N + 1)); [ "$CP_N" -gt 120 ] || print -r -- '[redacted: sensitive metadata field]'; continue; fi
        case "$CP_LINE" in '[unknown:'*) CP_N=$((CP_N + 1)); [ "$CP_N" -gt 120 ] || { clip_metadata_line "$CP_LINE" 160; print -r -- "$CP_CLIPPED"; }; continue ;; esac
        case "$CP_LINE" in ''|[!A-Za-z_]*|*[!A-Za-z0-9_]*) continue ;; esac
        case "$CP_LINE" in gf_*|snd_pcm*|snd_mixer*|usbd_*|media_*|aoi_*|Mm*|Mmf*|nv_*|Nv*|MPEGH264*|H264*)
            CP_N=$((CP_N + 1)); [ "$CP_N" -le 120 ] || continue
            clip_metadata_line "$CP_LINE" 160; print -r -- "$CP_CLIPPED" ;;
        esac
    done
    [ "$CP_N" -le 120 ] || print -r -- '[API string line cap reached]'
}
processes() {
    inventory_stage_begin processes
    section processes
    if has pidin; then
        # %A arguments and %E environment are intentionally absent.
        bounded pidin -F '%a %b %N %J' 2>&1 | process_metadata
    else
        unknown 'pidin unavailable; process metadata withheld'
    fi
    inventory_stage_end processes
}
nodes() {
    inventory_stage_begin device_names
    section device_names
    bounded ls -ld /dev/io-usb* /dev/usb* /dev/*audio* /dev/*pcm* \
        /dev/*screen* /dev/*input* /dev/*video* /dev/layermanager \
        /dev/io-display /dev/io-media* /dev/snd /dev/snd/* 2>&1
    inventory_stage_end device_names
    inventory_stage_begin nvidia_dsp_nodes_only
    section nvidia_dsp_nodes_only
    bounded ls -ld /dev/nvdsprm* /dev/nvminirm* /dev/dspipc \
        /dev/shmem/nvdsprm.finished /dev/shmem/nvminirm.finished 2>&1
    inventory_stage_end nvidia_dsp_nodes_only
}
network() {
    inventory_stage_begin network
    section network
    if has ifconfig; then
        bounded ifconfig -a 2>&1 | network_metadata
    else
        unknown 'ifconfig unavailable; network metadata withheld'
    fi
    if has netstat; then bounded netstat -rn 2>&1 | network_metadata
    else unknown 'netstat unavailable; route metadata withheld'; fi
    inventory_stage_end network
}
if [ "$CP_MODE" = '--observe' ]; then
    section observation_policy
    cp_printf 'window_seconds=180\nnominal_interval_seconds=10\n'
    cp_printf 'timestamp_basis=QNX_clock_unverified\nelapsed_basis=pdksh_wall_clock_delta\n'
    cp_printf 'USB serials withheld; no pairing/authentication or media session attempted.\n'
    CP_SAMPLE=1
    while [ "$SECONDS" -lt "$CP_LIMIT" ]; do
        section "sample $CP_SAMPLE"
        cp_printf 'elapsed_seconds=%s\n' "$SECONDS"
        if has date; then bounded date -u '+timestamp=%Y-%m-%dT%H:%M:%SZ' 2>&1
        else cp_printf 'timestamp=unknown\n'; fi
        CP_PROGRESS_PHASE=query_usb
        CP_PROGRESS_STAGE=usb_descriptors
        CP_USB_QUERY_EXIT=unknown
        CP_USB_QUERY_SAMPLE="$CP_SAMPLE"
        progress_publish
        section usb_descriptors
        if has usb; then
            # Three v flags expose interfaces/endpoints; no -V private descriptors.
            bounded usb -vvv 2>&1 | usb_metadata
            progress_read_usb_result
        else
            unknown 'usb unavailable; USB descriptors withheld'
            cp_printf 'usb_query_exit=unknown\nusb_device_addresses_observed=unknown\n'
            CP_USB_QUERY_EXIT=unknown
            CP_USB_UNKNOWN_COUNT=$((CP_USB_UNKNOWN_COUNT + 1))
        fi
        CP_PROGRESS_PHASE=sampling
        CP_PROGRESS_STAGE=sample_metadata
        progress_publish
        processes
        nodes
        network
        CP_COMPLETED_SAMPLES=$((CP_COMPLETED_SAMPLES + 1))
        [ -z "$CP_PROGRESS_DIR" ] || cp_printf 'progress_sample_returned=%s\n' "$CP_SAMPLE"
        progress_publish
        CP_SAMPLE=$((CP_SAMPLE + 1))
        CP_PAUSE=$((CP_LIMIT - SECONDS))
        [ "$CP_PAUSE" -gt 0 ] || break
        [ "$CP_PAUSE" -le 10 ] || CP_PAUSE=10
        CP_PROGRESS_PHASE=waiting
        CP_PROGRESS_STAGE=sampling_interval
        progress_publish
        sleep "$CP_PAUSE" &
        CP_TASK_PID=$!
        wait "$CP_TASK_PID" 2>/dev/null
        CP_TASK_PID=
    done
    section observation_end
    cp_printf 'elapsed_seconds=%s\ncompleted_samples=%s\n' "$SECONDS" "$CP_COMPLETED_SAMPLES"
    cp_printf 'Descriptor changes are observations, not proof of AMI ownership or CarPlay support.\n'
    progress_finish 0
    exit 0
fi
inventory_stage_begin system
section system
bounded uname -a 2>&1
if has date; then bounded date -u '+timestamp=%Y-%m-%dT%H:%M:%SZ' 2>&1; fi
if has pidin; then bounded pidin info 2>&1; fi
for identity in /dev/shmem/sw_trainname.txt /dev/shmem/MUVersion.txt; do
    if [ -f "$identity" ] && [ ! -L "$identity" ]; then bounded identity_metadata "$identity" 2>&1
    else unknown "identity_absent_or_symlink=$identity"; fi
done
inventory_stage_end system
inventory_stage_begin available_diagnostic_tools
section available_diagnostic_tools
for utility in print printf rmdir mkdir rm mv sleep sync cat cmp chmod head ls uname usb pidin ifconfig netstat nicinfo mount df file strings od cksum awk date ksh; do
    command -v "$utility" 2>/dev/null || unknown "unavailable_utility=$utility"
done
cp_printf 'metadata_filters=pdksh_builtin_v1\nhead_dependency=none\nawk_dependency=none\n'
inventory_stage_end available_diagnostic_tools
processes
nodes
inventory_stage_begin all_device_names_only
section all_device_names_only
bounded ls -la /dev 2>&1
inventory_stage_end all_device_names_only
inventory_stage_begin usb_descriptors
section usb_descriptors
if [ "$CP_MODE" = inventory ]; then
    cp_printf 'Collected by concurrent observation.txt to avoid parallel USB enumeration.\n'
elif has usb; then
    bounded usb -vvv 2>&1 | usb_metadata
else
    unknown 'usb unavailable; USB descriptors withheld'
    cp_printf 'usb_query_exit=unknown\nusb_device_addresses_observed=unknown\n'
fi
inventory_stage_end usb_descriptors
network
inventory_stage_begin mounted_storage
section mounted_storage
bounded mount 2>&1
bounded df 2>&1
inventory_stage_end mounted_storage
inventory_stage_begin candidate_library_names
section candidate_library_names
for directory in /lib /lib/dll /usr/lib /usr/lib/dll /opt/lib /proc/boot \
    /usr/photon/lib /mnt/efs-system/lib /mnt/efs-system/usr/lib; do
    if [ -d "$directory" ]; then
        cp_printf 'directory=%s\n' "$directory"
        bounded ls -l "$directory" 2>&1 | library_metadata
    else
        unknown "candidate_library_directory_absent=$directory"
    fi
done
inventory_stage_end candidate_library_names
inventory_stage_begin selected_driver_configs
section selected_driver_configs
# Explicit allowlist. No ipod.cfg content, WLAN/BT/lockdown/persistence/keys.
for config in /etc/iom.cfg /etc/mmelauncher.cfg /etc/mme.conf \
    /etc/io-display.cfg /etc/graphics.conf /etc/videoctrl-Audi3G.cfg \
    /etc/system/config/display_single.conf /etc/system/config/display_dual.conf \
    /etc/audio.cfg /etc/io-audio.cfg /etc/usb.cfg /etc/input.cfg \
    /etc/pci-3g_9411.cfg /etc/umass-enum_def.cfg /etc/umass-enum_hub.cfg \
    /etc/layermanager_nvidia.cfg /etc/layermanagerV2_nvidia.cfg /etc/layermanagerV2_nvidia_single.cfg \
    /etc/layermanagerV2_nvidia_dual.cfg; do
    CP_CONFIG_SOURCE=$(trusted_config_path "$config")
    if [ -n "$CP_CONFIG_SOURCE" ]; then
        cp_printf '\nconfig=%s\n' "$config"
        cp_printf 'resolved_config=%s\n' "$CP_CONFIG_SOURCE"
        bounded first_config_lines "$CP_CONFIG_SOURCE" 2>&1 | limited_metadata
    else
        unknown "config_absent_or_untrusted_symlink=$config"
    fi
done
inventory_stage_end selected_driver_configs
inventory_stage_begin ami_config_names_only
section ami_config_names_only
bounded ls -ld /etc/ipod.cfg /etc/*audi.ipod /lib/dll/iofs-usb-ipod.so \
    /usr/lib/dll/iofs-usb-ipod.so /lib/dll/deva-ctrl-ipod.so \
    /usr/lib/dll/deva-ctrl-ipod.so 2>&1
inventory_stage_end ami_config_names_only
inventory_stage_begin nvidia_dsp_image_identity
section nvidia_dsp_image_identity
# Metadata of a data image; never execute or reload it.
if [ -f /etc/NvDspCodecs.elf ] && [ ! -L /etc/NvDspCodecs.elf ]; then
    bounded ls -l /etc/NvDspCodecs.elf 2>&1
    if has cksum; then bounded cksum /etc/NvDspCodecs.elf 2>&1
    else unknown 'DSP_image_cksum_utility_unavailable'; fi
    if has file; then bounded file /etc/NvDspCodecs.elf 2>&1
    else unknown 'DSP_image_file_utility_unavailable'; fi
else
    unknown 'DSP_image_absent_or_symlink; no_hardware_conclusion'
fi
inventory_stage_end nvidia_dsp_image_identity
inventory_stage_begin selected_binary_metadata
section selected_binary_metadata
for utility in file od strings; do
    has "$utility" || unknown "binary_metadata_utility_unavailable=$utility"
done
for binary in /usr/lib/ldqnx.so.2 /proc/boot/libc.so.2 \
    /usr/lib/libgf.so.1 /usr/lib/libasound.so.2 /usr/lib/libusbdi.so.2 \
    /lib/libusbdi.so.2 /mnt/efs-system/lib/libusbdi.so.2 \
    /usr/lib/libmmedia.so.1 /usr/lib/libmmfilter.so.1 /usr/lib/libaoi.so.1 \
    /lib/dll/mme-imgprc-gf.so /usr/lib/dll/mme-imgprc-gf.so \
    /sbin/io-usb /sbin/io-media-nvidia /sbin/io-audio /usr/sbin/io-audio \
    /usr/sbin/io-display /usr/bin/dev-videoctrl \
    /usr/bin/devg-NVMiniRM /usr/bin/layermanagerV2; do
    if [ -f "$binary" ]; then
        cp_printf '\nbinary=%s\n' "$binary"
        bounded ls -l "$binary" 2>&1
        if has file; then bounded file "$binary" 2>&1; fi
        if has od; then bounded od -An -tx1 -N64 "$binary" 2>&1; fi
        # Known API-name strings, not an export table or API-contract check.
        if has strings; then
            bounded strings "$binary" 2>&1 | api_string_metadata
        fi
    else
        unknown "candidate_binary_absent=$binary"
    fi
done
inventory_stage_end selected_binary_metadata
section end
cp_printf 'elapsed_seconds=%s\n' "$SECONDS"
cp_printf 'elapsed_basis=pdksh_wall_clock_delta\n'
cp_printf 'Inventory only: USB ownership, GPU calls, H264 decode, audio routing, input subscription and MFi acceptance remain untested.\n'
cp_printf 'Missing tools, timeouts and absent candidate paths mean unknown, not missing hardware support.\n'
if [ "$SECONDS" -ge "$CP_LIMIT" ]; then progress_finish 124; exit 124; fi
progress_finish 0
exit 0
