#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 || ( "$1" != "--red" && "$1" != "--budget-red" &&
                        "$1" != "--radio-repeat-red" && "$1" != "--timer-red" ) ]]; then
    printf 'usage: %s --red|--budget-red|--radio-repeat-red|--timer-red\n' "$0" >&2
    exit 2
fi

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
CC_BIN="${CC:-cc}"
QUEUE_SOURCE="${MICROBIT_ROOT}/app/protocol/ble_mesh_tx_queue.c"
TIMER_SOURCE="${MICROBIT_ROOT}/app/protocol/tron_timer_config.c"
MODE="$1"

if [[ "${MODE}" == "--red" ]]; then
    TEST_SOURCE="${MICROBIT_ROOT}/tests/protocol/test_tavrn_future_bearer_red.c"
    TEST_BINARY="${BUILD_DIR}/test_tavrn_future_bearer"
elif [[ "${MODE}" == "--radio-repeat-red" ]]; then
    TEST_SOURCE="${MICROBIT_ROOT}/tests/protocol/test_tavrn_future_bearer_radio_repeat_red.c"
    TEST_BINARY="${BUILD_DIR}/test_tavrn_future_bearer_radio_repeat"
elif [[ "${MODE}" == "--timer-red" ]]; then
    TEST_SOURCE="${MICROBIT_ROOT}/tests/protocol/test_tavrn_future_bearer_timer_red.c"
    TEST_BINARY="${BUILD_DIR}/test_tavrn_future_bearer_timer"
else
    TEST_SOURCE="${MICROBIT_ROOT}/tests/protocol/test_tavrn_future_bearer_budget_red.c"
    TEST_BINARY="${BUILD_DIR}/test_tavrn_future_bearer_budget"
fi

cleanup() {
    rm -rf "${BUILD_DIR}"
}
trap cleanup EXIT

REQUIRED_SOURCES=("${TEST_SOURCE}" "${TIMER_SOURCE}")
if [[ "${MODE}" != "--timer-red" ]]; then
    REQUIRED_SOURCES+=("${QUEUE_SOURCE}")
fi
for source in "${REQUIRED_SOURCES[@]}"; do
    if [[ ! -f "${source}" ]]; then
        printf 'future bearer RED setup unavailable: expected %s\n' \
            "${source#"${MICROBIT_ROOT}/"}" >&2
        exit 2
    fi
done

if [[ "${MODE}" == "--radio-repeat-red" ]]; then
    for source in \
        "${MICROBIT_ROOT}/app/protocol/ble_mesh_scheduler.c" \
        "${MICROBIT_ROOT}/app/drivers/ble_radio.c" \
        "${MICROBIT_ROOT}/tests/protocol/support/ble_radio_host_shim.c"; do
        if [[ ! -f "${source}" ]]; then
            printf 'future bearer RED setup unavailable: expected %s\n' \
                "${source#"${MICROBIT_ROOT}/"}" >&2
            exit 2
        fi
    done
fi

require_timer_contract_line() {
    local file="$1"
    local expected="$2"

    if ! grep -Fqx -- "${expected}" "${file}"; then
        printf 'future bearer timer RED source contract missing %s in %s\n' \
            "${expected}" "${file#"${MICROBIT_ROOT}/"}" >&2
        return 1
    fi
}

require_timer_contract_fragment() {
    local file="$1"
    local expected="$2"

    if ! grep -Fq -- "${expected}" "${file}"; then
        printf 'future bearer timer RED source contract missing formula fragment %s in %s\n' \
            "${expected}" "${file#"${MICROBIT_ROOT}/"}" >&2
        return 1
    fi
}

require_timer_contract_absent() {
    local file="$1"
    local forbidden="$2"

    if grep -Fqx -- "${forbidden}" "${file}"; then
        printf 'future bearer timer RED source contract retains stale %s in %s\n' \
            "${forbidden}" "${file#"${MICROBIT_ROOT}/"}" >&2
        return 1
    fi
}

require_timer_source_contract() {
    local profile_source="${MICROBIT_ROOT}/app/ble_mesh_node/cmake/TronBleProfiles.cmake"
    local manifest_template="${MICROBIT_ROOT}/app/ble_mesh_node/config/tron_build_manifest.in"

    require_timer_contract_line "${profile_source}" \
        'tron_ble_timer(radio_tx_event_bound_ms 8 8 8)' &&
        require_timer_contract_line "${profile_source}" \
            'tron_ble_timer(radio_tx_repeated_event_bound_ms 14 14 14)' &&
        require_timer_contract_line "${profile_source}" \
            'tron_ble_timer(radio_tx_fault_cleanup_bound_ms 18 18 18)' &&
        require_timer_contract_line "${profile_source}" \
            'tron_ble_timer(link_tx_scheduler_attempt_bound_ms 3048 3048 3048)' &&
        require_timer_contract_line "${profile_source}" \
            'tron_ble_timer(link_response_window_sum_ms 750 750 750)' &&
        require_timer_contract_line "${profile_source}" \
            'tron_ble_timer(link_no_response_wall_bound_ms 9894 9894 9894)' &&
        require_timer_contract_absent "${profile_source}" \
            'tron_ble_timer(link_tx_scheduler_attempt_bound_ms 30 30 30)' &&
        require_timer_contract_absent "${profile_source}" \
            'tron_ble_timer(link_no_response_wall_bound_ms 840 840 840)' &&
        require_timer_contract_fragment "${profile_source}" \
            '(1 + 3) * ${TRON_TIMER_RADIO_STATE_TIMEOUT_MS}' &&
        require_timer_contract_fragment "${profile_source}" \
            '(1 + 6) * ${TRON_TIMER_RADIO_STATE_TIMEOUT_MS}' &&
        require_timer_contract_fragment "${profile_source}" \
            '(1 + 6 + 2) * ${TRON_TIMER_RADIO_STATE_TIMEOUT_MS}' &&
        require_timer_contract_fragment "${profile_source}" \
            '(${TRON_TIMER_SCHEDULER_CUSTODY_BYPASS_MAX} + 1) * (1000 + ${TRON_TIMER_RADIO_TX_REPEATED_EVENT_BOUND_MS} + ${TRON_TIMER_SCHEDULER_POLL_MAX_MS})' &&
        require_timer_contract_fragment "${profile_source}" \
            '${TRON_TIMER_LINK_MAX_ATTEMPTS} * ${TRON_TIMER_LINK_HACK_TIMEOUT_MS}' &&
        require_timer_contract_fragment "${profile_source}" \
            '${TRON_TIMER_LINK_RESPONSE_WINDOW_SUM_MS} + ${TRON_TIMER_LINK_MAX_ATTEMPTS} * ${TRON_TIMER_LINK_TX_SCHEDULER_ATTEMPT_BOUND_MS}' &&
        require_timer_contract_line "${manifest_template}" \
            'formula.radio_tx_event_bound_ms=(1+3)*timer.radio_state_timeout_ms' &&
        require_timer_contract_line "${manifest_template}" \
            'formula.radio_tx_repeated_event_bound_ms=(1+6)*timer.radio_state_timeout_ms' &&
        require_timer_contract_line "${manifest_template}" \
            'formula.radio_tx_fault_cleanup_bound_ms=(1+6+2)*timer.radio_state_timeout_ms' &&
        require_timer_contract_line "${manifest_template}" \
            'formula.link_tx_scheduler_attempt_bound_ms=(timer.scheduler_custody_bypass_max+1)*(1000+timer.radio_tx_repeated_event_bound_ms+timer.scheduler_poll_max_ms)' &&
        require_timer_contract_line "${manifest_template}" \
            'formula.link_response_window_sum_ms=timer.link_max_attempts*timer.link_hack_timeout_ms' &&
        require_timer_contract_line "${manifest_template}" \
            'formula.link_no_response_wall_bound_ms=timer.link_response_window_sum_ms+timer.link_max_attempts*timer.link_tx_scheduler_attempt_bound_ms'
}

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

set +e
"${CC_BIN}" "${COMMON_FLAGS[@]}" -c "${TEST_SOURCE}" \
    -o "${BUILD_DIR}/test.o" 2>&1 | tee "${BUILD_DIR}/compile.log"
compile_status=${PIPESTATUS[0]}
set -e

if [[ ${compile_status} -ne 0 ]]; then
    if [[ "${MODE}" == "--budget-red" ]] && grep -Fq \
        'BUDGET_RED_GATE: BLE_MESH_SCHED_BUDGET_API=1 is required' \
        "${BUILD_DIR}/compile.log"; then
        printf '%s\n' \
            'future bearer budget RED: expected staged API gate (BLE_MESH_SCHED_BUDGET_API=1)'
        exit 1
    fi
    if [[ "${MODE}" == "--radio-repeat-red" ]] && { \
        grep -Fq 'RADIO_REPEAT_RED_GATE: BLE_RADIO_REPEATED_ADV_API=1 is required' \
            "${BUILD_DIR}/compile.log" || \
        grep -Fq 'RADIO_REPEAT_RED_GATE: BLE_RADIO_REPEATED_ADV_API must equal 1' \
            "${BUILD_DIR}/compile.log"; }; then
        printf '%s\n' \
            'future bearer radio repeat RED: expected staged API gate (BLE_RADIO_REPEATED_ADV_API=1)'
        exit 1
    fi
    if [[ "${MODE}" == "--timer-red" ]] && { \
        grep -Fq 'TIMER_RED_GATE: TRON_TIMER_FUTURE_BEARER_API=1 is required' \
            "${BUILD_DIR}/compile.log" || \
        grep -Fq 'TIMER_RED_GATE: TRON_TIMER_FUTURE_BEARER_API must equal 1' \
            "${BUILD_DIR}/compile.log"; }; then
        printf '%s\n' \
            'future bearer timer RED: expected staged API gate (TRON_TIMER_FUTURE_BEARER_API=1)'
        exit 1
    fi
    printf 'future bearer %s compile failed for an unexpected reason\n' \
        "${MODE#--}" >&2
    exit 2
fi

if [[ "${MODE}" == "--timer-red" ]]; then
    if ! require_timer_source_contract; then
        printf '%s\n' 'future bearer timer RED source contract failed' >&2
        exit 2
    fi
    LINK_SOURCES=("${TIMER_SOURCE}")
else
    LINK_SOURCES=("${TIMER_SOURCE}" "${QUEUE_SOURCE}")
fi
if [[ "${MODE}" == "--budget-red" || "${MODE}" == "--radio-repeat-red" ]]; then
    LINK_SOURCES+=(
        "${MICROBIT_ROOT}/app/protocol/ble_mesh_scheduler.c"
        "${MICROBIT_ROOT}/app/drivers/ble_radio.c"
        "${MICROBIT_ROOT}/tests/protocol/support/ble_radio_host_shim.c"
    )
fi

if ! "${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${BUILD_DIR}/test.o" "${LINK_SOURCES[@]}" \
    -o "${TEST_BINARY}"; then
    printf 'future bearer %s link failed for an unexpected reason\n' \
        "${MODE#--}" >&2
    exit 2
fi

set +e
"${TEST_BINARY}" | tee "${BUILD_DIR}/red.log"
test_status=${PIPESTATUS[0]}
set -e

if [[ ${test_status} -eq 0 ]]; then
    if [[ "${MODE}" == "--red" ]]; then
        printf '%s\n' \
            'future bearer RED transitioned to green: queue metadata, EDF, and expiry contract pass'
    elif [[ "${MODE}" == "--budget-red" ]]; then
        printf '%s\n' \
            'future bearer budget RED transitioned to green: rolling accounting and custody hold contract pass'
    elif [[ "${MODE}" == "--timer-red" ]]; then
        printf '%s\n' \
            'future bearer timer RED transitioned to green: canonical bearer bounds and generated contract pass'
    else
        printf '%s\n' \
            'future bearer radio repeat RED transitioned to green: repeated radio and scheduler event evidence pass'
    fi
    exit 0
fi

if [[ "${MODE}" == "--red" ]]; then
    printf 'future bearer RED produced an unexpected result: exit=%d\n' \
        "${test_status}" >&2
    exit 1
fi

if [[ "${MODE}" == "--budget-red" ]]; then
    printf 'future bearer budget RED produced an unexpected result: exit=%d\n' \
        "${test_status}" >&2
    exit 2
fi

if [[ "${MODE}" == "--radio-repeat-red" ]]; then
    printf 'future bearer radio repeat RED produced an unexpected result: exit=%d\n' \
        "${test_status}" >&2
    exit 2
fi

if [[ "${MODE}" == "--timer-red" ]]; then
    printf 'future bearer timer RED produced an unexpected result: exit=%d\n' \
        "${test_status}" >&2
    exit 2
fi

printf 'future bearer %s produced an unexpected result: exit=%d\n' \
    "${MODE#--}" "${test_status}" >&2
exit 2
