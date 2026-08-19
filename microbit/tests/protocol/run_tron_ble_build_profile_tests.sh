#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="$(mktemp -d)"
TOOLCHAIN="${MICROBIT_ROOT}/cmake/arm-none-eabi-gcc.cmake"
FIXTURES="${MICROBIT_ROOT}/tests/protocol/fixtures"
SIX_BOARD_INVENTORY="${MICROBIT_ROOT}/hardware-results/2026-08-11-tavrn-six-board-inventory.tsv"
EXPIRY_REPORT=""
EXPIRY_FIXTURES=""
D2_INCREMENTAL_REPORT=""

if [[ $# -ne 0 ]]; then
    if [[ $# -eq 4 && "$1" == "--expiry-resource-acceptance-report" &&
          "$3" == "--expiry-resource-fixtures" ]]; then
        EXPIRY_REPORT="$2"
        EXPIRY_FIXTURES="$4"
        if [[ -e "$EXPIRY_REPORT" || ! -d "$EXPIRY_FIXTURES" ]]; then
            printf '%s\n' 'expiry resource report must be fresh and fixtures must be a directory' >&2
            exit 2
        fi
    elif [[ $# -eq 2 && "$1" == "--d2-application-incremental-report" ]]; then
        D2_INCREMENTAL_REPORT="$2"
        EXPIRY_FIXTURES="${MICROBIT_ROOT}/tests/protocol/fixtures/tavrn_expiry_resources"
        if [[ -e "$D2_INCREMENTAL_REPORT" ]]; then
            printf '%s\n' 'D2 application incremental report must be fresh' >&2
            exit 2
        fi
    else
        printf 'usage: %s [--expiry-resource-acceptance-report PATH --expiry-resource-fixtures DIR | --d2-application-incremental-report PATH]\n' \
            "$0" >&2
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
    if ! grep -Fq -- "$expected" "$WORK_DIR/$name.log"; then
        printf 'configure failure %s lacks expected diagnostic: %s\n' \
            "$name" "$expected" >&2
        return 1
    fi
}

build_target() {
    cmake --build "$WORK_DIR/$1" --target "$2" --parallel >"$WORK_DIR/$1.build.log" 2>&1
}

publisher_fail_with() {
    local name="$1"
    local expected="$2"
    local status
    shift 2

    set +e
    bash "$MICROBIT_ROOT/build-tavrn-ble.sh" "$@" --out "$WORK_DIR/$name" \
        >"$WORK_DIR/$name.log" 2>&1
    status=$?
    set -e
    if [[ $status -ne 2 ]]; then
        printf 'expected publisher argument failure status 2: %s (got %s)\n' \
            "$name" "$status" >&2
        return 1
    fi
    if ! grep -Fq -- "$expected" "$WORK_DIR/$name.log"; then
        printf 'publisher argument failure %s lacks expected diagnostic: %s\n' \
            "$name" "$expected" >&2
        return 1
    fi
}

publisher_configure_fail_with() {
    local name="$1"
    local expected="$2"
    local status
    shift 2

    set +e
    bash "$MICROBIT_ROOT/build-tavrn-ble.sh" "$@" --out "$WORK_DIR/$name" \
        >"$WORK_DIR/$name.log" 2>&1
    status=$?
    set -e
    if [[ $status -ne 1 ]]; then
        printf 'expected publisher configure failure status 1: %s (got %s)\n' \
            "$name" "$status" >&2
        return 1
    fi
    if ! grep -Fq -- "$expected" "$WORK_DIR/$name.log"; then
        printf 'publisher configure failure %s lacks expected diagnostic: %s\n' \
            "$name" "$expected" >&2
        return 1
    fi
}

make_clean_publisher_source() {
    local source_root="$WORK_DIR/clean-publisher-source"
    local kernel_origin="$WORK_DIR/clean-publisher-kernel-origin"

    mkdir -p "$source_root"
    cp -a "$MICROBIT_ROOT/." "$source_root/"
    cp -a "$MICROBIT_ROOT/libs/mtkernel_3" "$kernel_origin"
    rm -rf "$source_root/libs/mtkernel_3" "$kernel_origin/.git"
    git -C "$kernel_origin" init --quiet
    git -C "$kernel_origin" add .
    git -C "$kernel_origin" -c user.name=profile-test -c user.email=profile-test@example.invalid \
        commit --quiet -m fixture
    git -C "$source_root" init --quiet
    git -C "$source_root" -c protocol.file.allow=always submodule add --quiet \
        "$kernel_origin" libs/mtkernel_3
    git -C "$source_root" add .
    git -C "$source_root" -c user.name=profile-test -c user.email=profile-test@example.invalid \
        commit --quiet -m fixture
    printf '%s\n' "$source_root"
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

require_selected_source_count() {
    local manifest="$1"
    local source="$2"
    local expected="$3"

    python3 - "$manifest" "$source" "$expected" <<'PY'
import pathlib
import sys

manifest, source, expected = sys.argv[1], sys.argv[2], int(sys.argv[3])
count = 0
for line in pathlib.Path(manifest).read_text(encoding="utf-8").splitlines():
    key, separator, value = line.partition("=")
    if (separator and key.startswith("source.selected.") and
            key.removeprefix("source.selected.").isdigit() and value == source):
        count += 1
if count != expected:
    raise SystemExit(
        "selected source %s has count %d, expected %d in %s" %
        (source, count, expected, manifest))
PY
}

require_exact_selected_sources() {
    local manifest="$1"
    shift

    python3 - "$manifest" "$@" <<'PY'
import pathlib
import sys

manifest = pathlib.Path(sys.argv[1])
expected = set(sys.argv[2:])
actual = []
for line in manifest.read_text(encoding="utf-8").splitlines():
    key, separator, value = line.partition("=")
    if separator and key.startswith("source.selected.") and key[16:].isdigit():
        actual.append(value)
if len(actual) != len(set(actual)) or set(actual) != expected:
    raise SystemExit(
        "selected source closure mismatch\nexpected=%s\nactual=%s" %
        (sorted(expected), sorted(actual)))
PY
}

require_compile_definition_once() {
    local commands="$1"
    local source="$2"
    local definition="$3"
    local target="${4:-}"

    python3 - "$commands" "$source" "$definition" "$target" <<'PY'
import json
import pathlib
import shlex
import sys

commands_path = pathlib.Path(sys.argv[1])
source = pathlib.Path(sys.argv[2]).resolve()
definition = "-D" + sys.argv[3]
macro = definition.split("=", 1)[0]
target = sys.argv[4]
entries = json.loads(commands_path.read_text(encoding="utf-8"))
matches = []
for entry in entries:
    if target and (not isinstance(entry.get("output"), str) or
                   "CMakeFiles/%s.dir/" % target not in entry["output"]):
        continue
    candidate = pathlib.Path(entry["file"])
    if not candidate.is_absolute():
        candidate = pathlib.Path(entry.get("directory", commands_path.parent)) / candidate
    if candidate.resolve() == source:
        matches.append(entry)
if len(matches) != 1:
    raise SystemExit("expected one compile command for %s, found %d" % (source, len(matches)))
entry = matches[0]
arguments = entry.get("arguments")
if not isinstance(arguments, list):
    arguments = shlex.split(entry.get("command", ""))
definitions = [argument for argument in arguments
               if argument.startswith(macro)]
if definitions != [definition]:
    raise SystemExit("compile command for %s has %r, expected [%r]" %
                     (source, definitions, definition))
PY
}

require_compile_option_pair_once() {
    local commands="$1"
    local source="$2"
    local option="$3"
    local value="$4"

    python3 - "$commands" "$source" "$option" "$value" <<'PY'
import json
import pathlib
import shlex
import sys

commands_path = pathlib.Path(sys.argv[1])
source = pathlib.Path(sys.argv[2]).resolve()
option, value = sys.argv[3:]
entries = json.loads(commands_path.read_text(encoding="utf-8"))
matches = []
for entry in entries:
    candidate = pathlib.Path(entry["file"])
    if not candidate.is_absolute():
        candidate = pathlib.Path(entry.get("directory", commands_path.parent)) / candidate
    if candidate.resolve() == source:
        matches.append(entry)
if len(matches) != 1:
    raise SystemExit("expected one compile command for %s, found %d" % (source, len(matches)))
entry = matches[0]
arguments = entry.get("arguments")
if not isinstance(arguments, list):
    arguments = shlex.split(entry.get("command", ""))
pairs = [(arguments[index], arguments[index + 1])
         for index in range(len(arguments) - 1)]
if pairs.count((option, value)) != 1:
    raise SystemExit("compile command for %s lacks one %r %r pair" %
                     (source, option, value))
PY
}

require_no_initial_task_override() {
    local commands="$1"

    if grep -Fq -- '-DINITTASK_STKSZ' "$commands"; then
        printf 'non-routed compile commands unexpectedly override INITTASK_STKSZ: %s\n' \
            "$commands" >&2
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
    if [[ "$(grep -c '^timer\.' "$manifest")" -ne 75 ]]; then
        printf '%s timer manifest key count is not exactly 75\n' "$profile" >&2
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

    for candidate in "$out_dir"/*.manifest "$out_dir"/*/*.manifest; do
        [[ -f "$candidate" ]] || continue
        [[ "$candidate" == *.build-config.manifest ]] && continue
        manifests+=("$candidate")
    done
    if [[ ${#manifests[@]} -ne 1 ]]; then
        printf 'expected one published artifact manifest in or beneath %s\n' "$out_dir" >&2
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
    local failing=(fail-stack.json fail-headroom.json ceiling-4097-fail.json headroom-3841-fail.json
                   fail-ram.json fail-delta.json fail-capture.json
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
    local full_fast_map_unallocated_ram full_fast_runtime_ram_reserve full_fast_post_reserve_ram
    local full_fast_initial_task_stack full_fast_initial_task_static_frame
    local full_fast_initial_task_logical_headroom
    local full_balanced_elf full_balanced_map full_balanced_resource full_balanced_sources
    local full_balanced_commands full_balanced_su full_balanced_disassembly full_balanced_config
    local full_balanced_stack_gate full_balanced_gate full_balanced_stack_log
    local full_balanced_baseline_of_record full_balanced_before_inventory
    local full_balanced_after_inventory full_balanced_inventory_drift full_balanced_stack_chain
    local full_balanced_map_unallocated_ram full_balanced_runtime_ram_reserve full_balanced_post_reserve_ram
    local full_balanced_initial_task_stack full_balanced_initial_task_static_frame
    local full_balanced_initial_task_logical_headroom
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

    require_runtime_ram_report() {
        python3 - "$1" "$2" "$3" <<'PY'
import re
import sys

map_unallocated, reserve, post_reserve = sys.argv[1:]
if not all(re.fullmatch(r"[0-9]+", value) for value in
           (map_unallocated, reserve, post_reserve)):
    raise SystemExit(1)
map_unallocated, reserve, post_reserve = map(int, (map_unallocated, reserve, post_reserve))
if reserve != 13360 or map_unallocated < reserve or \
        post_reserve != map_unallocated - reserve or post_reserve < 8192:
    raise SystemExit(1)
PY
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
            --config-header "$config" --main-source "$MICROBIT_ROOT/app/tavrn_routed_node/src/main.c" \
            --preprocessed-main-out "${evidence_dir}/${profile}.stack-baseline.main.i" >"$output" 2>&1
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
            [[ "$(<"$output")" == *$'\nPASS '* ]] || return 1
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
    full_fast_map_unallocated_ram="$(acceptance_log_value MAP_UNALLOCATED_RAM_BYTES "${evidence_dir}/full_fast.resource-gate.log")"
    full_fast_runtime_ram_reserve="$(acceptance_log_value RUNTIME_RAM_RESERVE_BYTES "${evidence_dir}/full_fast.resource-gate.log")"
    full_fast_post_reserve_ram="$(acceptance_log_value POST_RESERVE_RAM_BYTES "${evidence_dir}/full_fast.resource-gate.log")"
    full_fast_initial_task_stack="$(acceptance_log_value INITIAL_TASK_STACK_BYTES "${evidence_dir}/full_fast.resource-gate.log")"
    full_fast_initial_task_static_frame="$(acceptance_log_value INITIAL_TASK_STATIC_FRAME_BYTES "${evidence_dir}/full_fast.resource-gate.log")"
    full_fast_initial_task_logical_headroom="$(acceptance_log_value INITIAL_TASK_LOGICAL_HEADROOM_BYTES "${evidence_dir}/full_fast.resource-gate.log")"
    full_balanced_map_unallocated_ram="$(acceptance_log_value MAP_UNALLOCATED_RAM_BYTES "${evidence_dir}/full_balanced.resource-gate.log")"
    full_balanced_runtime_ram_reserve="$(acceptance_log_value RUNTIME_RAM_RESERVE_BYTES "${evidence_dir}/full_balanced.resource-gate.log")"
    full_balanced_post_reserve_ram="$(acceptance_log_value POST_RESERVE_RAM_BYTES "${evidence_dir}/full_balanced.resource-gate.log")"
    full_balanced_initial_task_stack="$(acceptance_log_value INITIAL_TASK_STACK_BYTES "${evidence_dir}/full_balanced.resource-gate.log")"
    full_balanced_initial_task_static_frame="$(acceptance_log_value INITIAL_TASK_STATIC_FRAME_BYTES "${evidence_dir}/full_balanced.resource-gate.log")"
    full_balanced_initial_task_logical_headroom="$(acceptance_log_value INITIAL_TASK_LOGICAL_HEADROOM_BYTES "${evidence_dir}/full_balanced.resource-gate.log")"
    [[ "$full_fast_map_unallocated_ram" =~ ^[0-9]+$ &&
       "$full_fast_runtime_ram_reserve" == 13360 &&
       "$full_fast_post_reserve_ram" =~ ^[0-9]+$ &&
       "$full_fast_initial_task_stack" == 4096 &&
       "$full_fast_initial_task_static_frame" == 440 &&
       "$full_fast_initial_task_logical_headroom" == 3656 &&
       "$full_balanced_map_unallocated_ram" =~ ^[0-9]+$ &&
       "$full_balanced_runtime_ram_reserve" == 13360 &&
       "$full_balanced_post_reserve_ram" =~ ^[0-9]+$ &&
       "$full_balanced_initial_task_stack" == 4096 &&
       "$full_balanced_initial_task_static_frame" == 440 &&
       "$full_balanced_initial_task_logical_headroom" == 3656 ]] || return 1
    require_runtime_ram_report "$full_fast_map_unallocated_ram" \
        "$full_fast_runtime_ram_reserve" "$full_fast_post_reserve_ram" || return 1
    require_runtime_ram_report "$full_balanced_map_unallocated_ram" \
        "$full_balanced_runtime_ram_reserve" "$full_balanced_post_reserve_ram" || return 1
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
        printf 'full_fast.map_unallocated_ram_bytes=%s\n' "$full_fast_map_unallocated_ram"
        printf 'full_fast.runtime_ram_reserve_bytes=%s\n' "$full_fast_runtime_ram_reserve"
        printf 'full_fast.post_reserve_ram_bytes=%s\n' "$full_fast_post_reserve_ram"
        printf 'full_fast.initial_task_stack_bytes=%s\n' "$full_fast_initial_task_stack"
        printf 'full_fast.initial_task_static_frame_bytes=%s\n' "$full_fast_initial_task_static_frame"
        printf 'full_fast.initial_task_logical_headroom_bytes=%s\n' "$full_fast_initial_task_logical_headroom"
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
        printf 'full_balanced.map_unallocated_ram_bytes=%s\n' "$full_balanced_map_unallocated_ram"
        printf 'full_balanced.runtime_ram_reserve_bytes=%s\n' "$full_balanced_runtime_ram_reserve"
        printf 'full_balanced.post_reserve_ram_bytes=%s\n' "$full_balanced_post_reserve_ram"
        printf 'full_balanced.initial_task_stack_bytes=%s\n' "$full_balanced_initial_task_stack"
        printf 'full_balanced.initial_task_static_frame_bytes=%s\n' "$full_balanced_initial_task_static_frame"
        printf 'full_balanced.initial_task_logical_headroom_bytes=%s\n' "$full_balanced_initial_task_logical_headroom"
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

run_d2_application_incremental_acceptance() {
    local evidence_dir checker off_dir on_dir off_manifest on_manifest seal

    checker="${MICROBIT_ROOT}/scripts/check_tavrn_expiry_resources.py"
    evidence_dir="$(mktemp -d "${TMPDIR:-/tmp}/tron-d2-application-incremental.XXXXXX")"
    off_dir="${evidence_dir}/ingress-off"
    on_dir="${evidence_dir}/ingress-on"
    seal="${evidence_dir}/ingress-off.manifest.sha256"
    run_expiry_fixture_checks
    bash "${MICROBIT_ROOT}/build-tavrn-ble.sh" --target tavrn_routed_node \
        --feature FULL_TAVRN --repair ON --timer BALANCED --stack-usage \
        --app-node-number 6 --d2-current-resource-gate --out "$off_dir" >/dev/null
    off_manifest="$(expiry_manifest_path "$off_dir")"
    python3 "$checker" --seal-passed-artifact-manifest "$off_manifest" \
        --seal-output "$seal" >/dev/null
    bash "${MICROBIT_ROOT}/build-tavrn-ble.sh" --target tavrn_routed_node \
        --feature FULL_TAVRN --repair ON --timer BALANCED --stack-usage \
        --wearable-ingress ON --app-node-number 6 --d2-current-resource-gate \
        --application-resource-baseline "$off_dir" \
        --application-resource-baseline-seal "$seal" --out "$on_dir" >/dev/null
    on_manifest="$(expiry_manifest_path "$on_dir")"
    python3 "$checker" --d2-paired-acceptance-report "$D2_INCREMENTAL_REPORT" \
        --d2-off-artifact-manifest "$off_manifest" --d2-off-seal "$seal" \
        --d2-on-artifact-manifest "$on_manifest" >/dev/null
    [[ -s "$D2_INCREMENTAL_REPORT" ]] || return 1
    d2_pair_attack_must_fail() {
        local mode="$1"
        local attack_dir="${evidence_dir}/attack-${mode}"
        local attack_manifest attack_report status

        cp -a "$on_dir" "$attack_dir"
        python3 - "$attack_dir" "$mode" <<'PY'
import hashlib
import json
import pathlib
import re
import sys

directory = pathlib.Path(sys.argv[1])
mode = sys.argv[2]
manifest = next(path for path in directory.glob("*.manifest")
                if not path.name.endswith(".build-config.manifest"))
values = dict(line.split("=", 1) for line in manifest.read_text(encoding="utf-8").splitlines()
              if line)

if mode == "schema-only-contract":
    contract = directory / values["resource.contract.0.name"]
    contract.write_text(json.dumps({
        "schema": "tron.tavrn.expiry.required-stack-edges.v3"}), encoding="utf-8")
    changed = ("resource.contract.0",)
elif mode == "coordinated-metrics":
    resource = directory / values["resource.manifest.name"]
    document = json.loads(resource.read_text(encoding="utf-8"))
    document["stack"]["total_bytes"] = 1
    document["stack"]["headroom_bytes"] = 4863
    resource.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    gate = directory / values["resource.gate_report.name"]
    gate.write_text(
        re.sub(r"(?m)^MESH_STACK_TOTAL_BYTES=[0-9]+$", "MESH_STACK_TOTAL_BYTES=1",
               re.sub(r"(?m)^MESH_STACK_HEADROOM_BYTES=[0-9]+$",
                      "MESH_STACK_HEADROOM_BYTES=4863", gate.read_text(encoding="utf-8"))),
        encoding="utf-8")
    changed = ("resource.manifest", "resource.gate_report")
elif mode in {"mesh-chain-empty", "mesh-chain-missing", "mesh-chain-duplicate",
              "auxiliary-chain-empty"}:
    gate = directory / values["resource.gate_report.name"]
    contents = gate.read_text(encoding="utf-8")
    if mode == "mesh-chain-empty":
        contents, count = re.subn(r"(?m)^STACK_CHAIN=.*$", "STACK_CHAIN=[]", contents)
    elif mode == "mesh-chain-missing":
        contents, count = re.subn(r"(?m)^STACK_CHAIN=.*\n?", "", contents)
    elif mode == "mesh-chain-duplicate":
        match = re.search(r"(?m)^STACK_CHAIN=.*$", contents)
        if match is None:
            raise SystemExit("missing mesh STACK_CHAIN")
        contents = contents[:match.end()] + "\n" + match.group(0) + contents[match.end():]
        count = 1
    else:
        contents, count = re.subn(r"(?m)^MIND_UI_TASK_STACK_CHAIN=.*$",
                                  "MIND_UI_TASK_STACK_CHAIN=[]", contents)
    if count != 1:
        raise SystemExit("expected one chain line to mutate")
    gate.write_text(contents, encoding="utf-8")
    changed = ("resource.gate_report",)
else:
    raise SystemExit("unknown D2 attack")

for key in changed:
    path = directory / values[key + ".name"]
    values[key + ".sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    values[key + ".size"] = str(path.stat().st_size)
manifest.write_text("".join("%s=%s\n" % item for item in sorted(values.items())), encoding="utf-8")
PY
        attack_manifest="$(expiry_manifest_path "$attack_dir")"
        attack_report="${evidence_dir}/attack-${mode}.report"
        set +e
        python3 "$checker" --d2-paired-acceptance-report "$attack_report" \
            --d2-off-artifact-manifest "$off_manifest" --d2-off-seal "$seal" \
            --d2-on-artifact-manifest "$attack_manifest" >/dev/null 2>&1
        status=$?
        set -e
        if [[ $status -eq 0 ]]; then
            printf 'D2 self-consistent %s attack unexpectedly passed\n' "$mode" >&2
            return 1
        fi
    }
    d2_pair_attack_must_fail schema-only-contract
    d2_pair_attack_must_fail coordinated-metrics
    d2_pair_attack_must_fail mesh-chain-empty
    d2_pair_attack_must_fail mesh-chain-missing
    d2_pair_attack_must_fail mesh-chain-duplicate
    d2_pair_attack_must_fail auxiliary-chain-empty
    printf 'D2 application incremental acceptance artifacts: %s\n' "$evidence_dir"
}

if [[ -n "$D2_INCREMENTAL_REPORT" ]]; then
    run_d2_application_incremental_acceptance
    exit 0
fi

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
six_board_routed_candidate_args=(
    -DTRON_PHASE1_TARGET=ROUTED
    -DTRON_NODE_MODE=TAVRN_ROUTED
    -DTRON_HARDWARE_CANDIDATE=ON
    -DTRON_ADVA_OVERRIDE=18:42:de:52:4a:dd
    -DTRON_TARGET_PROBE_UID=9906360200052820cf57b9f988a30e16000000006e052820
    -DTRON_TARGET_INVENTORY_FILE="$SIX_BOARD_INVENTORY"
)
ROUTED_SID8_STATUS_INVENTORY="$WORK_DIR/tavrn_inventory_routed_sid8_status.tsv"
printf '%s\t%s\n' \
    board-a 00:42:de:52:4a:dd \
    board-b 00:43:0a:06:03:f8 \
    board-c 1e:33:a7:2f:8e:d8 \
    board-d 51:56:ae:12:21:ca \
    board-e be:65:0b:2c:96:d0 \
    board-f 56:a2:44:0e:9e:d6 > "$ROUTED_SID8_STATUS_INVENTORY"

# STACK-INIT-RED: a routed build must publish a dedicated initial-task stack
# capacity rather than silently inheriting the kernel's 1024-byte default.
configure_ok routed-initial-task-stack-red -DTRON_PHASE1_TARGET=ROUTED \
    -DTRON_NODE_MODE=TAVRN_ROUTED -DTAVRN_FEATURE_LEVEL=AODV_ONLY
initial_stack_red_manifest="$(routed_manifest_path routed-initial-task-stack-red)"
require_line 'build.initial_task_stack_bytes=4096' "$initial_stack_red_manifest"

# BEARER-CAP-01: routed queue storage is a generated target input. The default
# control stays four, while each accepted benchmark capacity is built from the
# same routed composition and reaches the shared queue header through the
# force-included generated configuration.
configure_ok routed-queue-capacity-default -DTRON_PHASE1_TARGET=ROUTED \
    -DTRON_NODE_MODE=TAVRN_ROUTED -DTAVRN_FEATURE_LEVEL=AODV_ONLY
routed_queue_default_manifest="$(routed_manifest_path routed-queue-capacity-default)"
routed_queue_default_config="$WORK_DIR/routed-queue-capacity-default/app/tavrn_routed_node/generated/tavrn_routed_node/tron_build_config.h"
require_line 'capacity.scheduler_tx_queue=4' "$routed_queue_default_manifest"
if ! grep -Fqx '#define TRON_BUILD_ROUTED_TX_QUEUE_CAPACITY 4u' \
        "$routed_queue_default_config"; then
    printf '%s\n' 'default routed queue capacity is not generated as four' >&2
    exit 1
fi
build_target routed-queue-capacity-default tavrn_routed_node
require_compile_option_pair_once \
    "$WORK_DIR/routed-queue-capacity-default/compile_commands.json" \
    "$MICROBIT_ROOT/app/protocol/ble_mesh_tx_queue.c" -include \
    "$routed_queue_default_config"

for routed_queue_capacity in 4 8 16 40; do
    routed_queue_name="routed-queue-capacity-${routed_queue_capacity}"
    configure_ok "$routed_queue_name" -DTRON_PHASE1_TARGET=ROUTED \
        -DTRON_NODE_MODE=TAVRN_ROUTED -DTAVRN_FEATURE_LEVEL=AODV_ONLY \
        -DTRON_ROUTED_TX_QUEUE_CAPACITY="$routed_queue_capacity"
    routed_queue_manifest="$(routed_manifest_path "$routed_queue_name")"
    routed_queue_config="$WORK_DIR/$routed_queue_name/app/tavrn_routed_node/generated/tavrn_routed_node/tron_build_config.h"
    require_line "capacity.scheduler_tx_queue=${routed_queue_capacity}" \
        "$routed_queue_manifest"
    if ! grep -Fqx "#define TRON_BUILD_ROUTED_TX_QUEUE_CAPACITY ${routed_queue_capacity}u" \
            "$routed_queue_config"; then
        printf 'generated routed queue capacity mismatch: %s\n' "$routed_queue_capacity" >&2
        exit 1
    fi
    build_target "$routed_queue_name" tavrn_routed_node
    require_compile_option_pair_once \
        "$WORK_DIR/$routed_queue_name/compile_commands.json" \
        "$MICROBIT_ROOT/app/protocol/ble_mesh_tx_queue.c" -include \
        "$routed_queue_config"
done
configure_fail_with routed-queue-capacity-invalid \
    'TRON_ROUTED_TX_QUEUE_CAPACITY must be exactly 4, 8, 16, or 40' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_ROUTED_TX_QUEUE_CAPACITY=5
configure_fail_with legacy-routed-queue-capacity \
    'TRON_ROUTED_TX_QUEUE_CAPACITY is available only for ROUTED targets' \
    -DTRON_PHASE1_TARGET=LEGACY -DTRON_ROUTED_TX_QUEUE_CAPACITY=8
configure_fail_with link-routed-queue-capacity \
    'TRON_ROUTED_TX_QUEUE_CAPACITY is available only for ROUTED targets' \
    -DTRON_PHASE1_TARGET=LINK -DTRON_ROUTED_TX_QUEUE_CAPACITY=40

# BUILD-P1-01: selected target isolates legacy source/identity composition.
configure_ok default-legacy -DTRON_PHASE1_TARGET=LEGACY
legacy_manifest="$(legacy_manifest_path default-legacy)"
require_line 'build.phase1_target=LEGACY' "$legacy_manifest"
require_line 'build.behavior=LEGACY_FLOOD' "$legacy_manifest"
require_line 'build.initial_task_stack_bytes=1024' "$legacy_manifest"
require_line 'capacity.scheduler_tx_queue=4' "$legacy_manifest"
require_line 'capacity.routed_initial_task_stack_bytes=4096' "$legacy_manifest"
require_line 'capacity.routed_initial_task_stack_bytes.state=NOT_APPLICABLE' "$legacy_manifest"
require_line 'capacity.routed_mesh_task_stack_bytes=4864' "$legacy_manifest"
require_line 'capacity.routed_mesh_task_stack_bytes.state=NOT_APPLICABLE' "$legacy_manifest"
require_line 'build.routed_logger_task_stack_bytes=1840' "$legacy_manifest"
require_line 'capacity.routed_logger_task_stack_bytes=1840' "$legacy_manifest"
require_line 'capacity.routed_logger_task_stack_bytes.state=NOT_APPLICABLE' "$legacy_manifest"
require_line 'resource.runtime_ram_reserve_bytes=0' "$legacy_manifest"
require_line 'identity.adva=NOT_APPLICABLE' "$legacy_manifest"
require_line 'link_test.peer_adva=NOT_APPLICABLE' "$legacy_manifest"
require_line 'bench.identify_display=OFF' "$legacy_manifest"
require_line 'bench.role_number=0' "$legacy_manifest"
require_selected_source_count "$legacy_manifest" 'app/drivers/display.c' 0
if grep '^source\.selected\.[0-9].*=' "$legacy_manifest" | grep -q 'tavrn_\|ble_link_v2_testbed'; then
    printf '%s\n' 'legacy source manifest unexpectedly imports routed/link sources' >&2
    exit 1
fi
if [[ "$(grep -c '^source\.selected\.[0-9].*=libs/mtkernel_3/include/sys/inittask.h$' "$legacy_manifest")" -ne 1 ]]; then
    printf '%s\n' 'legacy source manifest lacks exactly one initial-task header provenance record' >&2
    exit 1
fi
build_target default-legacy ble_mesh_node
require_no_initial_task_override "$WORK_DIR/default-legacy/compile_commands.json"
require_compile_option_pair_once "$WORK_DIR/default-legacy/compile_commands.json" \
    "$MICROBIT_ROOT/app/protocol/ble_mesh_tx_queue.c" -include \
    "$WORK_DIR/default-legacy/app/ble_mesh_node/generated/ble_mesh_node/tron_build_config.h"
legacy_config_header="$WORK_DIR/default-legacy/app/ble_mesh_node/generated/ble_mesh_node/tron_build_config.h"
if ! grep -Fqx '#define TRON_BUILD_INITIAL_TASK_STACK_BYTES 1024u' "$legacy_config_header" ||
   ! grep -Fqx '#define TRON_BUILD_ROUTED_TX_QUEUE_CAPACITY 4u' "$legacy_config_header" ||
   ! grep -Fqx '#define TRON_BUILD_RUNTIME_RAM_RESERVE_BYTES 0u' "$legacy_config_header"; then
    printf '%s\n' 'legacy generated config does not retain default initial-task RAM values' >&2
    exit 1
fi

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
require_line 'timer.link_no_response_wall_bound_ms=9894' "$runtime_manifest"
require_line 'capacity.link_custody.state=IMPLEMENTED' "$runtime_manifest"
require_line 'capacity.scheduler_tx_queue=4' "$runtime_manifest"
require_line 'capacity.aodv_routes.state=NOT_IMPLEMENTED' "$runtime_manifest"
require_line 'capacity.retry_log_mailbox=1' "$runtime_manifest"
require_line 'capacity.retry_log_mailbox.policy=RETAIN_OLDEST_DROP_NEWEST' "$runtime_manifest"
require_line 'capacity.retry_log_mailbox.dropped_telemetry=SATURATING_COUNTER' "$runtime_manifest"
require_line 'capacity.retry_log_mailbox.state=NOT_APPLICABLE' "$runtime_manifest"
require_line 'build.initial_task_stack_bytes=1024' "$runtime_manifest"
require_line 'capacity.routed_initial_task_stack_bytes=4096' "$runtime_manifest"
require_line 'capacity.routed_initial_task_stack_bytes.state=NOT_APPLICABLE' "$runtime_manifest"
require_line 'capacity.routed_mesh_task_stack_bytes=4864' "$runtime_manifest"
require_line 'capacity.routed_mesh_task_stack_bytes.state=NOT_APPLICABLE' "$runtime_manifest"
require_line 'build.routed_logger_task_stack_bytes=1840' "$runtime_manifest"
require_line 'capacity.routed_logger_task_stack_bytes=1840' "$runtime_manifest"
require_line 'capacity.routed_logger_task_stack_bytes.state=NOT_APPLICABLE' "$runtime_manifest"
require_line 'resource.runtime_ram_reserve_bytes=0' "$runtime_manifest"
require_line 'bench.identify_display=OFF' "$runtime_manifest"
require_line 'bench.role_number=0' "$runtime_manifest"
require_selected_source_count "$runtime_manifest" 'app/drivers/display.c' 0
build_target runtime-link ble_link_v2_testbed
require_no_initial_task_override "$WORK_DIR/runtime-link/compile_commands.json"
require_compile_option_pair_once "$WORK_DIR/runtime-link/compile_commands.json" \
    "$MICROBIT_ROOT/app/protocol/ble_mesh_tx_queue.c" -include \
    "$WORK_DIR/runtime-link/app/ble_link_v2_testbed/generated/ble_link_v2_testbed/tron_build_config.h"
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
   ! grep -Fqx '#define TRON_BUILD_INITIAL_TASK_STACK_BYTES 1024u' "$runtime_config_header" ||
   ! grep -Fqx '#define TRON_BUILD_ROUTED_TX_QUEUE_CAPACITY 4u' "$runtime_config_header" ||
   ! grep -Fqx '#define TRON_BUILD_RUNTIME_RAM_RESERVE_BYTES 0u' "$runtime_config_header" ||
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
require_line 'build.initial_task_stack_bytes=4096' "$routed_manifest"
require_line 'capacity.routed_initial_task_stack_bytes=4096' "$routed_manifest"
require_line 'capacity.routed_initial_task_stack_bytes.state=IMPLEMENTED' "$routed_manifest"
require_line 'build.routed_mesh_task_stack_bytes=4864' "$routed_manifest"
require_line 'capacity.routed_mesh_task_stack_bytes=4864' "$routed_manifest"
require_line 'capacity.routed_mesh_task_stack_bytes.state=IMPLEMENTED' "$routed_manifest"
require_line 'build.routed_logger_task_stack_bytes=1840' "$routed_manifest"
require_line 'capacity.routed_logger_task_stack_bytes=1840' "$routed_manifest"
require_line 'capacity.routed_logger_task_stack_bytes.state=IMPLEMENTED' "$routed_manifest"
require_line 'resource.runtime_ram_reserve_bytes=13360' "$routed_manifest"
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
require_line 'capacity.retry_log_mailbox=1' "$routed_manifest"
require_line 'capacity.retry_log_mailbox.policy=RETAIN_OLDEST_DROP_NEWEST' "$routed_manifest"
require_line 'capacity.retry_log_mailbox.dropped_telemetry=SATURATING_COUNTER' "$routed_manifest"
require_line 'capacity.retry_log_mailbox.state=IMPLEMENTED' "$routed_manifest"
require_line 'capacity.benchmark_accepted_fifo=1024' "$routed_manifest"
require_line 'capacity.benchmark_accepted_fifo.state=NOT_APPLICABLE' "$routed_manifest"
require_line 'capacity.benchmark_final_fifo=1024' "$routed_manifest"
require_line 'capacity.benchmark_final_fifo.state=NOT_APPLICABLE' "$routed_manifest"
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
require_line 'bench.identify_display=OFF' "$routed_manifest"
require_line 'bench.role_number=0' "$routed_manifest"
require_selected_source_count "$routed_manifest" 'app/drivers/display.c' 0
require_selected_source_count "$routed_manifest" 'app/tavrn_routed_node/src/routed_benchmark.c' 0
require_selected_source_count "$routed_manifest" 'app/tavrn_routed_node/src/routed_benchmark_observer.c' 0
require_selected_source_count "$routed_manifest" 'app/tavrn_routed_node/src/routed_benchmark_uart_tx.c' 0
if [[ "$(grep -c '^capacity\.[^.]*=' "$routed_manifest")" -ne 50 ]]; then
    printf '%s\n' 'routed AODV_ONLY capacity schema width is not exact' >&2
    exit 1
fi
require_unique_keys "$routed_manifest"
if [[ "$(grep -c '^source\.selected\.[0-9].*=libs/mtkernel_3/include/sys/inittask.h$' "$routed_manifest")" -ne 1 ]]; then
    printf '%s\n' 'routed AODV_ONLY source manifest lacks exactly one initial-task header provenance record' >&2
    exit 1
fi
if ! grep '^source\.selected\.[0-9].*=' "$routed_manifest" | grep -q 'app/protocol/aodv_core.c' ||
   ! grep '^source\.selected\.[0-9].*=' "$routed_manifest" | grep -q 'app/protocol/tavrn_router.c' ||
   ! grep '^source\.selected\.[0-9].*=' "$routed_manifest" | grep -q 'app/tavrn_routed_node/src/routed_cycle.c' ||
   ! grep '^source\.selected\.[0-9].*=' "$routed_manifest" | grep -q 'libs/mtkernel_3/include/sys/inittask.h' ||
    grep '^source\.selected\.[0-9].*=' "$routed_manifest" | grep -q 'tron_mesh_\|routed_full_telemetry\|tavrn_esc\|tavrn_gtt\|tavrn_full\|tavrn_maintenance\|tavrn_mentorship\|tavrn_smart_ttl\|tavrn_repair'; then
    printf '%s\n' 'routed AODV_ONLY source manifest has missing router/core or leaked sources' >&2
    exit 1
fi
build_target routed-aodv tavrn_routed_node
routed_aodv_config="$WORK_DIR/routed-aodv/app/tavrn_routed_node/generated/tavrn_routed_node/tron_build_config.h"
if ! grep -Fqx '#define TRON_BUILD_ROUTED_FULL_TAVRN 0' "$routed_aodv_config" ||
   ! grep -Fqx '#define TRON_BUILD_TEST_EXPIRY_FULL_TABLE 0' "$routed_aodv_config" ||
   ! grep -Fqx '#define TRON_BUILD_BENCH_ROLE_NUMBER 0u' "$routed_aodv_config" ||
    ! grep -Fqx '#define TRON_BUILD_BENCH_IDENTIFY_DISPLAY 0' "$routed_aodv_config" ||
    ! grep -Fqx '#define TRON_BUILD_ROUTED_MESH_TASK_STACK_BYTES 4864u' "$routed_aodv_config" ||
    ! grep -Fqx '#define TRON_BUILD_ROUTED_LOGGER_TASK_STACK_BYTES 1840u' "$routed_aodv_config" ||
    ! grep -Fqx '#define TRON_BUILD_INITIAL_TASK_STACK_BYTES 4096u' "$routed_aodv_config" ||
    ! grep -Fqx '#define TRON_BUILD_RUNTIME_RAM_RESERVE_BYTES 13360u' "$routed_aodv_config"; then
    printf '%s\n' 'AODV_ONLY generated config does not expose the selected feature macro' >&2
    exit 1
fi
require_compile_definition_once "$WORK_DIR/routed-aodv/compile_commands.json" \
    "$MICROBIT_ROOT/app/tavrn_routed_node/src/main.c" 'INITTASK_STKSZ=4096'
require_compile_definition_once "$WORK_DIR/routed-aodv/compile_commands.json" \
    "$MICROBIT_ROOT/app/tavrn_routed_node/src/main.c" 'TRON_ROUTED_LOGGER_TASK_STACK_BYTES=1840'
require_compile_definition_once "$WORK_DIR/routed-aodv/compile_commands.json" \
    "$MICROBIT_ROOT/libs/mtkernel_3/kernel/inittask/inittask.c" 'INITTASK_STKSZ=4096' \
    'mtkernel3_microbit_kernel_tavrn_routed_node'

# BUILD-P4-01: FULL_TAVRN adds K=1 ESC and mentorship around the routed
# main/router/AODV/GTT source closure.
configure_ok routed-full -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTRON_TIMER_PROFILE=FAST_TEST
routed_full_manifest="$(routed_manifest_path routed-full)"
require_line 'build.phase1_target=ROUTED' "$routed_full_manifest"
require_line 'build.behavior=TAVRN_ROUTED_FULL_TAVRN_ESC_K1_MENTORSHIP_ADAPTIVE_HELLO_LOCAL_EXPIRY_TARGETED_FRESHNESS_RREQ_VERIFICATION_TC_METADATA' "$routed_full_manifest"
require_line 'feature.level.effective=FULL_TAVRN' "$routed_full_manifest"
require_line 'feature.repair.effective=OFF' "$routed_full_manifest"
require_line 'capacity.repair_contexts.state=NOT_IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.repair_data.state=NOT_IMPLEMENTED' "$routed_full_manifest"
require_line 'build.initial_task_stack_bytes=4096' "$routed_full_manifest"
require_line 'capacity.routed_initial_task_stack_bytes=4096' "$routed_full_manifest"
require_line 'capacity.routed_initial_task_stack_bytes.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'build.routed_mesh_task_stack_bytes=4864' "$routed_full_manifest"
require_line 'capacity.routed_mesh_task_stack_bytes=4864' "$routed_full_manifest"
require_line 'capacity.routed_mesh_task_stack_bytes.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'build.routed_logger_task_stack_bytes=1840' "$routed_full_manifest"
require_line 'capacity.routed_logger_task_stack_bytes=1840' "$routed_full_manifest"
require_line 'capacity.routed_logger_task_stack_bytes.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'resource.runtime_ram_reserve_bytes=13360' "$routed_full_manifest"
    require_line 'build.implemented_capabilities=wire-v2,link-v2,custody,aodv,esc-k1,sid8-identity-context,mentorship-bootstrap,passive-gtt,smart-ttl,adaptive-sid8-hello,hello-ema-snap,hello-topology-reset,hello-broadcast-suppression,hello-liveness-hysteresis,hello-equality-dedupe,hello-gtt-liveness,local-expiry-demand,targeted-freshness-stage0,targeted-hello-request-response,targeted-runtime-binding,retained-hop-full-diameter-rreq-verification,tc-join-leave,general-route-metadata,maintenance-telemetry,tc-metadata-telemetry,typed-runtime-observability,rreq-scope-telemetry,gtt-snapshot' "$routed_full_manifest"
require_line 'fixed_k.state=1' "$routed_full_manifest"
require_line 'capacity.gtt_membership.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.retry_log_mailbox=1' "$routed_full_manifest"
require_line 'capacity.retry_log_mailbox.policy=RETAIN_OLDEST_DROP_NEWEST' "$routed_full_manifest"
require_line 'capacity.retry_log_mailbox.dropped_telemetry=SATURATING_COUNTER' "$routed_full_manifest"
require_line 'capacity.retry_log_mailbox.state=IMPLEMENTED' "$routed_full_manifest"
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
require_line 'capacity.metadata_candidates=4' "$routed_full_manifest"
require_line 'capacity.metadata_candidates.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.tc_uuid=16' "$routed_full_manifest"
require_line 'capacity.tc_uuid.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.tc_subject=16' "$routed_full_manifest"
require_line 'capacity.tc_subject.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.tc_origin=16' "$routed_full_manifest"
require_line 'capacity.tc_origin.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'capacity.tc_relay=16' "$routed_full_manifest"
require_line 'capacity.tc_relay.state=IMPLEMENTED' "$routed_full_manifest"
require_line 'bound.mentor_failure_protocol_ms=3850' "$routed_full_manifest"
require_line 'bound.mentor_failure_protocol_ms.scope=PROTOCOL_DEADLINES_ONLY_EXCLUDES_SCHEDULER_APPLICATION_CADENCE' "$routed_full_manifest"
require_line 'formula.mentor_failure_protocol_ms=timer.mentor_rssi_weak_delay_ms+timer.mentor_jitter_max_ms+timer.mentor_offer_window_ms+timer.mentor_page_attempts*timer.mentor_page_timeout_ms+timer.mentor_self_bootstrap_ms' "$routed_full_manifest"
require_line 'bench.identify_display=OFF' "$routed_full_manifest"
require_line 'bench.role_number=0' "$routed_full_manifest"
require_line 'application.node_number=1' "$routed_full_manifest"
require_line 'application.wearable_ingress.requested=OFF' "$routed_full_manifest"
require_line 'application.wearable_ingress.effective=OFF' "$routed_full_manifest"
require_selected_source_count "$routed_full_manifest" 'app/drivers/display.c' 0
require_selected_source_count "$routed_full_manifest" 'app/tavrn_routed_node/src/routed_benchmark.c' 0
require_selected_source_count "$routed_full_manifest" 'app/tavrn_routed_node/src/routed_benchmark_observer.c' 0
require_selected_source_count "$routed_full_manifest" 'app/tavrn_routed_node/src/routed_benchmark_uart_tx.c' 0
require_selected_source_count "$routed_full_manifest" 'app/tavrn_routed_node/src/routed_benchmark_full.c' 0
require_selected_source_count "$routed_full_manifest" \
    'app/mind_application/mind_application_wire.c' 0
require_selected_source_count "$routed_full_manifest" \
    'app/mind_application/mind_application_ingress.c' 0
require_selected_source_count "$routed_full_manifest" \
    'app/mind_application/mind_event_forwarder.c' 0
if [[ "$(grep -c '^capacity\.[^.]*=' "$routed_full_manifest")" -ne 50 ]]; then
    printf '%s\n' 'routed FULL_TAVRN capacity schema width is not exact' >&2
    exit 1
fi
require_unique_keys "$routed_full_manifest"
if [[ "$(grep -c '^source\.selected\.[0-9].*=libs/mtkernel_3/include/sys/inittask.h$' "$routed_full_manifest")" -ne 1 ]]; then
    printf '%s\n' 'FULL_TAVRN source manifest lacks exactly one initial-task header provenance record' >&2
    exit 1
fi
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
if ! grep '^source\.selected\.[0-9].*=' "$routed_full_manifest" | \
    grep -q 'libs/mtkernel_3/include/sys/inittask.h'; then
    printf '%s\n' 'FULL_TAVRN source manifest lacks initial-task header provenance' >&2
    exit 1
fi
if grep '^source\.selected\.[0-9].*=' "$routed_full_manifest" | grep -q 'tron_mesh_\|tavrn_repair'; then
    printf '%s\n' 'FULL_TAVRN source manifest leaks unimplemented feature sources' >&2
    exit 1
fi
require_exact_selected_sources "$routed_full_manifest" \
    'app/tavrn_routed_node/src/main.c' \
    'generated/tron_build_info.c' \
    'generated/tron_timer_config.c' \
    'app/drivers/ble_radio.c' \
    'app/protocol/ble_mesh_scheduler.c' \
    'app/protocol/ble_mesh_tx_queue.c' \
    'app/tavrn_routed_node/src/routed_cycle.c' \
    'app/protocol/aodv_core.c' \
    'app/protocol/tavrn_link_v2.c' \
    'app/protocol/tavrn_router.c' \
    'app/protocol/tavrn_wire_v2.c' \
    'app/tavrn_routed_node/src/routed_full_telemetry.c' \
    'app/protocol/tavrn_esc.c' \
    'app/protocol/tavrn_full.c' \
    'app/protocol/tavrn_full_maintenance_binding.c' \
    'app/protocol/tavrn_gtt.c' \
    'app/protocol/tavrn_maintenance.c' \
    'app/protocol/tavrn_mentorship.c' \
    'app/protocol/tavrn_smart_ttl.c' \
    'libs/mtkernel_3/include/sys/inittask.h'
build_target routed-full tavrn_routed_node
routed_full_elf="$WORK_DIR/routed-full/firmware/tavrn_routed_node/tavrn_routed_node.elf"
for marker in 'routed cycle_diagnostic' 'routed stats now=' \
              'routed expiry_sweep' 'routed rreq_lifecycle'; do
    if ! grep -aFq -- "$marker" "$routed_full_elf"; then
        printf 'verbose FULL_TAVRN ELF lacks telemetry marker: %s\n' "$marker" >&2
        exit 1
    fi
done
routed_full_config="$WORK_DIR/routed-full/app/tavrn_routed_node/generated/tavrn_routed_node/tron_build_config.h"
if ! grep -Fqx '#define TRON_BUILD_ROUTED_FULL_TAVRN 1' "$routed_full_config" ||
   ! grep -Fqx '#define TRON_BUILD_ENABLE_WEARABLE_INGRESS 0' "$routed_full_config" ||
   ! grep -Fqx '#define TRON_BUILD_APP_NODE_NUMBER 1u' "$routed_full_config" ||
   ! grep -Fqx '#define TRON_BUILD_LOCAL_REPAIR 0' "$routed_full_config" ||
   ! grep -Fqx '#define TRON_BUILD_TEST_EXPIRY_FULL_TABLE 0' "$routed_full_config" ||
   ! grep -Fqx '#define TRON_BUILD_BENCH_ROLE_NUMBER 0u' "$routed_full_config" ||
    ! grep -Fqx '#define TRON_BUILD_BENCH_IDENTIFY_DISPLAY 0' "$routed_full_config" ||
    ! grep -Fqx '#define TRON_BUILD_ROUTED_MESH_TASK_STACK_BYTES 4864u' "$routed_full_config" ||
    ! grep -Fqx '#define TRON_BUILD_ROUTED_LOGGER_TASK_STACK_BYTES 1840u' "$routed_full_config" ||
    ! grep -Fqx '#define TRON_BUILD_INITIAL_TASK_STACK_BYTES 4096u' "$routed_full_config" ||
    ! grep -Fqx '#define TRON_BUILD_RUNTIME_RAM_RESERVE_BYTES 13360u' "$routed_full_config"; then
    printf '%s\n' 'FULL_TAVRN generated config does not expose the selected feature macro' >&2
    exit 1
fi
require_compile_definition_once "$WORK_DIR/routed-full/compile_commands.json" \
    "$MICROBIT_ROOT/app/tavrn_routed_node/src/main.c" 'INITTASK_STKSZ=4096'
require_compile_definition_once "$WORK_DIR/routed-full/compile_commands.json" \
    "$MICROBIT_ROOT/app/tavrn_routed_node/src/main.c" 'TRON_ROUTED_LOGGER_TASK_STACK_BYTES=1840'
require_compile_definition_once "$WORK_DIR/routed-full/compile_commands.json" \
    "$MICROBIT_ROOT/libs/mtkernel_3/kernel/inittask/inittask.c" 'INITTASK_STKSZ=4096' \
    'mtkernel3_microbit_kernel_tavrn_routed_node'

# APP-PROFILE-01: production FULL_TAVRN ingress has a closed Layer-7 source
# set and cannot coexist with benchmark observability sources.
configure_ok routed-full-production -DTRON_PHASE1_TARGET=ROUTED \
    -DTRON_NODE_MODE=TAVRN_ROUTED -DTAVRN_FEATURE_LEVEL=FULL_TAVRN \
    -DTAVRN_ENABLE_LOCAL_REPAIR=ON -DTRON_ENABLE_WEARABLE_INGRESS=ON \
    -DTRON_APP_NODE_NUMBER=6
routed_full_production_manifest="$(routed_manifest_path routed-full-production)"
routed_full_production_config="$WORK_DIR/routed-full-production/app/tavrn_routed_node/generated/tavrn_routed_node/tron_build_config.h"
for expected in \
    'feature.repair.effective=ON' \
    'application.node_number=6' \
    'application.wearable_ingress.requested=ON' \
    'application.wearable_ingress.effective=ON' \
    'application.root_plane.effective=ON' \
    'application.root_registry_capacity=16' \
    'application.root_campaign_capacity=16' \
    'application.root_ack_capacity=16' \
    'application.event_capacity=16' \
    'application.ingress_seen_capacity=16' \
    'application.ingress_queue_capacity=8' \
    'application.final_inbox_capacity=8' \
    'application.logger_capacity=8' \
    'application.uart_rx_ring_capacity=32' \
    'application.command_mailbox_capacity=8' \
    'application.uart_task_stack_bytes=512' \
    'application.ui_task_stack_bytes=512' \
    'application.display_task_stack_bytes=512' \
    'application.task_system_stack_bytes=128' \
    'application.task_stack_alignment_bytes=8' \
    'application.uart_task_static_buffer_bytes=640' \
    'application.ui_task_static_buffer_bytes=640' \
    'application.display_task_static_buffer_bytes=640' \
    'hook.enabled=OFF' \
    'hook.rx_block_adva=NOT_CONFIGURED' \
    'hook.hack_drop_peer_adva=NOT_CONFIGURED' \
    'hook.hack_drop_count=0' \
    'hook.busy_admission_count=0' \
    'hook.collision_peer_adva=NOT_CONFIGURED' \
    'bench.mode=OFF' \
    'link_test.initiator=OFF'; do
    require_line "$expected" "$routed_full_production_manifest"
done
require_selected_source_count "$routed_full_production_manifest" \
    'app/mind_application/mind_application_wire.c' 1
require_selected_source_count "$routed_full_production_manifest" \
    'app/mind_application/mind_application_ingress.c' 1
for source in mind_event_forwarder.c mind_topology_adapter.c mind_root_plane.c mind_root_coordinator.c mind_root_inbox.c \
              mind_command.c mind_log.c mind_log_formatter.c mind_uart.c mind_audio.c mind_ui.c; do
    require_selected_source_count "$routed_full_production_manifest" \
        "app/mind_application/${source}" 1
done
require_selected_source_count "$routed_full_production_manifest" \
    'app/drivers/display.c' 1
require_selected_source_count "$routed_full_production_manifest" \
    'app/tavrn_routed_node/src/routed_benchmark.c' 0
require_selected_source_count "$routed_full_production_manifest" \
    'app/tavrn_routed_node/src/routed_benchmark_observer.c' 0
require_selected_source_count "$routed_full_production_manifest" \
    'app/tavrn_routed_node/src/routed_benchmark_uart_tx.c' 0
require_selected_source_count "$routed_full_production_manifest" \
    'app/tavrn_routed_node/src/routed_benchmark_full.c' 0
require_exact_selected_sources "$routed_full_production_manifest" \
    'app/tavrn_routed_node/src/main.c' \
    'generated/tron_build_info.c' \
    'generated/tron_timer_config.c' \
    'app/drivers/ble_radio.c' \
    'app/protocol/ble_mesh_scheduler.c' \
    'app/protocol/ble_mesh_tx_queue.c' \
    'app/tavrn_routed_node/src/routed_cycle.c' \
    'app/protocol/aodv_core.c' \
    'app/protocol/tavrn_link_v2.c' \
    'app/protocol/tavrn_router.c' \
    'app/protocol/tavrn_wire_v2.c' \
    'app/tavrn_routed_node/src/routed_full_telemetry.c' \
    'app/protocol/tavrn_esc.c' \
    'app/protocol/tavrn_full.c' \
    'app/protocol/tavrn_full_maintenance_binding.c' \
    'app/protocol/tavrn_full_repair_binding.c' \
    'app/protocol/tavrn_repair.c' \
    'app/protocol/tavrn_gtt.c' \
    'app/protocol/tavrn_maintenance.c' \
    'app/protocol/tavrn_mentorship.c' \
    'app/protocol/tavrn_smart_ttl.c' \
    'app/mind_application/mind_application_wire.c' \
    'app/mind_application/mind_application_ingress.c' \
    'app/mind_application/mind_event_forwarder.c' \
    'app/mind_application/mind_topology_adapter.c' \
    'app/mind_application/mind_root_plane.c' \
    'app/mind_application/mind_root_coordinator.c' \
    'app/mind_application/mind_root_inbox.c' \
    'app/mind_application/mind_command.c' \
    'app/mind_application/mind_log.c' \
    'app/mind_application/mind_log_formatter.c' \
    'app/mind_application/mind_uart.c' \
    'app/mind_application/mind_audio.c' \
    'app/mind_application/mind_ui.c' \
    'app/drivers/display.c' \
    'libs/mtkernel_3/include/sys/inittask.h'
if ! grep -Fqx '#define TRON_BUILD_ENABLE_WEARABLE_INGRESS 1' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_APP_NODE_NUMBER 6u' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_MIND_ROOT_CAPACITY 16u' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_MIND_CAMPAIGN_CAPACITY 16u' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_MIND_ROOT_ACK_CAPACITY 16u' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_MIND_EVENT_CAPACITY 16u' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_MIND_INGRESS_QUEUE_CAPACITY 8u' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_MIND_FINAL_INBOX_CAPACITY 8u' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_MIND_LOG_CAPACITY 8u' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_MIND_UART_TASK_STACK_BYTES 512u' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_MIND_UI_TASK_STACK_BYTES 512u' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_MIND_DISPLAY_TASK_STACK_BYTES 512u' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_MIND_TASK_SYSTEM_STACK_BYTES 128u' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_MIND_TASK_STACK_ALIGNMENT_BYTES 8u' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_MIND_UART_TASK_STATIC_BUFFER_BYTES 640u' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_MIND_UI_TASK_STATIC_BUFFER_BYTES 640u' "$routed_full_production_config" ||
    ! grep -Fqx '#define TRON_BUILD_MIND_DISPLAY_TASK_STATIC_BUFFER_BYTES 640u' "$routed_full_production_config"; then
    printf '%s\n' 'production generated config lacks wearable ingress/node-number seams' >&2
    exit 1
fi
build_target routed-full-production tavrn_routed_node
routed_full_production_elf="$WORK_DIR/routed-full-production/firmware/tavrn_routed_node/tavrn_routed_node.elf"
for marker in 'mind_command_v1' 'mind_root_v1' 'mind_event_v1' \
              'routed router_fault reason=' 'routed cycle_fault now_ms='; do
    if ! grep -aFq -- "$marker" "$routed_full_production_elf"; then
        printf 'quiet production ELF lacks required record marker: %s\n' "$marker" >&2
        exit 1
    fi
done
configure_fail_with wearable-ingress-aodv \
    'TRON_ENABLE_WEARABLE_INGRESS=ON requires ROUTED FULL_TAVRN' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_ENABLE_WEARABLE_INGRESS=ON
configure_fail_with wearable-ingress-benchmark \
    'TRON_ENABLE_WEARABLE_INGRESS=ON is incompatible with TRON_BENCHMARK_MODE=ON' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTRON_TIMER_PROFILE=BALANCED \
    -DTRON_ENABLE_TEST_HOOKS=ON -DTRON_BENCHMARK_MODE=ON \
    -DTRON_BENCH_ROLE_NUMBER=2 -DTRON_LINK_TEST_PEER_ADVA=dc:4b:0a:06:03:f8 \
    -DTRON_ENABLE_WEARABLE_INGRESS=ON
configure_fail_with app-node-number-zero \
    'TRON_APP_NODE_NUMBER must be in 1..6' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTRON_APP_NODE_NUMBER=0
configure_fail_with app-node-number-out-of-range \
    'TRON_APP_NODE_NUMBER is outside 0..6' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTRON_APP_NODE_NUMBER=7

# BENCH-IDENT-01: identification is an explicitly hooked routed artifact. Both
# feature levels record their role/display state and contain the display source.
identify_aodv_args=(
    -DTRON_PHASE1_TARGET=ROUTED
    -DTRON_NODE_MODE=TAVRN_ROUTED
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY
    -DTRON_ENABLE_TEST_HOOKS=ON
    -DTRON_BENCH_ROLE_NUMBER=3
    -DTRON_BENCH_IDENTIFY_DISPLAY=ON
)
configure_ok routed-identify-aodv "${identify_aodv_args[@]}"
identify_aodv_manifest="$(routed_manifest_path routed-identify-aodv)"
identify_aodv_config="$WORK_DIR/routed-identify-aodv/app/tavrn_routed_node/generated/tavrn_routed_node/tron_build_config.h"
require_line 'feature.level.effective=AODV_ONLY' "$identify_aodv_manifest"
require_line 'hook.enabled=ON' "$identify_aodv_manifest"
require_line 'bench.identify_display=ON' "$identify_aodv_manifest"
require_line 'bench.role_number=3' "$identify_aodv_manifest"
require_selected_source_count "$identify_aodv_manifest" 'app/drivers/display.c' 1
if ! grep -Fqx '#define TRON_BUILD_BENCH_ROLE_NUMBER 3u' "$identify_aodv_config" ||
   ! grep -Fqx '#define TRON_BUILD_BENCH_IDENTIFY_DISPLAY 1' "$identify_aodv_config" ||
   ! grep -Fq 'role_number=3 identify_display=ON' "$identify_aodv_config"; then
    printf '%s\n' 'AODV_ONLY identification generated config lacks exact role/display evidence' >&2
    exit 1
fi
build_target routed-identify-aodv tavrn_routed_node

identify_full_args=(
    -DTRON_PHASE1_TARGET=ROUTED
    -DTRON_NODE_MODE=TAVRN_ROUTED
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN
    -DTRON_ENABLE_TEST_HOOKS=ON
    -DTRON_BENCH_ROLE_NUMBER=6
    -DTRON_BENCH_IDENTIFY_DISPLAY=ON
)
configure_ok routed-identify-full "${identify_full_args[@]}"
identify_full_manifest="$(routed_manifest_path routed-identify-full)"
identify_full_config="$WORK_DIR/routed-identify-full/app/tavrn_routed_node/generated/tavrn_routed_node/tron_build_config.h"
require_line 'feature.level.effective=FULL_TAVRN' "$identify_full_manifest"
require_line 'hook.enabled=ON' "$identify_full_manifest"
require_line 'bench.identify_display=ON' "$identify_full_manifest"
require_line 'bench.role_number=6' "$identify_full_manifest"
require_selected_source_count "$identify_full_manifest" 'app/drivers/display.c' 1
if ! grep -Fqx '#define TRON_BUILD_BENCH_ROLE_NUMBER 6u' "$identify_full_config" ||
   ! grep -Fqx '#define TRON_BUILD_BENCH_IDENTIFY_DISPLAY 1' "$identify_full_config" ||
   ! grep -Fq 'role_number=6 identify_display=ON' "$identify_full_config"; then
    printf '%s\n' 'FULL_TAVRN identification generated config lacks exact role/display evidence' >&2
    exit 1
fi
build_target routed-identify-full tavrn_routed_node

configure_fail_with identify-display-hooks-off \
    'TRON_BENCH_IDENTIFY_DISPLAY=ON requires TRON_ENABLE_TEST_HOOKS=ON' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_BENCH_ROLE_NUMBER=1 \
    -DTRON_BENCH_IDENTIFY_DISPLAY=ON
configure_fail_with identify-display-role-zero \
    'TRON_BENCH_IDENTIFY_DISPLAY=ON requires TRON_BENCH_ROLE_NUMBER in 1..6' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_BENCH_IDENTIFY_DISPLAY=ON
configure_fail_with identify-display-invalid-state \
    'TRON_BENCH_IDENTIFY_DISPLAY must be exactly ON or OFF' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_BENCH_IDENTIFY_DISPLAY=MAYBE
configure_fail_with identify-role-malformed \
    'TRON_BENCH_ROLE_NUMBER must be an unsigned decimal or hexadecimal integer' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_BENCH_ROLE_NUMBER=three
configure_fail_with identify-role-out-of-range \
    'TRON_BENCH_ROLE_NUMBER is outside 0..6' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_BENCH_ROLE_NUMBER=7
configure_fail_with identify-display-legacy \
    'TRON_BENCH_IDENTIFY_DISPLAY=ON requires ROUTED target' \
    -DTRON_PHASE1_TARGET=LEGACY -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_BENCH_ROLE_NUMBER=1 -DTRON_BENCH_IDENTIFY_DISPLAY=ON
configure_fail_with identify-display-link \
    'TRON_BENCH_IDENTIFY_DISPLAY=ON requires ROUTED target' \
    -DTRON_PHASE1_TARGET=LINK -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_BENCH_ROLE_NUMBER=1 -DTRON_BENCH_IDENTIFY_DISPLAY=ON
configure_fail_with identify-role-link \
    'LINK target rejects legacy node/peer-ID hooks' \
    -DTRON_PHASE1_TARGET=LINK -DTRON_BENCH_ROLE_NUMBER=1

# The glyph bytes and guarded calls are intentionally source-locked because the
# display driver is hardware-bound and has no host-only framebuffer fixture.
display_driver="$MICROBIT_ROOT/app/drivers/display.c"
routed_main="$MICROBIT_ROOT/app/tavrn_routed_node/src/main.c"
for glyph in \
    '    { 0x04u, 0x06u, 0x04u, 0x04u, 0x0eu },' \
    '    { 0x0eu, 0x11u, 0x08u, 0x04u, 0x1fu },' \
    '    { 0x0eu, 0x11u, 0x0cu, 0x11u, 0x0eu },' \
    '    { 0x08u, 0x0cu, 0x0au, 0x1fu, 0x08u },' \
    '    { 0x1fu, 0x01u, 0x0fu, 0x10u, 0x0fu },' \
    '    { 0x0eu, 0x01u, 0x0fu, 0x11u, 0x0eu },'; do
    if ! grep -Fqx "$glyph" "$display_driver"; then
        printf 'identification display glyph is missing: %s\n' "$glyph" >&2
        exit 1
    fi
done
if ! grep -Fqx 'static const UB role_digit_glyphs[6][5] = {' "$display_driver" ||
   ! grep -Fqx '    display_set_rows(role_digit_glyphs[digit - 1u]);' "$display_driver" ||
   ! grep -Fq 'routed startup feature=%s role_number=%u identify_display=%u' "$routed_main" ||
   [[ "$(grep -Fxc '    display_init();' "$routed_main")" -ne 1 ]] ||
   [[ "$(grep -Fxc '    display_show_digit(TRON_BUILD_BENCH_ROLE_NUMBER);' "$routed_main")" -ne 1 ]] ||
    ! awk '
         /^#if TRON_BUILD_BENCH_IDENTIFY_DISPLAY && !TRON_BUILD_BENCHMARK_MODE$/ { guarded = 1; next }
        guarded && /^#endif$/ { guarded = 0; next }
        guarded && /^    display_init\(\);$/ { init = 1 }
        guarded && /^    display_show_digit\(TRON_BUILD_BENCH_ROLE_NUMBER\);$/ { digit = 1 }
        END { exit !(init && digit) }
    ' "$routed_main"; then
    printf '%s\n' 'identification display calls must remain one guarded routed startup action' >&2
    exit 1
fi

# BENCH-OBS-01: continuous control observability is restricted to explicit
# routed builds.  Normal routed source closures contain none of its state or
# callbacks; FULL keeps its destination resolver benchmark-only.
benchmark_aodv_args=(
    -DTRON_PHASE1_TARGET=ROUTED
    -DTRON_NODE_MODE=TAVRN_ROUTED
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY
    -DTRON_TIMER_PROFILE=BALANCED
    -DTRON_ENABLE_TEST_HOOKS=ON
    -DTRON_BENCHMARK_MODE=ON
    -DTRON_BENCH_ROLE_NUMBER=1
    -DTRON_LINK_TEST_PEER_ADVA=dc:4b:0a:06:03:f8
    -DTRON_TEST_RX_BLOCK_ADVA=dc:4b:0a:06:03:f8
)
configure_ok routed-benchmark-aodv "${benchmark_aodv_args[@]}"
benchmark_aodv_manifest="$(routed_manifest_path routed-benchmark-aodv)"
benchmark_aodv_config="$WORK_DIR/routed-benchmark-aodv/app/tavrn_routed_node/generated/tavrn_routed_node/tron_build_config.h"
for expected in \
    'bench.mode=ON' \
    'bench.control_observability=COMPILE_TIME_OPTIONAL' \
    'bench.heartbeat_interval_ms=1000' \
    'bench.burst_start_ms=60000' \
    'bench.burst_period_ms=450000' \
    'bench.burst_interval_ms=100' \
    'bench.burst_duration_ms=60000' \
    'bench.burst_slots=600' \
    'bench.origin_role=1' \
    'bench.destination_role=3' \
    'bench.role_number=1' \
    'bench.identify_display=OFF' \
    'build.routed_logger_task_stack_bytes=1840' \
    'capacity.routed_logger_task_stack_bytes=1840' \
    'capacity.routed_logger_task_stack_bytes.state=IMPLEMENTED' \
    'capacity.benchmark_accepted_fifo=1024' \
    'capacity.benchmark_accepted_fifo.policy=RETAIN_OLDEST_DROP_NEWEST' \
    'capacity.benchmark_accepted_fifo.dropped_telemetry=SATURATING_COUNTER' \
    'capacity.benchmark_accepted_fifo.state=IMPLEMENTED' \
    'capacity.benchmark_final_fifo=1024' \
    'capacity.benchmark_final_fifo.policy=RETAIN_OLDEST_DROP_NEWEST' \
    'capacity.benchmark_final_fifo.dropped_telemetry=SATURATING_COUNTER' \
    'capacity.benchmark_final_fifo.state=IMPLEMENTED'; do
    require_line "$expected" "$benchmark_aodv_manifest"
done
require_selected_source_count "$benchmark_aodv_manifest" \
    'app/tavrn_routed_node/src/routed_benchmark.c' 1
require_selected_source_count "$benchmark_aodv_manifest" \
    'app/tavrn_routed_node/src/routed_benchmark_observer.c' 1
require_selected_source_count "$benchmark_aodv_manifest" \
    'app/tavrn_routed_node/src/routed_benchmark_uart_tx.c' 1
require_selected_source_count "$benchmark_aodv_manifest" \
    'app/tavrn_routed_node/src/routed_benchmark_full.c' 0
if ! grep -Fqx '#define TRON_BUILD_BENCHMARK_MODE 1' "$benchmark_aodv_config" ||
    ! grep -Fqx '#define TRON_BUILD_BENCHMARK_ACCEPTED_FIFO_CAPACITY 1024u' "$benchmark_aodv_config" ||
    ! grep -Fqx '#define TRON_BUILD_BENCHMARK_FINAL_FIFO_CAPACITY 1024u' "$benchmark_aodv_config" ||
    ! grep -Fqx '#define TRON_BUILD_ROUTED_LOGGER_TASK_STACK_BYTES 1840u' "$benchmark_aodv_config" ||
    grep -Fq 'TRON_BUILD_BENCH_WARMUP_MS' "$benchmark_aodv_config"; then
    printf '%s\n' 'AODV benchmark generated config lacks benchmark fields' >&2
    exit 1
fi
if ! grep -Fqx '#define ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY 1024u' \
        "$MICROBIT_ROOT/app/tavrn_routed_node/src/routed_benchmark.h" ||
    ! grep -Fqx '#define ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY 1024u' \
        "$MICROBIT_ROOT/app/tavrn_routed_node/src/routed_benchmark.h" ||
    ! grep -Fq 'ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY == 1024u' \
        "$MICROBIT_ROOT/app/tavrn_routed_node/src/routed_benchmark.h" ||
    ! grep -Fq 'ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY == 1024u' \
        "$MICROBIT_ROOT/app/tavrn_routed_node/src/routed_benchmark.h" ||
    ! grep -Fq 'TRON_BUILD_BENCHMARK_ACCEPTED_FIFO_CAPACITY ==' \
        "$MICROBIT_ROOT/app/tavrn_routed_node/src/main.c" ||
    ! grep -Fq 'TRON_BUILD_BENCHMARK_FINAL_FIFO_CAPACITY ==' \
        "$MICROBIT_ROOT/app/tavrn_routed_node/src/main.c"; then
    printf '%s\n' 'benchmark source/config capacity binding is absent' >&2
    exit 1
fi
build_target routed-benchmark-aodv tavrn_routed_node

benchmark_full_args=("${benchmark_aodv_args[@]}")
benchmark_full_args[2]=-DTAVRN_FEATURE_LEVEL=FULL_TAVRN
benchmark_full_args[6]=-DTRON_BENCH_ROLE_NUMBER=6
configure_ok routed-benchmark-full "${benchmark_full_args[@]}"
benchmark_full_manifest="$(routed_manifest_path routed-benchmark-full)"
require_line 'feature.level.effective=FULL_TAVRN' "$benchmark_full_manifest"
require_line 'bench.mode=ON' "$benchmark_full_manifest"
require_line 'bench.burst_period_ms=450000' "$benchmark_full_manifest"
require_selected_source_count "$benchmark_full_manifest" \
    'app/tavrn_routed_node/src/routed_benchmark.c' 1
require_selected_source_count "$benchmark_full_manifest" \
    'app/tavrn_routed_node/src/routed_benchmark_observer.c' 1
require_selected_source_count "$benchmark_full_manifest" \
    'app/tavrn_routed_node/src/routed_benchmark_uart_tx.c' 1
require_selected_source_count "$benchmark_full_manifest" \
    'app/tavrn_routed_node/src/routed_benchmark_full.c' 1
require_selected_source_count "$benchmark_full_manifest" \
    'app/mind_application/mind_application_wire.c' 0
require_selected_source_count "$benchmark_full_manifest" \
    'app/mind_application/mind_application_ingress.c' 0
require_selected_source_count "$benchmark_full_manifest" \
    'app/mind_application/mind_event_forwarder.c' 0
require_exact_selected_sources "$benchmark_full_manifest" \
    'app/tavrn_routed_node/src/main.c' \
    'generated/tron_build_info.c' \
    'generated/tron_timer_config.c' \
    'app/drivers/ble_radio.c' \
    'app/protocol/ble_mesh_scheduler.c' \
    'app/protocol/ble_mesh_tx_queue.c' \
    'app/tavrn_routed_node/src/routed_cycle.c' \
    'app/protocol/aodv_core.c' \
    'app/protocol/tavrn_link_v2.c' \
    'app/protocol/tavrn_router.c' \
    'app/protocol/tavrn_wire_v2.c' \
    'app/tavrn_routed_node/src/routed_benchmark.c' \
    'app/tavrn_routed_node/src/routed_benchmark_observer.c' \
    'app/tavrn_routed_node/src/routed_benchmark_uart_tx.c' \
    'app/tavrn_routed_node/src/routed_full_telemetry.c' \
    'app/protocol/tavrn_esc.c' \
    'app/protocol/tavrn_full.c' \
    'app/protocol/tavrn_full_maintenance_binding.c' \
    'app/protocol/tavrn_gtt.c' \
    'app/protocol/tavrn_maintenance.c' \
    'app/protocol/tavrn_mentorship.c' \
    'app/protocol/tavrn_smart_ttl.c' \
    'app/tavrn_routed_node/src/routed_benchmark_full.c' \
    'app/drivers/display.c' \
    'libs/mtkernel_3/include/sys/inittask.h'
build_target routed-benchmark-full tavrn_routed_node

configure_fail_with benchmark-invalid-state \
    'TRON_BENCHMARK_MODE must be exactly ON or OFF' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_BENCHMARK_MODE=MAYBE
configure_fail_with benchmark-target \
    'TRON_BENCHMARK_MODE=ON requires ROUTED target' \
    -DTRON_PHASE1_TARGET=LINK -DTRON_BENCHMARK_MODE=ON
configure_fail_with benchmark-hooks \
    'TRON_BENCHMARK_MODE=ON requires TRON_ENABLE_TEST_HOOKS=ON' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_BENCHMARK_MODE=ON \
    -DTRON_BENCH_ROLE_NUMBER=1 -DTRON_LINK_TEST_PEER_ADVA=dc:4b:0a:06:03:f8
configure_fail_with benchmark-identify \
    'TRON_BENCHMARK_MODE=ON requires TRON_BENCH_IDENTIFY_DISPLAY=OFF' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_BENCHMARK_MODE=ON -DTRON_BENCH_IDENTIFY_DISPLAY=ON \
    -DTRON_BENCH_ROLE_NUMBER=1 -DTRON_LINK_TEST_PEER_ADVA=dc:4b:0a:06:03:f8 \
    -DTRON_TEST_RX_BLOCK_ADVA=dc:4b:0a:06:03:f8
configure_fail_with benchmark-fast \
    'TRON_BENCHMARK_MODE=ON requires TRON_TIMER_PROFILE=BALANCED' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_BENCHMARK_MODE=ON -DTRON_TIMER_PROFILE=FAST_TEST \
    -DTRON_BENCH_ROLE_NUMBER=1 -DTRON_LINK_TEST_PEER_ADVA=dc:4b:0a:06:03:f8 \
    -DTRON_TEST_RX_BLOCK_ADVA=dc:4b:0a:06:03:f8
configure_fail_with benchmark-role \
    'TRON_BENCHMARK_MODE=ON requires TRON_BENCH_ROLE_NUMBER in 1..6' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_BENCHMARK_MODE=ON -DTRON_LINK_TEST_PEER_ADVA=dc:4b:0a:06:03:f8
configure_fail_with benchmark-peer \
    'TRON_BENCHMARK_MODE=ON requires TRON_LINK_TEST_PEER_ADVA' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_BENCHMARK_MODE=ON -DTRON_BENCH_ROLE_NUMBER=1
configure_fail_with benchmark-direct-block \
    'TRON_BENCHMARK_MODE roles 1 and 3 require TRON_TEST_RX_BLOCK_ADVA' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_BENCHMARK_MODE=ON -DTRON_BENCH_ROLE_NUMBER=1 \
    -DTRON_LINK_TEST_PEER_ADVA=dc:4b:0a:06:03:f8

# BUILD-P6-01: local repair is an opt-in FULL_TAVRN composition.  Its sources,
# generated macro, manifest state and fixed capacities are absent from repair-off.
configure_ok routed-full-repair -DTRON_PHASE1_TARGET=ROUTED \
    -DTRON_NODE_MODE=TAVRN_ROUTED -DTAVRN_FEATURE_LEVEL=FULL_TAVRN \
    -DTAVRN_ENABLE_LOCAL_REPAIR=ON -DTRON_TIMER_PROFILE=FAST_TEST
routed_full_repair_manifest="$(routed_manifest_path routed-full-repair)"
require_line 'feature.repair.requested=ON' "$routed_full_repair_manifest"
require_line 'feature.repair.effective=ON' "$routed_full_repair_manifest"
require_line 'capacity.repair_contexts.state=IMPLEMENTED' "$routed_full_repair_manifest"
require_line 'capacity.repair_data.state=IMPLEMENTED' "$routed_full_repair_manifest"
require_line 'build.behavior=TAVRN_ROUTED_FULL_TAVRN_ESC_K1_MENTORSHIP_ADAPTIVE_HELLO_LOCAL_EXPIRY_TARGETED_FRESHNESS_RREQ_VERIFICATION_TC_METADATA_LOCAL_REPAIR' "$routed_full_repair_manifest"
for source in tavrn_repair.c tavrn_full_repair_binding.c; do
    if ! grep '^source\.selected\.[0-9].*=' "$routed_full_repair_manifest" | \
        grep -q "app/protocol/${source}"; then
        printf 'repair-on FULL_TAVRN source manifest lacks %s\n' "$source" >&2
        exit 1
    fi
done
routed_full_repair_config="$WORK_DIR/routed-full-repair/app/tavrn_routed_node/generated/tavrn_routed_node/tron_build_config.h"
if ! grep -Fqx '#define TRON_BUILD_LOCAL_REPAIR 1' "$routed_full_repair_config" ||
   ! grep -Fq 'repair=ON' "$routed_full_repair_config"; then
    printf '%s\n' 'repair-on generated config lacks effective repair evidence' >&2
    exit 1
fi
build_target routed-full-repair tavrn_routed_node
configure_fail_with repair-aodv 'TAVRN_ENABLE_LOCAL_REPAIR=ON requires ROUTED FULL_TAVRN' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTAVRN_ENABLE_LOCAL_REPAIR=ON
configure_fail repair-legacy -DTRON_PHASE1_TARGET=LEGACY -DTAVRN_ENABLE_LOCAL_REPAIR=ON

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
# BUILD-P1-05: generic inventory validation always gates full AdvA/SID16;
# routed AODV candidates report SID8 without gating it, while FULL fixed-k=1
# candidates require the complete fleet SID8 namespace to be safe.
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
configure_fail_with routed-candidate-inventory-count \
    'ROUTED candidate requires exactly 6 inventory records' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_HARDWARE_CANDIDATE=ON \
    -DTRON_ADVA_OVERRIDE=18:42:de:52:4a:dd -DTRON_TARGET_PROBE_UID=board-a \
    -DTRON_TARGET_INVENTORY_FILE="$FIXTURES/tavrn_inventory_valid.tsv"
configure_ok routed-candidate-aodv "${six_board_routed_candidate_args[@]}" \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY
routed_candidate_aodv_manifest="$(routed_manifest_path routed-candidate-aodv)"
for expected in \
    'candidate.configured=ON' \
    'candidate.scope=UNHOOKED_ACCEPTANCE' \
    'identity.width=SID16' \
    'identity.inventory.record_count=6' \
    'identity.inventory.full_adva.unique=yes' \
    'identity.inventory.sid16.unique=yes' \
    'identity.inventory.sid16.nonreserved=yes' \
    'identity.inventory.sid8.unique=yes' \
    'identity.inventory.sid8.nonreserved=yes' \
    'identity.inventory.selected_width=SID16' \
    'identity.target_probe_uid=9906360200052820cf57b9f988a30e16000000006e052820' \
    'identity.adva=18:42:de:52:4a:dd'; do
    require_line "$expected" "$routed_candidate_aodv_manifest"
done
configure_ok routed-candidate-full-repair "${six_board_routed_candidate_args[@]}" \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTAVRN_ENABLE_LOCAL_REPAIR=ON
routed_candidate_full_manifest="$(routed_manifest_path routed-candidate-full-repair)"
for expected in \
    'candidate.configured=ON' \
    'candidate.scope=UNHOOKED_ACCEPTANCE' \
    'feature.repair.effective=ON' \
    'identity.width=SID8' \
    'identity.inventory.selected_width=SID8' \
    'identity.inventory.sid8.unique=yes' \
    'identity.inventory.sid8.nonreserved=yes'; do
    require_line "$expected" "$routed_candidate_full_manifest"
done
configure_ok routed-candidate-uid-boolean-token \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=AODV_ONLY -DTRON_HARDWARE_CANDIDATE=ON \
    -DTRON_ADVA_OVERRIDE=be:65:0b:2c:96:d0 \
    -DTRON_TARGET_PROBE_UID=99063602000528200b9c563b9bdebe86000000006e052820 \
    -DTRON_TARGET_INVENTORY_FILE="$SIX_BOARD_INVENTORY"
routed_candidate_uid_boolean_manifest="$(routed_manifest_path routed-candidate-uid-boolean-token)"
require_line 'identity.adva=be:65:0b:2c:96:d0' "$routed_candidate_uid_boolean_manifest"
configure_ok routed-candidate-full-benchmark "${six_board_routed_candidate_args[@]}" \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTRON_TIMER_PROFILE=BALANCED \
    -DTRON_ENABLE_TEST_HOOKS=ON -DTRON_BENCHMARK_MODE=ON \
    -DTRON_BENCH_ROLE_NUMBER=1 -DTRON_LINK_TEST_PEER_ADVA=1e:33:a7:2f:8e:d8 \
    -DTRON_TEST_RX_BLOCK_ADVA=1e:33:a7:2f:8e:d8
routed_candidate_benchmark_manifest="$(routed_manifest_path routed-candidate-full-benchmark)"
require_line 'candidate.scope=BENCH_HOOKED_RESTRICTED' "$routed_candidate_benchmark_manifest"
require_line 'identity.width=SID8' "$routed_candidate_benchmark_manifest"
require_line 'identity.inventory.selected_width=SID8' "$routed_candidate_benchmark_manifest"
configure_ok routed-candidate-aodv-sid8-status -DTRON_PHASE1_TARGET=ROUTED \
    -DTRON_NODE_MODE=TAVRN_ROUTED -DTAVRN_FEATURE_LEVEL=AODV_ONLY \
    -DTRON_HARDWARE_CANDIDATE=ON -DTRON_ADVA_OVERRIDE=00:42:de:52:4a:dd \
    -DTRON_TARGET_PROBE_UID=board-a \
    -DTRON_TARGET_INVENTORY_FILE="$ROUTED_SID8_STATUS_INVENTORY"
routed_candidate_aodv_sid8_manifest="$(routed_manifest_path routed-candidate-aodv-sid8-status)"
require_line 'identity.inventory.record_count=6' "$routed_candidate_aodv_sid8_manifest"
require_line 'identity.inventory.selected_width=SID16' "$routed_candidate_aodv_sid8_manifest"
require_line 'identity.inventory.sid8.unique=no' "$routed_candidate_aodv_sid8_manifest"
require_line 'identity.inventory.sid8.nonreserved=no' "$routed_candidate_aodv_sid8_manifest"
configure_fail_with routed-candidate-full-sid8-status \
    'FULL_TAVRN candidate requires unique nonreserved inventory SID8' \
    -DTRON_PHASE1_TARGET=ROUTED -DTRON_NODE_MODE=TAVRN_ROUTED \
    -DTAVRN_FEATURE_LEVEL=FULL_TAVRN -DTRON_HARDWARE_CANDIDATE=ON \
    -DTRON_ADVA_OVERRIDE=00:42:de:52:4a:dd -DTRON_TARGET_PROBE_UID=board-a \
    -DTRON_TARGET_INVENTORY_FILE="$ROUTED_SID8_STATUS_INVENTORY"
configure_fail_with routed-candidate-role-malformed \
    'TRON_BENCH_ROLE must be a sanitized label' \
    "${six_board_routed_candidate_args[@]}" -DTAVRN_FEATURE_LEVEL=AODV_ONLY \
    -DTRON_BENCH_ROLE='role!'
configure_fail_with routed-candidate-benchmark-topology-mismatch \
    'Benchmark roles 1/3 require RX block equal peer AdvA' \
    "${six_board_routed_candidate_args[@]}" -DTAVRN_FEATURE_LEVEL=FULL_TAVRN \
    -DTRON_TIMER_PROFILE=BALANCED -DTRON_ENABLE_TEST_HOOKS=ON \
    -DTRON_BENCHMARK_MODE=ON -DTRON_BENCH_ROLE_NUMBER=1 \
    -DTRON_LINK_TEST_PEER_ADVA=1e:33:a7:2f:8e:d8 \
    -DTRON_TEST_RX_BLOCK_ADVA=51:56:ae:12:21:ca

# BUILD-P1-06: exact fixed-capacity/timer and generated source surfaces.
if grep '^source\.selected\.[0-9].*=' "$runtime_manifest" | grep -q 'tron_mesh_\|aodv_\|tavrn_full\|tavrn_gtt\|tavrn_repair'; then
    printf '%s\n' 'link source manifest imports legacy or future sources' >&2
    exit 1
fi
if [[ "$(grep -c '^source\.selected\.[0-9].*=libs/mtkernel_3/include/sys/inittask.h$' "$runtime_manifest")" -ne 1 ]]; then
    printf '%s\n' 'link source manifest lacks exactly one initial-task header provenance record' >&2
    exit 1
fi
require_line 'source.selected.7=generated/tron_timer_config.c' "$runtime_manifest"
require_line 'timer.radio_tx_event_bound_ms=8' "$runtime_manifest"
require_line 'timer.radio_tx_repeated_event_bound_ms=14' "$runtime_manifest"
require_line 'timer.radio_tx_fault_cleanup_bound_ms=18' "$runtime_manifest"
require_line 'timer.link_tx_scheduler_attempt_bound_ms=3048' "$runtime_manifest"
require_line 'timer.link_response_window_sum_ms=750' "$runtime_manifest"
require_line 'timer.link_no_response_wall_bound_ms=9894' "$runtime_manifest"
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
if [[ "$(grep -c '^timer\.' "$runtime_manifest")" -ne 75 ]] ||
    [[ "$(grep -c '^capacity\.[^.]*=' "$runtime_manifest")" -ne 50 ]]; then
    printf '%s\n' 'manifest timer/capacity schema width is not exact' >&2
    exit 1
fi
require_unique_keys "$runtime_manifest"

# BUILD-P1-07: the development publisher emits sorted, unique, post-link
# provenance.  Its clean-source field follows the invoking checkout.  When
# that checkout is dirty, --candidate alone must still fail with exact evidence.
publisher_source_dirty=no
if [[ -n "$(git -C "$MICROBIT_ROOT" status --porcelain --untracked-files=all -- .)" ]]; then
    publisher_source_dirty=yes
fi
publisher_clean_source=yes
if [[ "$publisher_source_dirty" == "yes" ]]; then
    publisher_clean_source=no
fi
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
    "candidate.clean_source=${publisher_clean_source}" \
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
if [[ "$publisher_source_dirty" == "yes" ]]; then
    set +e
    bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target ble_link_v2_testbed \
        --candidate --adva 18:42:de:52:4a:dd --probe-uid board-a \
        --inventory "$FIXTURES/tavrn_inventory_valid.tsv" --out "$publisher_out" \
        >"$WORK_DIR/published.log" 2>&1
    candidate_status=$?
    set -e
    if [[ $candidate_status -ne 1 ]]; then
        printf 'dirty candidate publisher exit status=%s, expected 1\n' "$candidate_status" >&2
        exit 1
    fi
    if ! grep -Fq 'Candidate publication refused: source_dirty=yes submodule_dirty=no' \
        "$WORK_DIR/published.log"; then
        printf '%s\n' 'dirty candidate publisher did not report the dirty source state' >&2
        exit 1
    fi
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
for manifest_path in "${routed_published_manifest[@]}"; do
    routed_published_name="$(manifest_value artifact.name "$manifest_path")"
    for expected in \
        'candidate.configured=OFF' \
        'candidate.scope=DEVELOPMENT_ONLY' \
        'candidate.eligible=no' \
        'candidate.hardware_purpose=no' \
        'candidate.hook_bench_eligible=no' \
        'candidate.unhooked_acceptance=no' \
        'identity.adva=RUNTIME_FICR'; do
        require_line "$expected" "$manifest_path"
    done
    if [[ "$routed_published_name" != *candidate0-development* ]]; then
        printf 'generic routed artifact lacks clear ineligible candidate state: %s\n' \
            "$routed_published_name" >&2
        exit 1
    fi
done
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

# BUILD-P6-02: a disposable clean repository with a real clean submodule
# exercises board-bound routed publication.  This must not borrow the dirty
# parent worktree's status and uses the exact six-board inventory.
clean_publisher_root="$(make_clean_publisher_source)"
clean_inventory="$clean_publisher_root/hardware-results/2026-08-11-tavrn-six-board-inventory.tsv"
clean_inventory_hash="$(sha256sum "$clean_inventory" | cut -d' ' -f1)"
clean_uid=9906360200052820cf57b9f988a30e16000000006e052820
clean_adva=18:42:de:52:4a:dd
clean_candidate_args=(
    --target tavrn_routed_node --candidate --adva "$clean_adva" --probe-uid "$clean_uid" \
    --inventory "$clean_inventory"
)
clean_aodv_out="$WORK_DIR/clean-routed-aodv"
bash "$clean_publisher_root/build-tavrn-ble.sh" "${clean_candidate_args[@]}" \
    --feature AODV_ONLY --out "$clean_aodv_out" >"$WORK_DIR/clean-aodv-publish.log"
clean_aodv_manifest="$(expiry_manifest_path "$clean_aodv_out")"
clean_aodv_name="$(manifest_value artifact.name "$clean_aodv_manifest")"
clean_aodv_bundle="$(dirname "$clean_aodv_manifest")"
if [[ "$clean_aodv_bundle" != "$clean_aodv_out/$clean_aodv_name" ]] ||
   [[ "$clean_aodv_manifest" != "$clean_aodv_bundle/${clean_aodv_name}.manifest" ]] ||
   ! grep -Fqx "Published ELF: $clean_aodv_bundle/${clean_aodv_name}.elf" \
       "$WORK_DIR/clean-aodv-publish.log"; then
    printf '%s\n' 'candidate publication did not create or report its immutable bundle path' >&2
    exit 1
fi
for expected in \
    'candidate.configured=ON' \
    'candidate.scope=UNHOOKED_ACCEPTANCE' \
    'candidate.clean_source=yes' \
    'candidate.eligible=yes' \
    'candidate.hardware_purpose=yes' \
    'candidate.hook_bench_eligible=no' \
    'candidate.unhooked_acceptance=yes' \
    'identity.adva=18:42:de:52:4a:dd' \
    "identity.target_probe_uid=${clean_uid}" \
    'identity.width=SID16' \
    'identity.inventory.record_count=6' \
    'identity.inventory.full_adva.unique=yes' \
    'identity.inventory.sid16.unique=yes' \
    'identity.inventory.sid16.nonreserved=yes' \
    'identity.inventory.sid8.unique=yes' \
    'identity.inventory.sid8.nonreserved=yes' \
    'identity.inventory.selected_width=SID16' \
    "identity.inventory.sha256=${clean_inventory_hash}" \
    'source.submodule.count=1' \
    'source.submodule.0.path=libs/mtkernel_3' \
    'source.submodule.0.state=clean' \
    'source.submodule.0.dirty=no' \
    'source.submodule_dirty=no'; do
    require_line "$expected" "$clean_aodv_manifest"
done
if [[ "$clean_aodv_name" != *candidate1-unhooked-acceptance* ]]; then
    printf 'routed AODV candidate artifact lacks unhooked state: %s\n' "$clean_aodv_name" >&2
    exit 1
fi
for evidence_key in \
    evidence.ninja_commands evidence.compile_commands evidence.build_ninja evidence.cmake_cache \
    evidence.target_config_manifest evidence.target_config_header evidence.disassembly \
    evidence.selected_sources; do
    require_evidence_sidecar "$evidence_key" "$clean_aodv_bundle" "$clean_aodv_manifest"
done
if ! grep -Eq '^artifact\.elf\.sha256=[0-9a-f]{64}$' "$clean_aodv_manifest" ||
   ! grep -Eq '^source\.selected\.sha256=[0-9a-f]{64}$' "$clean_aodv_manifest" ||
   ! grep -Eq '^source\.inventory\.sha256=[0-9a-f]{64}$' "$clean_aodv_manifest"; then
    printf '%s\n' 'clean AODV candidate is missing artifact/source hash provenance' >&2
    exit 1
fi
clean_full_out="$WORK_DIR/clean-routed-full-repair"
bash "$clean_publisher_root/build-tavrn-ble.sh" "${clean_candidate_args[@]}" \
    --feature FULL_TAVRN --repair ON --out "$clean_full_out" >/dev/null
clean_full_manifest="$(expiry_manifest_path "$clean_full_out")"
clean_full_name="$(manifest_value artifact.name "$clean_full_manifest")"
clean_full_bundle="$(dirname "$clean_full_manifest")"
if [[ "$clean_full_bundle" != "$clean_full_out/$clean_full_name" ]]; then
    printf '%s\n' 'FULL candidate publication is not contained by its artifact bundle' >&2
    exit 1
fi
for expected in \
    'candidate.scope=UNHOOKED_ACCEPTANCE' \
    'candidate.eligible=yes' \
    'candidate.hook_bench_eligible=no' \
    'candidate.unhooked_acceptance=yes' \
    'feature.repair.effective=ON' \
    'identity.width=SID8' \
    'identity.inventory.selected_width=SID8' \
    'identity.inventory.sid8.unique=yes' \
    'identity.inventory.sid8.nonreserved=yes'; do
    require_line "$expected" "$clean_full_manifest"
done
if [[ "$clean_full_name" != *repair1*candidate1-unhooked-acceptance* ]]; then
    printf 'routed FULL repair candidate lacks clear unhooked state: %s\n' "$clean_full_name" >&2
    exit 1
fi
clean_benchmark_out="$WORK_DIR/clean-routed-full-benchmark"
bash "$clean_publisher_root/build-tavrn-ble.sh" "${clean_candidate_args[@]}" \
    --feature FULL_TAVRN --timer BALANCED --enable-hooks ON --benchmark ON \
    --role A --role-number 1 --peer-adva 1e:33:a7:2f:8e:d8 \
    --rx-block-adva 1e:33:a7:2f:8e:d8 --out "$clean_benchmark_out" >/dev/null
clean_benchmark_manifest="$(expiry_manifest_path "$clean_benchmark_out")"
clean_benchmark_name="$(manifest_value artifact.name "$clean_benchmark_manifest")"
clean_benchmark_bundle="$(dirname "$clean_benchmark_manifest")"
if [[ "$clean_benchmark_bundle" != "$clean_benchmark_out/$clean_benchmark_name" ]]; then
    printf '%s\n' 'benchmark candidate publication is not contained by its artifact bundle' >&2
    exit 1
fi
for expected in \
    'candidate.configured=ON' \
    'candidate.scope=BENCH_HOOKED_RESTRICTED' \
    'candidate.clean_source=yes' \
    'candidate.eligible=no' \
    'candidate.hardware_purpose=yes' \
    'candidate.hook_bench_eligible=yes' \
    'candidate.unhooked_acceptance=no' \
    'hook.enabled=ON' \
    'bench.mode=ON' \
    'identity.width=SID8' \
    'identity.inventory.selected_width=SID8'; do
    require_line "$expected" "$clean_benchmark_manifest"
done
if [[ "$clean_benchmark_name" != *candidate1-bench-hooked-restricted* ]]; then
    printf 'hooked routed benchmark artifact lacks restricted state: %s\n' \
        "$clean_benchmark_name" >&2
    exit 1
fi

# Candidate publication writes its complete bundle only under a hidden staging
# name.  A final-preparation copy failure must leave neither a final bundle nor
# a hidden staging directory in the requested output directory.
final_preparation_bin="$WORK_DIR/final-preparation-bin"
mkdir -p "$final_preparation_bin"
real_install="$(command -v install)"
cat > "$final_preparation_bin/install" <<'EOF'
#!/usr/bin/env bash
for argument in "$@"; do
    if [[ "$argument" == "$TRON_TEST_FINAL_PREPARATION_OUT"/.*.staging.* ]]; then
        exit 97
    fi
done
exec "$TRON_TEST_REAL_INSTALL" "$@"
EOF
chmod +x "$final_preparation_bin/install"
final_preparation_out="$WORK_DIR/final-preparation-publish"
mkdir -p "$final_preparation_out"
touch "$final_preparation_out/unrelated-existing-artifact"
set +e
TRON_TEST_FINAL_PREPARATION_OUT="$final_preparation_out" \
TRON_TEST_REAL_INSTALL="$real_install" PATH="$final_preparation_bin:$PATH" \
    bash "$clean_publisher_root/build-tavrn-ble.sh" "${clean_candidate_args[@]}" \
    --feature AODV_ONLY --out "$final_preparation_out" \
    >"$WORK_DIR/final-preparation.log" 2>&1
final_preparation_status=$?
set -e
if [[ $final_preparation_status -ne 97 ]] ||
   [[ -e "$final_preparation_out/$clean_aodv_name" ]] ||
   [[ -L "$final_preparation_out/$clean_aodv_name" ]]; then
    printf '%s\n' 'candidate final-preparation failure created a final bundle' >&2
    exit 1
fi
shopt -s nullglob dotglob
final_preparation_entries=("$final_preparation_out"/*)
shopt -u nullglob dotglob
if [[ ${#final_preparation_entries[@]} -ne 1 ]] ||
   [[ "${final_preparation_entries[0]}" != "$final_preparation_out/unrelated-existing-artifact" ]]; then
    printf '%s\n' 'candidate final-preparation failure left staged output behind' >&2
    exit 1
fi

# Repeating a candidate invocation must refuse the pre-existing immutable
# bundle and leave the published provenance unchanged.
clean_aodv_manifest_hash="$(sha256sum "$clean_aodv_manifest" | cut -d' ' -f1)"
set +e
bash "$clean_publisher_root/build-tavrn-ble.sh" "${clean_candidate_args[@]}" \
    --feature AODV_ONLY --out "$clean_aodv_out" \
    >"$WORK_DIR/candidate-collision.log" 2>&1
candidate_collision_status=$?
set -e
if [[ $candidate_collision_status -ne 1 ]] ||
   ! grep -Fqx "Candidate publication refused: final bundle already exists: $clean_aodv_bundle" \
       "$WORK_DIR/candidate-collision.log" ||
   [[ "$(sha256sum "$clean_aodv_manifest" | cut -d' ' -f1)" != "$clean_aodv_manifest_hash" ]]; then
    printf '%s\n' 'candidate bundle collision did not preserve the original bundle' >&2
    exit 1
fi
shopt -s nullglob dotglob
candidate_collision_entries=("$clean_aodv_out"/*)
shopt -u nullglob dotglob
if [[ ${#candidate_collision_entries[@]} -ne 1 ]] ||
   [[ "${candidate_collision_entries[0]}" != "$clean_aodv_bundle" ]]; then
    printf '%s\n' 'candidate collision left or replaced output entries' >&2
    exit 1
fi

# Six clean board-bound candidates may share one requested output directory,
# but each role must contribute exactly one sibling immutable bundle.
six_candidate_out="$WORK_DIR/six-candidate-bundles"
six_candidate_roles=(A B C D E F)
six_candidate_count=0
while IFS=$'\t' read -r six_candidate_uid six_candidate_adva; do
    [[ "$six_candidate_uid" == \#* ]] && continue
    bash "$clean_publisher_root/build-tavrn-ble.sh" --target tavrn_routed_node \
        --candidate --feature AODV_ONLY --role "${six_candidate_roles[six_candidate_count]}" \
        --adva "$six_candidate_adva" --probe-uid "$six_candidate_uid" \
        --inventory "$clean_inventory" --out "$six_candidate_out" >/dev/null
    ((six_candidate_count += 1))
done < "$clean_inventory"
if [[ $six_candidate_count -ne 6 ]]; then
    printf 'six-board candidate loop ran %s times, expected 6\n' "$six_candidate_count" >&2
    exit 1
fi
shopt -s nullglob dotglob
six_candidate_bundles=("$six_candidate_out"/*)
shopt -u nullglob dotglob
if [[ ${#six_candidate_bundles[@]} -ne 6 ]]; then
    printf 'six candidate invocations produced %s output entries, expected six bundles\n' \
        "${#six_candidate_bundles[@]}" >&2
    exit 1
fi
for six_candidate_bundle in "${six_candidate_bundles[@]}"; do
    if [[ ! -d "$six_candidate_bundle" || -L "$six_candidate_bundle" ]]; then
        printf 'six-board candidate output is not a real bundle directory: %s\n' \
            "$six_candidate_bundle" >&2
        exit 1
    fi
    six_candidate_manifest="$(expiry_manifest_path "$six_candidate_bundle")"
    six_candidate_name="$(manifest_value artifact.name "$six_candidate_manifest")"
    if [[ "$(basename "$six_candidate_bundle")" != "$six_candidate_name" ]] ||
       [[ "$six_candidate_manifest" != "$six_candidate_bundle/${six_candidate_name}.manifest" ]] ||
       [[ ! -s "$six_candidate_bundle/${six_candidate_name}.elf" ]]; then
        printf 'six-board candidate bundle is incomplete or misnamed: %s\n' \
            "$six_candidate_bundle" >&2
        exit 1
    fi
done

# Publication must remain staged until both the post-build candidate source
# snapshot and evidence generation have passed.  The wrapper dirties the clean
# source only while CMake is building, after the publisher's first snapshot.
post_build_dirty_bin="$WORK_DIR/post-build-dirty-bin"
mkdir -p "$post_build_dirty_bin"
post_build_dirty_source="$clean_publisher_root/.candidate-post-build-dirty"
real_cmake="$(command -v cmake)"
cat > "$post_build_dirty_bin/cmake" <<'EOF'
#!/usr/bin/env bash
if [[ "${1:-}" == "--build" ]]; then
    touch "$TRON_TEST_POST_BUILD_DIRTY_SOURCE"
fi
exec "$TRON_TEST_REAL_CMAKE" "$@"
EOF
chmod +x "$post_build_dirty_bin/cmake"
post_build_dirty_out="$WORK_DIR/post-build-dirty-publish"
mkdir -p "$post_build_dirty_out"
touch "$post_build_dirty_out/unrelated-existing-artifact"
set +e
TRON_TEST_POST_BUILD_DIRTY_SOURCE="$post_build_dirty_source" \
TRON_TEST_REAL_CMAKE="$real_cmake" PATH="$post_build_dirty_bin:$PATH" \
    bash "$clean_publisher_root/build-tavrn-ble.sh" "${clean_candidate_args[@]}" \
    --feature AODV_ONLY --out "$post_build_dirty_out" \
    >"$WORK_DIR/post-build-dirty.log" 2>&1
post_build_dirty_status=$?
set -e
rm -f "$post_build_dirty_source"
if [[ $post_build_dirty_status -ne 1 ]] ||
   ! grep -Fq 'Candidate publication refused: source state changed during build/evidence' \
       "$WORK_DIR/post-build-dirty.log"; then
    printf '%s\n' 'candidate publisher did not reject post-build source dirtiness' >&2
    exit 1
fi
shopt -s nullglob dotglob
post_build_dirty_entries=("$post_build_dirty_out"/*)
shopt -u nullglob dotglob
if [[ ${#post_build_dirty_entries[@]} -ne 1 ]] ||
   [[ "${post_build_dirty_entries[0]}" != "$post_build_dirty_out/unrelated-existing-artifact" ]]; then
    printf '%s\n' 'post-build candidate failure published a new artifact' >&2
    exit 1
fi

# Evidence and resource gates run against the staging directory.  Each forced
# late failure must leave an existing output directory untouched.
evidence_failure_bin="$WORK_DIR/evidence-failure-bin"
mkdir -p "$evidence_failure_bin"
cat > "$evidence_failure_bin/arm-none-eabi-objdump" <<'EOF'
#!/usr/bin/env bash
exit 97
EOF
chmod +x "$evidence_failure_bin/arm-none-eabi-objdump"
evidence_failure_out="$WORK_DIR/evidence-failure-publish"
mkdir -p "$evidence_failure_out"
touch "$evidence_failure_out/unrelated-existing-artifact"
set +e
PATH="$evidence_failure_bin:$PATH" bash "$MICROBIT_ROOT/build-tavrn-ble.sh" \
    --target ble_link_v2_testbed --out "$evidence_failure_out" \
    >"$WORK_DIR/evidence-failure.log" 2>&1
evidence_failure_status=$?
set -e
if [[ $evidence_failure_status -eq 0 ]]; then
    printf '%s\n' 'forced evidence failure unexpectedly published successfully' >&2
    exit 1
fi
shopt -s nullglob dotglob
evidence_failure_entries=("$evidence_failure_out"/*)
shopt -u nullglob dotglob
if [[ ${#evidence_failure_entries[@]} -ne 1 ]] ||
   [[ "${evidence_failure_entries[0]}" != "$evidence_failure_out/unrelated-existing-artifact" ]]; then
    printf '%s\n' 'evidence failure published a new artifact' >&2
    exit 1
fi

resource_failure_baseline="$WORK_DIR/resource-failure-baseline"
mkdir -p "$resource_failure_baseline"
touch "$resource_failure_baseline/fast.before.map"
printf '{}\n' > "$resource_failure_baseline/fast.baseline.manifest"
printf 'invalid baseline hash\n' > "$resource_failure_baseline/fast.baseline.sha256"
resource_failure_bin="$WORK_DIR/resource-failure-bin"
mkdir -p "$resource_failure_bin"
cat > "$resource_failure_bin/python3" <<'EOF'
#!/usr/bin/env bash
if [[ "${1:-}" == "$TRON_TEST_RESOURCE_CHECKER" && " $* " == *' --resource-manifest '* ]]; then
    exit 97
fi
exec "$TRON_TEST_REAL_PYTHON3" "$@"
EOF
chmod +x "$resource_failure_bin/python3"
resource_failure_out="$WORK_DIR/resource-failure-publish"
mkdir -p "$resource_failure_out"
touch "$resource_failure_out/unrelated-existing-artifact"
real_python3="$(command -v python3)"
set +e
TRON_TEST_RESOURCE_CHECKER="$MICROBIT_ROOT/scripts/check_tavrn_expiry_resources.py" \
TRON_TEST_REAL_PYTHON3="$real_python3" PATH="$resource_failure_bin:$PATH" \
    bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target tavrn_routed_node \
    --feature FULL_TAVRN --timer FAST_TEST --stack-usage \
    --resource-baseline "$resource_failure_baseline/fast.baseline.manifest" \
    --out "$resource_failure_out" >"$WORK_DIR/resource-failure.log" 2>&1
resource_failure_status=$?
set -e
if [[ $resource_failure_status -ne 97 ]] ||
   ! grep -Fq 'Resource gate failed while staging artifacts' "$WORK_DIR/resource-failure.log"; then
    printf '%s\n' 'forced resource gate failure did not reach the staged gate' >&2
    exit 1
fi
shopt -s nullglob dotglob
resource_failure_entries=("$resource_failure_out"/*)
shopt -u nullglob dotglob
if [[ ${#resource_failure_entries[@]} -ne 1 ]] ||
   [[ "${resource_failure_entries[0]}" != "$resource_failure_out/unrelated-existing-artifact" ]]; then
    printf '%s\n' 'resource gate failure published a new artifact' >&2
    exit 1
fi

touch "$clean_publisher_root/.candidate-source-dirty"
set +e
bash "$clean_publisher_root/build-tavrn-ble.sh" "${clean_candidate_args[@]}" \
    --feature AODV_ONLY --out "$WORK_DIR/clean-dirty-source" \
    >"$WORK_DIR/clean-dirty-source.log" 2>&1
clean_dirty_source_status=$?
set -e
rm -f "$clean_publisher_root/.candidate-source-dirty"
if [[ $clean_dirty_source_status -ne 1 ]] ||
   ! grep -Fq 'Candidate publication refused: source_dirty=yes submodule_dirty=no' \
       "$WORK_DIR/clean-dirty-source.log"; then
    printf '%s\n' 'routed candidate did not reject a dirty source tree' >&2
    exit 1
fi
touch "$clean_publisher_root/libs/mtkernel_3/.candidate-submodule-dirty"
set +e
bash "$clean_publisher_root/build-tavrn-ble.sh" "${clean_candidate_args[@]}" \
    --feature AODV_ONLY --out "$WORK_DIR/clean-dirty-submodule" \
    >"$WORK_DIR/clean-dirty-submodule.log" 2>&1
clean_dirty_submodule_status=$?
set -e
if [[ $clean_dirty_submodule_status -ne 1 ]] ||
   ! grep -Fq 'submodule_dirty=yes' "$WORK_DIR/clean-dirty-submodule.log"; then
    printf '%s\n' 'routed candidate did not reject a dirty submodule' >&2
    exit 1
fi

publisher_fail_with routed-candidate-missing-inputs \
    '--candidate requires --adva, --probe-uid, and --inventory' \
    --target tavrn_routed_node --candidate
publisher_fail_with legacy-candidate-rejected \
    'Candidate publication is available only for ble_link_v2_testbed or tavrn_routed_node' \
    --target ble_mesh_node --candidate --adva "$clean_adva" --probe-uid "$clean_uid" \
    --inventory "$SIX_BOARD_INVENTORY"
publisher_configure_fail_with routed-candidate-unknown-uid \
    'Candidate target UID has no inventory record' \
    --target tavrn_routed_node --feature AODV_ONLY --candidate --adva "$clean_adva" \
    --probe-uid not-in-inventory --inventory "$SIX_BOARD_INVENTORY"
publisher_configure_fail_with routed-candidate-mismatch \
    'Candidate override must byte-equal the selected inventory AdvA' \
    --target tavrn_routed_node --feature AODV_ONLY --candidate --adva dc:4b:0a:06:03:f8 \
    --probe-uid "$clean_uid" --inventory "$SIX_BOARD_INVENTORY"
publisher_configure_fail_with routed-full-candidate-sid8 \
    'FULL_TAVRN candidate requires unique nonreserved inventory SID8' \
    --target tavrn_routed_node --feature FULL_TAVRN --candidate --adva 00:42:de:52:4a:dd \
    --probe-uid board-a --inventory "$ROUTED_SID8_STATUS_INVENTORY"
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

# BENCH-IDENT-02: the publisher accepts and records the explicit display mode,
# while rejecting every invalid argument combination before a build starts.
identify_publisher_out="$WORK_DIR/identify-published"
bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target tavrn_routed_node \
    --feature AODV_ONLY --enable-hooks ON --role-number 2 --identify-display ON \
    --out "$identify_publisher_out" >/dev/null
identify_published_manifest=()
for manifest_path in "$identify_publisher_out"/*.manifest; do
    [[ "$manifest_path" == *.build-config.manifest ]] && continue
    identify_published_manifest+=("$manifest_path")
done
if [[ ${#identify_published_manifest[@]} -ne 1 ]]; then
    printf '%s\n' 'identification publisher did not create exactly one artifact manifest' >&2
    exit 1
fi
require_line 'bench.identify_display=ON' "${identify_published_manifest[0]}"
require_line 'bench.role_number=2' "${identify_published_manifest[0]}"
require_line 'hook.enabled=ON' "${identify_published_manifest[0]}"
publisher_fail_with identify-wrapper-invalid-state \
    '--identify-display must be ON or OFF' \
    --target tavrn_routed_node --identify-display MAYBE
publisher_fail_with identify-wrapper-role-malformed \
    '--role-number must be a decimal integer in 0..6' \
    --target tavrn_routed_node --role-number three
publisher_fail_with identify-wrapper-role-out-of-range \
    '--role-number must be a decimal integer in 0..6' \
    --target tavrn_routed_node --role-number 7
publisher_fail_with identify-wrapper-legacy \
    '--identify-display ON requires --target tavrn_routed_node' \
    --target ble_mesh_node --enable-hooks ON --role-number 1 --identify-display ON
publisher_fail_with identify-wrapper-link \
    '--identify-display ON requires --target tavrn_routed_node' \
    --target ble_link_v2_testbed --enable-hooks ON --role-number 1 --identify-display ON
publisher_fail_with identify-wrapper-hooks-off \
    '--identify-display ON requires --enable-hooks ON' \
    --target tavrn_routed_node --feature AODV_ONLY --role-number 1 --identify-display ON
publisher_fail_with identify-wrapper-role-zero \
    '--identify-display ON requires --role-number 1..6' \
    --target tavrn_routed_node --feature AODV_ONLY --enable-hooks ON \
    --identify-display ON

# BENCH-OBS-02: wrapper validation is explicit before a build; its accepted
# artifact exposes the same benchmark contract as direct CMake configuration.
benchmark_publisher_out="$WORK_DIR/benchmark-published"
bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target tavrn_routed_node \
    --feature AODV_ONLY --timer BALANCED --enable-hooks ON --benchmark ON \
    --role-number 1 --peer-adva dc:4b:0a:06:03:f8 \
    --rx-block-adva dc:4b:0a:06:03:f8 \
    --out "$benchmark_publisher_out" >/dev/null
benchmark_published_manifest=()
for manifest_path in "$benchmark_publisher_out"/*.manifest; do
    [[ "$manifest_path" == *.build-config.manifest ]] && continue
    benchmark_published_manifest+=("$manifest_path")
done
if [[ ${#benchmark_published_manifest[@]} -ne 1 ]]; then
    printf '%s\n' 'benchmark publisher did not create exactly one artifact manifest' >&2
    exit 1
fi
for expected in 'bench.mode=ON' 'bench.control_observability=COMPILE_TIME_OPTIONAL' \
                'bench.heartbeat_interval_ms=1000' 'bench.burst_start_ms=60000' \
                'bench.burst_period_ms=450000' 'bench.role_number=1' \
                'bench.identify_display=OFF'; do
    require_line "$expected" "${benchmark_published_manifest[0]}"
done
require_selected_source_count "${benchmark_published_manifest[0]}" \
    'app/mind_application/mind_application_wire.c' 0
require_selected_source_count "${benchmark_published_manifest[0]}" \
    'app/mind_application/mind_application_ingress.c' 0
require_selected_source_count "${benchmark_published_manifest[0]}" \
    'app/mind_application/mind_event_forwarder.c' 0
require_selected_source_count "${benchmark_published_manifest[0]}" \
    'app/tavrn_routed_node/src/routed_benchmark_uart_tx.c' 1
benchmark_resource_publisher_out="$WORK_DIR/benchmark-resource-published"
bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target tavrn_routed_node \
    --feature FULL_TAVRN --timer BALANCED --enable-hooks ON --benchmark ON \
    --role-number 6 --peer-adva 1e:33:a7:2f:8e:d8 --stack-usage \
    --out "$benchmark_resource_publisher_out" >/dev/null
benchmark_resource_published_manifest="$(expiry_manifest_path "$benchmark_resource_publisher_out")"
benchmark_resource_name="$(manifest_value artifact.name "$benchmark_resource_published_manifest")"
benchmark_resource_document="$(expiry_manifest_field_path "$benchmark_resource_publisher_out" \
    "$benchmark_resource_published_manifest" resource.manifest.name)"
benchmark_resource_config="$(expiry_manifest_field_path "$benchmark_resource_publisher_out" \
    "$benchmark_resource_published_manifest" evidence.target_config_header.name)"
python3 - "$MICROBIT_ROOT/app/tavrn_routed_node/src/routed_benchmark.h" \
    "$benchmark_resource_config" "$benchmark_resource_published_manifest" \
    "$benchmark_resource_document" "$benchmark_resource_name" <<'PY'
import json
import pathlib
import sys

source, config, manifest_path, resource_path, artifact_name = map(pathlib.Path, sys.argv[1:])
source_text = source.read_text(encoding="utf-8")
config_text = config.read_text(encoding="utf-8")
for macro in ("ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY",
              "ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY"):
    if f"#define {macro} 1024u\n" not in source_text:
        raise SystemExit("benchmark source FIFO capacity is not 1024")
for macro in ("TRON_BUILD_BENCHMARK_ACCEPTED_FIFO_CAPACITY",
              "TRON_BUILD_BENCHMARK_FINAL_FIFO_CAPACITY"):
    if f"#define {macro} 1024u\n" not in config_text:
        raise SystemExit("generated benchmark FIFO capacity is not 1024")
manifest = dict(line.split("=", 1) for line in
                manifest_path.read_text(encoding="utf-8").splitlines() if line)
if manifest.get("artifact.name") != artifact_name.name or \
        any(manifest.get(f"capacity.benchmark_{fifo}_fifo") != "1024" or
            manifest.get(f"capacity.benchmark_{fifo}_fifo.policy") !=
            "RETAIN_OLDEST_DROP_NEWEST" or
            manifest.get(f"capacity.benchmark_{fifo}_fifo.dropped_telemetry") !=
            "SATURATING_COUNTER" or
            manifest.get(f"capacity.benchmark_{fifo}_fifo.state") != "IMPLEMENTED"
            for fifo in ("accepted", "final")) or \
        manifest.get("resource.fixed_state.declared_delta_bytes") != "60372":
    raise SystemExit("benchmark FIFO/fixed-state manifest binding differs")
resource = json.loads(resource_path.read_text(encoding="utf-8"))
if resource["fixed_state"] != {
        "after_bytes": 87564,
        "before_bytes": 87564,
        "declared_delta_bytes": 60372,
        "unexplained_delta_bytes": 0,
}:
    raise SystemExit("benchmark resource fixed-state binding differs")
PY
publisher_fail_with benchmark-wrapper-invalid-state \
    '--benchmark must be ON or OFF' --target tavrn_routed_node --benchmark MAYBE
publisher_fail_with benchmark-wrapper-warmup-obsolete \
    'Unknown argument: --warmup-ms' \
    --target tavrn_routed_node --warmup-ms not-a-number
publisher_fail_with benchmark-wrapper-target \
    '--benchmark ON requires --target tavrn_routed_node' \
    --target ble_mesh_node --benchmark ON
publisher_fail_with benchmark-wrapper-hooks \
    '--benchmark ON requires --enable-hooks ON' \
    --target tavrn_routed_node --benchmark ON --role-number 1 \
    --peer-adva dc:4b:0a:06:03:f8 --rx-block-adva dc:4b:0a:06:03:f8
publisher_fail_with benchmark-wrapper-identify \
    '--benchmark ON requires --identify-display OFF' \
    --target tavrn_routed_node --enable-hooks ON --benchmark ON --identify-display ON \
    --role-number 1 --peer-adva dc:4b:0a:06:03:f8 --rx-block-adva dc:4b:0a:06:03:f8
publisher_fail_with benchmark-wrapper-fast \
    '--benchmark ON requires --timer BALANCED' \
    --target tavrn_routed_node --timer FAST_TEST --enable-hooks ON --benchmark ON \
    --role-number 1 --peer-adva dc:4b:0a:06:03:f8 --rx-block-adva dc:4b:0a:06:03:f8
publisher_fail_with benchmark-wrapper-role \
    '--benchmark ON requires --role-number 1..6' \
    --target tavrn_routed_node --enable-hooks ON --benchmark ON \
    --peer-adva dc:4b:0a:06:03:f8
publisher_fail_with benchmark-wrapper-peer \
    '--benchmark ON requires --peer-adva' \
    --target tavrn_routed_node --enable-hooks ON --benchmark ON --role-number 1
publisher_fail_with benchmark-wrapper-direct-block \
    '--benchmark ON roles 1 and 3 require --rx-block-adva' \
    --target tavrn_routed_node --timer BALANCED --enable-hooks ON --benchmark ON \
    --role-number 1 --peer-adva dc:4b:0a:06:03:f8
publisher_fail_with benchmark-wrapper-wearable-ingress \
    '--benchmark ON is incompatible with --wearable-ingress ON' \
    --target tavrn_routed_node --feature FULL_TAVRN --timer BALANCED \
    --enable-hooks ON --benchmark ON --role-number 2 \
    --peer-adva dc:4b:0a:06:03:f8 --wearable-ingress ON

# BEARER-CAP-02: the publisher forwards the routed-only queue variant through
# the generated build manifest and rejects invalid or lower-target requests.
routed_queue_publisher_out="$WORK_DIR/routed-queue-published"
bash "$MICROBIT_ROOT/build-tavrn-ble.sh" --target tavrn_routed_node \
    --feature AODV_ONLY --routed-tx-queue-capacity 16 \
    --out "$routed_queue_publisher_out" >/dev/null
routed_queue_published_manifest=()
for manifest_path in "$routed_queue_publisher_out"/*.manifest; do
    [[ "$manifest_path" == *.build-config.manifest ]] && continue
    routed_queue_published_manifest+=("$manifest_path")
done
if [[ ${#routed_queue_published_manifest[@]} -ne 1 ]]; then
    printf '%s\n' 'routed queue publisher did not create exactly one artifact manifest' >&2
    exit 1
fi
require_line 'capacity.scheduler_tx_queue=16' "${routed_queue_published_manifest[0]}"
publisher_fail_with routed-queue-wrapper-invalid \
    '--routed-tx-queue-capacity must be 4, 8, 16, or 40' \
    --target tavrn_routed_node --routed-tx-queue-capacity 5
publisher_fail_with routed-queue-wrapper-link \
    '--routed-tx-queue-capacity is valid only with --target tavrn_routed_node' \
    --target ble_link_v2_testbed --routed-tx-queue-capacity 8

printf '%s\n' 'tron BLE build/profile tests passed'
