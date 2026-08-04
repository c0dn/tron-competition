#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
CC_BIN="${CC:-cc}"
MODE="production"

cleanup() {
    rm -rf "${BUILD_DIR}"
}
trap cleanup EXIT

if [[ $# -gt 1 || ( $# -eq 1 && "$1" != "--red" ) ]]; then
    printf 'usage: %s [--red]\n' "$0" >&2
    exit 2
fi
if [[ $# -eq 1 ]]; then
    MODE="red"
fi

COMMON_FLAGS=(
    -std=c99
    -Wall
    -Wextra
    -Werror
    -DBLE_RADIO_HOST_TEST
    -I"${MICROBIT_ROOT}/app/protocol"
    -I"${MICROBIT_ROOT}/app/drivers"
    -I"${MICROBIT_ROOT}/tests/protocol/support"
)

if [[ "${MODE}" == "red" ]]; then
    WIRE_SOURCES=(
        "${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_wire_v2_red_backend.c"
    )
    LINK_SOURCES=(
        "${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_link_v2_red_backend.c"
    )
    RADIO_SOURCES=(
        "${MICROBIT_ROOT}/tests/protocol/red_support/ble_mesh_radio_scheduler_queue_red_backend.c"
    )
else
    WIRE_SOURCES=(
        "${MICROBIT_ROOT}/app/protocol/tavrn_wire_v2.c"
    )
    LINK_SOURCES=(
        "${MICROBIT_ROOT}/app/protocol/tavrn_link_v2.c"
        "${MICROBIT_ROOT}/app/protocol/tavrn_wire_v2.c"
        "${MICROBIT_ROOT}/app/protocol/ble_mesh_tx_queue.c"
        "${MICROBIT_ROOT}/tests/protocol/support/ble_mesh_scheduler_port.c"
    )
    RADIO_SOURCES=(
        "${MICROBIT_ROOT}/app/protocol/ble_mesh_tx_queue.c"
        "${MICROBIT_ROOT}/app/protocol/ble_mesh_scheduler.c"
        "${MICROBIT_ROOT}/app/drivers/ble_radio.c"
        "${MICROBIT_ROOT}/tests/protocol/support/ble_radio_host_shim.c"
    )
    for source in "${WIRE_SOURCES[@]}" "${LINK_SOURCES[@]}" "${RADIO_SOURCES[@]}"; do
        if [[ ! -f "${source}" ]]; then
            printf 'production mode unavailable: expected Phase 1 source %s\n' \
                "${source#"${MICROBIT_ROOT}/"}" >&2
            exit 2
        fi
    done
fi

"${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/protocol/test_tavrn_wire_v2.c" \
    "${WIRE_SOURCES[@]}" \
    -o "${BUILD_DIR}/test_tavrn_wire_v2"

"${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/protocol/test_tavrn_link_v2.c" \
    "${LINK_SOURCES[@]}" \
    -o "${BUILD_DIR}/test_tavrn_link_v2"

"${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/protocol/test_tavrn_radio_scheduler_v2.c" \
    "${RADIO_SOURCES[@]}" \
    -o "${BUILD_DIR}/test_tavrn_radio_scheduler_v2"

set +e
"${BUILD_DIR}/test_tavrn_wire_v2"
wire_status=$?
"${BUILD_DIR}/test_tavrn_link_v2"
link_status=$?
"${BUILD_DIR}/test_tavrn_radio_scheduler_v2"
radio_status=$?
set -e

if [[ ${wire_status} -ne 0 || ${link_status} -ne 0 || ${radio_status} -ne 0 ]]; then
    exit 1
fi
