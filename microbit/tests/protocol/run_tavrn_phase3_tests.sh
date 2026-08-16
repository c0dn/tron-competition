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
    -I"${MICROBIT_ROOT}/app/drivers"
    -I"${MICROBIT_ROOT}/app/protocol"
    -I"${MICROBIT_ROOT}/tests/protocol/support"
)
TESTS=(
    "${MICROBIT_ROOT}/tests/protocol/test_tavrn_gtt.c"
    "${MICROBIT_ROOT}/tests/protocol/test_tavrn_smart_ttl.c"
    "${MICROBIT_ROOT}/tests/protocol/test_tavrn_router.c"
    "${MICROBIT_ROOT}/tests/protocol/test_tavrn_phase3_integration.c"
)
HEADERS=(
    "${MICROBIT_ROOT}/app/protocol/tavrn_gtt.h"
    "${MICROBIT_ROOT}/app/protocol/tavrn_smart_ttl.h"
    "${MICROBIT_ROOT}/app/protocol/tavrn_router.h"
    "${MICROBIT_ROOT}/app/protocol/tavrn_full.h"
)

if [[ "${MODE}" == "red" ]]; then
    PHASE_SOURCES=(
        "${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase3_red_backend.c"
    )
else
    PHASE_SOURCES=(
        "${MICROBIT_ROOT}/app/protocol/tavrn_gtt.c"
        "${MICROBIT_ROOT}/app/protocol/tavrn_smart_ttl.c"
        "${MICROBIT_ROOT}/app/protocol/tavrn_router.c"
        "${MICROBIT_ROOT}/app/protocol/tavrn_full.c"
    )
fi

COMMON_SOURCES=(
    "${MICROBIT_ROOT}/app/protocol/tavrn_wire_v2.c"
    "${MICROBIT_ROOT}/app/protocol/aodv_core.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_link_v2.c"
    "${MICROBIT_ROOT}/app/protocol/ble_mesh_tx_queue.c"
    "${MICROBIT_ROOT}/tests/protocol/support/ble_mesh_scheduler_port.c"
    "${MICROBIT_ROOT}/app/protocol/tron_timer_config.c"
)

for source in "${HEADERS[@]}" "${TESTS[@]}" "${COMMON_SOURCES[@]}" \
              "${PHASE_SOURCES[@]}"; do
    if [[ ! -f "${source}" ]]; then
        printf 'Phase 3 %s mode unavailable: expected source %s\n' "${MODE}" \
            "${source#"${MICROBIT_ROOT}/"}" >&2
        exit 2
    fi
done

compile_test() {
    local test_source="$1"
    local output_name="$2"

    if ! "${CC_BIN}" "${COMMON_FLAGS[@]}" \
        "${test_source}" "${COMMON_SOURCES[@]}" "${PHASE_SOURCES[@]}" \
        -o "${BUILD_DIR}/${output_name}"; then
        printf 'Phase 3 %s compile failed: %s\n' "${MODE}" \
            "${test_source#"${MICROBIT_ROOT}/tests/protocol/"}" >&2
        exit 2
    fi
}

compile_test "${TESTS[0]}" test_tavrn_gtt
compile_test "${TESTS[1]}" test_tavrn_smart_ttl
compile_test "${TESTS[2]}" test_tavrn_router
compile_test "${TESTS[3]}" test_tavrn_phase3_integration

if [[ "${MODE}" != "red" ]]; then
    "${BUILD_DIR}/test_tavrn_gtt"
    "${BUILD_DIR}/test_tavrn_smart_ttl"
    "${BUILD_DIR}/test_tavrn_router"
    "${BUILD_DIR}/test_tavrn_phase3_integration"
    exit 0
fi

set +e
"${BUILD_DIR}/test_tavrn_gtt" | tee "${BUILD_DIR}/gtt-red.log"
gtt_status=${PIPESTATUS[0]}
"${BUILD_DIR}/test_tavrn_smart_ttl" | tee "${BUILD_DIR}/smart-ttl-red.log"
smart_ttl_status=${PIPESTATUS[0]}
"${BUILD_DIR}/test_tavrn_router" | tee "${BUILD_DIR}/router-red.log"
router_status=${PIPESTATUS[0]}
"${BUILD_DIR}/test_tavrn_phase3_integration" | tee "${BUILD_DIR}/integration-red.log"
integration_status=${PIPESTATUS[0]}
set -e

if [[ ${gtt_status} -ne 1 || ${smart_ttl_status} -ne 1 ||
      ${router_status} -ne 1 || ${integration_status} -ne 1 ]]; then
    printf '%s\n' 'Phase 3 RED expected assertion exit 1 from every test binary' >&2
    exit 2
fi

RED_LOGS=(
    "${BUILD_DIR}/gtt-red.log"
    "${BUILD_DIR}/smart-ttl-red.log"
    "${BUILD_DIR}/router-red.log"
    "${BUILD_DIR}/integration-red.log"
)
for requirement in GTT-01 GTT-02 GTT-03 GTT-04 GTT-05 GTT-06; do
    found=0
    for log in "${RED_LOGS[@]}"; do
        if grep -Fq "FAIL ${requirement}:" "${log}"; then
            found=1
            break
        fi
    done
    if [[ ${found} -eq 0 ]]; then
        printf 'Phase 3 RED missing required assertion tag %s\n' "${requirement}" >&2
        exit 2
    fi
done
if grep -Fq 'FAIL AODV-04:' "${RED_LOGS[@]}"; then
    printf '%s\n' 'Phase 3 RED changed the AODV_ONLY no-hint characterization' >&2
    exit 2
fi

exit 1
