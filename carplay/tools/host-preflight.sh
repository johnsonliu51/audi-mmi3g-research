#!/bin/sh
# Read-only host inventory for the QNX SH4 cross-build setup.
set -u

printf '%s\n' 'MMI3G native probe host preflight'
uname -a
printf '\n%s\n' 'Candidate tools:'

for tool in qcc q++ nto-sh-gcc ntosh-gcc ntosh-objdump nto-readelf sh4-linux-gnu-gcc sh4-linux-gnu-objdump; do
    if command -v "$tool" >/dev/null 2>&1; then
        printf '  %-24s %s\n' "$tool" "$(command -v "$tool")"
    else
        printf '  %-24s %s\n' "$tool" 'not found'
    fi
done

printf '\n%s\n' 'QNX environment:'
for variable in QNX_HOST QNX_TARGET QCC_CONF_PATH; do
    value=$(printenv "$variable" 2>/dev/null || true)
    if [ -n "$value" ]; then
        printf '  %-24s %s\n' "$variable" "$value"
    else
        printf '  %-24s %s\n' "$variable" 'unset'
    fi
done

if command -v qcc >/dev/null 2>&1; then
    printf '\n%s\n' 'SH4 target configuration (if installed):'
    workdir=$(mktemp -d /tmp/mmi3g-qnx-sh4-preflight.XXXXXX) || exit 1
    trap 'rm -rf "$workdir"' 0 HUP INT TERM
    qcc -Vgcc_ntoshle -v -c -x c /dev/null -o "$workdir/preflight.o" 2>&1 || true
else
    printf '\n%s\n' 'No QNX qcc found; a Linux SH4 compiler alone cannot establish QNX ABI compatibility.'
fi
