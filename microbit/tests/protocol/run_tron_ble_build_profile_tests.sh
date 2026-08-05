#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="$(mktemp -d)"
TOOLCHAIN="${MICROBIT_ROOT}/cmake/arm-none-eabi-gcc.cmake"
FIXTURES="${MICROBIT_ROOT}/tests/protocol/fixtures"

cleanup() {
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

configure_ok() {
    local name="$1"
    shift
    cmake -S "$MICROBIT_ROOT" -B "$WORK_DIR/$name" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        "$@" >"$WORK_DIR/$name.log" 2>&1
}

configure_fail() {
    local name="$1"
    shift
    if cmake -S "$MICROBIT_ROOT" -B "$WORK_DIR/$name" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        "$@" >"$WORK_DIR/$name.log" 2>&1; then
        printf 'expected configure failure: %s\n' "$name" >&2
        return 1
    fi
}

build_target() {
    cmake --build "$WORK_DIR/$1" --target "$2" --parallel >"$WORK_DIR/$1.build.log" 2>&1
}

require_line() {
    local expected="$1"
    local file="$2"
    if ! grep -Fqx "$expected" "$file"; then
        printf 'missing manifest line %s in %s\n' "$expected" "$file" >&2
        return 1
    fi
}

require_unique_keys() {
    local file="$1"
    local duplicate_keys
    duplicate_keys="$(cut -d= -f1 "$file" | LC_ALL=C sort | uniq -d)"
    if [[ -n "$duplicate_keys" ]]; then
        printf 'duplicate manifest keys in %s:\n%s\n' "$file" "$duplicate_keys" >&2
        return 1
    fi
}

manifest_value() {
    local key="$1"
    local file="$2"
    local line

    line="$(grep -F "${key}=" "$file")"
    if [[ -z "$line" || "$line" == *$'\n'* ]]; then
        printf 'expected exactly one manifest key %s in %s\n' "$key" "$file" >&2
        return 1
    fi
    printf '%s\n' "${line#*=}"
}

require_evidence_sidecar() {
    local evidence_key="$1"
    local out_dir="$2"
    local manifest="$3"
    local name
    local expected_sha
    local expected_size

    name="$(manifest_value "${evidence_key}.name" "$manifest")"
    expected_sha="$(manifest_value "${evidence_key}.sha256" "$manifest")"
    expected_size="$(manifest_value "${evidence_key}.size" "$manifest")"
    if [[ ! -s "$out_dir/$name" ]]; then
        printf 'missing or empty evidence sidecar %s\n' "$out_dir/$name" >&2
        return 1
    fi
    if [[ "$(sha256sum "$out_dir/$name" | cut -d' ' -f1)" != "$expected_sha" ]] ||
       [[ "$(wc -c < "$out_dir/$name")" != "$expected_size" ]]; then
        printf 'evidence sidecar hash or size mismatch: %s\n' "$name" >&2
        return 1
    fi
}

legacy_manifest_path() {
    printf '%s/firmware/ble_mesh_node/tron-build-config.manifest\n' "$WORK_DIR/$1"
}

link_manifest_path() {
    printf '%s/firmware/ble_link_v2_testbed/tron-build-config.manifest\n' "$WORK_DIR/$1"
}

valid_candidate_args=(
    -DTRON_PHASE1_TARGET=LINK
    -DTRON_HARDWARE_CANDIDATE=ON
    -DTRON_ADVA_OVERRIDE=18:42:de:52:4a:dd
    -DTRON_TARGET_PROBE_UID=board-a
    -DTRON_TARGET_INVENTORY_FILE="$FIXTURES/tavrn_inventory_valid.tsv"
)

# BUILD-P1-01: selected target isolates legacy source/identity composition.
configure_ok default-legacy -DTRON_PHASE1_TARGET=LEGACY
legacy_manifest="$(legacy_manifest_path default-legacy)"
require_line 'build.phase1_target=LEGACY' "$legacy_manifest"
require_line 'build.behavior=LEGACY_FLOOD' "$legacy_manifest"
require_line 'identity.adva=NOT_APPLICABLE' "$legacy_manifest"
require_line 'link_test.peer_adva=NOT_APPLICABLE' "$legacy_manifest"
if grep '^source\.selected\.[0-9].*=' "$legacy_manifest" | grep -q 'tavrn_\|ble_link_v2_testbed'; then
    printf '%s\n' 'legacy source manifest unexpectedly imports routed/link sources' >&2
    exit 1
fi
build_target default-legacy ble_mesh_node

# BUILD-P1-02: link harness has target-scoped runtime identity and buildable
# generated timer authority without a routed node feature level.
configure_ok runtime-link -DTRON_PHASE1_TARGET=LINK -DTRON_TIMER_PROFILE=FAST_TEST
runtime_manifest="$(link_manifest_path runtime-link)"
require_line 'build.phase1_target=LINK' "$runtime_manifest"
require_line 'build.node_mode.effective=NOT_APPLICABLE' "$runtime_manifest"
require_line 'identity.adva=RUNTIME_FICR' "$runtime_manifest"
require_line 'identity.sid16=RUNTIME_FICR' "$runtime_manifest"
require_line 'identity.sid8=RUNTIME_FICR' "$runtime_manifest"
require_line 'identity.width=SID16' "$runtime_manifest"
require_line 'link_test.peer_adva=NOT_CONFIGURED' "$runtime_manifest"
require_line 'timer.aodv_node_traversal_ms=10' "$runtime_manifest"
require_line 'timer.link_no_response_wall_bound_ms=840' "$runtime_manifest"
require_line 'capacity.link_custody.state=IMPLEMENTED' "$runtime_manifest"
require_line 'capacity.aodv_routes.state=NOT_IMPLEMENTED' "$runtime_manifest"
build_target runtime-link ble_link_v2_testbed
runtime_timer_source="$WORK_DIR/runtime-link/app/ble_link_v2_testbed/generated/ble_link_v2_testbed/tron_timer_config.c"
runtime_config_header="$WORK_DIR/runtime-link/app/ble_link_v2_testbed/generated/ble_link_v2_testbed/tron_build_config.h"
if ! grep -Fqx '    .aodv_node_traversal_ms = 10u,' "$runtime_timer_source" ||
   ! grep -Fqx '    .stats_ms = 1000u,' "$runtime_timer_source" ||
   ! grep -Fqx '    .link_hack_timeout_ms = 250u,' "$runtime_timer_source"; then
    printf '%s\n' 'generated FAST_TEST timer object does not match its manifest values' >&2
    exit 1
fi
if [[ ! -f "$runtime_config_header" ]] ||
   ! grep -Fq '#define TRON_BUILD_RUNTIME_CONFIG_EVIDENCE "poc=link_v2_harness' "$runtime_config_header" ||
   ! grep -Fq 'timer.scheduler_poll_max_ms=2' "$runtime_config_header" ||
   ! grep -Fq 'hook.hack_drop_count=0' "$runtime_config_header"; then
    printf '%s\n' 'generated runtime configuration lacks parseable harness evidence fields' >&2
    exit 1
fi

# BUILD-P1-03: effective legacy alias is compiled and cannot bypass hooks OFF.
configure_ok legacy-alias -DTRON_PHASE1_TARGET=LEGACY -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_NODE_BLOCK_DIRECT_PEER_ID=7
legacy_alias_manifest="$(legacy_manifest_path legacy-alias)"
require_line 'hook.legacy_alias_input=7' "$legacy_alias_manifest"
require_line 'hook.legacy_rx_block_peer_id=7' "$legacy_alias_manifest"
build_target legacy-alias ble_mesh_node
if ! grep -q -- '-DTRON_NODE_BLOCK_DIRECT_PEER_ID=7' "$WORK_DIR/legacy-alias/compile_commands.json"; then
    printf '%s\n' 'legacy effective hook alias was not propagated to compile definitions' >&2
    exit 1
fi
configure_fail hooks-off-alias -DTRON_PHASE1_TARGET=LEGACY \
    -DTRON_NODE_BLOCK_DIRECT_PEER_ID=7

# BUILD-P1-04: direct contamination is a configure-time error in both directions.
configure_fail legacy-routed-identity -DTRON_PHASE1_TARGET=LEGACY \
    -DTRON_ADVA_OVERRIDE=18:42:de:52:4a:dd
configure_fail legacy-routed-hook -DTRON_PHASE1_TARGET=LEGACY -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_TEST_BUSY_ADMISSION_COUNT=1
configure_fail legacy-candidate -DTRON_PHASE1_TARGET=LEGACY -DTRON_HARDWARE_CANDIDATE=ON
configure_fail link-legacy-node -DTRON_PHASE1_TARGET=LINK -DTRON_NODE_ID=7
configure_fail link-legacy-mode -DTRON_PHASE1_TARGET=LINK -DTRON_NODE_MODE=LEGACY_FLOOD
configure_fail link-legacy-hook -DTRON_PHASE1_TARGET=LINK -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_NODE_BLOCK_DIRECT_PEER_ID=7
configure_fail invalid-mode -DTRON_PHASE1_TARGET=LINK -DTRON_NODE_MODE=TAVRN_ROUTED
configure_fail invalid-aodv -DTRON_PHASE1_TARGET=LINK -DTAVRN_FEATURE_LEVEL=AODV_ONLY
configure_fail invalid-full -DTRON_PHASE1_TARGET=LINK -DTAVRN_FEATURE_LEVEL=FULL_TAVRN
configure_fail fake-repair -DTRON_PHASE1_TARGET=LINK -DTAVRN_ENABLE_LOCAL_REPAIR=ON
configure_fail fake-patient -DTRON_PHASE1_TARGET=LINK -DTRON_ENABLE_PATIENT_BRIDGE=ON
configure_fail invalid-network -DTRON_PHASE1_TARGET=LINK -DTRON_NETWORK_ID=0
configure_fail collision-unimplemented -DTRON_PHASE1_TARGET=LINK -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_TEST_COLLISION_PEER_ADVA=dc:4b:0a:06:03:f8

# BUILD-P1-05: candidate inventory validates every full AdvA and requires the
# selected Phase 1 SID16 namespace, while SID8 collision/reservation remains
# report-only until fixed-k is implemented.
configure_fail candidate-missing -DTRON_PHASE1_TARGET=LINK -DTRON_HARDWARE_CANDIDATE=ON
configure_fail inventory-malformed "${valid_candidate_args[@]}" \
    -DTRON_TARGET_INVENTORY_FILE="$FIXTURES/tavrn_inventory_malformed.tsv"
configure_fail inventory-data-whitespace "${valid_candidate_args[@]}" \
    -DTRON_TARGET_INVENTORY_FILE="$FIXTURES/tavrn_inventory_whitespace.tsv"
configure_fail inventory-duplicate-uid "${valid_candidate_args[@]}" \
    -DTRON_TARGET_INVENTORY_FILE="$FIXTURES/tavrn_inventory_duplicate_uid.tsv"
configure_fail inventory-duplicate-adva "${valid_candidate_args[@]}" \
    -DTRON_TARGET_INVENTORY_FILE="$FIXTURES/tavrn_inventory_duplicate_adva.tsv"
configure_fail inventory-duplicate-sid16 "${valid_candidate_args[@]}" \
    -DTRON_TARGET_INVENTORY_FILE="$FIXTURES/tavrn_inventory_duplicate_sid16.tsv"
configure_fail inventory-reserved-sid16 "${valid_candidate_args[@]}" \
    -DTRON_TARGET_INVENTORY_FILE="$FIXTURES/tavrn_inventory_reserved_sid16.tsv"
configure_fail inventory-malformed-random-static "${valid_candidate_args[@]}" \
    -DTRON_TARGET_INVENTORY_FILE="$FIXTURES/tavrn_inventory_malformed_random_static.tsv"
configure_fail inventory-mismatch -DTRON_PHASE1_TARGET=LINK -DTRON_HARDWARE_CANDIDATE=ON \
    -DTRON_ADVA_OVERRIDE=dc:4b:0a:06:03:f8 -DTRON_TARGET_PROBE_UID=board-a \
    -DTRON_TARGET_INVENTORY_FILE="$FIXTURES/tavrn_inventory_valid.tsv"
configure_ok candidate-inputs-valid "${valid_candidate_args[@]}"
candidate_manifest="$(link_manifest_path candidate-inputs-valid)"
require_line 'identity.inventory.record_count=2' "$candidate_manifest"
require_line 'identity.inventory.full_adva.unique=yes' "$candidate_manifest"
require_line 'identity.inventory.sid16.unique=yes' "$candidate_manifest"
require_line 'identity.inventory.sid16.nonreserved=yes' "$candidate_manifest"
require_line 'identity.inventory.sid8.unique=yes' "$candidate_manifest"
require_line 'identity.inventory.sid8.nonreserved=yes' "$candidate_manifest"
require_line 'identity.inventory.selected_width=SID16' "$candidate_manifest"
configure_ok candidate-hooked "${valid_candidate_args[@]}" -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_TEST_BUSY_ADMISSION_COUNT=1
hooked_candidate_manifest="$(link_manifest_path candidate-hooked)"
require_line 'candidate.scope=BENCH_HOOKED_RESTRICTED' "$hooked_candidate_manifest"
require_line 'hook.enabled=ON' "$hooked_candidate_manifest"
require_line 'hook.busy_admission_count=1' "$hooked_candidate_manifest"
configure_ok sid8-status -DTRON_PHASE1_TARGET=LINK -DTRON_HARDWARE_CANDIDATE=ON \
    -DTRON_ADVA_OVERRIDE=00:42:de:52:4a:dd -DTRON_TARGET_PROBE_UID=board-a \
    -DTRON_TARGET_INVENTORY_FILE="$FIXTURES/tavrn_inventory_sid8_status.tsv"
sid8_manifest="$(link_manifest_path sid8-status)"
require_line 'identity.sid8=0' "$sid8_manifest"
require_line 'identity.inventory.sid8.unique=no' "$sid8_manifest"
require_line 'identity.inventory.sid8.nonreserved=no' "$sid8_manifest"

# BUILD-P1-06: exact fixed-capacity/timer and generated source surfaces.
if grep '^source\.selected\.[0-9].*=' "$runtime_manifest" | grep -q 'tron_mesh_\|aodv_\|tavrn_full\|tavrn_gtt\|tavrn_repair'; then
    printf '%s\n' 'link source manifest imports legacy or future sources' >&2
    exit 1
fi
require_line 'source.selected.7=generated/tron_timer_config.c' "$runtime_manifest"
require_line 'timer.radio_tx_event_bound_ms=8' "$runtime_manifest"
require_line 'timer.link_tx_scheduler_attempt_bound_ms=30' "$runtime_manifest"
require_line 'timer.link_response_window_sum_ms=750' "$runtime_manifest"
require_line 'formula.link_no_response_wall_bound_ms=timer.link_response_window_sum_ms+timer.link_max_attempts*timer.link_tx_scheduler_attempt_bound_ms' "$runtime_manifest"
link_testbed_main="$MICROBIT_ROOT/app/ble_link_v2_testbed/src/main.c"
if ! grep -Fqx '    config.hack_turnaround_ms = tron_timer_config.radio_tx_event_bound_ms;' \
    "$link_testbed_main" ||
   grep -Fq 'timer.hack_turnaround' "$runtime_manifest"; then
    printf '%s\n' 'link HACK turnaround must source the existing radio bound without a timer key' >&2
    exit 1
fi
if [[ "$(grep -Fxc '    record_valid_rf_rx(event);' "$link_testbed_main")" -ne 1 ]] ||
   ! awk '
       /^static void handle_scheduler_event\(/ { in_handler = 1 }
       /^static void maybe_submit_diagnostic_data\(/ { in_handler = 0 }
       in_handler && /^    if \(event == NULL\) \{/ { saw_null_guard = 1 }
       in_handler && saw_null_guard && /^    \}/ { null_guard_complete = 1 }
       in_handler && /^    record_valid_rf_rx\(event\);$/ {
           record_line = NR
           record_after_null_guard = null_guard_complete
       }
       in_handler && /event->type == BLE_MESH_SCHED_EVENT_RADIO_FAULT/ {
           fault_line = NR
       }
       in_handler && /TRON_BUILD_RX_BLOCK_ADVA_ENABLED/ { rx_block_line = NR }
       in_handler && /^    capture_duplicate_hack\(event, &capture\);$/ {
           hack_drop_line = NR
       }
       END {
           exit !(record_line && record_after_null_guard &&
                  record_line < fault_line && record_line < rx_block_line &&
                  record_line < hack_drop_line)
       }
   ' "$link_testbed_main"; then
    printf '%s\n' 'link testbed RF telemetry must record once at the RX boundary before hooks' >&2
    exit 1
fi
if ! grep -Fqx '    link_testbed_rf_telemetry_record_rx_event(&rf_telemetry, &config, event);' \
    "$link_testbed_main"; then
    printf '%s\n' 'link testbed RX boundary must use the pure telemetry classifier' >&2
    exit 1
fi
if ! awk '
        /^LOCAL void link_mesh_task\(/ { in_task = 1 }
        /^LOCAL void link_logger_task\(/ { in_task = 0 }
        in_task && /^        uint32_t poll_return;$/ { declaration_line = NR }
        in_task && /^                \(void\)ble_mesh_scheduler_poll\(/ {
            poll_line = NR
        }
        in_task && /^                poll_return = now_ms\(\);$/ {
            return_line = NR
        }
        in_task && /^                link_testbed_poll_gate_complete\(&poll_gate, poll_return\);$/ {
            gate_line = NR
        }
        in_task && /^                    handle_scheduler_event\(&scheduler_event, poll_return\);$/ {
            event_line = NR
        }
        END {
            exit !(declaration_line && poll_line && return_line && gate_line &&
                   event_line && poll_line < return_line &&
                   return_line < gate_line && gate_line < event_line)
        }
    ' "$link_testbed_main" ||
   grep -Fq 'handle_scheduler_event(&scheduler_event, poll_start)' "$link_testbed_main"; then
    printf '%s\n' 'link testbed must dispatch poll events using the post-poll timestamp' >&2
    exit 1
fi
if ! awk '
        /^LOCAL void link_mesh_task\(/ { in_task = 1 }
        /^LOCAL void link_logger_task\(/ { in_task = 0 }
        in_task && /^    uint32_t start_rx_return;$/ { startup_declaration = NR }
        in_task && /^    link_testbed_poll_gate_init\(&poll_gate\);$/ {
            startup_init = NR
        }
        in_task && /^    diagnostic_push_simple\(LINK_TEST_DIAG_BOOT, now_ms\(\), 0u\);$/ {
            startup_boot = NR
        }
        in_task && /^    ble_mesh_scheduler_start_rx\(&link_scheduler, now_ms\(\)\);$/ {
            startup_rx = NR
        }
        in_task && /^    start_rx_return = now_ms\(\);$/ { startup_return = NR }
        in_task && /^    link_testbed_poll_gate_complete\(&poll_gate, start_rx_return\);$/ {
            startup_complete = NR
        }
        in_task && /^    while \(1\) {$/ { loop_line = NR }
        in_task && /tavrn_link_v2_dispatch\(&link_instance, now_ms\(\), &link_event\)/ {
            dispatch_line = NR
        }
        in_task && /^        yield_delay = mesh_yield_delay_ms;$/ {
            max_yield_line = NR
        }
        in_task && /^        if \(mesh_fault_latched == 0u && poll_gate.have_poll_return != 0u\) {$/ {
            healthy_line = NR
        }
        in_task && /^            yield_delay = link_testbed_mesh_remaining_yield_ms\($/ {
            remaining_line = NR
        }
        in_task && /^                poll_gate.last_poll_return_ms, now_ms\(\),$/ {
            remaining_args = NR
        }
        in_task && /^                tron_timer_config.scheduler_poll_max_ms\);$/ {
            remaining_budget = NR
        }
        in_task && /^        if \(yield_delay != 0u\) {$/ { delay_guard = NR }
        in_task && /^            \(void\)tk_dly_tsk\(yield_delay\);$/ {
            delay_call = NR
        }
        END {
            exit !(startup_declaration && startup_init && startup_boot &&
                   startup_rx && startup_return && startup_complete &&
                   loop_line && startup_declaration < startup_init &&
                   startup_init < startup_boot && startup_boot < startup_rx &&
                   startup_rx < startup_return &&
                   startup_return < startup_complete &&
                   startup_complete < loop_line && dispatch_line &&
                   max_yield_line && healthy_line && remaining_line &&
                   remaining_args == remaining_line + 1 &&
                   remaining_budget == remaining_line + 2 && delay_guard &&
                   delay_call && dispatch_line < max_yield_line &&
                   max_yield_line < healthy_line &&
                   healthy_line < remaining_line &&
                   remaining_budget < delay_guard && delay_guard < delay_call)
        }
    ' "$link_testbed_main" ||
   ! grep -Fq 'mesh_yield_max_ms=%lu mesh_yield_policy=remaining_slack' \
        "$link_testbed_main" ||
   grep -Fq '(void)tk_dly_tsk(mesh_yield_delay_ms);' "$link_testbed_main"; then
    printf '%s\n' 'link testbed must yield only unconsumed healthy-cycle poll slack' >&2
    exit 1
fi
if grep -Fq 'TAVRN_LINK_V2_HOST_TEST_IMMEDIATE_HACK' \
    "$WORK_DIR/runtime-link/compile_commands.json"; then
    printf '%s\n' 'firmware compile commands must not contain the host-only immediate HACK macro' >&2
    exit 1
fi
if [[ "$(grep -c '^timer\.' "$runtime_manifest")" -ne 71 ]] ||
   [[ "$(grep -c '^capacity\.[^.]*=' "$runtime_manifest")" -ne 29 ]]; then
    printf '%s\n' 'manifest timer/capacity schema width is not exact' >&2
    exit 1
fi
require_unique_keys "$runtime_manifest"

# BUILD-P1-07: the development publisher emits sorted, unique, post-link
# provenance.  --candidate alone cannot make an artifact eligible, and this
# dirty worktree must fail the exact candidate exit status.
publisher_out="$WORK_DIR/published"
bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target ble_link_v2_testbed \
    --timer FAST_TEST --out "$publisher_out" >/dev/null
published_manifest=()
for manifest_path in "$publisher_out"/*.manifest; do
    [[ "$manifest_path" == *.build-config.manifest ]] && continue
    published_manifest+=("$manifest_path")
done
if [[ ${#published_manifest[@]} -ne 1 ]]; then
    printf '%s\n' 'development publisher did not create exactly one manifest' >&2
    exit 1
fi
LC_ALL=C sort -c "${published_manifest[0]}"
require_unique_keys "${published_manifest[0]}"
published_artifact_name="$(manifest_value artifact.name "${published_manifest[0]}")"
for expected in \
    'candidate.eligible=no' \
    'candidate.hardware_purpose=no' \
    'candidate.clean_source=no' \
    'candidate.unhooked_acceptance=no' \
    'candidate.hook_bench_eligible=no' \
    'identity.adva=RUNTIME_FICR' \
    'manifest.schema=1' \
    'source.submodule.count=1' \
    'source.submodule.0.path=libs/mtkernel_3' \
    'source.submodule.0.state=clean' \
    'wire.routed.version=2' \
    'build.c_flags=EXACT_PUBLISHED_NINJA_COMMANDS' \
    'build.asm_flags=EXACT_PUBLISHED_NINJA_COMMANDS' \
    'build.link_flags=EXACT_PUBLISHED_NINJA_COMMANDS' \
    'capacity.aodv_routes.state=NOT_IMPLEMENTED'; do
    require_line "$expected" "${published_manifest[0]}"
done
require_line "build.c_flags.evidence=${published_artifact_name}.ninja-commands.txt" "${published_manifest[0]}"
require_line "build.asm_flags.evidence=${published_artifact_name}.ninja-commands.txt" "${published_manifest[0]}"
require_line "build.link_flags.evidence=${published_artifact_name}.ninja-commands.txt" "${published_manifest[0]}"
require_line "build.compile_commands.path=${published_artifact_name}.compile_commands.json" "${published_manifest[0]}"
require_line "build.link_command.evidence=${published_artifact_name}.ninja-commands.txt" "${published_manifest[0]}"
require_line "build.ninja_file.path=${published_artifact_name}.build.ninja" "${published_manifest[0]}"
require_line "build.target_config_header.path=${published_artifact_name}.build-config.h" "${published_manifest[0]}"
require_line "build.target_config_manifest.path=${published_artifact_name}.build-config.manifest" "${published_manifest[0]}"
require_line "evidence.ninja_commands.name=${published_artifact_name}.ninja-commands.txt" "${published_manifest[0]}"
require_line "evidence.compile_commands.name=${published_artifact_name}.compile_commands.json" "${published_manifest[0]}"
require_line "evidence.build_ninja.name=${published_artifact_name}.build.ninja" "${published_manifest[0]}"
require_line "evidence.cmake_cache.name=${published_artifact_name}.cmake-cache.txt" "${published_manifest[0]}"
require_line "evidence.target_config_manifest.name=${published_artifact_name}.build-config.manifest" "${published_manifest[0]}"
require_line "evidence.target_config_header.name=${published_artifact_name}.build-config.h" "${published_manifest[0]}"
for evidence_key in \
    evidence.ninja_commands \
    evidence.compile_commands \
    evidence.build_ninja \
    evidence.cmake_cache \
    evidence.target_config_manifest \
    evidence.target_config_header; do
    require_evidence_sidecar "$evidence_key" "$publisher_out" "${published_manifest[0]}"
done
commands_sidecar="${publisher_out}/$(manifest_value evidence.ninja_commands.name "${published_manifest[0]}")"
compile_commands_sidecar="${publisher_out}/$(manifest_value evidence.compile_commands.name "${published_manifest[0]}")"
cmake_cache_sidecar="${publisher_out}/$(manifest_value evidence.cmake_cache.name "${published_manifest[0]}")"
config_manifest_sidecar="${publisher_out}/$(manifest_value evidence.target_config_manifest.name "${published_manifest[0]}")"
config_header_sidecar="${publisher_out}/$(manifest_value evidence.target_config_header.name "${published_manifest[0]}")"
if ! grep -Fq -- '-mcpu=cortex-m4' "$commands_sidecar" ||
   ! grep -Fq -- '-mfloat-abi=hard' "$commands_sidecar" ||
   ! grep -Fq -- '-include tk/tkernel.h' "$commands_sidecar" ||
   ! grep -Fq -- 'tkernel_map.ld' "$commands_sidecar" ||
   ! grep -Fq -- '-include tk/tkernel.h' "$compile_commands_sidecar"; then
    printf '%s\n' 'published command evidence lacks effective CPU/ABI/include/linker options' >&2
    exit 1
fi
if ! grep -Fq 'CMAKE_TOOLCHAIN_FILE:FILEPATH=' "$cmake_cache_sidecar" ||
   ! grep -Fqx 'manifest.schema=1' "$config_manifest_sidecar" ||
   ! grep -Fq 'TRON_BUILD_RUNTIME_CONFIG_EVIDENCE' "$config_header_sidecar"; then
    printf '%s\n' 'published CMake/config evidence is incomplete' >&2
    exit 1
fi
if ! grep -Eq '^artifact\.elf\.sha256=[0-9a-f]{64}$' "${published_manifest[0]}" ||
   ! grep -Eq '^source\.selected\.sha256=[0-9a-f]{64}$' "${published_manifest[0]}" ||
   ! grep -Eq '^build\.compile_commands\.sha256=[0-9a-f]{64}$' "${published_manifest[0]}"; then
    printf '%s\n' 'publisher manifest lacks required post-link/source/command hash fields' >&2
    exit 1
fi
set +e
bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target ble_link_v2_testbed \
    --candidate --adva 18:42:de:52:4a:dd --probe-uid board-a \
    --inventory "$FIXTURES/tavrn_inventory_valid.tsv" --out "$publisher_out" \
    >/dev/null 2>&1
candidate_status=$?
set -e
if [[ $candidate_status -ne 1 ]]; then
    printf 'dirty candidate publisher exit status=%s, expected 1\n' "$candidate_status" >&2
    exit 1
fi

printf '%s\n' 'tron BLE build/profile tests passed'
