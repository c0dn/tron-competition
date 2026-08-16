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

if [[ $# -gt 1 || ( $# -eq 1 && "$1" != "--red" &&
                    "$1" != "--turnaround-red" ) ]]; then
    printf 'usage: %s [--red|--turnaround-red]\n' "$0" >&2
    exit 2
fi
if [[ $# -eq 1 ]]; then
    MODE="${1#--}"
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
LINK_OBJECTS=()

compile_or_exit() {
    if ! "$@"; then
        if [[ "${MODE}" == "turnaround-red" ]]; then
            exit 2
        fi
        exit 1
    fi
}

if [[ "${MODE}" == "red" ]]; then
    WIRE_SOURCES=(
        "${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_wire_v2_red_backend.c"
    )
    LINK_SOURCES=(
        "${MICROBIT_ROOT}/app/protocol/tron_timer_config.c"
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
        "${MICROBIT_ROOT}/app/protocol/tron_timer_config.c"
        "${MICROBIT_ROOT}/app/protocol/tavrn_wire_v2.c"
        "${MICROBIT_ROOT}/app/protocol/ble_mesh_tx_queue.c"
        "${MICROBIT_ROOT}/tests/protocol/support/ble_mesh_scheduler_port.c"
    )
    if [[ "${MODE}" == "turnaround-red" ]]; then
        compile_or_exit "${CC_BIN}" "${COMMON_FLAGS[@]}" \
            -DTAVRN_LINK_V2_HOST_TEST_IMMEDIATE_HACK \
            -c "${MICROBIT_ROOT}/app/protocol/tavrn_link_v2.c" \
            -o "${BUILD_DIR}/tavrn_link_v2_turnaround_red.o"
        LINK_OBJECTS=("${BUILD_DIR}/tavrn_link_v2_turnaround_red.o")
    else
        LINK_SOURCES+=("${MICROBIT_ROOT}/app/protocol/tavrn_link_v2.c")
        LINK_OBJECTS=()
    fi
    RADIO_SOURCES=(
        "${MICROBIT_ROOT}/app/protocol/tron_timer_config.c"
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

compile_or_exit "${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/protocol/test_tavrn_wire_v2.c" \
    "${WIRE_SOURCES[@]}" \
    -o "${BUILD_DIR}/test_tavrn_wire_v2"

compile_or_exit "${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/protocol/test_tavrn_link_v2.c" \
    "${LINK_SOURCES[@]}" "${LINK_OBJECTS[@]}" \
    -o "${BUILD_DIR}/test_tavrn_link_v2"

compile_or_exit "${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/protocol/test_tavrn_radio_scheduler_v2.c" \
    "${RADIO_SOURCES[@]}" \
    -o "${BUILD_DIR}/test_tavrn_radio_scheduler_v2"

set +e
"${BUILD_DIR}/test_tavrn_wire_v2"
wire_status=$?
if [[ "${MODE}" == "turnaround-red" ]]; then
    "${BUILD_DIR}/test_tavrn_link_v2" | tee "${BUILD_DIR}/turnaround-red-link.log"
    link_status=${PIPESTATUS[0]}
else
    "${BUILD_DIR}/test_tavrn_link_v2"
    link_status=$?
fi
"${BUILD_DIR}/test_tavrn_radio_scheduler_v2"
radio_status=$?
set -e

if [[ "${MODE}" == "turnaround-red" ]]; then
    if [[ ${wire_status} -ne 0 || ${radio_status} -ne 0 || ${link_status} -ne 1 ]] ||
       ! grep -Fq 'item != NULL && item->not_before_ms == expected_not_before_ms' \
            "${BUILD_DIR}/turnaround-red-link.log" ||
       ! grep -Fq 'tavrn_link_v2 RED tests failed: 8 assertion(s)' \
            "${BUILD_DIR}/turnaround-red-link.log"; then
        printf '%s\n' 'turnaround RED did not isolate the HACK due-time assertions' >&2
        exit 2
    fi
    exit 1
fi

if [[ ${wire_status} -ne 0 || ${link_status} -ne 0 || ${radio_status} -ne 0 ]]; then
    exit 1
fi
