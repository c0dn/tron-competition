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
COMMON_SOURCES=(
    "${MICROBIT_ROOT}/tests/protocol/test_tavrn_phase5_targeted_freshness.c"
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
SOURCES=("${COMMON_SOURCES[@]}")
if [[ "${MODE}" == "red" ]]; then
    COMMON_FLAGS+=( -DTAVRN_PHASE5_TARGETED_RED_MODE )
    SOURCES+=("${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase5_targeted_freshness_red_backend.c")
fi

for source in "${SOURCES[@]}" \
              "${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase5_targeted_freshness_contract.h"; do
    if [[ ! -f "${source}" ]]; then
        printf 'Targeted freshness %s setup unavailable: expected %s\n' "${MODE}" \
            "${source#"${MICROBIT_ROOT}/"}" >&2
        exit 2
    fi
done

if ! "${CC_BIN}" "${COMMON_FLAGS[@]}" "${SOURCES[@]}" \
        -o "${BUILD_DIR}/test_tavrn_phase5_targeted_freshness"; then
    printf 'Targeted freshness %s compile/link failed\n' "${MODE}" >&2
    exit 2
fi

run_runtime_integration_assertions() {
    python3 - "${MICROBIT_ROOT}/app/tavrn_routed_node/src/main.c" \
        "${MICROBIT_ROOT}/app/protocol/tavrn_full_maintenance_binding.c" \
        "${MICROBIT_ROOT}/app/protocol/tavrn_full_maintenance_binding.h" \
        "${MICROBIT_ROOT}/app/ble_mesh_node/cmake/TronBleProfiles.cmake" <<'PY'
import pathlib
import sys

main_path, binding_path, binding_header_path, profile_path = map(pathlib.Path, sys.argv[1:])
main = main_path.read_text(encoding="utf-8")
binding = binding_path.read_text(encoding="utf-8")
binding_header = binding_header_path.read_text(encoding="utf-8")
profile = profile_path.read_text(encoding="utf-8")

def require(condition, message):
    if not condition:
        raise SystemExit("Targeted freshness runtime integration assertion failed: " + message)

def position(source, token):
    value = source.find(token)
    require(value >= 0, "missing " + token)
    return value

for field in (
    "aodv_net_traversal_ms",
    "freshness_response_min_ms",
    "freshness_response_max_ms",
):
    assignment = "maintenance_config.%s =" % field
    start = position(main, assignment)
    require(main.find("tron_timer_config.%s;" % field, start) >= start,
            "FULL maintenance config does not select " + field)

rx_targeted = position(main, "tavrn_maintenance_is_targeted_control(")
rx_ordinary = position(main, "tavrn_maintenance_handle_rx_control(")
require(rx_targeted < rx_ordinary,
        "targeted RX selector does not precede ordinary HELLO maintenance")
ordinary_condition = position(main, "mentorship_trace.rx_control.control.pdu[5] == 0x80u")
require(rx_targeted < ordinary_condition < rx_ordinary,
        "ordinary maintenance path is not exact 0x80 after targeted RX")
targeted_dispatch = main[rx_targeted:ordinary_condition]
require(targeted_dispatch.count("TAVRN_TARGETED_FRESHNESS_INVALID") == 1 and
        targeted_dispatch.count("TAVRN_ROUTER_EVENT_INVALID") == 1,
        "targeted RX does not map exactly its INVALID result to router INVALID")
require("TAVRN_TARGETED_FRESHNESS_DROPPED" not in targeted_dispatch and
        "routed_targeted_rx_status !=" not in targeted_dispatch,
        "targeted RX terminalizes a non-INVALID result")
require(main.count("tavrn_maintenance_high_token_scheduler_event(") == 2,
        "FULL shared high-token scheduler/fault dispatcher count is not exact")
require(main.count("tavrn_maintenance_targeted_scheduler_event(") == 0 and
        main.count("tavrn_maintenance_verification_scheduler_event(") == 0,
        "FULL runtime bypasses the shared high-token dispatcher")

binding_order = [
    "tavrn_maintenance_owner_pre_tick(",
    "tavrn_router_tick_ex(",
    "tavrn_mentorship_tick(",
    "tavrn_router_local_broadcast_snapshot(",
    "tavrn_maintenance_observe_local_broadcast(",
    "tavrn_maintenance_activate(",
    "tavrn_maintenance_targeted_owner_tick(",
    "tavrn_maintenance_owner_post_tick(",
]
positions = [position(binding, token) for token in binding_order]
require(positions == sorted(positions), "FULL owner tick order changed")
require(binding.count("tavrn_maintenance_targeted_owner_tick(") == 1,
        "FULL binding does not perform exactly one targeted owner tick")
for token in (
    "targeted_owner_status",
    "targeted_owner_action",
    "maintenance_counters",
):
    require(token in binding_header and token in binding,
            "binding result does not expose " + token)

require("#if TRON_BUILD_ROUTED_FULL_TAVRN" in main,
        "FULL runtime guard is absent from routed main")
require("TAVRN_ROUTED_FULL_TAVRN_ESC_K1_MENTORSHIP_ADAPTIVE_HELLO_LOCAL_EXPIRY_TARGETED_FRESHNESS_RREQ_VERIFICATION_TC_METADATA" in profile,
        "FULL behavior name omits retained RREQ verification")
for capability in (
    "local-expiry-demand",
    "targeted-freshness-stage0",
    "targeted-hello-request-response",
    "targeted-runtime-binding",
):
    require(capability in profile, "FULL capability list omits " + capability)
require("set(TRON_BUILD_BEHAVIOR \"TAVRN_ROUTED_AODV_ONLY\")" in profile,
        "AODV_ONLY profile branch is absent")
PY
}

set +e
"${BUILD_DIR}/test_tavrn_phase5_targeted_freshness" | tee "${BUILD_DIR}/${MODE}.log"
test_status=${PIPESTATUS[0]}
set -e

if [[ "${MODE}" == "green" ]]; then
    if [[ ${test_status} -eq 0 ]] && ! grep -q '^FAIL \|^STRUCTURAL ' "${BUILD_DIR}/${MODE}.log"; then
        if ! run_runtime_integration_assertions; then
            exit 2
        fi
        printf '%s\n' 'Targeted freshness GREEN production assertions passed'
        exit 0
    fi
    printf 'Targeted freshness GREEN is not a complete production result: exit=%d\n' \
        "${test_status}" >&2
    exit 2
fi

expected_tags=(MAINT-06 MAINT-07 META-01 META-03 META-04 SERIAL-01)
expected_reached=(
    wire-and-malformed
    targeted-immediate-receiver
    stage0-route-timeout
    relay-dedupe-metadata
    target-intermediary-suppression
    tokens-terminal-tombstones
    serial-ordinary-admission-retry
)
mapfile -t actual_tags < <(grep '^FAIL ' "${BUILD_DIR}/red.log" | \
    while IFS= read -r line; do
        tag="${line#FAIL }"
        printf '%s\n' "${tag%%:*}"
    done)

if [[ ${test_status} -ne 1 ]] || grep -q '^STRUCTURAL ' "${BUILD_DIR}/red.log"; then
    printf 'Targeted freshness RED malformed result: exit=%d structural=%s\n' \
        "${test_status}" "$(grep -c '^STRUCTURAL ' "${BUILD_DIR}/red.log" || true)" >&2
    exit 2
fi
for tag in "${actual_tags[@]}"; do
    known=0
    for expected in "${expected_tags[@]}"; do
        if [[ "${tag}" == "${expected}" ]]; then
            known=1
        fi
    done
    if [[ ${known} -eq 0 ]]; then
        printf 'Targeted freshness RED emitted unexpected tag %s\n' "${tag}" >&2
        exit 2
    fi
done
for expected in "${expected_tags[@]}"; do
    if ! grep -q "^FAIL ${expected}:" "${BUILD_DIR}/red.log"; then
        printf 'Targeted freshness RED did not reach required tag %s\n' "${expected}" >&2
        exit 2
    fi
done
for reached in "${expected_reached[@]}"; do
    if [[ "$(grep -Fxc "REACHED ${reached}" "${BUILD_DIR}/red.log" || true)" != "1" ]]; then
        printf 'Targeted freshness RED did not reach scenario %s\n' "${reached}" >&2
        exit 2
    fi
done
receiver_block="$(python3 - "${BUILD_DIR}/red.log" <<'PY'
import pathlib
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
start = text.index("REACHED targeted-immediate-receiver\n")
end = text.index("REACHED stage0-route-timeout\n", start)
print(text[start:end], end="")
PY
)"
for tag in MAINT-06 META-04; do
    if ! grep -q "^FAIL ${tag}:" <<<"${receiver_block}"; then
        printf 'Targeted freshness RED receiver scenario did not fail tag %s\n' "${tag}" >&2
        exit 2
    fi
done
if grep -q ': assertion failed: test_' "${BUILD_DIR}/red.log"; then
    printf '%s\n' 'Targeted freshness RED failure was attributed to a test wrapper, not policy' >&2
    exit 2
fi

printf '%s\n' 'Targeted freshness RED: all scenario bodies reached; missing-policy assertions retained'
exit 1
