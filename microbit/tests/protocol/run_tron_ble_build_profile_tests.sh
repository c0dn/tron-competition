#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="$(mktemp -d)"
TOOLCHAIN="${MICROBIT_ROOT}/cmake/arm-none-eabi-gcc.cmake"
FIXTURES="${MICROBIT_ROOT}/tests/protocol/fixtures"
EXPIRY_REPORT=""
EXPIRY_FIXTURES=""

if [[ $# -ne 0 ]]; then
    if [[ $# -ne 4 || "$1" != "--expiry-resource-acceptance-report" ||
          "$3" != "--expiry-resource-fixtures" ]]; then
        printf 'usage: %s [--expiry-resource-acceptance-report PATH --expiry-resource-fixtures DIR]\n' \
            "$0" >&2
        exit 2
    fi
    EXPIRY_REPORT="$2"
    EXPIRY_FIXTURES="$4"
    if [[ -e "$EXPIRY_REPORT" || ! -d "$EXPIRY_FIXTURES" ]]; then
        printf '%s\n' 'expiry resource report must be fresh and fixtures must be a directory' >&2
        exit 2
    fi
fi

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

configure_fail_with() {
    local name="$1"
    local expected="$2"
    shift 2
    configure_fail "$name" "$@"
    if ! grep -Fq "$expected" "$WORK_DIR/$name.log"; then
        printf 'configure failure %s lacks expected diagnostic: %s\n' \
            "$name" "$expected" >&2
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

require_timer_profile_surface() {
    local profile="$1"
    local manifest="$2"
    local timer_source="$3"
    local verification_window_ms="$4"

    require_line "build.timer_profile=${profile}" "$manifest"
    require_line 'timer.aodv_rreq_retries=2' "$manifest"
    require_line 'timer.freshness_response_min_ms=10' "$manifest"
    require_line 'timer.freshness_response_max_ms=100' "$manifest"
    require_line "timer.verification_window_ms=${verification_window_ms}" "$manifest"
    if [[ "$(grep -c '^timer\.' "$manifest")" -ne 73 ]]; then
        printf '%s timer manifest key count is not exactly 73\n' "$profile" >&2
        return 1
    fi
    if ! grep -Fqx '    .aodv_rreq_retries = 2u,' "$timer_source" ||
       ! grep -Fqx '    .freshness_response_min_ms = 10u,' "$timer_source" ||
       ! grep -Fqx '    .freshness_response_max_ms = 100u,' "$timer_source" ||
       ! grep -Fqx "    .verification_window_ms = ${verification_window_ms}u," "$timer_source"; then
        printf '%s generated timer object does not match its manifest values\n' \
            "$profile" >&2
        return 1
    fi
}

manifest_value() {
    local key="$1"
    local file="$2"
    local line=""
    local candidate=""
    local count=0

    while IFS= read -r candidate || [[ -n "$candidate" ]]; do
        if [[ "$candidate" == "${key}="* ]]; then
            line="$candidate"
            ((count += 1))
        fi
    done < "$file"
    if [[ $count -ne 1 ]]; then
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

routed_manifest_path() {
    printf '%s/firmware/tavrn_routed_node/tron-build-config.manifest\n' "$WORK_DIR/$1"
}

expiry_manifest_path() {
    local out_dir="$1"
    local manifests=()
    local candidate

    for candidate in "$out_dir"/*.manifest; do
        [[ -f "$candidate" ]] || continue
        [[ "$candidate" == *.build-config.manifest ]] && continue
        manifests+=("$candidate")
    done
    if [[ ${#manifests[@]} -ne 1 ]]; then
        printf 'expected one published artifact manifest in %s\n' "$out_dir" >&2
        return 1
    fi
    printf '%s\n' "${manifests[0]}"
}

expiry_manifest_field_path() {
    local out_dir="$1"
    local manifest="$2"
    local key="$3"
    local name

    name="$(manifest_value "$key" "$manifest")"
    if [[ ! -f "$out_dir/$name" ]]; then
        printf 'expiry evidence %s is missing from %s\n' "$name" "$out_dir" >&2
        return 1
    fi
    printf '%s\n' "$out_dir/$name"
}

expiry_su_hash() {
    python3 - "$1" <<'PY'
import glob
import hashlib
import pathlib
import sys

paths = [pathlib.Path(path) for path in sorted(glob.glob(sys.argv[1], recursive=True))]
if not paths:
    raise SystemExit(1)
root = pathlib.Path(sys.argv[1].split("**", 1)[0]).resolve()
records = "".join(
    f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.resolve().relative_to(root).as_posix()}\n"
    for path in paths)
print(hashlib.sha256(records.encode("utf-8")).hexdigest())
PY
}

run_expiry_fixture_checks() {
    local checker="${MICROBIT_ROOT}/scripts/check_tavrn_expiry_resources.py"
    local fixture status
    local passing=(baseline.json pass.json balanced-baseline.json balanced-pass.json)
    # Every threshold-failing fixture must reach its own named checker failure.
    local failing=(fail-stack.json fail-headroom.json fail-ram.json fail-delta.json fail-capture.json
                   fail-heap.json fail-binding.json
                   balanced-fail-stack.json balanced-fail-headroom.json balanced-fail-ram.json
                   balanced-fail-delta.json balanced-fail-capture.json
                   preprocessed-main-inactive.json preprocessed-main-unguarded.json)
    local malformed=(malformed.json missing-key.json duplicate-key.json)

    for fixture in stack-max-bytes.json declared-leaf-pass.json unknown-leaf-fail.json \
                    map-vector-overlap.json sibling-operation-edges.json \
                    sibling-operation-heavier.json; do
        python3 "${MICROBIT_ROOT}/tests/protocol/test_tavrn_expiry_resources.py" \
            "$EXPIRY_FIXTURES/$fixture"
    done
    for fixture in "${passing[@]}"; do
        python3 "$checker" --fixture "$EXPIRY_FIXTURES/$fixture" >/dev/null
    done
    for fixture in "${failing[@]}"; do
        set +e
        python3 "$checker" --fixture "$EXPIRY_FIXTURES/$fixture" --expect-fail >/dev/null 2>&1
        status=$?
        set -e
        if [[ $status -ne 1 ]]; then
            printf 'threshold-failing fixture %s exit=%s, expected 1\n' "$fixture" "$status" >&2
            return 1
        fi
    done
    for fixture in "${malformed[@]}"; do
        set +e
        python3 "$checker" --fixture "$EXPIRY_FIXTURES/$fixture" >/dev/null 2>&1
        status=$?
        set -e
        if [[ $status -ne 2 ]]; then
            printf 'malformed fixture %s exit=%s, expected 2\n' "$fixture" "$status" >&2
            return 1
        fi
    done
}

run_expiry_resource_acceptance() {
    local evidence_dir report_tmp baseline_dir checker
    local full_fast_dir full_fast_hook_dir full_balanced_dir aodv_dir
    local full_fast_manifest full_fast_hook_manifest full_balanced_manifest aodv_manifest
    local full_fast_name full_balanced_name aodv_name
    local full_fast_elf full_fast_map full_fast_resource full_fast_sources full_fast_commands
    local full_fast_su full_fast_disassembly full_fast_config full_fast_stack_gate full_fast_gate
    local full_fast_stack_log full_fast_baseline_of_record full_fast_before_inventory
    local full_fast_after_inventory full_fast_inventory_drift full_fast_stack_chain
    local full_balanced_elf full_balanced_map full_balanced_resource full_balanced_sources
    local full_balanced_commands full_balanced_su full_balanced_disassembly full_balanced_config
    local full_balanced_stack_gate full_balanced_gate full_balanced_stack_log
    local full_balanced_baseline_of_record full_balanced_before_inventory
    local full_balanced_after_inventory full_balanced_inventory_drift full_balanced_stack_chain
    local aodv_elf aodv_map aodv_sources aodv_commands aodv_disassembly aodv_config
    local full_fast_elf_hash full_fast_map_hash full_fast_manifest_hash full_fast_resource_hash
    local full_fast_sources_hash full_fast_source_inventory_hash full_fast_commands_hash
    local full_fast_su_hash full_fast_disassembly_hash full_fast_config_hash full_fast_log_hash
    local full_fast_stack_log_hash
    local full_balanced_elf_hash full_balanced_map_hash full_balanced_manifest_hash full_balanced_resource_hash
    local full_balanced_sources_hash full_balanced_source_inventory_hash full_balanced_commands_hash
    local full_balanced_su_hash full_balanced_disassembly_hash full_balanced_config_hash full_balanced_log_hash
    local full_balanced_stack_log_hash
    local aodv_elf_hash aodv_map_hash aodv_manifest_hash aodv_sources_hash
    local aodv_source_inventory_hash aodv_commands_hash aodv_disassembly_hash aodv_config_hash

    require_report_file() {
        local path="$1"
        local expected_hash="$2"

        [[ -s "$path" ]] || return 1
        [[ "$(sha256sum "$path" | cut -d' ' -f1)" == "$expected_hash" ]]
    }

    acceptance_log_value() {
        local key="$1"
        local file="$2"
        local line=""
        local candidate=""
        local count=0

        while IFS= read -r candidate || [[ -n "$candidate" ]]; do
            if [[ "$candidate" == "${key}="* ]]; then
                line="$candidate"
                ((count += 1))
            fi
        done < "$file"
        [[ $count -eq 1 ]] || return 1
        printf '%s\n' "${line#*=}"
    }

    run_stack_baseline_gate() {
        local profile="$1"
        local resource="$2"
        local elf="$3"
        local map="$4"
        local manifest="$5"
        local sources="$6"
        local commands="$7"
        local su_glob="$8"
        local disassembly="$9"
        local config="${10}"
        local baseline="${11}"
        local baseline_map="${12}"
        local baseline_hash="${13}"
        local output="${evidence_dir}/${profile}.stack-baseline.log"

        python3 "$checker" --stack-baseline-only --resource-manifest "$resource" \
            --full-elf "$elf" --full-map "$map" --full-manifest "$manifest" \
            --before-map "$baseline_map" --baseline-manifest "$baseline" \
            --baseline-sha256 "$baseline_hash" --selected-sources "$sources" \
            --su-glob "$su_glob" --stack-root routed_mesh_task \
            --required-edge-manifest "$EXPIRY_FIXTURES/required-stack-edges.json" \
            --resolve-operation-edge 'routed_cycle_operations.router_scheduler_event=routed_cycle_router_scheduler_event' \
            --resolve-operation-edge 'routed_cycle_operations.router_tick=routed_cycle_router_tick' \
            --disassembly "$disassembly" --compile-commands "$commands" \
            --config-header "$config" >"$output" 2>&1
        [[ "$(acceptance_log_value BASELINE_OF_RECORD "$output")" ]] || return 1
        [[ "$(acceptance_log_value BEFORE_SOURCE_INVENTORY_SHA256 "$output")" =~ ^[0-9a-f]{64}$ ]] || return 1
        [[ "$(acceptance_log_value AFTER_SOURCE_INVENTORY_SHA256 "$output")" =~ ^[0-9a-f]{64}$ ]] || return 1
        [[ "$(acceptance_log_value SOURCE_INVENTORY_DRIFT "$output")" =~ ^(yes|no)$ ]] || return 1
        [[ "$(<"$output")" == *"PASS "* ]] || return 1
        printf '%s\n' STACK_BASELINE_PASSED
    }

    run_live_resource_gate() {
        local profile="$1"
        local resource="$2"
        local elf="$3"
        local map="$4"
        local manifest="$5"
        local sources="$6"
        local commands="$7"
        local su_glob="$8"
        local disassembly="$9"
        local config="${10}"
        local baseline="${11}"
        local baseline_map="${12}"
        local baseline_hash="${13}"
        local output="${evidence_dir}/${profile}.resource-gate.log"
        local status binding_present capture_artifact_manifest="${14:-}"
        local capture_args=()

        if [[ -n "$capture_artifact_manifest" ]]; then
            capture_args=(--capture-artifact-manifest "$capture_artifact_manifest")
        fi

        binding_present="$(arm-none-eabi-nm --defined-only "$elf" | \
            python3 -c 'import sys; print("yes" if any(line.rstrip().endswith(" tavrn_full_maintenance_binding_tick") for line in sys.stdin) else "no")')"
        set +e
        python3 "$checker" --resource-manifest "$resource" --full-elf "$elf" --full-map "$map" \
            --full-manifest "$manifest" --before-map "$baseline_map" \
            --baseline-manifest "$baseline" --baseline-sha256 "$baseline_hash" \
            --selected-sources "$sources" --su-glob "$su_glob" --stack-root routed_mesh_task \
            --required-edge-manifest "$EXPIRY_FIXTURES/required-stack-edges.json" \
            --resolve-operation-edge 'routed_cycle_operations.router_scheduler_event=routed_cycle_router_scheduler_event' \
            --resolve-operation-edge 'routed_cycle_operations.router_tick=routed_cycle_router_tick' \
            --disassembly "$disassembly" --compile-commands "$commands" --config-header "$config" \
            --main-source "$MICROBIT_ROOT/app/tavrn_routed_node/src/main.c" \
            --preprocessed-main-out "${evidence_dir}/${profile}.main.i" \
             --aodv-elf "$aodv_elf" --aodv-disassembly "$aodv_disassembly" \
             --require-binding-call "${capture_args[@]}" >"$output" 2>&1
        status=$?
        set -e
        if [[ "$binding_present" == yes ]]; then
            [[ $status -eq 0 ]] || return 1
            printf '%s\n' PASSED
            return 0
        fi
        if [[ $status -eq 1 && "$(<"$output")" == *"FAIL binding:"* ]]; then
            printf '%s\n' EXPECTED_BINDING_ABSENT
            return 0
        fi
        return 1
    }

    evidence_dir="$(mktemp -d "${TMPDIR:-/tmp}/tron-expiry-resource-acceptance.XXXXXX")"
    checker="${MICROBIT_ROOT}/scripts/check_tavrn_expiry_resources.py"
    baseline_dir="${TAVRN_EXPIRY_BASELINE_DIR:-/tmp/opencode/tavrn-expiry-baseline}"
    [[ -f "$baseline_dir/fast.before.map" && -f "$baseline_dir/fast.baseline.manifest" &&
       -f "$baseline_dir/fast.baseline.sha256" && -f "$baseline_dir/balanced.before.map" &&
       -f "$baseline_dir/balanced.baseline.manifest" &&
       -f "$baseline_dir/balanced.baseline.sha256" ]] || {
        printf 'immutable expiry baseline is incomplete: %s\n' "$baseline_dir" >&2
        return 1
    }
    full_fast_dir="$evidence_dir/full-fast"
    full_fast_hook_dir="$evidence_dir/full-fast-hook"
    full_balanced_dir="$evidence_dir/full-balanced"
    aodv_dir="$evidence_dir/aodv-only"
    bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target tavrn_routed_node \
        --feature FULL_TAVRN --timer FAST_TEST --stack-usage \
        --resource-baseline "$baseline_dir/fast.baseline.manifest" \
        --out "$full_fast_dir" >/dev/null
    bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target tavrn_routed_node \
        --feature FULL_TAVRN --timer FAST_TEST --stack-usage --enable-hooks ON \
        --expiry-full-table ON --out "$full_fast_hook_dir" >/dev/null
    bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target tavrn_routed_node \
        --feature FULL_TAVRN --timer BALANCED --stack-usage \
        --resource-baseline "$baseline_dir/balanced.baseline.manifest" \
        --out "$full_balanced_dir" >/dev/null
    bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target tavrn_routed_node \
        --feature AODV_ONLY --timer FAST_TEST --out "$aodv_dir" >/dev/null
    full_fast_manifest="$(expiry_manifest_path "$full_fast_dir")"
    full_fast_hook_manifest="$(expiry_manifest_path "$full_fast_hook_dir")"
    full_balanced_manifest="$(expiry_manifest_path "$full_balanced_dir")"
    aodv_manifest="$(expiry_manifest_path "$aodv_dir")"
    full_fast_name="$(manifest_value artifact.name "$full_fast_manifest")"
    full_balanced_name="$(manifest_value artifact.name "$full_balanced_manifest")"
    aodv_name="$(manifest_value artifact.name "$aodv_manifest")"
    require_line 'hook.enabled=ON' "$full_fast_hook_manifest" &&
        require_line 'hook.expiry_full_table=ON' "$full_fast_hook_manifest" || return 1
    full_fast_elf="$full_fast_dir/${full_fast_name}.elf"
    full_fast_map="$full_fast_dir/${full_fast_name}.map"
    full_fast_resource="$(expiry_manifest_field_path "$full_fast_dir" "$full_fast_manifest" resource.manifest.name)"
    full_fast_sources="$(expiry_manifest_field_path "$full_fast_dir" "$full_fast_manifest" source.inventory.name)"
    full_fast_commands="$(expiry_manifest_field_path "$full_fast_dir" "$full_fast_manifest" evidence.compile_commands.name)"
    full_fast_su="$full_fast_dir/$(manifest_value resource.su_glob "$full_fast_manifest")"
    full_fast_disassembly="$(expiry_manifest_field_path "$full_fast_dir" "$full_fast_manifest" evidence.disassembly.name)"
    full_fast_config="$(expiry_manifest_field_path "$full_fast_dir" "$full_fast_manifest" evidence.target_config_header.name)"
    full_balanced_elf="$full_balanced_dir/${full_balanced_name}.elf"
    full_balanced_map="$full_balanced_dir/${full_balanced_name}.map"
    full_balanced_resource="$(expiry_manifest_field_path "$full_balanced_dir" "$full_balanced_manifest" resource.manifest.name)"
    full_balanced_sources="$(expiry_manifest_field_path "$full_balanced_dir" "$full_balanced_manifest" source.inventory.name)"
    full_balanced_commands="$(expiry_manifest_field_path "$full_balanced_dir" "$full_balanced_manifest" evidence.compile_commands.name)"
    full_balanced_su="$full_balanced_dir/$(manifest_value resource.su_glob "$full_balanced_manifest")"
    full_balanced_disassembly="$(expiry_manifest_field_path "$full_balanced_dir" "$full_balanced_manifest" evidence.disassembly.name)"
    full_balanced_config="$(expiry_manifest_field_path "$full_balanced_dir" "$full_balanced_manifest" evidence.target_config_header.name)"
    aodv_elf="$aodv_dir/${aodv_name}.elf"
    aodv_map="$aodv_dir/${aodv_name}.map"
    aodv_sources="$(expiry_manifest_field_path "$aodv_dir" "$aodv_manifest" source.inventory.name)"
    aodv_commands="$(expiry_manifest_field_path "$aodv_dir" "$aodv_manifest" evidence.compile_commands.name)"
    aodv_disassembly="$(expiry_manifest_field_path "$aodv_dir" "$aodv_manifest" evidence.disassembly.name)"
    aodv_config="$(expiry_manifest_field_path "$aodv_dir" "$aodv_manifest" evidence.target_config_header.name)"
    require_report_file "$full_fast_elf" "$(manifest_value artifact.elf.sha256 "$full_fast_manifest")" &&
        require_report_file "$full_fast_map" "$(manifest_value artifact.map.sha256 "$full_fast_manifest")" &&
        require_report_file "$full_fast_commands" "$(manifest_value evidence.compile_commands.sha256 "$full_fast_manifest")" &&
        require_report_file "$full_fast_disassembly" "$(manifest_value evidence.disassembly.sha256 "$full_fast_manifest")" &&
        require_report_file "$full_balanced_elf" "$(manifest_value artifact.elf.sha256 "$full_balanced_manifest")" &&
        require_report_file "$full_balanced_map" "$(manifest_value artifact.map.sha256 "$full_balanced_manifest")" &&
        require_report_file "$full_balanced_commands" "$(manifest_value evidence.compile_commands.sha256 "$full_balanced_manifest")" &&
        require_report_file "$full_balanced_disassembly" "$(manifest_value evidence.disassembly.sha256 "$full_balanced_manifest")" &&
        require_report_file "$aodv_elf" "$(manifest_value artifact.elf.sha256 "$aodv_manifest")" &&
        require_report_file "$aodv_map" "$(manifest_value artifact.map.sha256 "$aodv_manifest")" &&
        require_report_file "$aodv_commands" "$(manifest_value evidence.compile_commands.sha256 "$aodv_manifest")" &&
        require_report_file "$aodv_disassembly" "$(manifest_value evidence.disassembly.sha256 "$aodv_manifest")" || return 1
    shopt -s globstar
    if ! compgen -G "$full_fast_su" > /dev/null || ! compgen -G "$full_balanced_su" > /dev/null; then
        printf '%s\n' 'fresh FULL acceptance build lacks .su evidence' >&2
        return 1
    fi
    shopt -u globstar
    run_expiry_fixture_checks
    full_fast_stack_log="${evidence_dir}/full_fast.stack-baseline.log"
    full_balanced_stack_log="${evidence_dir}/full_balanced.stack-baseline.log"
    full_fast_stack_gate="$(run_stack_baseline_gate full_fast "$full_fast_resource" "$full_fast_elf" "$full_fast_map" "$full_fast_manifest" "$full_fast_sources" "$full_fast_commands" "$full_fast_su" "$full_fast_disassembly" "$full_fast_config" "$baseline_dir/fast.baseline.manifest" "$baseline_dir/fast.before.map" "$baseline_dir/fast.baseline.sha256")" || return 1
    full_balanced_stack_gate="$(run_stack_baseline_gate full_balanced "$full_balanced_resource" "$full_balanced_elf" "$full_balanced_map" "$full_balanced_manifest" "$full_balanced_sources" "$full_balanced_commands" "$full_balanced_su" "$full_balanced_disassembly" "$full_balanced_config" "$baseline_dir/balanced.baseline.manifest" "$baseline_dir/balanced.before.map" "$baseline_dir/balanced.baseline.sha256")" || return 1
    full_fast_baseline_of_record="$(acceptance_log_value BASELINE_OF_RECORD "$full_fast_stack_log")"
    full_fast_before_inventory="$(acceptance_log_value BEFORE_SOURCE_INVENTORY_SHA256 "$full_fast_stack_log")"
    full_fast_after_inventory="$(acceptance_log_value AFTER_SOURCE_INVENTORY_SHA256 "$full_fast_stack_log")"
    full_fast_inventory_drift="$(acceptance_log_value SOURCE_INVENTORY_DRIFT "$full_fast_stack_log")"
    full_fast_stack_chain="$(acceptance_log_value STACK_CHAIN "$full_fast_stack_log")"
    full_balanced_baseline_of_record="$(acceptance_log_value BASELINE_OF_RECORD "$full_balanced_stack_log")"
    full_balanced_before_inventory="$(acceptance_log_value BEFORE_SOURCE_INVENTORY_SHA256 "$full_balanced_stack_log")"
    full_balanced_after_inventory="$(acceptance_log_value AFTER_SOURCE_INVENTORY_SHA256 "$full_balanced_stack_log")"
    full_balanced_inventory_drift="$(acceptance_log_value SOURCE_INVENTORY_DRIFT "$full_balanced_stack_log")"
    full_balanced_stack_chain="$(acceptance_log_value STACK_CHAIN "$full_balanced_stack_log")"
    full_fast_gate="$(run_live_resource_gate full_fast "$full_fast_resource" "$full_fast_elf" "$full_fast_map" "$full_fast_manifest" "$full_fast_sources" "$full_fast_commands" "$full_fast_su" "$full_fast_disassembly" "$full_fast_config" "$baseline_dir/fast.baseline.manifest" "$baseline_dir/fast.before.map" "$baseline_dir/fast.baseline.sha256" "$full_fast_hook_manifest")" || return 1
    full_balanced_gate="$(run_live_resource_gate full_balanced "$full_balanced_resource" "$full_balanced_elf" "$full_balanced_map" "$full_balanced_manifest" "$full_balanced_sources" "$full_balanced_commands" "$full_balanced_su" "$full_balanced_disassembly" "$full_balanced_config" "$baseline_dir/balanced.baseline.manifest" "$baseline_dir/balanced.before.map" "$baseline_dir/balanced.baseline.sha256")" || return 1
    full_fast_elf_hash="$(sha256sum "$full_fast_elf" | cut -d' ' -f1)"
    full_fast_map_hash="$(sha256sum "$full_fast_map" | cut -d' ' -f1)"
    full_fast_manifest_hash="$(sha256sum "$full_fast_manifest" | cut -d' ' -f1)"
    full_fast_resource_hash="$(sha256sum "$full_fast_resource" | cut -d' ' -f1)"
    full_fast_sources_hash="$(sha256sum "$full_fast_sources" | cut -d' ' -f1)"
    full_fast_source_inventory_hash="$(manifest_value source.inventory.sha256 "$full_fast_manifest")"
    full_fast_commands_hash="$(sha256sum "$full_fast_commands" | cut -d' ' -f1)"
    full_fast_su_hash="$(expiry_su_hash "$full_fast_su")"
    full_fast_disassembly_hash="$(sha256sum "$full_fast_disassembly" | cut -d' ' -f1)"
    full_fast_config_hash="$(sha256sum "$full_fast_config" | cut -d' ' -f1)"
    full_fast_stack_log_hash="$(sha256sum "$full_fast_stack_log" | cut -d' ' -f1)"
    full_fast_log_hash="$(sha256sum "${evidence_dir}/full_fast.resource-gate.log" | cut -d' ' -f1)"
    full_balanced_elf_hash="$(sha256sum "$full_balanced_elf" | cut -d' ' -f1)"
    full_balanced_map_hash="$(sha256sum "$full_balanced_map" | cut -d' ' -f1)"
    full_balanced_manifest_hash="$(sha256sum "$full_balanced_manifest" | cut -d' ' -f1)"
    full_balanced_resource_hash="$(sha256sum "$full_balanced_resource" | cut -d' ' -f1)"
    full_balanced_sources_hash="$(sha256sum "$full_balanced_sources" | cut -d' ' -f1)"
    full_balanced_source_inventory_hash="$(manifest_value source.inventory.sha256 "$full_balanced_manifest")"
    full_balanced_commands_hash="$(sha256sum "$full_balanced_commands" | cut -d' ' -f1)"
    full_balanced_su_hash="$(expiry_su_hash "$full_balanced_su")"
    full_balanced_disassembly_hash="$(sha256sum "$full_balanced_disassembly" | cut -d' ' -f1)"
    full_balanced_config_hash="$(sha256sum "$full_balanced_config" | cut -d' ' -f1)"
    full_balanced_stack_log_hash="$(sha256sum "$full_balanced_stack_log" | cut -d' ' -f1)"
    full_balanced_log_hash="$(sha256sum "${evidence_dir}/full_balanced.resource-gate.log" | cut -d' ' -f1)"
    aodv_elf_hash="$(sha256sum "$aodv_elf" | cut -d' ' -f1)"
    aodv_map_hash="$(sha256sum "$aodv_map" | cut -d' ' -f1)"
    aodv_manifest_hash="$(sha256sum "$aodv_manifest" | cut -d' ' -f1)"
    aodv_sources_hash="$(sha256sum "$aodv_sources" | cut -d' ' -f1)"
    aodv_source_inventory_hash="$(manifest_value source.inventory.sha256 "$aodv_manifest")"
    aodv_commands_hash="$(sha256sum "$aodv_commands" | cut -d' ' -f1)"
    aodv_disassembly_hash="$(sha256sum "$aodv_disassembly" | cut -d' ' -f1)"
    aodv_config_hash="$(sha256sum "$aodv_config" | cut -d' ' -f1)"
    [[ "$full_fast_elf_hash" == "$(manifest_value artifact.elf.sha256 "$full_fast_manifest")" &&
       "$full_fast_map_hash" == "$(manifest_value artifact.map.sha256 "$full_fast_manifest")" &&
       "$full_fast_resource_hash" == "$(manifest_value resource.manifest.sha256 "$full_fast_manifest")" &&
       "$full_fast_sources_hash" == "$(manifest_value evidence.selected_sources.sha256 "$full_fast_manifest")" &&
       "$full_fast_su_hash" == "$(manifest_value resource.su.sha256 "$full_fast_manifest")" &&
       "$full_fast_disassembly_hash" == "$(manifest_value evidence.disassembly.sha256 "$full_fast_manifest")" &&
       "$full_fast_config_hash" == "$(manifest_value evidence.target_config_header.sha256 "$full_fast_manifest")" &&
       "$full_balanced_elf_hash" == "$(manifest_value artifact.elf.sha256 "$full_balanced_manifest")" &&
       "$full_balanced_map_hash" == "$(manifest_value artifact.map.sha256 "$full_balanced_manifest")" &&
       "$full_balanced_resource_hash" == "$(manifest_value resource.manifest.sha256 "$full_balanced_manifest")" &&
       "$full_balanced_sources_hash" == "$(manifest_value evidence.selected_sources.sha256 "$full_balanced_manifest")" &&
       "$full_balanced_su_hash" == "$(manifest_value resource.su.sha256 "$full_balanced_manifest")" &&
       "$full_balanced_disassembly_hash" == "$(manifest_value evidence.disassembly.sha256 "$full_balanced_manifest")" &&
       "$full_balanced_config_hash" == "$(manifest_value evidence.target_config_header.sha256 "$full_balanced_manifest")" &&
       "$aodv_elf_hash" == "$(manifest_value artifact.elf.sha256 "$aodv_manifest")" &&
       "$aodv_map_hash" == "$(manifest_value artifact.map.sha256 "$aodv_manifest")" &&
       "$aodv_sources_hash" == "$(manifest_value evidence.selected_sources.sha256 "$aodv_manifest")" &&
       "$aodv_disassembly_hash" == "$(manifest_value evidence.disassembly.sha256 "$aodv_manifest")" &&
       "$aodv_config_hash" == "$(manifest_value evidence.target_config_header.sha256 "$aodv_manifest")" ]] || return 1
    report_tmp="${evidence_dir}/expiry-resource-acceptance.env"
    {
        printf 'full_fast.elf=%s\n' "$full_fast_elf"
        printf 'full_fast.elf.sha256=%s\n' "$full_fast_elf_hash"
        printf 'full_fast.map=%s\n' "$full_fast_map"
        printf 'full_fast.map.sha256=%s\n' "$full_fast_map_hash"
        printf 'full_fast.manifest=%s\n' "$full_fast_manifest"
        printf 'full_fast.manifest.sha256=%s\n' "$full_fast_manifest_hash"
        printf 'full_fast.resource_manifest=%s\n' "$full_fast_resource"
        printf 'full_fast.selected_sources=%s\n' "$full_fast_sources"
        printf 'full_fast.source_inventory_sha256=%s\n' "$full_fast_source_inventory_hash"
        printf 'full_fast.compile_commands=%s\n' "$full_fast_commands"
        printf 'full_fast.compile_commands.sha256=%s\n' "$full_fast_commands_hash"
        printf 'full_fast.su_glob=%s\n' "$full_fast_su"
        printf 'full_fast.su_sha256=%s\n' "$full_fast_su_hash"
        printf 'full_fast.disassembly=%s\n' "$full_fast_disassembly"
        printf 'full_fast.disassembly.sha256=%s\n' "$full_fast_disassembly_hash"
        printf 'full_fast.config_header=%s\n' "$full_fast_config"
        printf 'full_fast.stack_baseline_gate=%s\n' "$full_fast_stack_gate"
        printf 'full_fast.stack_baseline_log=%s\n' "$full_fast_stack_log"
        printf 'full_fast.baseline_of_record=%s\n' "$full_fast_baseline_of_record"
        printf 'full_fast.before_source_inventory_sha256=%s\n' "$full_fast_before_inventory"
        printf 'full_fast.after_source_inventory_sha256=%s\n' "$full_fast_after_inventory"
        printf 'full_fast.source_inventory_drift=%s\n' "$full_fast_inventory_drift"
        printf 'full_fast.stack.chain=%s\n' "$full_fast_stack_chain"
        printf 'full_fast.resource_gate=%s\n' "$full_fast_gate"
        printf 'full_fast.resource_gate_log=%s\n' "${evidence_dir}/full_fast.resource-gate.log"
        printf 'full_balanced.elf=%s\n' "$full_balanced_elf"
        printf 'full_balanced.elf.sha256=%s\n' "$full_balanced_elf_hash"
        printf 'full_balanced.map=%s\n' "$full_balanced_map"
        printf 'full_balanced.map.sha256=%s\n' "$full_balanced_map_hash"
        printf 'full_balanced.manifest=%s\n' "$full_balanced_manifest"
        printf 'full_balanced.manifest.sha256=%s\n' "$full_balanced_manifest_hash"
        printf 'full_balanced.resource_manifest=%s\n' "$full_balanced_resource"
        printf 'full_balanced.selected_sources=%s\n' "$full_balanced_sources"
        printf 'full_balanced.source_inventory_sha256=%s\n' "$full_balanced_source_inventory_hash"
        printf 'full_balanced.compile_commands=%s\n' "$full_balanced_commands"
        printf 'full_balanced.compile_commands.sha256=%s\n' "$full_balanced_commands_hash"
        printf 'full_balanced.su_glob=%s\n' "$full_balanced_su"
        printf 'full_balanced.su_sha256=%s\n' "$full_balanced_su_hash"
        printf 'full_balanced.disassembly=%s\n' "$full_balanced_disassembly"
        printf 'full_balanced.disassembly.sha256=%s\n' "$full_balanced_disassembly_hash"
        printf 'full_balanced.config_header=%s\n' "$full_balanced_config"
        printf 'full_balanced.stack_baseline_gate=%s\n' "$full_balanced_stack_gate"
        printf 'full_balanced.stack_baseline_log=%s\n' "$full_balanced_stack_log"
        printf 'full_balanced.baseline_of_record=%s\n' "$full_balanced_baseline_of_record"
        printf 'full_balanced.before_source_inventory_sha256=%s\n' "$full_balanced_before_inventory"
        printf 'full_balanced.after_source_inventory_sha256=%s\n' "$full_balanced_after_inventory"
        printf 'full_balanced.source_inventory_drift=%s\n' "$full_balanced_inventory_drift"
        printf 'full_balanced.stack.chain=%s\n' "$full_balanced_stack_chain"
        printf 'full_balanced.resource_gate=%s\n' "$full_balanced_gate"
        printf 'full_balanced.resource_gate_log=%s\n' "${evidence_dir}/full_balanced.resource-gate.log"
        printf 'aodv_only.elf=%s\n' "$aodv_elf"
        printf 'aodv_only.elf.sha256=%s\n' "$aodv_elf_hash"
        printf 'aodv_only.map=%s\n' "$aodv_map"
        printf 'aodv_only.map.sha256=%s\n' "$aodv_map_hash"
        printf 'aodv_only.manifest=%s\n' "$aodv_manifest"
        printf 'aodv_only.manifest.sha256=%s\n' "$aodv_manifest_hash"
        printf 'aodv_only.selected_sources=%s\n' "$aodv_sources"
        printf 'aodv_only.source_inventory_sha256=%s\n' "$aodv_source_inventory_hash"
        printf 'aodv_only.compile_commands=%s\n' "$aodv_commands"
        printf 'aodv_only.compile_commands.sha256=%s\n' "$aodv_commands_hash"
        printf 'aodv_only.disassembly=%s\n' "$aodv_disassembly"
        printf 'aodv_only.disassembly.sha256=%s\n' "$aodv_disassembly_hash"
        printf 'aodv_only.config_header=%s\n' "$aodv_config"
        printf 'full_fast.resource_gate_log.sha256=%s\n' "$full_fast_log_hash"
        printf 'full_balanced.resource_gate_log.sha256=%s\n' "$full_balanced_log_hash"
        printf 'full_fast.stack_baseline_log.sha256=%s\n' "$full_fast_stack_log_hash"
        printf 'full_balanced.stack_baseline_log.sha256=%s\n' "$full_balanced_stack_log_hash"
    } > "$report_tmp"
    [[ -s "$report_tmp" ]] || return 1
    mv "$report_tmp" "$EXPIRY_REPORT"
    printf 'expiry resource acceptance artifacts: %s\n' "$evidence_dir"
}

if [[ -n "$EXPIRY_REPORT" ]]; then
    run_expiry_resource_acceptance
    exit 0
fi

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
require_line 'formula.verification_window_ms=timer.aodv_net_traversal_ms+2*timer.aodv_path_discovery_ms' "$runtime_manifest"
require_line 'timer.link_no_response_wall_bound_ms=840' "$runtime_manifest"
require_line 'capacity.link_custody.state=IMPLEMENTED' "$runtime_manifest"
require_line 'capacity.aodv_routes.state=NOT_IMPLEMENTED' "$runtime_manifest"
build_target runtime-link ble_link_v2_testbed
runtime_timer_source="$WORK_DIR/runtime-link/app/ble_link_v2_testbed/generated/ble_link_v2_testbed/tron_timer_config.c"
runtime_config_header="$WORK_DIR/runtime-link/app/ble_link_v2_testbed/generated/ble_link_v2_testbed/tron_build_config.h"
require_timer_profile_surface FAST_TEST "$runtime_manifest" "$runtime_timer_source" 1500
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

# BUILD-P1-02a: SOAK keeps the same freshness bounds and ordinary AODV retries
# while selecting its larger verification window in the generated object.
configure_ok timer-soak -DTRON_PHASE1_TARGET=LINK -DTRON_TIMER_PROFILE=SOAK
soak_manifest="$(link_manifest_path timer-soak)"
build_target timer-soak ble_link_v2_testbed
soak_timer_source="$WORK_DIR/timer-soak/app/ble_link_v2_testbed/generated/ble_link_v2_testbed/tron_timer_config.c"
require_timer_profile_surface SOAK "$soak_manifest" "$soak_timer_source" 6000

# BUILD-P2-01: AODV_ONLY is one routed composition, not the Phase 1 link
# harness. It contains the real router/core and excludes FULL feature sources.
configure_ok routed-aodv -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_TIMER_PROFILE=FAST_TEST \
    -DTRON_ADVA_OVERRIDE=18:42:de:52:4a:dd \
    -DTRON_LINK_TEST_PEER_ADVA=dc:4b:0a:06:03:f8 \
    -DTRON_LINK_TEST_INITIATOR=ON -DTRON_LINK_TEST_TRANSACTION_TARGET=1
routed_manifest="$(routed_manifest_path routed-aodv)"
require_line 'build.phase1_target=ROUTED' "$routed_manifest"
require_line 'build.behavior=TAVRN_ROUTED_AODV_ONLY' "$routed_manifest"
require_line 'build.node_mode.effective=TAVRN_ROUTED' "$routed_manifest"
require_line 'feature.level.effective=AODV_ONLY' "$routed_manifest"
require_line 'build.implemented_capabilities=wire-v2,link-v2,custody,aodv,aodv-only,typed-runtime-observability,rreq-scope-telemetry' "$routed_manifest"
require_line 'capacity.aodv_routes.state=IMPLEMENTED' "$routed_manifest"
require_line 'capacity.aodv_action_queue.state=IMPLEMENTED' "$routed_manifest"
require_line 'capacity.router_failure_obligations=16' "$routed_manifest"
require_line 'capacity.router_failure_obligations.state=IMPLEMENTED' "$routed_manifest"
require_line 'capacity.router_failure_overflow=1' "$routed_manifest"
require_line 'capacity.router_failure_overflow.policy=PRESERVE_AND_FAIL_STOP' "$routed_manifest"
require_line 'capacity.router_failure_overflow.state=IMPLEMENTED_FAIL_STOP' "$routed_manifest"
require_line 'capacity.router_post_ack_pending_data=1' "$routed_manifest"
require_line 'capacity.router_delivery_reservation=1' "$routed_manifest"
require_line 'capacity.router_retained_aodv_action=1' "$routed_manifest"
require_line 'capacity.gtt_membership.state=NOT_IMPLEMENTED' "$routed_manifest"
require_line 'capacity.router_pending_incarnation_reset.state=IMPLEMENTED' "$routed_manifest"
require_line 'capacity.mentor_offers.state=NOT_IMPLEMENTED' "$routed_manifest"
require_line 'capacity.mentor_join_dedupe.state=NOT_IMPLEMENTED' "$routed_manifest"
require_line 'capacity.mentor_join_obligations.state=NOT_IMPLEMENTED' "$routed_manifest"
require_line 'capacity.mentor_sync_dedupe.state=NOT_IMPLEMENTED' "$routed_manifest"
require_line 'capacity.mentor_session.state=NOT_IMPLEMENTED' "$routed_manifest"
require_line 'capacity.mentor_snapshot.state=NOT_IMPLEMENTED' "$routed_manifest"
require_line 'capacity.maintenance_dedupe.state=NOT_IMPLEMENTED' "$routed_manifest"
require_line 'capacity.maintenance_epoch.state=NOT_IMPLEMENTED' "$routed_manifest"
require_line 'capacity.maintenance_pending.state=NOT_IMPLEMENTED' "$routed_manifest"
require_line 'link_test.peer_adva=dc:4b:0a:06:03:f8' "$routed_manifest"
if [[ "$(grep -c '^capacity\.[^.]*=' "$routed_manifest")" -ne 39 ]]; then
    printf '%s\n' 'routed AODV_ONLY capacity schema width is not exact' >&2
    exit 1
fi
require_unique_keys "$routed_manifest"
if ! grep '^source\.selected\.[0-9].*=' "$routed_manifest" | grep -q 'app/protocol/aodv_core.c' ||
   ! grep '^source\.selected\.[0-9].*=' "$routed_manifest" | grep -q 'app/protocol/tavrn_router.c' ||
   ! grep '^source\.selected\.[0-9].*=' "$routed_manifest" | grep -q 'app/tavrn_routed_node/src/routed_cycle.c' ||
    grep '^source\.selected\.[0-9].*=' "$routed_manifest" | grep -q 'tron_mesh_\|routed_full_telemetry\|tavrn_esc\|tavrn_gtt\|tavrn_full\|tavrn_maintenance\|tavrn_mentorship\|tavrn_smart_ttl\|tavrn_repair'; then
    printf '%s\n' 'routed AODV_ONLY source manifest has missing router/core or leaked sources' >&2
    exit 1
fi
build_target routed-aodv tavrn_routed_node
routed_aodv_config="$WORK_DIR/routed-aodv/app/tavrn_routed_node/generated/tavrn_routed_node/tron_build_config.h"
if ! grep -Fqx '#define TRON_BUILD_ROUTED_FULL_TAVRN 0' "$routed_aodv_config" ||
   ! grep -Fqx '#define TRON_BUILD_TEST_EXPIRY_FULL_TABLE 0' "$routed_aodv_config"; then
    printf '%s\n' 'AODV_ONLY generated config does not expose the selected feature macro' >&2
    exit 1
fi

# BUILD-P4-01: FULL_TAVRN adds K=1 ESC and mentorship around the routed
# main/router/AODV/GTT source closure.
configure_ok routed-full -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTRON_TIMER_PROFILE=FAST_TEST
routed_full_manifest="$(routed_manifest_path routed-full)"
require_line 'build.phase1_target=ROUTED' "$routed_full_manifest"
require_line 'build.behavior=TAVRN_ROUTED_FULL_TAVRN_ESC_K1_MENTORSHIP_ADAPTIVE_HELLO' "$routed_full_manifest"
require_line 'feature.level.effective=FULL_TAVRN' "$routed_full_manifest"
require_line 'build.implemented_capabilities=wire-v2,link-v2,custody,aodv,esc-k1,sid8-identity-context,mentorship-bootstrap,passive-gtt,smart-ttl,adaptive-sid8-hello,hello-ema-snap,hello-topology-reset,hello-broadcast-suppression,hello-liveness-hysteresis,hello-equality-dedupe,hello-gtt-liveness,maintenance-telemetry,typed-runtime-observability,rreq-scope-telemetry,gtt-snapshot' "$routed_full_manifest"
require_line 'fixed_k.state=1' "$routed_full_manifest"
require_line 'capacity.gtt_membership.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.router_failure_overflow.state=IMPLEMENTED_FAIL_STOP' "$routed_full_manifest"
require_line 'capacity.router_pending_incarnation_reset.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.mentor_offers.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.mentor_join_dedupe=16' "$routed_full_manifest"
require_line 'capacity.mentor_join_dedupe.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.mentor_join_obligations=3' "$routed_full_manifest"
require_line 'capacity.mentor_join_obligations.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.mentor_sync_dedupe=8' "$routed_full_manifest"
require_line 'capacity.mentor_sync_dedupe.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.mentor_session.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.mentor_snapshot.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.maintenance_dedupe=16' "$routed_full_manifest"
require_line 'capacity.maintenance_dedupe.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.maintenance_epoch=16' "$routed_full_manifest"
require_line 'capacity.maintenance_epoch.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.maintenance_pending=1' "$routed_full_manifest"
require_line 'capacity.maintenance_pending.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'bound.mentor_failure_protocol_ms=3850' "$routed_full_manifest"
require_line 'bound.mentor_failure_protocol_ms.scope=PROTOCOL_DEADLINES_ONLY_EXCLUDES_SCHEDULER_APPLICATION_CADENCE' "$routed_full_manifest"
require_line 'formula.mentor_failure_protocol_ms=timer.mentor_rssi_weak_delay_ms+timer.mentor_jitter_max_ms+timer.mentor_offer_window_ms+timer.mentor_page_attempts*timer.mentor_page_timeout_ms+timer.mentor_self_bootstrap_ms' "$routed_full_manifest"
if [[ "$(grep -c '^capacity\.[^.]*=' "$routed_full_manifest")" -ne 39 ]]; then
    printf '%s\n' 'routed FULL_TAVRN capacity schema width is not exact' >&2
    exit 1
fi
require_unique_keys "$routed_full_manifest"
for source in tavrn_router.c tavrn_esc.c tavrn_gtt.c tavrn_maintenance.c tavrn_mentorship.c tavrn_smart_ttl.c tavrn_full.c; do
    if ! grep '^source\.selected\.[0-9].*=' "$routed_full_manifest" | grep -q "app/protocol/${source}"; then
        printf 'FULL_TAVRN source manifest lacks %s\n' "$source" >&2
        exit 1
    fi
done
for source in routed_cycle.c routed_full_telemetry.c; do
    if ! grep '^source\.selected\.[0-9].*=' "$routed_full_manifest" | grep -q "app/tavrn_routed_node/src/${source}"; then
        printf 'FULL_TAVRN source manifest lacks %s\n' "$source" >&2
        exit 1
    fi
done
if grep '^source\.selected\.[0-9].*=' "$routed_full_manifest" | grep -q 'tron_mesh_\|tavrn_repair'; then
    printf '%s\n' 'FULL_TAVRN source manifest leaks unimplemented feature sources' >&2
    exit 1
fi
build_target routed-full tavrn_routed_node
routed_full_config="$WORK_DIR/routed-full/app/tavrn_routed_node/generated/tavrn_routed_node/tron_build_config.h"
if ! grep -Fqx '#define TRON_BUILD_ROUTED_FULL_TAVRN 1' "$routed_full_config" ||
   ! grep -Fqx '#define TRON_BUILD_TEST_EXPIRY_FULL_TABLE 0' "$routed_full_config"; then
    printf '%s\n' 'FULL_TAVRN generated config does not expose the selected feature macro' >&2
    exit 1
fi

# BUILD-P4-02: the explicit FULL-only collision hook synthesizes one unique
# full AdvA/SID16 while duplicating exactly the selected peer's SID8.
collision_args=(
    -DTRON_PHASE1_TARGET=ROUTED
    -DTRON_NODE_MODE=TAVRN_ROUTED
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN
    -DTRON_TIMER_PROFILE=BALANCED
    -DTRON_ENABLE_TEST_HOOKS=ON
    -DTRON_ADVA_OVERRIDE=18:42:de:52:4a:dd
    -DTRON_TEST_COLLISION_PEER_ADVA=dc:4b:0a:06:03:f8
)
configure_ok routed-full-collision "${collision_args[@]}"
collision_manifest="$(routed_manifest_path routed-full-collision)"
require_line 'hook.enabled=ON' "$collision_manifest"
require_line 'hook.collision_peer_adva=dc:4b:0a:06:03:f8' "$collision_manifest"
require_line 'identity.adva=dc:42:de:52:4a:dd' "$collision_manifest"
require_line 'identity.adva_override=18:42:de:52:4a:dd' "$collision_manifest"
require_line 'identity.source=SYNTHETIC_COLLISION_HOOK' "$collision_manifest"
require_line 'identity.sid8=220' "$collision_manifest"
require_line 'identity.sid16=17116' "$collision_manifest"
collision_config="$WORK_DIR/routed-full-collision/app/tavrn_routed_node/generated/tavrn_routed_node/tron_build_config.h"
if ! grep -Fqx '#define TRON_BUILD_LOCAL_ADVA_ENABLED 1' "$collision_config" ||
   ! grep -Fqx '#define TRON_BUILD_LOCAL_ADVA_BYTES { 0xdc, 0x42, 0xde, 0x52, 0x4a, 0xdd }' "$collision_config" ||
   ! grep -Fqx '#define TRON_BUILD_COLLISION_PEER_ADVA_ENABLED 1' "$collision_config" ||
   ! grep -Fqx '#define TRON_BUILD_COLLISION_PEER_ADVA_BYTES { 0xdc, 0x4b, 0x0a, 0x06, 0x03, 0xf8 }' "$collision_config"; then
    printf '%s\n' 'collision build config lacks effective local/peer identity bytes' >&2
    exit 1
fi
build_target routed-full-collision tavrn_routed_node
collision_timer_source="$WORK_DIR/routed-full-collision/app/tavrn_routed_node/generated/tavrn_routed_node/tron_timer_config.c"
require_timer_profile_surface BALANCED "$collision_manifest" "$collision_timer_source" 6000

# EXPIRY-HOOK-01: the complete-table capture hook has one intentionally narrow
# build domain and its generated config/manifest retain that provenance.
expiry_hook_args=(
    -DTRON_PHASE1_TARGET=ROUTED
    -DTRON_NODE_MODE=TAVRN_ROUTED
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN
    -DTRON_TIMER_PROFILE=FAST_TEST
    -DTRON_ENABLE_TEST_HOOKS=ON
    -DTRON_TEST_EXPIRY_FULL_TABLE=ON
)
configure_ok routed-full-expiry-table "${expiry_hook_args[@]}"
expiry_hook_manifest="$(routed_manifest_path routed-full-expiry-table)"
expiry_hook_config="$WORK_DIR/routed-full-expiry-table/app/tavrn_routed_node/generated/tavrn_routed_node/tron_build_config.h"
require_line 'hook.enabled=ON' "$expiry_hook_manifest"
require_line 'hook.expiry_full_table=ON' "$expiry_hook_manifest"
if ! grep -Fqx '#define TRON_BUILD_TEST_EXPIRY_FULL_TABLE 1' "$expiry_hook_config"; then
    printf '%s\n' 'expiry full-table hook generated config is not active' >&2
    exit 1
fi
build_target routed-full-expiry-table tavrn_routed_node
configure_fail_with expiry-table-aodv \
    'TRON_TEST_EXPIRY_FULL_TABLE=ON requires ROUTED FULL_TAVRN' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_TIMER_PROFILE=FAST_TEST \
    -DTRON_ENABLE_TEST_HOOKS=ON -DTRON_TEST_EXPIRY_FULL_TABLE=ON
configure_fail_with expiry-table-link \
    'TRON_TEST_EXPIRY_FULL_TABLE=ON requires ROUTED FULL_TAVRN' \
    -DTRON_PHASE1_TARGET=LINK -DTRON_TIMER_PROFILE=FAST_TEST \
    -DTRON_ENABLE_TEST_HOOKS=ON -DTRON_TEST_EXPIRY_FULL_TABLE=ON
configure_fail_with expiry-table-balanced \
    'TRON_TEST_EXPIRY_FULL_TABLE=ON requires TRON_TIMER_PROFILE=FAST_TEST' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTRON_TIMER_PROFILE=BALANCED \
    -DTRON_ENABLE_TEST_HOOKS=ON -DTRON_TEST_EXPIRY_FULL_TABLE=ON
configure_fail_with expiry-table-hooks-off \
    'TRON_TEST_EXPIRY_FULL_TABLE=ON requires TRON_ENABLE_TEST_HOOKS=ON' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTRON_TIMER_PROFILE=FAST_TEST \
    -DTRON_TEST_EXPIRY_FULL_TABLE=ON
configure_fail_with collision-hooks-off \
    'TRON_TEST_COLLISION_PEER_ADVA requires TRON_ENABLE_TEST_HOOKS=ON' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTRON_ADVA_OVERRIDE=18:42:de:52:4a:dd \
    -DTRON_TEST_COLLISION_PEER_ADVA=dc:4b:0a:06:03:f8
configure_fail_with collision-aodv \
    'TRON_TEST_COLLISION_PEER_ADVA requires ROUTED FULL_TAVRN' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_ADVA_OVERRIDE=18:42:de:52:4a:dd \
    -DTRON_TEST_COLLISION_PEER_ADVA=dc:4b:0a:06:03:f8
configure_fail_with collision-link \
    'TRON_TEST_COLLISION_PEER_ADVA requires ROUTED FULL_TAVRN' \
    -DTRON_PHASE1_TARGET=LINK -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_ADVA_OVERRIDE=18:42:de:52:4a:dd \
    -DTRON_TEST_COLLISION_PEER_ADVA=dc:4b:0a:06:03:f8
configure_fail_with collision-missing-base \
    'TRON_TEST_COLLISION_PEER_ADVA requires TRON_ADVA_OVERRIDE' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_TEST_COLLISION_PEER_ADVA=dc:4b:0a:06:03:f8
configure_fail_with collision-reserved-sid8 \
    'TRON_TEST_COLLISION_PEER_ADVA must derive a nonreserved SID8' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_ADVA_OVERRIDE=18:42:de:52:4a:dd \
    -DTRON_TEST_COLLISION_PEER_ADVA=00:4b:0a:06:03:f8
configure_fail_with collision-already-equal-sid8 \
    'TRON_TEST_COLLISION_PEER_ADVA SID8 must differ from the base override' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_ADVA_OVERRIDE=dc:42:de:52:4a:dd \
    -DTRON_TEST_COLLISION_PEER_ADVA=dc:4b:0a:06:03:f8
configure_fail_with collision-synthetic-equals-peer \
    'Synthetic collision AdvA must remain distinct from its peer' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_ADVA_OVERRIDE=18:4b:0a:06:03:f8 \
    -DTRON_TEST_COLLISION_PEER_ADVA=dc:4b:0a:06:03:f8
configure_fail_with collision-synthetic-sid16 \
    'Synthetic collision SID16 must remain distinct from its peer' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_ADVA_OVERRIDE=18:4b:de:52:4a:dd \
    -DTRON_TEST_COLLISION_PEER_ADVA=dc:4b:0a:06:03:f8

# BUILD-P2-02: target selection is fail-closed and keeps link/legacy isolated.
configure_fail routed-missing-feature -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED
configure_fail routed-wrong-mode -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=NOT_APPLICABLE \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY
configure_fail routed-invalid-feature -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=SID8
configure_fail routed-candidate -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_HARDWARE_CANDIDATE=ON

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
if [[ "$(grep -c '^timer\.' "$runtime_manifest")" -ne 73 ]] ||
    [[ "$(grep -c '^capacity\.[^.]*=' "$runtime_manifest")" -ne 39 ]]; then
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

# BUILD-P3-02: development publication supports both routed feature levels,
# retains their distinct source manifests, and does not weaken candidate rules.
routed_publisher_out="$WORK_DIR/routed-published"
bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target tavrn_routed_node \
    --feature AODV_ONLY --timer FAST_TEST --out "$routed_publisher_out" >/dev/null
bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target tavrn_routed_node \
    --feature FULL_TAVRN --timer FAST_TEST --out "$routed_publisher_out" >/dev/null
routed_published_manifest=()
for manifest_path in "$routed_publisher_out"/*.manifest; do
    [[ "$manifest_path" == *.build-config.manifest ]] && continue
    routed_published_manifest+=("$manifest_path")
done
if [[ ${#routed_published_manifest[@]} -ne 2 ]]; then
    printf '%s\n' 'routed publisher did not create one manifest per feature level' >&2
    exit 1
fi
for manifest_path in "${routed_published_manifest[@]}"; do
    require_unique_keys "$manifest_path"
done
if ! grep -Fqx 'feature.level.effective=AODV_ONLY' "${routed_published_manifest[0]}" &&
   ! grep -Fqx 'feature.level.effective=AODV_ONLY' "${routed_published_manifest[1]}"; then
    printf '%s\n' 'routed AODV_ONLY publisher manifest is missing' >&2
    exit 1
fi
if ! grep -Fqx 'feature.level.effective=FULL_TAVRN' "${routed_published_manifest[0]}" &&
   ! grep -Fqx 'feature.level.effective=FULL_TAVRN' "${routed_published_manifest[1]}"; then
    printf '%s\n' 'routed FULL_TAVRN publisher manifest is missing' >&2
    exit 1
fi
routed_published_name_a="$(manifest_value artifact.name "${routed_published_manifest[0]}")"
routed_published_name_b="$(manifest_value artifact.name "${routed_published_manifest[1]}")"
if [[ "$routed_published_name_a" == "$routed_published_name_b" ]]; then
    printf '%s\n' 'routed publisher feature artifact names are not distinct' >&2
    exit 1
fi
if [[ "$routed_published_name_a" != *full-tavrn-phase5-adaptive-hello* &&
      "$routed_published_name_b" != *full-tavrn-phase5-adaptive-hello* ]]; then
    printf '%s\n' 'FULL publisher artifact lacks the Phase 5 adaptive-HELLO tag' >&2
    exit 1
fi
collision_publisher_out="$WORK_DIR/collision-published"
bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target tavrn_routed_node \
    --feature FULL_TAVRN --timer BALANCED --enable-hooks ON \
    --adva 18:42:de:52:4a:dd --collision-peer-adva dc:4b:0a:06:03:f8 \
    --out "$collision_publisher_out" >/dev/null
collision_published_manifest=()
for manifest_path in "$collision_publisher_out"/*.manifest; do
    [[ "$manifest_path" == *.build-config.manifest ]] && continue
    collision_published_manifest+=("$manifest_path")
done
if [[ ${#collision_published_manifest[@]} -ne 1 ]]; then
    printf '%s\n' 'collision publisher did not create exactly one artifact manifest' >&2
    exit 1
fi
collision_published_name="$(manifest_value artifact.name "${collision_published_manifest[0]}")"
if [[ "$collision_published_name" != *full-tavrn-phase5-adaptive-hello*advadc42de524add* ]]; then
    printf 'collision publisher used misleading artifact identity: %s\n' \
        "$collision_published_name" >&2
    exit 1
fi
set +e
bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target ble_link_v2_testbed \
    --feature FULL_TAVRN --out "$routed_publisher_out" >/dev/null 2>&1
non_routed_feature_status=$?
set -e
if [[ $non_routed_feature_status -ne 2 ]]; then
    printf 'non-routed --feature exit status=%s, expected 2\n' \
        "$non_routed_feature_status" >&2
    exit 1
fi

printf '%s\n' 'tron BLE build/profile tests passed'
