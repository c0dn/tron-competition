#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
CC_BIN="${CC:-cc}"

cleanup() {
    rm -rf "${BUILD_DIR}"
}
trap cleanup EXIT

if [[ $# -ne 1 || ( "$1" != "--red" && "$1" != "--green" ) ]]; then
    printf 'usage: %s --red|--green\n' "$0" >&2
    exit 2
fi

MODE="${1#--}"
TEST_SOURCE="${MICROBIT_ROOT}/tests/protocol/test_tavrn_phase6_repair.c"
BINDING_TEST_SOURCE="${MICROBIT_ROOT}/tests/protocol/test_tavrn_full_repair_binding.c"
CONTRACT_HEADER="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase6_repair_contract.h"
RED_BACKEND="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase6_repair_red_backend.c"
REPAIR_HEADER="${MICROBIT_ROOT}/app/protocol/tavrn_repair.h"
REPAIR_SOURCE="${MICROBIT_ROOT}/app/protocol/tavrn_repair.c"
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
SOURCES=(
    "${TEST_SOURCE}"
    "${MICROBIT_ROOT}/app/protocol/tavrn_wire_v2.c"
    "${MICROBIT_ROOT}/app/protocol/aodv_core.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_link_v2.c"
    "${MICROBIT_ROOT}/app/protocol/ble_mesh_tx_queue.c"
    "${MICROBIT_ROOT}/tests/protocol/support/ble_mesh_scheduler_port.c"
    "${MICROBIT_ROOT}/app/protocol/tron_timer_config.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_gtt.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_smart_ttl.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_full.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_router.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_esc.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_mentorship.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_maintenance.c"
)

if [[ "${MODE}" == "red" ]]; then
    COMMON_FLAGS+=( -DTAVRN_PHASE6_REPAIR_RED_MODE )
    SOURCES+=( "${RED_BACKEND}" )
else
    if [[ ! -f "${REPAIR_HEADER}" || ! -f "${REPAIR_SOURCE}" ]] ||
       ! grep -q '^#define TAVRN_REPAIR_API 1' "${REPAIR_HEADER}"; then
        printf '%s\n' 'Phase 6 repair GREEN production API is not defined'
        exit 2
    fi
    SOURCES+=( "${REPAIR_SOURCE}" )
fi

for source in "${CONTRACT_HEADER}" "${SOURCES[@]}"; do
    if [[ ! -f "${source}" ]]; then
        printf 'Phase 6 repair %s setup unavailable: expected %s\n' "${MODE}" \
            "${source#"${MICROBIT_ROOT}/"}" >&2
        exit 2
    fi
done

if ! "${CC_BIN}" "${COMMON_FLAGS[@]}" "${SOURCES[@]}" \
        -o "${BUILD_DIR}/test_tavrn_phase6_repair"; then
    printf 'Phase 6 repair %s compile/link failed\n' "${MODE}" >&2
    exit 2
fi

if [[ "${MODE}" == "green" ]]; then
    BINDING_SOURCES=(
        "${BINDING_TEST_SOURCE}"
        "${MICROBIT_ROOT}/app/protocol/tavrn_wire_v2.c"
        "${MICROBIT_ROOT}/app/protocol/aodv_core.c"
        "${MICROBIT_ROOT}/app/protocol/tavrn_link_v2.c"
        "${MICROBIT_ROOT}/app/protocol/ble_mesh_tx_queue.c"
        "${MICROBIT_ROOT}/tests/protocol/support/ble_mesh_scheduler_port.c"
        "${MICROBIT_ROOT}/app/protocol/tron_timer_config.c"
        "${MICROBIT_ROOT}/app/protocol/tavrn_gtt.c"
        "${MICROBIT_ROOT}/app/protocol/tavrn_smart_ttl.c"
        "${MICROBIT_ROOT}/app/protocol/tavrn_full.c"
        "${MICROBIT_ROOT}/app/protocol/tavrn_router.c"
        "${MICROBIT_ROOT}/app/protocol/tavrn_maintenance.c"
        "${REPAIR_SOURCE}"
        "${MICROBIT_ROOT}/app/protocol/tavrn_full_repair_binding.c"
    )
    for source in "${BINDING_SOURCES[@]}"; do
        if [[ ! -f "${source}" ]]; then
            printf 'Phase 6 repair GREEN binding setup unavailable: expected %s\n' \
                "${source#"${MICROBIT_ROOT}/"}" >&2
            exit 2
        fi
    done
    if ! "${CC_BIN}" "${COMMON_FLAGS[@]}" "${BINDING_SOURCES[@]}" \
            -o "${BUILD_DIR}/test_tavrn_full_repair_binding"; then
        printf '%s\n' 'Phase 6 repair GREEN binding compile/link failed' >&2
        exit 2
    fi
fi

set +e
"${BUILD_DIR}/test_tavrn_phase6_repair" | tee "${BUILD_DIR}/${MODE}.log"
test_status=${PIPESTATUS[0]}
set -e

if [[ "${MODE}" == "green" ]]; then
    set +e
    "${BUILD_DIR}/test_tavrn_full_repair_binding" | tee "${BUILD_DIR}/binding-green.log"
    binding_status=${PIPESTATUS[0]}
    set -e
    if [[ ${test_status} -eq 0 && ${binding_status} -eq 0 ]] && \
       ! grep -q '^FAIL \|^STRUCTURAL ' "${BUILD_DIR}/${MODE}.log" \
       && \
       ! grep -q '^FAIL \|^STRUCTURAL ' "${BUILD_DIR}/binding-green.log"; then
        printf '%s\n' 'Phase 6 repair GREEN production assertions passed'
        exit 0
    fi
    if [[ ( ${test_status} -eq 1 || ${binding_status} -eq 1 ) ]] && \
       ! grep -q '^STRUCTURAL ' "${BUILD_DIR}/${MODE}.log" \
       && \
       ! grep -q '^STRUCTURAL ' "${BUILD_DIR}/binding-green.log"; then
        printf '%s\n' 'Phase 6 repair GREEN remains red: production policy is incomplete'
        exit 1
    fi
    printf 'Phase 6 repair GREEN malformed result: exit=%d\n' "${test_status}" >&2
    exit 2
fi

expected_tags=(REPAIR-01 ACK-006 REPAIR-02 AODV-08 REPAIR-04 REPAIR-05 REPAIR-03 SERIAL-05 MAINT-08)
expected_reached=(
    strict-config-capacity
    owned-transit-and-negative-triggers
    atomic-setup-held-obligations
    same-destination-reserve-commit-rollback
    smart-ttl-full-scope-fresh-ids-no-inner-retry
    matching-alternate-rrep-and-rejections
    active-rreq-snapshot-data-not-admitted
    total-timeout-cooldown-wrap
    deferred-rerr-held-leave-exactly-once
    repair-off-equivalence
)
mapfile -t actual_tags < <(grep '^FAIL ' "${BUILD_DIR}/red.log" | \
    while IFS= read -r line; do
        tag="${line#FAIL }"
        printf '%s\n' "${tag%%:*}"
    done)

if [[ ${test_status} -ne 1 ]] || grep -q '^STRUCTURAL ' "${BUILD_DIR}/red.log"; then
    printf 'Phase 6 repair RED malformed result: exit=%d structural=%s\n' \
        "${test_status}" "$(grep -c '^STRUCTURAL ' "${BUILD_DIR}/red.log" || true)" >&2
    exit 2
fi
if [[ ${#actual_tags[@]} -ne ${#expected_tags[@]} ]]; then
    printf 'Phase 6 repair RED emitted %d tags, expected %d\n' \
        "${#actual_tags[@]}" "${#expected_tags[@]}" >&2
    exit 2
fi
for index in "${!expected_tags[@]}"; do
    if [[ "${actual_tags[index]}" != "${expected_tags[index]}" ]]; then
        printf 'Phase 6 repair RED tag %d was %s, expected %s\n' "${index}" \
            "${actual_tags[index]}" "${expected_tags[index]}" >&2
        exit 2
    fi
done
for reached in "${expected_reached[@]}"; do
    if [[ "$(grep -Fxc "REACHED ${reached}" "${BUILD_DIR}/red.log" || true)" != "1" ]]; then
        printf 'Phase 6 repair RED did not reach scenario %s\n' "${reached}" >&2
        exit 2
    fi
done
if grep -q ': assertion failed: test_' "${BUILD_DIR}/red.log"; then
    printf '%s\n' 'Phase 6 repair RED failure was attributed to a test wrapper, not policy' >&2
    exit 2
fi

printf '%s\n' 'Phase 6 repair RED: all scenario bodies reached; missing-policy assertions retained'
exit 1
