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

if [[ $# -ne 1 || ( "$1" != "--red" && "$1" != "--green" &&
                       "$1" != "--adaptive-red" && "$1" != "--adaptive-green" &&
                       "$1" != "--expiry-red" && "$1" != "--expiry-host-green" &&
                       "$1" != "--expiry-green" ) ]]; then
    printf 'usage: %s --red|--green|--adaptive-red|--adaptive-green|--expiry-red|--expiry-host-green|--expiry-green\n' "$0" >&2
    exit 2
fi
MODE="${1#--}"
MAIN_SOURCE="${MICROBIT_ROOT}/app/tavrn_routed_node/src/main.c"
FULL_BINDING_SOURCE="${MICROBIT_ROOT}/app/protocol/tavrn_full_maintenance_binding.c"
FULL_BINDING_HEADER="${MICROBIT_ROOT}/app/protocol/tavrn_full_maintenance_binding.h"
ROUTED_CYCLE_SOURCE="${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_cycle.c"
ROUTED_CYCLE_HEADER="${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_cycle.h"
FULL_PROFILE_SOURCE="${MICROBIT_ROOT}/app/ble_mesh_node/cmake/TronBleProfiles.cmake"
RESOURCE_CHECKER="${MICROBIT_ROOT}/scripts/check_tavrn_expiry_resources.py"
RESOURCE_BUILD_SCRIPT="${MICROBIT_ROOT}/build-tavrn-ble.sh"
BUILD_PROFILE_RUNNER="${MICROBIT_ROOT}/tests/protocol/run_tron_ble_build_profile_tests.sh"
RESOURCE_FIXTURES="${MICROBIT_ROOT}/tests/protocol/fixtures/tavrn_expiry_resources"
RESOURCE_REQUIRED_STACK_EDGES="${RESOURCE_FIXTURES}/required-stack-edges.json"
RESOURCE_BASELINE_FIXTURE="${RESOURCE_FIXTURES}/baseline.json"
RESOURCE_PASS_FIXTURE="${RESOURCE_FIXTURES}/pass.json"
RESOURCE_FAIL_STACK_FIXTURE="${RESOURCE_FIXTURES}/fail-stack.json"
RESOURCE_FAIL_HEADROOM_FIXTURE="${RESOURCE_FIXTURES}/fail-headroom.json"
RESOURCE_FAIL_RAM_FIXTURE="${RESOURCE_FIXTURES}/fail-ram.json"
RESOURCE_FAIL_DELTA_FIXTURE="${RESOURCE_FIXTURES}/fail-delta.json"
RESOURCE_FAIL_CAPTURE_FIXTURE="${RESOURCE_FIXTURES}/fail-capture.json"
RESOURCE_BALANCED_BASELINE_FIXTURE="${RESOURCE_FIXTURES}/balanced-baseline.json"
RESOURCE_BALANCED_PASS_FIXTURE="${RESOURCE_FIXTURES}/balanced-pass.json"
RESOURCE_BALANCED_FAIL_STACK_FIXTURE="${RESOURCE_FIXTURES}/balanced-fail-stack.json"
RESOURCE_BALANCED_FAIL_HEADROOM_FIXTURE="${RESOURCE_FIXTURES}/balanced-fail-headroom.json"
RESOURCE_BALANCED_FAIL_RAM_FIXTURE="${RESOURCE_FIXTURES}/balanced-fail-ram.json"
RESOURCE_BALANCED_FAIL_DELTA_FIXTURE="${RESOURCE_FIXTURES}/balanced-fail-delta.json"
RESOURCE_BALANCED_FAIL_CAPTURE_FIXTURE="${RESOURCE_FIXTURES}/balanced-fail-capture.json"

COMMON_FLAGS=(
    -std=c99
    -Wall
    -Wextra
    -Werror
    -DBLE_RADIO_HOST_TEST
    -I"${MICROBIT_ROOT}/app/drivers"
    -I"${MICROBIT_ROOT}/app/protocol"
    -I"${MICROBIT_ROOT}/app/tavrn_routed_node/src"
    -I"${MICROBIT_ROOT}/tests/protocol/support"
    -I"${MICROBIT_ROOT}/tests/protocol/red_support"
)
TEST_SOURCE="${MICROBIT_ROOT}/tests/protocol/test_tavrn_phase5_maintenance.c"
EXPIRY_TEST_SOURCE="${MICROBIT_ROOT}/tests/protocol/test_tavrn_phase5_expiry_demand.c"
CONTRACT_HEADER="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase5_maintenance_contract.h"
EXPIRY_CONTRACT_HEADER="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase5_expiry_demand_contract.h"
EXPIRY_ADAPTER=""
COMMON_SOURCES=(
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
    "${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_full_telemetry.c"
)

if [[ "${MODE}" == "expiry-red" ]]; then
    TEST_SOURCE="${EXPIRY_TEST_SOURCE}"
    COMMON_FLAGS+=(
        -DTAVRN_PHASE5_EXPIRY_RED_MODE
        "-DTAVRN_PHASE5_MENTORSHIP_SOURCE_PATH=\"${MICROBIT_ROOT}/app/protocol/tavrn_mentorship.c\""
        "-DTAVRN_PHASE5_FULL_BINDING_SOURCE_PATH=\"${FULL_BINDING_SOURCE}\""
        "-DTAVRN_PHASE5_ROUTED_MAIN_PATH=\"${MAIN_SOURCE}\""
        "-DTAVRN_PHASE5_ROUTED_CYCLE_SOURCE_PATH=\"${ROUTED_CYCLE_SOURCE}\""
        "-DTAVRN_PHASE5_ROUTED_CYCLE_HEADER_PATH=\"${ROUTED_CYCLE_HEADER}\""
        "-DTAVRN_PHASE5_FULL_PROFILE_SOURCE_PATH=\"${FULL_PROFILE_SOURCE}\""
        "-DTAVRN_PHASE5_RESOURCE_CHECKER_PATH=\"${RESOURCE_CHECKER}\""
        "-DTAVRN_PHASE5_RESOURCE_BUILD_SCRIPT_PATH=\"${RESOURCE_BUILD_SCRIPT}\""
        "-DTAVRN_PHASE5_BUILD_PROFILE_RUNNER_PATH=\"${BUILD_PROFILE_RUNNER}\""
        "-DTAVRN_PHASE5_EXPIRY_RUNNER_PATH=\"${MICROBIT_ROOT}/tests/protocol/run_tavrn_phase5_maintenance_tests.sh\""
        "-DTAVRN_PHASE5_EXPIRY_SUMMARIZER_PATH=\"${MICROBIT_ROOT}/scripts/summarize_tavrn_expiry_capture.py\""
    )
    MAINTENANCE_SOURCE="${MICROBIT_ROOT}/app/protocol/tavrn_maintenance.c"
    EXPIRY_ADAPTER="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase5_expiry_demand_red_backend.c"
elif [[ "${MODE}" == "expiry-host-green" || "${MODE}" == "expiry-green" ]]; then
    TEST_SOURCE="${EXPIRY_TEST_SOURCE}"
    # This compiles the available production slice.  Each fallback is scoped to
    # its own future API marker, so implemented GTT/demand symbols are real.
    COMMON_FLAGS+=(
        -DTAVRN_MAINTENANCE_API
        "-DTAVRN_PHASE5_MENTORSHIP_SOURCE_PATH=\"${MICROBIT_ROOT}/app/protocol/tavrn_mentorship.c\""
        "-DTAVRN_PHASE5_FULL_BINDING_SOURCE_PATH=\"${FULL_BINDING_SOURCE}\""
        "-DTAVRN_PHASE5_ROUTED_MAIN_PATH=\"${MAIN_SOURCE}\""
        "-DTAVRN_PHASE5_ROUTED_CYCLE_SOURCE_PATH=\"${ROUTED_CYCLE_SOURCE}\""
        "-DTAVRN_PHASE5_ROUTED_CYCLE_HEADER_PATH=\"${ROUTED_CYCLE_HEADER}\""
        "-DTAVRN_PHASE5_FULL_PROFILE_SOURCE_PATH=\"${FULL_PROFILE_SOURCE}\""
        "-DTAVRN_PHASE5_RESOURCE_CHECKER_PATH=\"${RESOURCE_CHECKER}\""
        "-DTAVRN_PHASE5_RESOURCE_BUILD_SCRIPT_PATH=\"${RESOURCE_BUILD_SCRIPT}\""
        "-DTAVRN_PHASE5_BUILD_PROFILE_RUNNER_PATH=\"${BUILD_PROFILE_RUNNER}\""
        "-DTAVRN_PHASE5_EXPIRY_RUNNER_PATH=\"${MICROBIT_ROOT}/tests/protocol/run_tavrn_phase5_maintenance_tests.sh\""
        "-DTAVRN_PHASE5_EXPIRY_SUMMARIZER_PATH=\"${MICROBIT_ROOT}/scripts/summarize_tavrn_expiry_capture.py\""
    )
    MAINTENANCE_SOURCE="${MICROBIT_ROOT}/app/protocol/tavrn_maintenance.c"
    EXPIRY_ADAPTER="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase5_expiry_demand_red_backend.c"
    # The binding remains the final all-symbol acceptance marker.
    if [[ -f "${FULL_BINDING_SOURCE}" && -f "${FULL_BINDING_HEADER}" ]]; then
        COMMON_FLAGS+=( -DTAVRN_MAINTENANCE_EXPIRY_DEMAND_API )
    fi
elif [[ "${MODE}" == "adaptive-red" ]]; then
    # RED remains an assertion-level absence proof against the frozen seam.
    COMMON_FLAGS+=(
        -DTAVRN_MAINTENANCE_ADAPTIVE_CONTRACT
        "-DTAVRN_PHASE5_FULL_BINDING_SOURCE_PATH=\"${FULL_BINDING_SOURCE}\""
        "-DTAVRN_PHASE5_ROUTED_MAIN_PATH=\"${MAIN_SOURCE}\""
    )
    MAINTENANCE_SOURCE="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase5_maintenance_red_backend.c"
elif [[ "${MODE}" == "adaptive-green" ]]; then
    # GREEN compiles the production maintenance API and behavior directly.
    COMMON_FLAGS+=(
        -DTAVRN_MAINTENANCE_ADAPTIVE_CONTRACT
        -DTAVRN_MAINTENANCE_API
        "-DTAVRN_PHASE5_FULL_BINDING_SOURCE_PATH=\"${FULL_BINDING_SOURCE}\""
        "-DTAVRN_PHASE5_ROUTED_MAIN_PATH=\"${MAIN_SOURCE}\""
    )
    MAINTENANCE_SOURCE="${MICROBIT_ROOT}/app/protocol/tavrn_maintenance.c"
elif [[ "${MODE}" == "green" &&
      -f "${MICROBIT_ROOT}/app/protocol/tavrn_maintenance.c" &&
      -f "${MICROBIT_ROOT}/app/protocol/tavrn_maintenance.h" ]]; then
    COMMON_FLAGS+=( -DTAVRN_MAINTENANCE_API )
    MAINTENANCE_SOURCE="${MICROBIT_ROOT}/app/protocol/tavrn_maintenance.c"
else
    MAINTENANCE_SOURCE="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase5_maintenance_red_backend.c"
fi
SOURCES=("${TEST_SOURCE}" "${COMMON_SOURCES[@]}")
if [[ -n "${MAINTENANCE_SOURCE}" ]]; then
    SOURCES+=("${MAINTENANCE_SOURCE}")
fi
if [[ -n "${EXPIRY_ADAPTER}" ]]; then
    SOURCES+=("${EXPIRY_ADAPTER}")
fi
# Future FULL-only source-list plumbing: once the production binding exists,
# expiry GREEN compiles it directly.  RED deliberately keeps its unique
# unavailable alias and never links this production source.
if [[ ( "${MODE}" == "expiry-host-green" || "${MODE}" == "expiry-green" ) &&
      -f "${FULL_BINDING_SOURCE}" ]]; then
    SOURCES+=("${FULL_BINDING_SOURCE}")
fi

REQUIRED_SOURCES=("${CONTRACT_HEADER}" "${SOURCES[@]}")
if [[ "${MODE}" == "expiry-red" || "${MODE}" == "expiry-host-green" ||
      "${MODE}" == "expiry-green" ]]; then
    REQUIRED_SOURCES+=("${EXPIRY_CONTRACT_HEADER}")
fi
for source in "${REQUIRED_SOURCES[@]}"; do
    if [[ ! -f "${source}" ]]; then
        printf 'Phase 5 maintenance %s setup unavailable: expected source %s\n' \
            "${MODE}" "${source#"${MICROBIT_ROOT}/"}" >&2
        exit 2
    fi
done

if ! "${CC_BIN}" "${COMMON_FLAGS[@]}" "${SOURCES[@]}" \
        -o "${BUILD_DIR}/test_tavrn_phase5_maintenance"; then
    printf 'Phase 5 maintenance %s compile failed\n' "${MODE}" >&2
    exit 2
fi

set +e
"${BUILD_DIR}/test_tavrn_phase5_maintenance" | tee "${BUILD_DIR}/${MODE}.log"
test_status=${PIPESTATUS[0]}
set -e

failure_count="$(grep -c '^FAIL ' "${BUILD_DIR}/${MODE}.log" || true)"
expected_requirements=(MAINT-02 GTT-03 SERIAL-03 BEARER-04)
expected_summary='tavrn_phase5_maintenance RED tests failed: 4 assertion(s)'

expiry_result_is_exact() {
    local expected_requirements=(GTT-02 GTT-03 GTT-05 MAINT-01 MAINT-02 MAINT-04 MAINT-05)
    local actual_requirements=()
    local expected_summary='tavrn_phase5_expiry_demand RED tests failed: 7 assertion(s)'
    local fail_line
    local index

    if [[ "${MODE}" == "expiry-host-green" || "${MODE}" == "expiry-green" ]]; then
        expected_summary='tavrn_phase5_expiry_demand GREEN tests failed: 7 assertion(s)'
    fi

    if [[ ${test_status} -ne 1 || "${failure_count}" != "7" ]]; then
        return 1
    fi
    while IFS= read -r fail_line; do
        if [[ "${fail_line}" =~ ^FAIL[[:space:]]+([^:]+): ]]; then
            actual_requirements+=("${BASH_REMATCH[1]}")
        fi
    done < <(grep '^FAIL ' "${BUILD_DIR}/${MODE}.log" || true)
    if [[ ${#actual_requirements[@]} -ne ${#expected_requirements[@]} ]]; then
        return 1
    fi
    for index in "${!expected_requirements[@]}"; do
        if [[ "${actual_requirements[index]}" != "${expected_requirements[index]}" ]]; then
            return 1
        fi
    done
    [[ "$(grep -Fxc "${expected_summary}" "${BUILD_DIR}/${MODE}.log" || true)" == "1" ]]
}

expiry_green_partial_result_is_exact() {
    local expected_requirements=(MAINT-01 MAINT-02 MAINT-04 MAINT-05)
    local actual_requirements=()
    local fail_line
    local index

    if [[ ${test_status} -ne 1 || "${failure_count}" != "4" ]]; then
        return 1
    fi
    while IFS= read -r fail_line; do
        if [[ "${fail_line}" =~ ^FAIL[[:space:]]+([^:]+): ]]; then
            actual_requirements+=("${BASH_REMATCH[1]}")
        fi
    done < <(grep '^FAIL ' "${BUILD_DIR}/${MODE}.log" || true)
    if [[ ${#actual_requirements[@]} -ne ${#expected_requirements[@]} ]]; then
        return 1
    fi
    for index in "${!expected_requirements[@]}"; do
        if [[ "${actual_requirements[index]}" != "${expected_requirements[index]}" ]]; then
            return 1
        fi
    done
    [[ "$(grep -Fxc 'tavrn_phase5_expiry_demand GREEN tests failed: 4 assertion(s)' \
        "${BUILD_DIR}/${MODE}.log" || true)" == "1" ]]
}

acceptance_report_value() {
    local key="$1"
    local report="$2"
    local line=""

    while IFS= read -r line; do
        if [[ "$line" == "${key}="* ]]; then
            printf '%s\n' "${line#*=}"
            return 0
        fi
    done < "$report"
    return 1
}

selected_source_inventory_hash() {
    local source_manifest="$1"
    local relative_path=""
    local digest_file="${BUILD_DIR}/selected-source-digests.txt"

    : > "$digest_file"
    while IFS= read -r relative_path; do
        local source_digest=""

        [[ -z "$relative_path" ]] && continue
        if [[ "$relative_path" == /* || ! -f "${MICROBIT_ROOT}/${relative_path}" ]]; then
            return 1
        fi
        source_digest="$(sha256sum "${MICROBIT_ROOT}/${relative_path}" | cut -d' ' -f1)"
        printf '%s  %s\n' "$source_digest" "$relative_path" >> "$digest_file"
    done < "$source_manifest"
    [[ -s "$digest_file" ]] || return 1
    sha256sum "$digest_file" | cut -d' ' -f1
}

require_binding_symbol_contract() {
    local elf="$1"
    local expected_count="$2"
    local count

    count="$(arm-none-eabi-nm --defined-only "$elf" | \
        grep -Ec '[[:space:]]tavrn_full_maintenance_binding_tick$' || true)"
    [[ "$count" == "$expected_count" ]]
}

require_binding_callback_call() {
    local elf="$1"
    local count

    count="$(arm-none-eabi-objdump -d --disassemble=routed_cycle_router_tick "$elf" |
        grep -Ec '[[:space:]]bl([[:space:]]|\.w).*<tavrn_full_maintenance_binding_tick>' || true)"
    [[ "$count" == "1" ]]
}

require_no_binding_reference() {
    local elf="$1"
    local count

    count="$(arm-none-eabi-objdump -d "$elf" |
        grep -Fc '<tavrn_full_maintenance_binding_tick>' || true)"
    [[ "$count" == "0" ]]
}

run_expiry_firmware_acceptance() {
    local report="${BUILD_DIR}/expiry-firmware-acceptance.env"
    local full_fast_elf full_fast_map full_fast_manifest full_fast_sources full_fast_commands full_fast_su_glob full_fast_disassembly full_fast_config
    local full_balanced_elf full_balanced_map full_balanced_manifest full_balanced_sources full_balanced_commands full_balanced_su_glob full_balanced_disassembly full_balanced_config
    local aodv_elf aodv_map aodv_manifest aodv_sources aodv_commands aodv_disassembly
    local expected_hash actual_hash fixture
    local baseline_dir="${TAVRN_EXPIRY_BASELINE_DIR:?TAVRN_EXPIRY_BASELINE_DIR is required after host and firmware acceptance}"
    local fast_before_map="${baseline_dir}/fast.before.map"
    local fast_baseline_manifest="${baseline_dir}/fast.baseline.manifest"
    local fast_baseline_hash="${baseline_dir}/fast.baseline.sha256"
    local balanced_before_map="${baseline_dir}/balanced.before.map"
    local balanced_baseline_manifest="${baseline_dir}/balanced.baseline.manifest"
    local balanced_baseline_hash="${baseline_dir}/balanced.baseline.sha256"
    local capture_manifest="${TAVRN_EXPIRY_ARM_CAPTURE_MANIFEST:?TAVRN_EXPIRY_ARM_CAPTURE_MANIFEST is required after host and firmware acceptance}"
    local capture_artifact_manifest="${TAVRN_EXPIRY_ARM_CAPTURE_ARTIFACT_MANIFEST:?TAVRN_EXPIRY_ARM_CAPTURE_ARTIFACT_MANIFEST is required with the capture manifest}"

    # Future build-profile mode creates fresh temporary builds and writes only
    # returned paths plus selected-source inventory hashes into this report.
    "$BUILD_PROFILE_RUNNER" --expiry-resource-acceptance-report "$report" \
        --expiry-resource-fixtures "$RESOURCE_FIXTURES"
    [[ -s "$report" ]] || return 1

    full_fast_elf="$(acceptance_report_value full_fast.elf "$report")"
    full_fast_map="$(acceptance_report_value full_fast.map "$report")"
    full_fast_manifest="$(acceptance_report_value full_fast.manifest "$report")"
    full_fast_sources="$(acceptance_report_value full_fast.selected_sources "$report")"
    full_fast_commands="$(acceptance_report_value full_fast.compile_commands "$report")"
    full_fast_su_glob="$(acceptance_report_value full_fast.su_glob "$report")"
    full_fast_disassembly="$(acceptance_report_value full_fast.disassembly "$report")"
    full_fast_config="$(acceptance_report_value full_fast.config_header "$report")"
    full_balanced_elf="$(acceptance_report_value full_balanced.elf "$report")"
    full_balanced_map="$(acceptance_report_value full_balanced.map "$report")"
    full_balanced_manifest="$(acceptance_report_value full_balanced.manifest "$report")"
    full_balanced_sources="$(acceptance_report_value full_balanced.selected_sources "$report")"
    full_balanced_commands="$(acceptance_report_value full_balanced.compile_commands "$report")"
    full_balanced_su_glob="$(acceptance_report_value full_balanced.su_glob "$report")"
    full_balanced_disassembly="$(acceptance_report_value full_balanced.disassembly "$report")"
    full_balanced_config="$(acceptance_report_value full_balanced.config_header "$report")"
    aodv_elf="$(acceptance_report_value aodv_only.elf "$report")"
    aodv_map="$(acceptance_report_value aodv_only.map "$report")"
    aodv_manifest="$(acceptance_report_value aodv_only.manifest "$report")"
    aodv_sources="$(acceptance_report_value aodv_only.selected_sources "$report")"
    aodv_commands="$(acceptance_report_value aodv_only.compile_commands "$report")"
    aodv_disassembly="$(acceptance_report_value aodv_only.disassembly "$report")"
    [[ -f "$full_fast_elf" && -f "$full_fast_map" && -f "$full_fast_manifest" &&
       -f "$full_fast_sources" && -f "$full_fast_commands" && -f "$full_fast_disassembly" && -f "$full_fast_config" && -f "$full_balanced_elf" && -f "$full_balanced_map" &&
       -f "$full_balanced_manifest" && -f "$full_balanced_sources" && -f "$full_balanced_commands" && -f "$full_balanced_disassembly" && -f "$full_balanced_config" &&
       -f "$aodv_elf" && -f "$aodv_map" && -f "$aodv_manifest" && -f "$aodv_sources" &&
       -f "$aodv_commands" && -f "$aodv_disassembly" &&
       -f "$fast_before_map" && -f "$fast_baseline_manifest" && -f "$fast_baseline_hash" &&
       -f "$balanced_before_map" && -f "$balanced_baseline_manifest" && -f "$balanced_baseline_hash" &&
       -f "$RESOURCE_REQUIRED_STACK_EDGES" ]] ||
        return 1
    shopt -s globstar
    compgen -G "$full_fast_su_glob" > /dev/null || return 1
    compgen -G "$full_balanced_su_glob" > /dev/null || return 1
    shopt -u globstar
    require_binding_symbol_contract "$full_fast_elf" 1 &&
        require_binding_symbol_contract "$full_balanced_elf" 1 &&
        require_binding_symbol_contract "$aodv_elf" 0 &&
        require_binding_callback_call "$full_fast_elf" &&
        require_binding_callback_call "$full_balanced_elf" &&
        require_no_binding_reference "$aodv_elf" || return 1

    expected_hash="$(acceptance_report_value full_fast.source_inventory_sha256 "$report")"
    actual_hash="$(selected_source_inventory_hash "$full_fast_sources")"
    [[ -n "$expected_hash" && "$actual_hash" == "$expected_hash" ]] || return 1
    expected_hash="$(acceptance_report_value full_balanced.source_inventory_sha256 "$report")"
    actual_hash="$(selected_source_inventory_hash "$full_balanced_sources")"
    [[ -n "$expected_hash" && "$actual_hash" == "$expected_hash" ]] || return 1
    expected_hash="$(acceptance_report_value aodv_only.source_inventory_sha256 "$report")"
    actual_hash="$(selected_source_inventory_hash "$aodv_sources")"
    [[ -n "$expected_hash" && "$actual_hash" == "$expected_hash" ]] || return 1

    for fixture in "$RESOURCE_BASELINE_FIXTURE" "$RESOURCE_PASS_FIXTURE"; do
        python3 "$RESOURCE_CHECKER" --fixture "$fixture" --full-elf "$full_fast_elf" \
            --full-map "$full_fast_map" --full-manifest "$full_fast_manifest" \
            --before-map "$fast_before_map" --after-map "$full_fast_map" \
            --baseline-manifest "$fast_baseline_manifest" --baseline-sha256 "$fast_baseline_hash" --selected-sources "$full_fast_sources" \
            --su-glob "$full_fast_su_glob" --stack-root routed_mesh_task \
            --required-edge-manifest "$RESOURCE_REQUIRED_STACK_EDGES" \
            --resolve-operation-edge 'routed_cycle_operations.router_scheduler_event=routed_cycle_router_scheduler_event' \
            --resolve-operation-edge 'routed_cycle_operations.router_tick=routed_cycle_router_tick' \
            --disassembly "$full_fast_disassembly" \
            --compile-commands "$full_fast_commands" --config-header "$full_fast_config" --main-source "$MAIN_SOURCE" \
            --preprocessed-main-out "${BUILD_DIR}/full-fast-main.i" \
            --aodv-elf "$aodv_elf" --aodv-map "$aodv_map" --aodv-manifest "$aodv_manifest" \
            --aodv-compile-commands "$aodv_commands" --aodv-disassembly "$aodv_disassembly" --require-binding-call
    done
    for fixture in "$RESOURCE_FAIL_STACK_FIXTURE" "$RESOURCE_FAIL_HEADROOM_FIXTURE" \
                   "$RESOURCE_FAIL_RAM_FIXTURE" "$RESOURCE_FAIL_DELTA_FIXTURE" \
                   "$RESOURCE_FAIL_CAPTURE_FIXTURE"; do
        if python3 "$RESOURCE_CHECKER" --fixture "$fixture" --expect-fail \
            --full-elf "$full_fast_elf" --full-map "$full_fast_map" \
            --full-manifest "$full_fast_manifest" --compile-commands "$full_fast_commands" \
            --before-map "$fast_before_map" --after-map "$full_fast_map" \
            --baseline-manifest "$fast_baseline_manifest" --baseline-sha256 "$fast_baseline_hash" --selected-sources "$full_fast_sources" \
            --su-glob "$full_fast_su_glob" --stack-root routed_mesh_task \
            --required-edge-manifest "$RESOURCE_REQUIRED_STACK_EDGES" \
            --resolve-operation-edge 'routed_cycle_operations.router_scheduler_event=routed_cycle_router_scheduler_event' \
            --resolve-operation-edge 'routed_cycle_operations.router_tick=routed_cycle_router_tick' \
            --disassembly "$full_fast_disassembly" \
            --config-header "$full_fast_config" --main-source "$MAIN_SOURCE" --preprocessed-main-out "${BUILD_DIR}/full-fast-main.i" \
            --aodv-elf "$aodv_elf" --aodv-map "$aodv_map" --aodv-manifest "$aodv_manifest" \
            --aodv-compile-commands "$aodv_commands" --aodv-disassembly "$aodv_disassembly" --require-binding-call; then
            return 1
        fi
    done
    for fixture in "$RESOURCE_BALANCED_BASELINE_FIXTURE" "$RESOURCE_BALANCED_PASS_FIXTURE"; do
        python3 "$RESOURCE_CHECKER" --fixture "$fixture" --timer BALANCED \
            --full-elf "$full_balanced_elf" \
            --full-map "$full_balanced_map" --full-manifest "$full_balanced_manifest" \
            --before-map "$balanced_before_map" --after-map "$full_balanced_map" \
            --baseline-manifest "$balanced_baseline_manifest" --baseline-sha256 "$balanced_baseline_hash" --selected-sources "$full_balanced_sources" \
            --su-glob "$full_balanced_su_glob" --stack-root routed_mesh_task \
            --required-edge-manifest "$RESOURCE_REQUIRED_STACK_EDGES" \
            --resolve-operation-edge 'routed_cycle_operations.router_scheduler_event=routed_cycle_router_scheduler_event' \
            --resolve-operation-edge 'routed_cycle_operations.router_tick=routed_cycle_router_tick' \
            --disassembly "$full_balanced_disassembly" \
            --compile-commands "$full_balanced_commands" --config-header "$full_balanced_config" --main-source "$MAIN_SOURCE" \
            --preprocessed-main-out "${BUILD_DIR}/full-balanced-main.i" \
            --aodv-elf "$aodv_elf" --aodv-map "$aodv_map" --aodv-manifest "$aodv_manifest" \
            --aodv-compile-commands "$aodv_commands" --aodv-disassembly "$aodv_disassembly" --require-binding-call
    done
    for fixture in "$RESOURCE_BALANCED_FAIL_STACK_FIXTURE" \
                   "$RESOURCE_BALANCED_FAIL_HEADROOM_FIXTURE" \
                   "$RESOURCE_BALANCED_FAIL_RAM_FIXTURE" \
                   "$RESOURCE_BALANCED_FAIL_DELTA_FIXTURE" \
                   "$RESOURCE_BALANCED_FAIL_CAPTURE_FIXTURE"; do
        if python3 "$RESOURCE_CHECKER" --fixture "$fixture" --timer BALANCED \
            --expect-fail \
            --full-elf "$full_balanced_elf" --full-map "$full_balanced_map" \
            --full-manifest "$full_balanced_manifest" --compile-commands "$full_balanced_commands" \
            --before-map "$balanced_before_map" --after-map "$full_balanced_map" \
            --baseline-manifest "$balanced_baseline_manifest" --baseline-sha256 "$balanced_baseline_hash" --selected-sources "$full_balanced_sources" \
            --su-glob "$full_balanced_su_glob" --stack-root routed_mesh_task \
            --required-edge-manifest "$RESOURCE_REQUIRED_STACK_EDGES" \
            --resolve-operation-edge 'routed_cycle_operations.router_scheduler_event=routed_cycle_router_scheduler_event' \
            --resolve-operation-edge 'routed_cycle_operations.router_tick=routed_cycle_router_tick' \
            --disassembly "$full_balanced_disassembly" \
            --config-header "$full_balanced_config" --main-source "$MAIN_SOURCE" --preprocessed-main-out "${BUILD_DIR}/full-balanced-main.i" \
            --aodv-elf "$aodv_elf" --aodv-map "$aodv_map" --aodv-manifest "$aodv_manifest" \
            --aodv-compile-commands "$aodv_commands" --aodv-disassembly "$aodv_disassembly" --require-binding-call; then
            return 1
        fi
    done
    [[ -f "$capture_manifest" && -f "$capture_artifact_manifest" ]] || return 1
    python3 "$RESOURCE_CHECKER" --fixture "$RESOURCE_PASS_FIXTURE" \
        --hardware-capture "$capture_manifest" \
        --capture-artifact-manifest "$capture_artifact_manifest" \
        --full-elf "$full_fast_elf" \
        --full-map "$full_fast_map" --full-manifest "$full_fast_manifest" \
        --before-map "$fast_before_map" --after-map "$full_fast_map" \
        --baseline-manifest "$fast_baseline_manifest" --baseline-sha256 "$fast_baseline_hash" --selected-sources "$full_fast_sources" \
        --su-glob "$full_fast_su_glob" --stack-root routed_mesh_task \
        --required-edge-manifest "$RESOURCE_REQUIRED_STACK_EDGES" \
        --resolve-operation-edge 'routed_cycle_operations.router_scheduler_event=routed_cycle_router_scheduler_event' \
        --resolve-operation-edge 'routed_cycle_operations.router_tick=routed_cycle_router_tick' \
        --disassembly "$full_fast_disassembly" \
        --compile-commands "$full_fast_commands" --config-header "$full_fast_config" --main-source "$MAIN_SOURCE" \
        --preprocessed-main-out "${BUILD_DIR}/full-fast-main.i" \
        --aodv-elf "$aodv_elf" --aodv-map "$aodv_map" \
        --aodv-manifest "$aodv_manifest" --aodv-compile-commands "$aodv_commands" \
        --aodv-disassembly "$aodv_disassembly" --require-binding-call
}

if [[ "${MODE}" == "expiry-red" ]]; then
    if ! expiry_result_is_exact; then
        printf 'Phase 5 expiry RED requires the exact ordered seven-tag assertion result; got exit=%d fail=%s\n' \
            "${test_status}" "${failure_count}" >&2
        exit 2
    fi
    printf '%s\n' 'Phase 5 expiry RED absence proven: 7 expected assertion failures'
    exit 0
fi

if [[ "${MODE}" == "expiry-host-green" ]]; then
    if [[ ${test_status} -eq 0 && "${failure_count}" == "0" ]]; then
        python3 "${MICROBIT_ROOT}/tests/protocol/test_summarize_tavrn_expiry_capture.py"
        printf '%s\n' 'Phase 5 expiry host GREEN production suite passed'
        exit 0
    fi
    printf 'Phase 5 expiry host GREEN requires all production assertions; got exit=%d fail=%s\n' \
        "${test_status}" "${failure_count}" >&2
    exit 2
fi

if [[ "${MODE}" == "expiry-green" ]]; then
    if [[ ${test_status} -eq 0 && "${failure_count}" == "0" ]]; then
        if ! run_expiry_firmware_acceptance; then
            printf '%s\n' 'Phase 5 expiry GREEN firmware/resource acceptance failed' >&2
            exit 2
        fi
        exit 0
    fi
    if expiry_result_is_exact; then
        printf '%s\n' 'Phase 5 expiry GREEN remains red: local expiry/demand production behavior is absent'
        exit 1
    fi
    if expiry_green_partial_result_is_exact; then
        printf '%s\n' 'Phase 5 expiry GREEN partial: production GTT/demand checks pass; maintenance/binding tags MAINT-01, MAINT-02, MAINT-04, and MAINT-05 remain unavailable'
        exit 1
    fi
    printf 'Phase 5 expiry GREEN malformed result: exit=%d fail=%s\n' \
        "${test_status}" "${failure_count}" >&2
    exit 2
fi

if [[ "${MODE}" == "adaptive-red" || "${MODE}" == "adaptive-green" ]]; then
    expected_requirements=(MAINT-02)
    expected_summary='tavrn_phase5_maintenance ADAPTIVE RED tests failed: 1 assertion(s)'
    if [[ "${MODE}" == "adaptive-red" ]]; then
        if [[ ${test_status} -ne 1 || "${failure_count}" != "1" ]] ||
           [[ "$(grep -c '^FAIL MAINT-02:' "${BUILD_DIR}/${MODE}.log" || true)" != "1" ]] ||
           [[ "$(grep -Fxc "${expected_summary}" "${BUILD_DIR}/${MODE}.log" || true)" != "1" ]]; then
            printf 'Phase 5 adaptive RED requires exactly one MAINT-02 assertion failure; got exit=%d fail=%s\n' \
                "${test_status}" "${failure_count}" >&2
            exit 2
        fi
        printf '%s\n' 'Phase 5 adaptive RED absence proven: 1 expected MAINT-02 assertion failure'
        exit 0
    fi
    if [[ ${test_status} -eq 0 && "${failure_count}" == "0" ]]; then
        exit 0
    fi
    if [[ ${test_status} -eq 1 && "${failure_count}" == "1" ]] &&
       [[ "$(grep -Fxc "${expected_summary}" "${BUILD_DIR}/${MODE}.log" || true)" == "1" ]]; then
        printf '%s\n' 'Phase 5 adaptive GREEN remains red: adaptive cadence production behavior is absent'
        exit 1
    fi
    printf 'Phase 5 adaptive GREEN malformed result: exit=%d fail=%s\n' \
        "${test_status}" "${failure_count}" >&2
    exit 2
fi

if [[ "${MODE}" == "red" ]]; then
    if [[ ${test_status} -ne 1 || "${failure_count}" != "4" ]]; then
        printf 'Phase 5 maintenance RED requires exactly four assertion failures; got exit=%d fail=%s\n' \
            "${test_status}" "${failure_count}" >&2
        exit 2
    fi
    for requirement in "${expected_requirements[@]}"; do
        if [[ "$(grep -c "^FAIL ${requirement}:" "${BUILD_DIR}/${MODE}.log" || true)" != "1" ]]; then
            printf 'Phase 5 maintenance RED requires one exact %s failure\n' \
                "${requirement}" >&2
            exit 2
        fi
    done
    if [[ "$(grep -Fxc "${expected_summary}" "${BUILD_DIR}/${MODE}.log" || true)" != "1" ]]; then
        printf '%s\n' 'Phase 5 maintenance RED missing exact final assertion summary' >&2
        exit 2
    fi
    printf '%s\n' 'Phase 5 maintenance RED absence proven: 4 expected assertion failures'
    exit 0
fi

if [[ ${test_status} -eq 0 && "${failure_count}" == "0" ]]; then
    if [[ "$(grep -c 'tavrn_maintenance_handle_rx_control(' "${MAIN_SOURCE}" || true)" != "1" ]] ||
       [[ "$(grep -c '&mentorship_trace.rx_control' "${MAIN_SOURCE}" || true)" != "1" ]]; then
        printf '%s\n' 'Phase 5 production binding must dispatch exactly one copied mentorship RX control to maintenance' >&2
        exit 2
    fi
    exit 0
fi
if [[ ${test_status} -eq 1 && "${failure_count}" == "4" ]] &&
   [[ "$(grep -Fxc "${expected_summary}" "${BUILD_DIR}/${MODE}.log" || true)" == "1" ]]; then
    printf '%s\n' 'Phase 5 maintenance GREEN remains red: fixed-cadence HELLO production behavior is absent' >&2
    exit 1
fi
printf 'Phase 5 maintenance GREEN malformed result: exit=%d fail=%s\n' \
    "${test_status}" "${failure_count}" >&2
exit 2
