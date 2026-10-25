#!/bin/sh
# Build install and exact-removal SD packages from the pinned Toolkit builder.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TOOLKIT_DIR=${TOOLKIT_DIR:-/private/tmp/MMI3G-Toolkit-native-carplay-20260929}
TOOLKIT_COMMIT=e9ca3469835e3e1c78dabfb5cbe5676d2b752500
BUILD_DIR=${BUILD_DIR:-/private/tmp/mmi3g-native-carplay-build}
OUTPUT_ROOT=${OUTPUT_ROOT:-/private/tmp/mmi3g-native-probe-sd-package}
PROBE_BINARY=${PROBE_BINARY:-${BUILD_DIR}/mmi3g-native-probe}

if [ ! -d "${TOOLKIT_DIR}/.git" ]; then
    printf 'Pinned MMI3G-Toolkit checkout not found: %s\n' "${TOOLKIT_DIR}" >&2
    exit 1
fi
ACTUAL_COMMIT=$(git -C "${TOOLKIT_DIR}" rev-parse HEAD)
if [ "${ACTUAL_COMMIT}" != "${TOOLKIT_COMMIT}" ]; then
    printf 'Toolkit commit mismatch: expected %s, got %s\n' \
        "${TOOLKIT_COMMIT}" "${ACTUAL_COMMIT}" >&2
    exit 1
fi
if [ ! -f "${PROBE_BINARY}" ]; then
    printf 'Compiled probe not found: %s\n' "${PROBE_BINARY}" >&2
    printf 'Build it first with tools/build-native-probe.sh in the licensed QNX environment.\n' >&2
    exit 1
fi
if [ -e "${OUTPUT_ROOT}" ]; then
    printf 'Output path already exists; choose a new OUTPUT_ROOT: %s\n' "${OUTPUT_ROOT}" >&2
    exit 1
fi

TMPROOT=$(mktemp -d /private/tmp/mmi3g-native-probe-package.XXXXXX)
cleanup() {
    case "${TMPROOT}" in
        /private/tmp/mmi3g-native-probe-package.*) rm -rf "${TMPROOT}" ;;
        *) printf 'Refusing unexpected temporary cleanup path: %s\n' "${TMPROOT}" >&2 ;;
    esac
}
trap cleanup 0
trap 'exit 1' HUP INT TERM

TOOLKIT_STAGE="${TMPROOT}/toolkit"
MODULE_STAGE="${TOOLKIT_STAGE}/modules/native-probe"
mkdir -p "${TOOLKIT_STAGE}"
git -C "${TOOLKIT_DIR}" archive "${TOOLKIT_COMMIT}" | tar -xf - -C "${TOOLKIT_STAGE}"
mkdir -p "${MODULE_STAGE}/engdefs" "${MODULE_STAGE}/scripts" "${MODULE_STAGE}/packaging"
cp "${ROOT}/sd-module/module.json" "${MODULE_STAGE}/module.json"
cp "${ROOT}/src/native_probe.esd" "${MODULE_STAGE}/engdefs/NativeProbe.esd"
cp "${ROOT}/src/native_probe.sh" "${MODULE_STAGE}/scripts/native_probe.sh"
cp "${ROOT}/sd-module/scripts/native_probe_install.sh" "${MODULE_STAGE}/scripts/native_probe_install.sh"
cp "${PROBE_BINARY}" "${MODULE_STAGE}/mmi3g-native-probe"

mkdir -p "${OUTPUT_ROOT}"
python3 "${TOOLKIT_STAGE}/builder/build_sd.py" \
    -m native-probe --target-platform MMI3G+ -o "${OUTPUT_ROOT}/install"
rm -f "${OUTPUT_ROOT}/install/bin/showScreen" \
      "${OUTPUT_ROOT}/install/lib/running.png" \
      "${OUTPUT_ROOT}/install/lib/done.png"
cp "${ROOT}/sd-module/packaging/run-install.sh" "${OUTPUT_ROOT}/install/run.sh"
cp "${ROOT}/sd-module/scripts/native_probe_remove.sh" "${OUTPUT_ROOT}/install/uninstall.sh"

python3 "${TOOLKIT_STAGE}/builder/build_sd.py" \
    --uninstall -o "${OUTPUT_ROOT}/uninstall"
cp "${ROOT}/sd-module/packaging/run-remove.sh" "${OUTPUT_ROOT}/uninstall/run.sh"
cp "${ROOT}/sd-module/scripts/native_probe_remove.sh" "${OUTPUT_ROOT}/uninstall/uninstall.sh"

cat >"${OUTPUT_ROOT}/README.txt" <<'EOF'
MMI3G Native Probe SD package

install/: installs one uniquely named binary, wrapper, and GEM page after
collision checks. Review var/native-probe-install.log on the SD card.

uninstall/: removes only those exact probe paths. Review
var/native-probe-remove.log on the SD card.

The generic MMI3G-Toolkit global uninstaller is not used. This package and its
scripts have not yet been tested on the target car.
EOF

printf 'Created SD packages under %s\n' "${OUTPUT_ROOT}"
printf 'Install SD:   %s/install\n' "${OUTPUT_ROOT}"
printf 'Uninstall SD: %s/uninstall\n' "${OUTPUT_ROOT}"
