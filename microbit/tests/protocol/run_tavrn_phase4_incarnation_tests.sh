#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
CC_BIN="${CC:-cc}"
MODE=""

cleanup() {
    rm -rf "${BUILD_DIR}"
}
trap cleanup EXIT

if [[ $# -ne 1 || ( "$1" != "--red" && "$1" != "--green" ) ]]; then
    printf 'usage: %s --red|--green\n' "$0" >&2
    exit 2
fi
MODE="${1#--}"

COMMON_FLAGS=(
    -std=c99
    -Wall
    -Wextra
    -Werror
    -DBLE_RADIO_HOST_TEST
    -I"${MICROBIT_ROOT}/app/drivers"
    -I"${MICROBIT_ROOT}/app/protocol"
    -I"${MICROBIT_ROOT}/tests/protocol/support"
    -I"${MICROBIT_ROOT}/tests/protocol/red_support"
)
TEST_SOURCE="${MICROBIT_ROOT}/tests/protocol/test_tavrn_phase4_incarnation.c"
COMMON_SOURCES=(
    "${MICROBIT_ROOT}/app/protocol/tavrn_wire_v2.c"
    "${MICROBIT_ROOT}/app/protocol/aodv_core.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_link_v2.c"
    "${MICROBIT_ROOT}/app/protocol/ble_mesh_tx_queue.c"
    "${MICROBIT_ROOT}/tests/protocol/support/ble_mesh_scheduler_port.c"
    "${MICROBIT_ROOT}/app/protocol/tron_timer_config.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_router.c"
)
CONTRACT_HEADER="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase4_incarnation_contract.h"
RED_BACKEND="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase4_incarnation_red_backend.c"
SOURCES=("${TEST_SOURCE}" "${COMMON_SOURCES[@]}")

if [[ "${MODE}" == "red" ]]; then
    SOURCES+=("${RED_BACKEND}")
fi

for source in "${CONTRACT_HEADER}" "${SOURCES[@]}"; do
    if [[ ! -f "${source}" ]]; then
        printf 'Phase 4 incarnation %s setup unavailable: expected source %s\n' \
            "${MODE}" "${source#"${MICROBIT_ROOT}/"}" >&2
        exit 2
    fi
done

if ! "${CC_BIN}" "${COMMON_FLAGS[@]}" "${SOURCES[@]}" \
        -o "${BUILD_DIR}/test_tavrn_phase4_incarnation"; then
    printf 'Phase 4 incarnation %s compile failed\n' "${MODE}" >&2
    exit 2
fi

set +e
"${BUILD_DIR}/test_tavrn_phase4_incarnation" | tee "${BUILD_DIR}/${MODE}.log"
test_status=${PIPESTATUS[0]}
set -e

failure_count="$(grep -c '^FAIL ' "${BUILD_DIR}/${MODE}.log" || true)"
if [[ "${MODE}" == "green" ]]; then
    if [[ ${test_status} -eq 0 && "${failure_count}" == "0" ]]; then
        exit 0
    fi
    if [[ ${test_status} -eq 1 && "${failure_count}" -gt 0 ]]; then
        exit 1
    fi
    printf 'Phase 4 incarnation GREEN malformed result: exit=%d fail=%s\n' \
        "${test_status}" "${failure_count}" >&2
    exit 2
fi

if [[ ${test_status} -ne 1 || "${failure_count}" != "3" ]]; then
    printf 'Phase 4 incarnation RED requires exactly three assertion failures; got exit=%d fail=%s\n' \
        "${test_status}" "${failure_count}" >&2
    exit 2
fi
for requirement in SERIAL-04 BOOT-01 BOOT-06; do
    if [[ "$(grep -c "^FAIL ${requirement}:" "${BUILD_DIR}/${MODE}.log" || true)" != "1" ]]; then
        printf 'Phase 4 incarnation RED requires one exact %s failure\n' \
            "${requirement}" >&2
        exit 2
    fi
done
if [[ "$(grep -Fxc 'tavrn_phase4_incarnation RED tests failed: 3 assertion(s)' \
              "${BUILD_DIR}/${MODE}.log" || true)" != "1" ]]; then
    printf '%s\n' 'Phase 4 incarnation RED missing exact final assertion summary' >&2
    exit 2
fi
exit 1
