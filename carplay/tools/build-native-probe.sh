#!/bin/sh
# Build the tiny probe with the QNX SH4 compiler; writes only under /private/tmp.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
QCC=${QCC:-qcc}
QNX_VARIANT=${QNX_VARIANT:-gcc_ntoshle}
BUILD_DIR=${BUILD_DIR:-/private/tmp/mmi3g-native-carplay-build}

if ! command -v "$QCC" >/dev/null 2>&1; then
    printf 'QNX compiler not found: %s\n' "$QCC" >&2
    exit 127
fi

mkdir -p "$BUILD_DIR"
"$QCC" -V"$QNX_VARIANT" -Wall -Wextra -O2 \
    -o "$BUILD_DIR/mmi3g-native-probe" \
    "$ROOT/src/native_probe.c"

printf 'Built candidate: %s\n' "$BUILD_DIR/mmi3g-native-probe"
file "$BUILD_DIR/mmi3g-native-probe"
