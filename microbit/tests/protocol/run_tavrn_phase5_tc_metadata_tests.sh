#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
CC_BIN="${CC:-cc}"

cleanup() { rm -rf "${BUILD_DIR}"; }
trap cleanup EXIT

if [[ $# -ne 1 || ( "$1" != "--red" && "$1" != "--green" ) ]]; then
    printf 'usage: %s --red|--green\n' "$0" >&2
    exit 2
fi

MODE="${1#--}"
TEST_SOURCE="${MICROBIT_ROOT}/tests/protocol/test_tavrn_phase5_tc_metadata.c"
CONTRACT_HEADER="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase5_tc_metadata_contract.h"
RED_BACKEND="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase5_tc_metadata_red_backend.c"
MAINTENANCE_HEADER="${MICROBIT_ROOT}/app/protocol/tavrn_maintenance.h"
BASE_FLAGS=(
    -std=c99 -Wall -Wextra -Werror -DBLE_RADIO_HOST_TEST
    -I"${MICROBIT_ROOT}/app/drivers"
    -I"${MICROBIT_ROOT}/app/protocol"
    -I"${MICROBIT_ROOT}/tests/protocol/support"
    -I"${MICROBIT_ROOT}/tests/protocol/red_support"
)
PRODUCTION_SOURCES=(
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
    "${MICROBIT_ROOT}/app/protocol/tavrn_full_maintenance_binding.c"
)
COMMON_FLAGS=( "${BASE_FLAGS[@]}" )
SOURCES=( "${PRODUCTION_SOURCES[@]}" )

if [[ "${MODE}" == "red" ]]; then
    COMMON_FLAGS+=( -DTAVRN_PHASE5_TC_METADATA_RED_MODE )
    SOURCES+=( "${RED_BACKEND}" )
elif ! grep -q '^#define TAVRN_MAINTENANCE_TC_METADATA_API 1' "${MAINTENANCE_HEADER}"; then
    printf '%s\n' 'TC metadata GREEN production API is not defined'
    exit 2
fi
if [[ "${MODE}" == "green" ]]; then
    COMMON_FLAGS+=( -DTAVRN_PHASE5_TC_METADATA_CORRECTIVE_MODE )
fi

for source in "${CONTRACT_HEADER}" "${SOURCES[@]}"; do
    if [[ ! -f "${source}" ]]; then
        printf 'TC metadata %s setup unavailable: expected %s\n' "${MODE}" \
            "${source#"${MICROBIT_ROOT}/"}" >&2
        exit 2
    fi
done

if ! "${CC_BIN}" "${COMMON_FLAGS[@]}" "${SOURCES[@]}" \
        -o "${BUILD_DIR}/test_tavrn_phase5_tc_metadata"; then
    printf 'TC metadata %s compile/link failed\n' "${MODE}" >&2
    exit 2
fi

run_runtime_boundary_assertions() {
    python3 - "${MICROBIT_ROOT}/app/protocol/tavrn_full_maintenance_binding.c" \
        "${MICROBIT_ROOT}/app/tavrn_routed_node/src/main.c" \
        "${MICROBIT_ROOT}/app/protocol/tavrn_mentorship.c" \
        "${MICROBIT_ROOT}/app/protocol/tavrn_maintenance.c" \
        "${MICROBIT_ROOT}/app/protocol/tavrn_router.c" <<'PY'
import pathlib
import sys

binding_path, main_path, mentorship_path, maintenance_path, router_path = map(pathlib.Path, sys.argv[1:])
binding = binding_path.read_text(encoding="utf-8")
main = main_path.read_text(encoding="utf-8")
mentorship = mentorship_path.read_text(encoding="utf-8")
maintenance = maintenance_path.read_text(encoding="utf-8")
router = router_path.read_text(encoding="utf-8")

def require(condition, message):
    if not condition:
        raise SystemExit("TC metadata runtime-boundary assertion failed: " + message)

def function_body(source, signature):
    start = source.find(signature)
    require(start >= 0, "missing function " + signature)
    brace = source.find("{", start)
    require(brace >= 0, "missing body for " + signature)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    require(False, "unterminated body for " + signature)

require("tavrn_maintenance_metadata_attach_control(" in binding,
        "FULL binding does not attach metadata to routed controls")
require("tavrn_maintenance_metadata_receive_control(" in binding,
        "FULL binding does not consume admitted route-control metadata")
join_origin = function_body(mentorship, "static void originate_join(")
join_flush = function_body(mentorship, "static tavrn_mentorship_status_t flush_pending_join_obligation(")
require("tavrn_maintenance_tc_sequence_prepare(" in join_origin,
        "mentorship JOIN origin does not reserve the generic TC sequence")
require("tavrn_maintenance_tc_sequence_commit(" in join_flush,
        "mentorship JOIN enqueue does not commit the generic TC sequence")
require("next_join_sequence" not in mentorship,
        "mentorship still owns an independent JOIN sequence stream")
verified_departure = function_body(
    maintenance, "static tavrn_rreq_verification_status_t verification_checked_departure(")
verification_tick = function_body(
    maintenance, "tavrn_rreq_verification_status_t tavrn_maintenance_verification_owner_tick(")
retry_terminal = function_body(
    router, "static tavrn_router_event_status_t consume_owned_terminal(")
require("tavrn_maintenance_tc_on_verified_departure(" in verified_departure,
        "terminal verification checked departure is not a TC LEAVE source")
require("TAVRN_RREQ_VERIFICATION_WAIT_DIRECT_DEADLINE" in verification_tick and
        "tavrn_maintenance_tc_on_direct_timeout_departure(" in verification_tick,
        "direct-deadline checked departure is not a distinct TC LEAVE source")
require("TAVRN_LINK_EVENT_RETRY_EXHAUSTED" in retry_terminal and
        "tavrn_router_retry_exhausted_hook(" in retry_terminal and
        "failed_next_hop" in retry_terminal,
        "router RETRY_EXHAUSTED does not export failed-next-hop TC evidence")
require("tavrn_full_maintenance_tc_on_retry_exhausted(" in binding and
        "tavrn_maintenance_tc_on_retry_exhausted(" in binding,
        "FULL binding does not route retry exhaustion to TC maintenance")
tc_receive_case = function_body(mentorship, "tavrn_mentorship_handle_scheduler_event(")
require("if (mentorship->tc_metadata == NULL)" in tc_receive_case,
        "mentorship still owns inbound TC while FULL maintenance is installed")
expiry_sweep = function_body(maintenance, "tavrn_maintenance_sweep_expiry(")
require("tavrn_maintenance_tc_on_direct_timeout_departure(" in expiry_sweep,
        "ordinary direct timeout is not a TC LEAVE source")
for forbidden in (
    "TAVRN_TC_ORIGIN_NO_DEMAND_EXPIRY", "TAVRN_TC_ORIGIN_RREQ_EXHAUSTED",
    "TAVRN_TC_ORIGIN_BUSY", "TAVRN_TC_ORIGIN_REJECTED",
    "TAVRN_TC_ORIGIN_ENQUEUE_FAILURE", "TAVRN_TC_ORIGIN_ZERO_CHANNEL_FAILURE",
    "TAVRN_TC_ORIGIN_DEADLINE_EXPIRY", "TAVRN_TC_ORIGIN_RREP_ACK_TIMEOUT",
    "TAVRN_TC_ORIGIN_RADIO_FAULT", "TAVRN_TC_ORIGIN_SERVICE_FAULT",
    "TAVRN_TC_ORIGIN_REPAIR_FAILURE",
):
    require(forbidden not in maintenance + router + binding,
            "negative TC LEAVE cause is wired: " + forbidden)
require("#if TRON_BUILD_ROUTED_FULL_TAVRN" in main,
        "routed main has no FULL runtime boundary")
logger = function_body(main, "static void log_summary(")
mesh_scheduler = function_body(main, "static tavrn_router_phase_trace_t routed_cycle_router_scheduler_event(")
require("tavrn_maintenance_tc_metadata_telemetry(" in main and
        "routed tc_metadata origin_q=" in logger,
        "routed logger does not publish the compact copied TC/metadata record")
require("tm_printf" not in mesh_scheduler,
        "mesh scheduler path prints TC/metadata telemetry instead of the logger")
require(main.count("tavrn_maintenance_tc_metadata_telemetry(") == 1 and
        main.find("#if TRON_BUILD_ROUTED_FULL_TAVRN") <
        main.find("tavrn_maintenance_tc_metadata_telemetry("),
        "TC/metadata telemetry is not isolated behind the FULL runtime boundary")
PY
}

set +e
"${BUILD_DIR}/test_tavrn_phase5_tc_metadata" | tee "${BUILD_DIR}/${MODE}.log"
test_status=${PIPESTATUS[0]}
set -e

if [[ "${MODE}" == "green" ]]; then
    if [[ ${test_status} -eq 0 ]] && ! grep -q '^FAIL \|^STRUCTURAL ' "${BUILD_DIR}/${MODE}.log"; then
        if ! run_runtime_boundary_assertions; then
            exit 2
        fi
        printf '%s\n' 'TC metadata GREEN production assertions passed'
        exit 0
    fi
    if [[ ${test_status} -eq 1 ]] && ! grep -q '^STRUCTURAL ' "${BUILD_DIR}/${MODE}.log"; then
        printf '%s\n' 'TC metadata GREEN remains red: production policy is incomplete'
        exit 1
    fi
    printf 'TC metadata GREEN malformed result: exit=%d\n' "${test_status}" >&2
    exit 2
fi

expected_tags=(MAINT-05 MAINT-08 SERIAL-01 GTT-03 GTT-04 BUILD-01 META-01 META-02 META-03 META-04)
expected_reached=(
    confirmed-origin-shared-sequence
    tc-uuid-subject-ttl-relay
    shared-four-slot-priority-cooldown
    metadata-frame-budgets-no-truncation
    metadata-attribution-atomic-merge
)
mapfile -t actual_tags < <(grep '^FAIL ' "${BUILD_DIR}/red.log" | while IFS= read -r line; do
    tag="${line#FAIL }"
    printf '%s\n' "${tag%%:*}"
done)

if [[ ${test_status} -ne 1 ]] || grep -q '^STRUCTURAL ' "${BUILD_DIR}/red.log"; then
    printf 'TC metadata RED malformed result: exit=%d structural=%s\n' \
        "${test_status}" "$(grep -c '^STRUCTURAL ' "${BUILD_DIR}/red.log" || true)" >&2
    exit 2
fi
if [[ ${#actual_tags[@]} -ne ${#expected_tags[@]} ]]; then
    printf 'TC metadata RED emitted %d tags, expected %d\n' \
        "${#actual_tags[@]}" "${#expected_tags[@]}" >&2
    exit 2
fi
for index in "${!expected_tags[@]}"; do
    if [[ "${actual_tags[index]}" != "${expected_tags[index]}" ]]; then
        printf 'TC metadata RED tag %d was %s, expected %s\n' "${index}" \
            "${actual_tags[index]}" "${expected_tags[index]}" >&2
        exit 2
    fi
done
for reached in "${expected_reached[@]}"; do
    if [[ "$(grep -Fxc "REACHED ${reached}" "${BUILD_DIR}/red.log" || true)" != "1" ]]; then
        printf 'TC metadata RED did not reach scenario %s\n' "${reached}" >&2
        exit 2
    fi
done
if grep -q ': assertion failed: test_' "${BUILD_DIR}/red.log"; then
    printf '%s\n' 'TC metadata RED failure was attributed to a test wrapper, not policy' >&2
    exit 2
fi

CORRECTIVE_FLAGS=( "${BASE_FLAGS[@]}" -DTAVRN_PHASE5_TC_METADATA_CORRECTIVE_MODE )
if ! "${CC_BIN}" "${CORRECTIVE_FLAGS[@]}" "${PRODUCTION_SOURCES[@]}" \
        -o "${BUILD_DIR}/test_tavrn_phase5_tc_metadata_corrective"; then
    printf '%s\n' 'TC metadata corrective RED compile/link failed' >&2
    exit 2
fi

printf '%s\n' 'TC metadata corrective RED: running real production composition'
set +e
"${BUILD_DIR}/test_tavrn_phase5_tc_metadata_corrective" | tee "${BUILD_DIR}/corrective.log"
corrective_status=${PIPESTATUS[0]}
set -e

if [[ ${corrective_status} -ne 0 ]] || grep -q '^FAIL \|^STRUCTURAL ' "${BUILD_DIR}/corrective.log"; then
    printf 'TC metadata corrective production result malformed: exit=%d structural=%s failures=%s\n' \
        "${corrective_status}" "$(grep -c '^STRUCTURAL ' "${BUILD_DIR}/corrective.log" || true)" \
        "$(grep -c '^FAIL ' "${BUILD_DIR}/corrective.log" || true)" >&2
    exit 2
fi

printf '%s\n' 'TC metadata RED: 51 legacy assertions retained; production composition is green'
exit 1
