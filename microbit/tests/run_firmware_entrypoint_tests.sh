#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="$(mktemp -d)"
STUB_DIR="${WORK_DIR}/bin"
CMAKE_LOG="${WORK_DIR}/cmake.log"
PYOCD_LOG="${WORK_DIR}/pyocd.log"

cleanup() {
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

mkdir -p "${STUB_DIR}"

cat > "${STUB_DIR}/cmake" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

printf '%s\n' "$*" >> "${CMAKE_LOG:?CMAKE_LOG is required}"
EOF

cat > "${STUB_DIR}/pyocd" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

printf '%s\n' "$*" >> "${PYOCD_LOG:?PYOCD_LOG is required}"
EOF

chmod +x "${STUB_DIR}/cmake" "${STUB_DIR}/pyocd"

require_line() {
    local expected="$1"
    local file="$2"

    if ! grep -Fqx -- "${expected}" "${file}"; then
        printf 'missing expected command in %s: %s\n' "${file}" "${expected}" >&2
        return 1
    fi
}

run_build_profile() {
    local target="$1"
    local phase1_target="$2"
    local node_mode="$3"
    local feature_level="$4"

    : > "${CMAKE_LOG}"
    PATH="${STUB_DIR}:${PATH}" CMAKE_LOG="${CMAKE_LOG}" \
        "${MICROBIT_ROOT}/build.sh" "${target}" > /dev/null

    require_line \
        "-S ${MICROBIT_ROOT} -B ${MICROBIT_ROOT}/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=${MICROBIT_ROOT}/cmake/arm-none-eabi-gcc.cmake -DTRON_PHASE1_TARGET=${phase1_target} -DTRON_NODE_MODE=${node_mode} -DTAVRN_FEATURE_LEVEL=${feature_level}" \
        "${CMAKE_LOG}"
    require_line "--build ${MICROBIT_ROOT}/build --target ${target} --parallel" \
        "${CMAKE_LOG}"
}

run_build_profile test_firmware LEGACY LEGACY_FLOOD ""
run_build_profile ble_observer LEGACY LEGACY_FLOOD ""
run_build_profile ble_beacon LEGACY LEGACY_FLOOD ""
run_build_profile ble_mesh_node LEGACY LEGACY_FLOOD ""
run_build_profile wearable_app LEGACY LEGACY_FLOOD ""
run_build_profile wearable_test_injector LEGACY LEGACY_FLOOD ""
run_build_profile ble_link_v2_testbed LINK NOT_APPLICABLE ""
run_build_profile tavrn_routed_node ROUTED TAVRN_ROUTED AODV_ONLY

: > "${CMAKE_LOG}"
: > "${PYOCD_LOG}"
PATH="${STUB_DIR}:${PATH}" CMAKE_LOG="${CMAKE_LOG}" PYOCD_LOG="${PYOCD_LOG}" \
    "${MICROBIT_ROOT}/flash.sh" ble_link_v2_testbed --uid test-probe > /dev/null

require_line \
    "-S ${MICROBIT_ROOT} -B ${MICROBIT_ROOT}/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=${MICROBIT_ROOT}/cmake/arm-none-eabi-gcc.cmake -DTRON_PHASE1_TARGET=LINK -DTRON_NODE_MODE=NOT_APPLICABLE -DTAVRN_FEATURE_LEVEL=" \
    "${CMAKE_LOG}"
require_line "--build ${MICROBIT_ROOT}/build --target ble_link_v2_testbed --parallel" \
    "${CMAKE_LOG}"
require_line "erase --mass --uid test-probe" "${PYOCD_LOG}"
require_line "load ${MICROBIT_ROOT}/build/firmware/ble_link_v2_testbed/ble_link_v2_testbed.elf --uid test-probe" \
    "${PYOCD_LOG}"
require_line "reset --uid test-probe" "${PYOCD_LOG}"

printf '%s\n' 'firmware entrypoint profile and flash forwarding tests passed'
