#!/usr/bin/env bash
#
# Noninteractive Phase 1 artifact publisher.  It intentionally leaves the
# historical build-ble-node.sh CLI untouched while providing one reproducible
# publication path for the legacy node and the dedicated link-v2 harness.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")" && pwd)"
source "${repo_root}/scripts/common.sh"

target="ble_mesh_node"
out_dir="${repo_root}/artifacts"
timer_profile="BALANCED"
role="generic"
node_id="0"
network_id="1"
adva_override=""
probe_uid=""
inventory_file=""
candidate="OFF"
hooks="OFF"
rx_block_adva=""
hack_drop_adva=""
hack_drop_count="0"
busy_admission_count="0"
peer_adva=""
initiator="OFF"
tx_interval_ms="1000"
transaction_target="0"

usage() {
    cat <<'EOF'
Usage: ./build-tavrn-ble.sh [options]

Targets:
  --target ble_mesh_node|ble_link_v2_testbed

Common options:
  --out DIR                 Published artifact directory
  --timer FAST_TEST|BALANCED|SOAK
  --role LABEL              Label only; does not alter link behavior
  --network-id VALUE        Wire-v2 network ID (link target)
  --candidate               Require a clean, inventory-bound link candidate
  --adva xx:xx:xx:xx:xx:xx  Canonical configured AdvA (link target)
  --probe-uid UID           Candidate probe UID
  --inventory FILE          Strict UID<TAB>AdvA inventory

Legacy options:
  --node-id VALUE           Legacy label; zero derives its existing FICR label

Link-harness options:
  --peer-adva ADDR          Direct diagnostic peer AdvA
  --initiator ON|OFF        Emit bounded diagnostic DATA directly to peer
  --tx-interval-ms MS       Diagnostic DATA interval
  --transaction-target N    Number of diagnostic transactions
  --enable-hooks ON|OFF
  --rx-block-adva ADDR
  --hack-drop-adva ADDR
  --hack-drop-count N
  --busy-admission-count N
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target) target="${2:?Missing value for --target}"; shift 2 ;;
        --out) out_dir="${2:?Missing value for --out}"; shift 2 ;;
        --timer) timer_profile="${2:?Missing value for --timer}"; shift 2 ;;
        --role) role="${2:?Missing value for --role}"; shift 2 ;;
        --node-id) node_id="${2:?Missing value for --node-id}"; shift 2 ;;
        --network-id) network_id="${2:?Missing value for --network-id}"; shift 2 ;;
        --adva) adva_override="${2:?Missing value for --adva}"; shift 2 ;;
        --probe-uid) probe_uid="${2:?Missing value for --probe-uid}"; shift 2 ;;
        --inventory) inventory_file="${2:?Missing value for --inventory}"; shift 2 ;;
        --candidate) candidate="ON"; shift ;;
        --enable-hooks) hooks="${2:?Missing value for --enable-hooks}"; shift 2 ;;
        --rx-block-adva) rx_block_adva="${2:?Missing value for --rx-block-adva}"; shift 2 ;;
        --hack-drop-adva) hack_drop_adva="${2:?Missing value for --hack-drop-adva}"; shift 2 ;;
        --hack-drop-count) hack_drop_count="${2:?Missing value for --hack-drop-count}"; shift 2 ;;
        --busy-admission-count) busy_admission_count="${2:?Missing value for --busy-admission-count}"; shift 2 ;;
        --peer-adva) peer_adva="${2:?Missing value for --peer-adva}"; shift 2 ;;
        --initiator) initiator="${2:?Missing value for --initiator}"; shift 2 ;;
        --tx-interval-ms) tx_interval_ms="${2:?Missing value for --tx-interval-ms}"; shift 2 ;;
        --transaction-target) transaction_target="${2:?Missing value for --transaction-target}"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ "$target" != "ble_mesh_node" && "$target" != "ble_link_v2_testbed" ]]; then
    printf '%s\n' '--target must be ble_mesh_node or ble_link_v2_testbed' >&2
    exit 2
fi
if [[ "$candidate" == "ON" && "$target" != "ble_link_v2_testbed" ]]; then
    printf '%s\n' 'Phase 1 candidate publication is available only for ble_link_v2_testbed' >&2
    exit 2
fi
if [[ "$target" == "ble_mesh_node" &&
      ( -n "$adva_override" || -n "$probe_uid" || -n "$inventory_file" ||
        -n "$peer_adva" || "$initiator" != "OFF" || "$hooks" != "OFF" ||
        -n "$rx_block_adva" || -n "$hack_drop_adva" || "$hack_drop_count" != "0" ||
        "$busy_admission_count" != "0" || "$transaction_target" != "0" ) ]]; then
    printf '%s\n' 'Legacy publication rejects routed identity, harness, and routed-hook inputs' >&2
    exit 2
fi

case "$timer_profile" in
    FAST_TEST) timer_tag="fast" ;;
    BALANCED) timer_tag="balanced" ;;
    SOAK) timer_tag="soak" ;;
    *) printf '%s\n' '--timer must be FAST_TEST, BALANCED, or SOAK' >&2; exit 2 ;;
esac

build_dir="$(mktemp -d "${TMPDIR:-/tmp}/tron-ble-publish.XXXXXX")"
manifest_work="${build_dir}/manifest.unsorted"
trap 'rm -rf "$build_dir"' EXIT
mkdir -p "$out_dir"

if [[ "$target" == "ble_mesh_node" ]]; then
    phase1_target="LEGACY"
    node_mode="LEGACY_FLOOD"
else
    phase1_target="LINK"
    node_mode="NOT_APPLICABLE"
fi

cmake_args=(
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}"
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    -DTRON_PHASE1_TARGET="$phase1_target"
    -DTRON_NODE_MODE="$node_mode"
    -DTRON_TIMER_PROFILE="$timer_profile"
    -DTRON_BENCH_ROLE="$role"
    -DTRON_NODE_ID="$node_id"
    -DTRON_NETWORK_ID="$network_id"
    -DTRON_ADVA_OVERRIDE="$adva_override"
    -DTRON_TARGET_PROBE_UID="$probe_uid"
    -DTRON_TARGET_INVENTORY_FILE="$inventory_file"
    -DTRON_HARDWARE_CANDIDATE="$candidate"
    -DTRON_ENABLE_TEST_HOOKS="$hooks"
    -DTRON_TEST_RX_BLOCK_ADVA="$rx_block_adva"
    -DTRON_TEST_HACK_DROP_PEER_ADVA="$hack_drop_adva"
    -DTRON_TEST_HACK_DROP_COUNT="$hack_drop_count"
    -DTRON_TEST_BUSY_ADMISSION_COUNT="$busy_admission_count"
    -DTRON_LINK_TEST_PEER_ADVA="$peer_adva"
    -DTRON_LINK_TEST_INITIATOR="$initiator"
    -DTRON_LINK_TEST_TX_INTERVAL_MS="$tx_interval_ms"
    -DTRON_LINK_TEST_TRANSACTION_TARGET="$transaction_target"
)
cmake -S "$repo_root" -B "$build_dir" -G Ninja "${cmake_args[@]}"

source_dirty=no
if [[ -n "$(git -C "$repo_root" status --porcelain)" ]]; then
    source_dirty=yes
fi
submodule_status="$(git -C "$repo_root" submodule status --recursive)"
submodule_dirty=no
if [[ "$submodule_status" == *$'\n+'* || "$submodule_status" == +* ||
      "$submodule_status" == *$'\n-'* || "$submodule_status" == -* ||
       "$submodule_status" == *$'\nU'* || "$submodule_status" == U* ]]; then
    submodule_dirty=yes
fi
submodule_count=0
submodule_manifest_lines=""
while IFS= read -r submodule_line; do
    [[ -n "$submodule_line" ]] || continue
    submodule_prefix="${submodule_line:0:1}"
    submodule_record="${submodule_line:1}"
    submodule_record="${submodule_record# }"
    submodule_commit="${submodule_record%% *}"
    submodule_path="${submodule_record#"$submodule_commit"}"
    submodule_path="${submodule_path# }"
    submodule_path="${submodule_path%% *}"
    case "$submodule_prefix" in
        ' ') submodule_state=clean ;;
        -) submodule_state=uninitialized; submodule_dirty=yes ;;
        +) submodule_state=commit_mismatch; submodule_dirty=yes ;;
        U) submodule_state=conflict; submodule_dirty=yes ;;
        *) submodule_state=unknown; submodule_dirty=yes ;;
    esac
    submodule_worktree_dirty=no
    if [[ -d "$repo_root/$submodule_path" ]] &&
       [[ -n "$(git -C "$repo_root/$submodule_path" status --porcelain 2>/dev/null || true)" ]]; then
        submodule_worktree_dirty=yes
        submodule_dirty=yes
    fi
    submodule_manifest_lines+="source.submodule.${submodule_count}.commit=${submodule_commit}"$'\n'
    submodule_manifest_lines+="source.submodule.${submodule_count}.dirty=${submodule_worktree_dirty}"$'\n'
    submodule_manifest_lines+="source.submodule.${submodule_count}.path=${submodule_path}"$'\n'
    submodule_manifest_lines+="source.submodule.${submodule_count}.state=${submodule_state}"$'\n'
    ((submodule_count += 1))
done <<< "$submodule_status"
if [[ "$candidate" == "ON" && ( "$source_dirty" != no || "$submodule_dirty" != no ) ]]; then
    printf 'Candidate publication refused: source_dirty=%s submodule_dirty=%s\n' \
        "$source_dirty" "$submodule_dirty" >&2
    exit 1
fi

cmake --build "$build_dir" --target "$target" --parallel
source_dir="$(firmware_output_dir "$build_dir" "$target")"
source_elf="$(firmware_elf "$build_dir" "$target")"
source_hex="${source_dir}/${target}.hex"
source_map="${source_dir}/${target}.map"
config_manifest="${source_dir}/tron-build-config.manifest"
generated_config_header="${build_dir}/app/${target}/generated/${target}/tron_build_config.h"
test -s "$source_elf"
test -s "$source_hex"
test -s "$source_map"
test -s "$config_manifest"
test -s "$generated_config_header"
test -s "$build_dir/compile_commands.json"
test -s "$build_dir/build.ninja"
test -s "$build_dir/CMakeCache.txt"

commit="$(git -C "$repo_root" rev-parse HEAD)"
tree="$(git -C "$repo_root" rev-parse HEAD^{tree})"
commit12="${commit:0:12}"
if [[ "$target" == "ble_mesh_node" ]]; then
    if [[ "$node_id" == "0" || "$node_id" == "0x0" || "$node_id" == "0X0" ]]; then
        identity_tag="idficr"
    else
        printf -v identity_tag 'id%04x' "$((node_id))"
    fi
    artifact_base="tron-ble-legacy-na-${timer_tag}-candidate0-repair0-patient0-${role,,}-${identity_tag}-${commit12}"
else
    if [[ -z "$adva_override" ]]; then
        identity_tag="advaruntime-ficr"
    else
        identity_tag="adva${adva_override//:/}"
        identity_tag="${identity_tag,,}"
    fi
    candidate_bit=0
    [[ "$candidate" == "ON" ]] && candidate_bit=1
    artifact_base="tron-ble-linkv2-harness-${timer_tag}-candidate${candidate_bit}-${role,,}-${identity_tag}-${commit12}"
fi

install -m 0644 "$source_elf" "$out_dir/${artifact_base}.elf"
install -m 0644 "$source_hex" "$out_dir/${artifact_base}.hex"
install -m 0644 "$source_map" "$out_dir/${artifact_base}.map"

# Ninja's commands tool emits the exact commands needed to rebuild the chosen
# target, including its dependency closure.  Persist this and the generator
# inputs before the temporary build tree is removed.
commands_evidence_name="${artifact_base}.ninja-commands.txt"
compile_commands_evidence_name="${artifact_base}.compile_commands.json"
ninja_evidence_name="${artifact_base}.build.ninja"
cmake_cache_evidence_name="${artifact_base}.cmake-cache.txt"
config_manifest_evidence_name="${artifact_base}.build-config.manifest"
config_header_evidence_name="${artifact_base}.build-config.h"
ninja -C "$build_dir" -t commands "$target" > "$out_dir/${commands_evidence_name}"
install -m 0644 "$build_dir/compile_commands.json" \
    "$out_dir/${compile_commands_evidence_name}"
install -m 0644 "$build_dir/build.ninja" "$out_dir/${ninja_evidence_name}"
install -m 0644 "$build_dir/CMakeCache.txt" "$out_dir/${cmake_cache_evidence_name}"
install -m 0644 "$config_manifest" "$out_dir/${config_manifest_evidence_name}"
install -m 0644 "$generated_config_header" "$out_dir/${config_header_evidence_name}"
test -s "$out_dir/${commands_evidence_name}"
test -s "$out_dir/${compile_commands_evidence_name}"
test -s "$out_dir/${ninja_evidence_name}"
test -s "$out_dir/${cmake_cache_evidence_name}"
test -s "$out_dir/${config_manifest_evidence_name}"
test -s "$out_dir/${config_header_evidence_name}"

: > "$manifest_work"
while IFS= read -r manifest_line || [[ -n "$manifest_line" ]]; do
    [[ -n "$manifest_line" ]] || continue
    printf '%s\n' "$manifest_line" >> "$manifest_work"
done < "$config_manifest"
compile_commands_hash="$(sha256sum "$out_dir/${compile_commands_evidence_name}" | cut -d' ' -f1)"
if [[ "$candidate" == "ON" ]]; then
    candidate_hardware_purpose=yes
else
    candidate_hardware_purpose=no
fi
candidate_clean_source=no
if [[ "$source_dirty" == no && "$submodule_dirty" == no ]]; then
    candidate_clean_source=yes
fi
candidate_unhooked_acceptance=no
candidate_hook_bench_eligible=no
if [[ "$candidate" == "ON" && "$candidate_clean_source" == yes && "$hooks" == "OFF" ]]; then
    candidate_unhooked_acceptance=yes
fi
if [[ "$candidate" == "ON" && "$candidate_clean_source" == yes && "$hooks" == "ON" ]]; then
    candidate_hook_bench_eligible=yes
fi
candidate_eligible=no
if [[ "$candidate_unhooked_acceptance" == yes ]]; then
    candidate_eligible=yes
fi
{
    printf 'artifact.elf.name=%s.elf\n' "$artifact_base"
    printf 'artifact.elf.sha256=%s\n' "$(sha256sum "$out_dir/${artifact_base}.elf" | cut -d' ' -f1)"
    printf 'artifact.elf.size=%s\n' "$(wc -c < "$out_dir/${artifact_base}.elf")"
    printf 'artifact.hex.name=%s.hex\n' "$artifact_base"
    printf 'artifact.hex.sha256=%s\n' "$(sha256sum "$out_dir/${artifact_base}.hex" | cut -d' ' -f1)"
    printf 'artifact.hex.size=%s\n' "$(wc -c < "$out_dir/${artifact_base}.hex")"
    printf 'artifact.map.name=%s.map\n' "$artifact_base"
    printf 'artifact.map.sha256=%s\n' "$(sha256sum "$out_dir/${artifact_base}.map" | cut -d' ' -f1)"
    printf 'artifact.map.size=%s\n' "$(wc -c < "$out_dir/${artifact_base}.map")"
    printf 'artifact.name=%s\n' "$artifact_base"
    printf 'artifact.sha256=%s\n' "$(sha256sum "$out_dir/${artifact_base}.elf" | cut -d' ' -f1)"
    printf 'artifact.size=%s\n' "$(wc -c < "$out_dir/${artifact_base}.elf")"
    printf 'build.asm_flags.evidence=%s\n' "$commands_evidence_name"
    printf 'build.c_flags.evidence=%s\n' "$commands_evidence_name"
    printf 'build.cmake.path=%s\n' "$(command -v cmake)"
    printf 'build.cmake.sha256=%s\n' "$(sha256sum "$(command -v cmake)" | cut -d' ' -f1)"
    printf 'build.cmake.version=%s\n' "$(cmake --version | tr '\n' ' ' | cut -d' ' -f3)"
    printf 'build.compile_commands.sha256=%s\n' "$compile_commands_hash"
    printf 'build.compile_commands.path=%s\n' "$compile_commands_evidence_name"
    printf 'build.link_command.evidence=%s\n' "$commands_evidence_name"
    printf 'build.link_flags.evidence=%s\n' "$commands_evidence_name"
    printf 'build.ninja_file.path=%s\n' "$ninja_evidence_name"
    printf 'build.ninja_file.sha256=%s\n' "$(sha256sum "$out_dir/${ninja_evidence_name}" | cut -d' ' -f1)"
    printf 'build.ninja.path=%s\n' "$(command -v ninja)"
    printf 'build.ninja.sha256=%s\n' "$(sha256sum "$(command -v ninja)" | cut -d' ' -f1)"
    printf 'build.script.path=build-tavrn-ble.sh\n'
    printf 'build.script.sha256=%s\n' "$(sha256sum "$0" | cut -d' ' -f1)"
    printf 'build.toolchain.path=cmake/arm-none-eabi-gcc.cmake\n'
    printf 'build.toolchain.sha256=%s\n' "$(sha256sum "${TOOLCHAIN_FILE}" | cut -d' ' -f1)"
    printf 'build.utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'build.compiler.path=%s\n' "$(command -v arm-none-eabi-gcc)"
    printf 'build.compiler.sha256=%s\n' "$(sha256sum "$(command -v arm-none-eabi-gcc)" | cut -d' ' -f1)"
    compiler_version="$(arm-none-eabi-gcc --version)"
    compiler_version="${compiler_version%%$'\n'*}"
    printf 'build.compiler.version=%s\n' "$compiler_version"
    printf 'build.ninja.version=%s\n' "$(ninja --version)"
    printf 'build.target_commands.path=%s\n' "$commands_evidence_name"
    printf 'build.target_commands.target=%s\n' "$target"
    printf 'build.target_config_header.path=%s\n' "$config_header_evidence_name"
    printf 'build.target_config_manifest.path=%s\n' "$config_manifest_evidence_name"
    printf 'candidate.clean_source=%s\n' "$candidate_clean_source"
    printf 'candidate.eligible=%s\n' "$candidate_eligible"
    printf 'candidate.hardware_purpose=%s\n' "$candidate_hardware_purpose"
    printf 'candidate.hook_bench_eligible=%s\n' "$candidate_hook_bench_eligible"
    printf 'candidate.unhooked_acceptance=%s\n' "$candidate_unhooked_acceptance"
    printf 'source.commit=%s\n' "$commit"
    printf 'source.dirty=%s\n' "$source_dirty"
    printf 'source.submodule.count=%s\n' "$submodule_count"
    printf '%s' "$submodule_manifest_lines"
    printf 'source.submodule.sha256=%s\n' "$(printf '%s' "$submodule_status" | sha256sum | cut -d' ' -f1)"
    printf 'source.submodule_dirty=%s\n' "$submodule_dirty"
    printf 'source.tree=%s\n' "$tree"
    printf 'evidence.build_ninja.name=%s\n' "$ninja_evidence_name"
    printf 'evidence.build_ninja.sha256=%s\n' "$(sha256sum "$out_dir/${ninja_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.build_ninja.size=%s\n' "$(wc -c < "$out_dir/${ninja_evidence_name}")"
    printf 'evidence.cmake_cache.name=%s\n' "$cmake_cache_evidence_name"
    printf 'evidence.cmake_cache.sha256=%s\n' "$(sha256sum "$out_dir/${cmake_cache_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.cmake_cache.size=%s\n' "$(wc -c < "$out_dir/${cmake_cache_evidence_name}")"
    printf 'evidence.compile_commands.name=%s\n' "$compile_commands_evidence_name"
    printf 'evidence.compile_commands.sha256=%s\n' "$compile_commands_hash"
    printf 'evidence.compile_commands.size=%s\n' "$(wc -c < "$out_dir/${compile_commands_evidence_name}")"
    printf 'evidence.ninja_commands.name=%s\n' "$commands_evidence_name"
    printf 'evidence.ninja_commands.sha256=%s\n' "$(sha256sum "$out_dir/${commands_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.ninja_commands.size=%s\n' "$(wc -c < "$out_dir/${commands_evidence_name}")"
    printf 'evidence.target_config_header.name=%s\n' "$config_header_evidence_name"
    printf 'evidence.target_config_header.sha256=%s\n' "$(sha256sum "$out_dir/${config_header_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.target_config_header.size=%s\n' "$(wc -c < "$out_dir/${config_header_evidence_name}")"
    printf 'evidence.target_config_manifest.name=%s\n' "$config_manifest_evidence_name"
    printf 'evidence.target_config_manifest.sha256=%s\n' "$(sha256sum "$out_dir/${config_manifest_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.target_config_manifest.size=%s\n' "$(wc -c < "$out_dir/${config_manifest_evidence_name}")"
} >> "$manifest_work"

duplicate_keys="$(cut -d= -f1 "$manifest_work" | LC_ALL=C sort | uniq -d)"
if [[ -n "$duplicate_keys" ]]; then
    printf 'Manifest generation produced duplicate keys:\n%s\n' "$duplicate_keys" >&2
    exit 1
fi
LC_ALL=C sort "$manifest_work" > "$out_dir/${artifact_base}.manifest"

printf 'Published ELF: %s\n' "$out_dir/${artifact_base}.elf"
printf 'Published HEX: %s\n' "$out_dir/${artifact_base}.hex"
printf 'Published MAP: %s\n' "$out_dir/${artifact_base}.map"
printf 'Commands: %s\n' "$out_dir/${commands_evidence_name}"
printf 'Compile commands: %s\n' "$out_dir/${compile_commands_evidence_name}"
printf 'Build Ninja: %s\n' "$out_dir/${ninja_evidence_name}"
printf 'CMake cache: %s\n' "$out_dir/${cmake_cache_evidence_name}"
printf 'Target config: %s\n' "$out_dir/${config_manifest_evidence_name}"
printf 'Target config header: %s\n' "$out_dir/${config_header_evidence_name}"
printf 'Manifest: %s\n' "$out_dir/${artifact_base}.manifest"
