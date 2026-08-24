#!/usr/bin/env bash
#
# Noninteractive Phase 1 artifact publisher.  It intentionally leaves the
# historical build-ble-node.sh CLI untouched while providing one reproducible
# publication path for the legacy node, dedicated link-v2 harness, and routed
# AODV_ONLY proving ground.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")" && pwd)"
source "${repo_root}/scripts/common.sh"

target="ble_mesh_node"
out_dir="${repo_root}/artifacts"
timer_profile="BALANCED"
role="generic"
role_number="0"
identify_display="OFF"
benchmark="OFF"
node_id="0"
network_id="1"
adva_override=""
probe_uid=""
inventory_file=""
candidate="OFF"
hooks="OFF"
expiry_full_table="OFF"
rx_block_adva=""
hack_drop_adva=""
hack_drop_count="0"
busy_admission_count="0"
collision_peer_adva=""
peer_adva=""
initiator="OFF"
tx_interval_ms="1000"
transaction_target="0"
feature="AODV_ONLY"
feature_requested=no
stack_usage="OFF"
resource_baseline=""
application_resource_baseline=""
application_resource_baseline_seal=""
d2_current_resource_gate="OFF"
resource_checker="${repo_root}/scripts/check_tavrn_expiry_resources.py"
resource_gate="NOT_REQUESTED"
resource_mesh_stack_total_bytes="NOT_APPLICABLE"
resource_mesh_stack_headroom_bytes="NOT_APPLICABLE"
resource_logger_stack_total_bytes="NOT_APPLICABLE"
resource_logger_stack_headroom_bytes="NOT_APPLICABLE"
resource_application_logger_stack_total_bytes="NOT_APPLICABLE"
resource_application_logger_stack_headroom_bytes="NOT_APPLICABLE"
resource_application_uart_stack_total_bytes="NOT_APPLICABLE"
resource_application_uart_stack_headroom_bytes="NOT_APPLICABLE"
resource_application_ui_stack_total_bytes="NOT_APPLICABLE"
resource_application_ui_stack_headroom_bytes="NOT_APPLICABLE"
resource_application_display_stack_total_bytes="NOT_APPLICABLE"
resource_application_display_stack_headroom_bytes="NOT_APPLICABLE"
repair="OFF"
wearable_ingress="OFF"
app_node_number="1"
routed_tx_queue_capacity=""

usage() {
    cat <<'EOF'
Usage: ./build-tavrn-ble.sh [options]

Targets:
  --target ble_mesh_node|ble_link_v2_testbed|tavrn_routed_node

Common options:
  --out DIR                 Published artifact directory
  --timer FAST_TEST|BALANCED|SOAK
  --role LABEL              Label only; does not alter link behavior
  --role-number N           Routed identification role (0..6)
  --identify-display ON|OFF Routed hooks-only 5x5 identification display
   --benchmark ON|OFF       Continuous routed control-plane observability
   --network-id VALUE        Wire-v2 network ID (routed targets)
   --routed-tx-queue-capacity N
                            Routed TX queue capacity: 4, 8, 16, or 40
   --candidate               Require a clean, inventory-bound link or routed candidate
  --adva xx:xx:xx:xx:xx:xx  Canonical configured AdvA (routed targets)
  --probe-uid UID           Candidate probe UID
  --inventory FILE          Strict UID<TAB>AdvA inventory

Legacy options:
  --node-id VALUE           Legacy label; zero derives its existing FICR label

Routed PoC options:
   --feature AODV_ONLY|FULL_TAVRN
 --repair ON|OFF           Enable local repair (FULL_TAVRN only; default OFF)
   --wearable-ingress ON|OFF Enable direct wearable ingress (FULL_TAVRN only)
   --app-node-number N      Application node number (1..6)
  --peer-adva ADDR          Direct diagnostic peer AdvA
  --initiator ON|OFF        Emit bounded diagnostic DATA directly to peer
  --tx-interval-ms MS       Diagnostic DATA interval
  --transaction-target N    Number of diagnostic transactions
  --stack-usage              Publish GCC .su stack evidence (FULL_TAVRN only)
   --resource-baseline FILE   Validate FULL_TAVRN resources against frozen baseline
    --application-resource-baseline DIR
                               Validate wearable state against frozen FULL+repair
                               ingress-off evidence in DIR
    --application-resource-baseline-seal FILE
                                External SHA-256 seal for that artifact manifest
    --d2-current-resource-gate
                                Run the explicit non-historical D2 current resource gate
   --enable-hooks ON|OFF
   --expiry-full-table ON|OFF  FULL FAST_TEST hooks-only expiry table bench hook
  --rx-block-adva ADDR
  --hack-drop-adva ADDR
  --hack-drop-count N
  --busy-admission-count N
  --collision-peer-adva ADDR  FULL-only synthetic duplicate-SID8 bench hook
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target) target="${2:?Missing value for --target}"; shift 2 ;;
        --out) out_dir="${2:?Missing value for --out}"; shift 2 ;;
        --timer) timer_profile="${2:?Missing value for --timer}"; shift 2 ;;
        --role) role="${2:?Missing value for --role}"; shift 2 ;;
        --role-number) role_number="${2:?Missing value for --role-number}"; shift 2 ;;
        --identify-display) identify_display="${2:?Missing value for --identify-display}"; shift 2 ;;
        --benchmark) benchmark="${2:?Missing value for --benchmark}"; shift 2 ;;
        --node-id) node_id="${2:?Missing value for --node-id}"; shift 2 ;;
        --network-id) network_id="${2:?Missing value for --network-id}"; shift 2 ;;
        --adva) adva_override="${2:?Missing value for --adva}"; shift 2 ;;
        --probe-uid) probe_uid="${2:?Missing value for --probe-uid}"; shift 2 ;;
        --inventory) inventory_file="${2:?Missing value for --inventory}"; shift 2 ;;
        --candidate) candidate="ON"; shift ;;
        --enable-hooks) hooks="${2:?Missing value for --enable-hooks}"; shift 2 ;;
        --expiry-full-table) expiry_full_table="${2:?Missing value for --expiry-full-table}"; shift 2 ;;
        --rx-block-adva) rx_block_adva="${2:?Missing value for --rx-block-adva}"; shift 2 ;;
        --hack-drop-adva) hack_drop_adva="${2:?Missing value for --hack-drop-adva}"; shift 2 ;;
        --hack-drop-count) hack_drop_count="${2:?Missing value for --hack-drop-count}"; shift 2 ;;
        --busy-admission-count) busy_admission_count="${2:?Missing value for --busy-admission-count}"; shift 2 ;;
        --collision-peer-adva) collision_peer_adva="${2:?Missing value for --collision-peer-adva}"; shift 2 ;;
        --peer-adva) peer_adva="${2:?Missing value for --peer-adva}"; shift 2 ;;
        --initiator) initiator="${2:?Missing value for --initiator}"; shift 2 ;;
        --tx-interval-ms) tx_interval_ms="${2:?Missing value for --tx-interval-ms}"; shift 2 ;;
        --transaction-target) transaction_target="${2:?Missing value for --transaction-target}"; shift 2 ;;
        --feature) feature="${2:?Missing value for --feature}"; feature_requested=yes; shift 2 ;;
        --repair) repair="${2:?Missing value for --repair}"; shift 2 ;;
        --wearable-ingress) wearable_ingress="${2:?Missing value for --wearable-ingress}"; shift 2 ;;
        --app-node-number) app_node_number="${2:?Missing value for --app-node-number}"; shift 2 ;;
        --routed-tx-queue-capacity) routed_tx_queue_capacity="${2:?Missing value for --routed-tx-queue-capacity}"; shift 2 ;;
        --stack-usage) stack_usage="ON"; shift ;;
        --resource-baseline) resource_baseline="${2:?Missing value for --resource-baseline}"; shift 2 ;;
        --application-resource-baseline) application_resource_baseline="${2:?Missing value for --application-resource-baseline}"; shift 2 ;;
        --application-resource-baseline-seal) application_resource_baseline_seal="${2:?Missing value for --application-resource-baseline-seal}"; shift 2 ;;
        --d2-current-resource-gate) d2_current_resource_gate="ON"; shift ;;
        --help|-h) usage; exit 0 ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ "$target" != "ble_mesh_node" && "$target" != "ble_link_v2_testbed" &&
      "$target" != "tavrn_routed_node" ]]; then
    printf '%s\n' '--target must be ble_mesh_node, ble_link_v2_testbed, or tavrn_routed_node' >&2
    exit 2
fi
if [[ "$target" != "tavrn_routed_node" && "$feature_requested" == yes ]]; then
    printf '%s\n' '--feature is valid only with --target tavrn_routed_node' >&2
    exit 2
fi
if [[ -n "$routed_tx_queue_capacity" && "$target" != "tavrn_routed_node" ]]; then
    printf '%s\n' '--routed-tx-queue-capacity is valid only with --target tavrn_routed_node' >&2
    exit 2
fi
if [[ -n "$routed_tx_queue_capacity" && "$routed_tx_queue_capacity" != "4" &&
      "$routed_tx_queue_capacity" != "8" && "$routed_tx_queue_capacity" != "16" &&
      "$routed_tx_queue_capacity" != "40" ]]; then
    printf '%s\n' '--routed-tx-queue-capacity must be 4, 8, 16, or 40' >&2
    exit 2
fi
if [[ "$target" == "tavrn_routed_node" && "$feature" != "AODV_ONLY" &&
      "$feature" != "FULL_TAVRN" ]]; then
    printf '%s\n' '--feature must be AODV_ONLY or FULL_TAVRN for tavrn_routed_node' >&2
    exit 2
fi
if [[ "$identify_display" != "ON" && "$identify_display" != "OFF" ]]; then
    printf '%s\n' '--identify-display must be ON or OFF' >&2
    exit 2
fi
if [[ "$benchmark" != "ON" && "$benchmark" != "OFF" ]]; then
    printf '%s\n' '--benchmark must be ON or OFF' >&2
    exit 2
fi
if [[ ! "$role_number" =~ ^(0|[1-9][0-9]*)$ ]] ||
   [[ ${#role_number} -gt 1 || "$role_number" > "6" ]]; then
    printf '%s\n' '--role-number must be a decimal integer in 0..6' >&2
    exit 2
fi
if [[ "$identify_display" == "ON" && "$target" != "tavrn_routed_node" ]]; then
    printf '%s\n' '--identify-display ON requires --target tavrn_routed_node' >&2
    exit 2
fi
if [[ "$target" != "tavrn_routed_node" && "$role_number" != "0" ]]; then
    printf '%s\n' '--role-number is valid only with --target tavrn_routed_node' >&2
    exit 2
fi
if [[ "$identify_display" == "ON" && "$hooks" != "ON" ]]; then
    printf '%s\n' '--identify-display ON requires --enable-hooks ON' >&2
    exit 2
fi
if [[ "$identify_display" == "ON" && "$role_number" == "0" ]]; then
    printf '%s\n' '--identify-display ON requires --role-number 1..6' >&2
    exit 2
fi
if [[ "$benchmark" == "ON" ]]; then
    if [[ "$target" != "tavrn_routed_node" ]]; then
        printf '%s\n' '--benchmark ON requires --target tavrn_routed_node' >&2
        exit 2
    fi
    if [[ "$timer_profile" != "BALANCED" ]]; then
        printf '%s\n' '--benchmark ON requires --timer BALANCED' >&2
        exit 2
    fi
    if [[ "$hooks" != "ON" ]]; then
        printf '%s\n' '--benchmark ON requires --enable-hooks ON' >&2
        exit 2
    fi
    if [[ "$identify_display" != "OFF" ]]; then
        printf '%s\n' '--benchmark ON requires --identify-display OFF' >&2
        exit 2
    fi
    if [[ "$role_number" == "0" ]]; then
        printf '%s\n' '--benchmark ON requires --role-number 1..6' >&2
        exit 2
    fi
    if [[ -z "$peer_adva" ]]; then
        printf '%s\n' '--benchmark ON requires --peer-adva' >&2
        exit 2
    fi
    if [[ "$role_number" == "1" || "$role_number" == "3" ]] &&
       [[ -z "$rx_block_adva" ]]; then
        printf '%s\n' '--benchmark ON roles 1 and 3 require --rx-block-adva' >&2
        exit 2
    fi
fi
if [[ "$repair" != "ON" && "$repair" != "OFF" ]]; then
    printf '%s\n' '--repair must be ON or OFF' >&2
    exit 2
fi
if [[ "$repair" == "ON" &&
      ( "$target" != "tavrn_routed_node" || "$feature" != "FULL_TAVRN" ) ]]; then
    printf '%s\n' '--repair ON requires --target tavrn_routed_node --feature FULL_TAVRN' >&2
    exit 2
fi
if [[ "$wearable_ingress" != "ON" && "$wearable_ingress" != "OFF" ]]; then
    printf '%s\n' '--wearable-ingress must be ON or OFF' >&2
    exit 2
fi
if [[ "$wearable_ingress" == "ON" &&
      ( "$target" != "tavrn_routed_node" || "$feature" != "FULL_TAVRN" ) ]]; then
    printf '%s\n' '--wearable-ingress ON requires --target tavrn_routed_node --feature FULL_TAVRN' >&2
    exit 2
fi
if [[ "$benchmark" == "ON" && "$wearable_ingress" == "ON" ]]; then
    printf '%s\n' '--benchmark ON is incompatible with --wearable-ingress ON' >&2
    exit 2
fi
if [[ ! "$app_node_number" =~ ^[1-6]$ ]]; then
    printf '%s\n' '--app-node-number must be a decimal integer in 1..6' >&2
    exit 2
fi
if [[ "$expiry_full_table" != "ON" && "$expiry_full_table" != "OFF" ]]; then
    printf '%s\n' '--expiry-full-table must be ON or OFF' >&2
    exit 2
fi
if [[ "$expiry_full_table" == "ON" ]]; then
    if [[ "$target" != "tavrn_routed_node" || "$feature" != "FULL_TAVRN" ]]; then
        printf '%s\n' '--expiry-full-table requires --target tavrn_routed_node --feature FULL_TAVRN' >&2
        exit 2
    fi
    if [[ "$timer_profile" != "FAST_TEST" ]]; then
        printf '%s\n' '--expiry-full-table requires --timer FAST_TEST' >&2
        exit 2
    fi
    if [[ "$hooks" != "ON" ]]; then
        printf '%s\n' '--expiry-full-table requires --enable-hooks ON' >&2
        exit 2
    fi
fi
if [[ -n "$resource_baseline" && "$stack_usage" != "ON" ]]; then
    printf '%s\n' '--resource-baseline requires --stack-usage' >&2
    exit 2
fi
if [[ "$d2_current_resource_gate" == "ON" && "$stack_usage" != "ON" ]]; then
    printf '%s\n' '--d2-current-resource-gate requires --stack-usage' >&2
    exit 2
fi
if [[ "$d2_current_resource_gate" == "ON" && -n "$resource_baseline" ]]; then
    printf '%s\n' '--d2-current-resource-gate is distinct from --resource-baseline' >&2
    exit 2
fi
if [[ -n "$application_resource_baseline" && "$stack_usage" != "ON" ]]; then
    printf '%s\n' '--application-resource-baseline requires --stack-usage' >&2
    exit 2
fi
if [[ -n "$application_resource_baseline" && -z "$application_resource_baseline_seal" ]]; then
    printf '%s\n' '--application-resource-baseline requires --application-resource-baseline-seal' >&2
    exit 2
fi
if [[ -n "$application_resource_baseline_seal" && -z "$application_resource_baseline" ]]; then
    printf '%s\n' '--application-resource-baseline-seal requires --application-resource-baseline' >&2
    exit 2
fi
if [[ -n "$application_resource_baseline" && "$wearable_ingress" != "ON" ]]; then
    printf '%s\n' '--application-resource-baseline requires --wearable-ingress ON' >&2
    exit 2
fi
if [[ -n "$application_resource_baseline" && -z "$resource_baseline" &&
      "$d2_current_resource_gate" != "ON" ]]; then
    printf '%s\n' '--application-resource-baseline requires --resource-baseline' >&2
    exit 2
fi
if [[ "$d2_current_resource_gate" == "ON" ]]; then
    if [[ "$target" != "tavrn_routed_node" || "$feature" != "FULL_TAVRN" ||
          "$repair" != "ON" || "$timer_profile" != "BALANCED" ||
          "$app_node_number" != "6" || "$hooks" != "OFF" ||
          "$benchmark" != "OFF" ]]; then
        printf '%s\n' '--d2-current-resource-gate requires FULL+repair BALANCED node 6 with hooks/benchmark OFF' >&2
        exit 2
    fi
    if [[ "$wearable_ingress" == "ON" && -z "$application_resource_baseline" ]]; then
        printf '%s\n' 'D2 ingress ON requires --application-resource-baseline and its external seal' >&2
        exit 2
    fi
    if [[ "$wearable_ingress" == "OFF" && -n "$application_resource_baseline" ]]; then
        printf '%s\n' 'D2 ingress OFF must not consume an application baseline' >&2
        exit 2
    fi
fi
if [[ "$wearable_ingress" == "ON" && -n "$resource_baseline" &&
      -z "$application_resource_baseline" ]]; then
    printf '%s\n' '--resource-baseline with --wearable-ingress ON requires --application-resource-baseline' >&2
    exit 2
fi
if [[ "$stack_usage" == "ON" &&
      ( "$target" != "tavrn_routed_node" || "$feature" != "FULL_TAVRN" ) ]]; then
    printf '%s\n' '--stack-usage is supported only for --target tavrn_routed_node --feature FULL_TAVRN' >&2
    exit 2
fi
if [[ -n "$resource_baseline" && ! -f "$resource_baseline" ]]; then
    printf 'Resource baseline does not exist: %s\n' "$resource_baseline" >&2
    exit 2
fi
if [[ -n "$application_resource_baseline" && ! -d "$application_resource_baseline" ]]; then
    printf 'Application resource baseline directory does not exist: %s\n' \
        "$application_resource_baseline" >&2
    exit 2
fi
if [[ -n "$application_resource_baseline_seal" && ! -f "$application_resource_baseline_seal" ]]; then
    printf 'Application resource baseline seal does not exist: %s\n' \
        "$application_resource_baseline_seal" >&2
    exit 2
fi
if [[ "$candidate" == "ON" && "$target" != "ble_link_v2_testbed" &&
      "$target" != "tavrn_routed_node" ]]; then
    printf '%s\n' 'Candidate publication is available only for ble_link_v2_testbed or tavrn_routed_node' >&2
    exit 2
fi
if [[ "$candidate" == "ON" &&
      ( -z "$adva_override" || -z "$probe_uid" || -z "$inventory_file" ) ]]; then
    printf '%s\n' '--candidate requires --adva, --probe-uid, and --inventory' >&2
    exit 2
fi
if [[ "$target" == "ble_mesh_node" &&
      ( "$candidate" == "ON" || -n "$adva_override" || -n "$probe_uid" || -n "$inventory_file" ||
        -n "$peer_adva" || "$initiator" != "OFF" || "$hooks" != "OFF" ||
         -n "$rx_block_adva" || -n "$hack_drop_adva" || "$hack_drop_count" != "0" ||
         "$busy_admission_count" != "0" || -n "$collision_peer_adva" ||
         "$transaction_target" != "0" ) ]]; then
    printf '%s\n' 'Legacy publication rejects routed identity, harness, and routed-hook inputs' >&2
    exit 2
fi
if [[ "$target" == "tavrn_routed_node" &&
      ( -n "$hack_drop_adva" || "$hack_drop_count" != "0" ||
        "$busy_admission_count" != "0" ) ]]; then
    printf '%s\n' 'Routed feature levels reject link-harness-only inputs' >&2
    exit 2
fi

case "$timer_profile" in
    FAST_TEST) timer_tag="fast" ;;
    BALANCED) timer_tag="balanced" ;;
    SOAK) timer_tag="soak" ;;
    *) printf '%s\n' '--timer must be FAST_TEST, BALANCED, or SOAK' >&2; exit 2 ;;
esac

if [[ "$out_dir" != /* ]]; then
    out_dir="$(pwd -P)/${out_dir}"
fi

build_dir="$(mktemp -d "${TMPDIR:-/tmp}/tron-ble-publish.XXXXXX")"
publication_dir=""
candidate_staging_dir=""
manifest_work="${build_dir}/manifest.unsorted"
cleanup_publication() {
    rm -rf "$build_dir"
    if [[ -n "$candidate_staging_dir" ]]; then
        rm -rf "$candidate_staging_dir"
    fi
}
trap cleanup_publication EXIT
trap 'exit 128' HUP INT QUIT TERM

if [[ "$target" == "ble_mesh_node" ]]; then
    phase1_target="LEGACY"
    node_mode="LEGACY_FLOOD"
    feature_level=""
elif [[ "$target" == "ble_link_v2_testbed" ]]; then
    phase1_target="LINK"
    node_mode="NOT_APPLICABLE"
    feature_level=""
else
    phase1_target="ROUTED"
    node_mode="TAVRN_ROUTED"
    feature_level="$feature"
fi

cmake_args=(
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}"
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    -DTRON_PHASE1_TARGET="$phase1_target"
    -DTRON_NODE_MODE="$node_mode"
    -DTAVRN_FEATURE_LEVEL="$feature_level"
    -DTAVRN_ENABLE_LOCAL_REPAIR="$repair"
    -DTRON_ENABLE_WEARABLE_INGRESS="$wearable_ingress"
    -DTRON_APP_NODE_NUMBER="$app_node_number"
    -DTRON_STACK_USAGE="$stack_usage"
    -DTRON_TIMER_PROFILE="$timer_profile"
    -DTRON_BENCH_ROLE="$role"
    -DTRON_BENCH_ROLE_NUMBER="$role_number"
    -DTRON_BENCH_IDENTIFY_DISPLAY="$identify_display"
    -DTRON_BENCHMARK_MODE="$benchmark"
    -DTRON_NODE_ID="$node_id"
    -DTRON_NETWORK_ID="$network_id"
    -DTRON_ADVA_OVERRIDE="$adva_override"
    -DTRON_TARGET_PROBE_UID="$probe_uid"
    -DTRON_TARGET_INVENTORY_FILE="$inventory_file"
    -DTRON_HARDWARE_CANDIDATE="$candidate"
    -DTRON_ENABLE_TEST_HOOKS="$hooks"
    -DTRON_TEST_EXPIRY_FULL_TABLE="$expiry_full_table"
    -DTRON_TEST_RX_BLOCK_ADVA="$rx_block_adva"
    -DTRON_TEST_HACK_DROP_PEER_ADVA="$hack_drop_adva"
    -DTRON_TEST_HACK_DROP_COUNT="$hack_drop_count"
    -DTRON_TEST_BUSY_ADMISSION_COUNT="$busy_admission_count"
    -DTRON_TEST_COLLISION_PEER_ADVA="$collision_peer_adva"
    -DTRON_LINK_TEST_PEER_ADVA="$peer_adva"
    -DTRON_LINK_TEST_INITIATOR="$initiator"
    -DTRON_LINK_TEST_TX_INTERVAL_MS="$tx_interval_ms"
    -DTRON_LINK_TEST_TRANSACTION_TARGET="$transaction_target"
)
if [[ -n "$routed_tx_queue_capacity" ]]; then
    cmake_args+=(
        -DTRON_ROUTED_TX_QUEUE_CAPACITY="$routed_tx_queue_capacity"
    )
fi
cmake -S "$repo_root" -B "$build_dir" -G Ninja "${cmake_args[@]}"

observe_source_state() {
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
    commit="$(git -C "$repo_root" rev-parse HEAD)"
    tree="$(git -C "$repo_root" rev-parse HEAD^{tree})"
    worktree_content_sha256="$(python3 "$resource_checker" --print-worktree-content-digest)"
}

observe_source_state
pre_source_dirty="$source_dirty"
pre_submodule_dirty="$submodule_dirty"
pre_commit="$commit"
pre_tree="$tree"
pre_worktree_content_sha256="$worktree_content_sha256"
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

resource_inherited_fixed_state_delta=0
resource_application_fixed_state_delta=0
resource_declared_fixed_state_delta=0
if [[ "$stack_usage" == "ON" ]]; then
    build_behavior=""
    while IFS='=' read -r manifest_key manifest_value; do
        if [[ "$manifest_key" == "build.behavior" ]]; then
            build_behavior="$manifest_value"
            break
        fi
    done < "$config_manifest"
    if [[ "$build_behavior" == \
        "TAVRN_ROUTED_FULL_TAVRN_ESC_K1_MENTORSHIP_ADAPTIVE_HELLO_LOCAL_EXPIRY_TARGETED_FRESHNESS_RREQ_VERIFICATION_TC_METADATA" ||
          "$build_behavior" == \
        "TAVRN_ROUTED_FULL_TAVRN_ESC_K1_MENTORSHIP_ADAPTIVE_HELLO_LOCAL_EXPIRY_TARGETED_FRESHNESS_RREQ_VERIFICATION_TC_METADATA_LOCAL_REPAIR" ]]; then
        # Stage 0 accounts for 1044 bytes and the retained RREQ-verification
        # context/accounting adds 948 bytes. TC/metadata adds 2592 bytes for
        # exact 16-entry origin/relay retention, four shared metadata slots,
        # and the owner-held atomic GTT rollback image. The copied FULL-only
        # TC/metadata logger snapshot adds 32 bytes. One exact retry-exhausted
        # LEAVE overflow obligation adds 8 bytes. The routed initial-task growth
        # is dynamically allocated and is accounted by the separate runtime RAM
        # reserve, not by the fixed-state allowance. Future .data or .bss growth
        # remains subject to the checker's unexplained limit.
        resource_inherited_fixed_state_delta=4624
        if [[ "$repair" == "ON" ]]; then
            # Local repair adds one bounded 436-byte repair context, one
            # 56-byte production binding, and one 188-byte copied tick result.
            resource_inherited_fixed_state_delta=5304
        fi
        if [[ "$benchmark" == "ON" ]]; then
            # Observer-v3 replaces the old copied-attempt queue with independent
            # accepted and final FIFOs. Their independent metadata adds 12 bytes
            # and exact workload-split counters add 32 bytes: the reviewed
            # 44-byte fixed-state delta changes 71088 to 71132. Benchmark-only
            # UARTE adds a 16 KiB EasyDMA ring plus 40 bytes of state, changing
            # the measured fixed state to 87564. Complete-record accounting
            # adds four more bytes of ring state. Keep the reviewed inherited
            # declaration at 60372 bytes without local repair (61056 with it),
            # rather than deriving it from queue arithmetic.
            if [[ "$repair" == "ON" ]]; then
                resource_inherited_fixed_state_delta=61056
            else
                resource_inherited_fixed_state_delta=60372
            fi
        fi
        if [[ "$wearable_ingress" == "ON" ]]; then
            # ARM FULL+repair node-6 map evidence against the immutable
            # ingress-off FULL+repair baseline measures +8032 bytes. Exact ELF
            # symbol accounting attributes 8016 bytes to added application
            # owners, including the bounded GTT response state, and removes the
            # 4-byte diagnostic pending value. The remaining 20 bytes are
            # measured linker placement. Keep this allowance
            # separate from inherited TAVRN state and re-check it from fresh
            # ELF/MAP/sizeof evidence.
            resource_application_fixed_state_delta=8032
        fi
        resource_declared_fixed_state_delta=$((resource_inherited_fixed_state_delta + resource_application_fixed_state_delta))
    fi
fi

commit12="${commit:0:12}"
if [[ "$target" == "ble_mesh_node" ]]; then
    if [[ "$node_id" == "0" || "$node_id" == "0x0" || "$node_id" == "0X0" ]]; then
        identity_tag="idficr"
    else
        printf -v identity_tag 'id%04x' "$((node_id))"
    fi
    artifact_base="tron-ble-legacy-na-${timer_tag}-candidate0-repair0-patient0-${role,,}-${identity_tag}-${commit12}"
elif [[ "$target" == "ble_link_v2_testbed" ]]; then
    if [[ -z "$adva_override" ]]; then
        identity_tag="advaruntime-ficr"
    else
        identity_tag="adva${adva_override//:/}"
        identity_tag="${identity_tag,,}"
    fi
    candidate_bit=0
    [[ "$candidate" == "ON" ]] && candidate_bit=1
    artifact_base="tron-ble-linkv2-harness-${timer_tag}-candidate${candidate_bit}-${role,,}-${identity_tag}-${commit12}"
else
    effective_adva=""
    while IFS='=' read -r manifest_key manifest_value; do
        if [[ "$manifest_key" == "identity.adva" ]]; then
            effective_adva="$manifest_value"
            break
        fi
    done < "$config_manifest"
    if [[ -z "$effective_adva" ]]; then
        printf '%s\n' 'Generated routed manifest lacks identity.adva' >&2
        exit 2
    fi
    if [[ "$effective_adva" == "RUNTIME_FICR" ]]; then
        identity_tag="advaruntime-ficr"
    else
        identity_tag="adva${effective_adva//:/}"
        identity_tag="${identity_tag,,}"
    fi
    if [[ "$feature" == "FULL_TAVRN" ]]; then
        feature_tag="full-tavrn-phase5-adaptive-hello"
        if [[ "$hooks" == "ON" && "$expiry_full_table" == "ON" ]]; then
            feature_tag="${feature_tag}-expiry-full-table-hook"
        fi
    else
        feature_tag="aodv-only"
    fi
    repair_bit=0
    [[ "$repair" == "ON" ]] && repair_bit=1
    if [[ "$candidate" == "ON" && "$hooks" == "ON" ]]; then
        candidate_tag="candidate1-bench-hooked-restricted"
    elif [[ "$candidate" == "ON" ]]; then
        candidate_tag="candidate1-unhooked-acceptance"
    else
        candidate_tag="candidate0-development"
    fi
    artifact_base="tron-ble-routed-${feature_tag}-repair${repair_bit}-${timer_tag}-${candidate_tag}-${role,,}-${identity_tag}-${commit12}"
fi

if [[ "$candidate" == "ON" ]]; then
    final_bundle_dir="${out_dir}/${artifact_base}"
    mkdir -p "$out_dir"
    if [[ -e "$final_bundle_dir" || -L "$final_bundle_dir" ]]; then
        printf 'Candidate publication refused: final bundle already exists: %s\n' \
            "$final_bundle_dir" >&2
        exit 1
    fi
    candidate_staging_dir="$(mktemp -d "${out_dir}/.${artifact_base}.staging.XXXXXX")"
    chmod 0755 "$candidate_staging_dir"
    publication_dir="$candidate_staging_dir"
else
    publication_dir="${build_dir}/publication"
    mkdir -p "$publication_dir"
fi

install -m 0644 "$source_elf" "$publication_dir/${artifact_base}.elf"
install -m 0644 "$source_hex" "$publication_dir/${artifact_base}.hex"
install -m 0644 "$source_map" "$publication_dir/${artifact_base}.map"

# Ninja's commands tool emits the exact commands needed to rebuild the chosen
# target, including its dependency closure.  Persist this and the generator
# inputs before the temporary build tree is removed.
commands_evidence_name="${artifact_base}.ninja-commands.txt"
compile_commands_evidence_name="${artifact_base}.compile_commands.json"
ninja_evidence_name="${artifact_base}.build.ninja"
cmake_cache_evidence_name="${artifact_base}.cmake-cache.txt"
config_manifest_evidence_name="${artifact_base}.build-config.manifest"
config_header_evidence_name="${artifact_base}.build-config.h"
generated_headers_evidence_dir="${publication_dir}/${artifact_base}.generated-headers"
disassembly_evidence_name="${artifact_base}.disassembly.txt"
resource_checker_evidence_name="${artifact_base}.check_tavrn_expiry_resources.py"
source_inventory_evidence_name="${artifact_base}.selected-sources.txt"
source_hashes_evidence_name="${artifact_base}.selected-source-hashes.txt"
complete_source_inventory_evidence_name="${artifact_base}.complete-selected-sources.txt"
complete_source_hashes_evidence_name="${artifact_base}.complete-selected-source-hashes.txt"
generated_sources_evidence_name="${artifact_base}.generated-sources"
generated_sources_evidence_dir="${publication_dir}/${generated_sources_evidence_name}"
ninja -C "$build_dir" -t commands "$target" > "$publication_dir/${commands_evidence_name}"
install -m 0644 "$build_dir/compile_commands.json" \
    "$publication_dir/${compile_commands_evidence_name}"
install -m 0644 "$build_dir/build.ninja" "$publication_dir/${ninja_evidence_name}"
install -m 0644 "$build_dir/CMakeCache.txt" "$publication_dir/${cmake_cache_evidence_name}"
install -m 0644 "$config_manifest" "$publication_dir/${config_manifest_evidence_name}"
install -m 0644 "$generated_config_header" "$publication_dir/${config_header_evidence_name}"
install -m 0644 "$resource_checker" "$publication_dir/${resource_checker_evidence_name}"
install -d "$generated_headers_evidence_dir"
install -m 0644 "$build_dir/app/${target}/generated/${target}/"*.h \
    "$generated_headers_evidence_dir/"
arm-none-eabi-objdump -d "$source_elf" > "$publication_dir/${disassembly_evidence_name}"
test -s "$publication_dir/${commands_evidence_name}"
test -s "$publication_dir/${compile_commands_evidence_name}"
test -s "$publication_dir/${ninja_evidence_name}"
test -s "$publication_dir/${cmake_cache_evidence_name}"
test -s "$publication_dir/${config_manifest_evidence_name}"
test -s "$publication_dir/${config_header_evidence_name}"
test -s "$publication_dir/${resource_checker_evidence_name}"
test -s "$generated_headers_evidence_dir/tron_build_config.h"
test -s "$generated_headers_evidence_dir/tron_build_info.h"
test -s "$publication_dir/${disassembly_evidence_name}"

# Preserve the historical checked-in inventory for the resource checker, then
# publish a complete closure that also binds generated translation units.  D2
# compares the latter so generated sources cannot disappear from provenance.
: > "${build_dir}/selected-sources.unsorted"
: > "${build_dir}/complete-selected-sources.unsorted"
install -d "$generated_sources_evidence_dir"
while IFS='=' read -r source_key source_path; do
    case "$source_key" in
        source.selected.[0-9]*)
            if [[ -z "$source_path" || "$source_path" == /* || "$source_path" == .. ||
                  "$source_path" == ../* || "$source_path" == */../* || "$source_path" == */.. ]]; then
                printf 'Selected source inventory is invalid: %s\n' "$source_path" >&2
                exit 2
            fi
            if [[ "$source_path" == generated/* ]]; then
                generated_source="${build_dir}/app/${target}/generated/${target}/${source_path#generated/}"
                if [[ ! -f "$generated_source" ]]; then
                    printf 'Generated selected source inventory is invalid: %s\n' "$source_path" >&2
                    exit 2
                fi
                mkdir -p "${generated_sources_evidence_dir}/$(dirname "${source_path#generated/}")"
                install -m 0644 "$generated_source" \
                    "${generated_sources_evidence_dir}/${source_path#generated/}"
            elif [[ ! -f "${repo_root}/${source_path}" ]]; then
                printf 'Selected source inventory is invalid: %s\n' "$source_path" >&2
                exit 2
            else
                printf '%s\n' "$source_path" >> "${build_dir}/selected-sources.unsorted"
            fi
            printf '%s\n' "$source_path" >> "${build_dir}/complete-selected-sources.unsorted"
            ;;
    esac
done < "$config_manifest"
if [[ ! -s "${build_dir}/selected-sources.unsorted" ||
      ! -s "${build_dir}/complete-selected-sources.unsorted" ]]; then
    printf '%s\n' 'Selected source inventory is empty' >&2
    exit 2
fi
LC_ALL=C sort "${build_dir}/selected-sources.unsorted" > "$publication_dir/${source_inventory_evidence_name}"
LC_ALL=C sort "${build_dir}/complete-selected-sources.unsorted" \
    > "$publication_dir/${complete_source_inventory_evidence_name}"
if [[ -n "$(uniq -d "$publication_dir/${source_inventory_evidence_name}")" ]]; then
    printf '%s\n' 'Selected source inventory contains duplicates' >&2
    exit 2
fi
if [[ -n "$(uniq -d "$publication_dir/${complete_source_inventory_evidence_name}")" ]]; then
    printf '%s\n' 'Complete selected source inventory contains duplicates' >&2
    exit 2
fi
source_inventory_hash="$(python3 - "$repo_root" "$publication_dir/${source_inventory_evidence_name}" <<'PY'
import hashlib
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
records = []
for line in pathlib.Path(sys.argv[2]).read_text(encoding="utf-8").splitlines():
    records.append(f"{hashlib.sha256((root / line).read_bytes()).hexdigest()}  {line}\n")
print(hashlib.sha256("".join(records).encode("utf-8")).hexdigest())
PY
)"
python3 - "$repo_root" "$publication_dir/${source_inventory_evidence_name}" \
    "$publication_dir/${source_hashes_evidence_name}" <<'PY'
import hashlib
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
paths = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8").splitlines()
records = []
for relative in paths:
    source = root / relative
    records.append(f"{hashlib.sha256(source.read_bytes()).hexdigest()}  {relative}\n")
pathlib.Path(sys.argv[3]).write_text("".join(records), encoding="utf-8")
PY
test -s "$publication_dir/${source_hashes_evidence_name}"
python3 - "$repo_root" "$generated_sources_evidence_dir" \
    "$publication_dir/${complete_source_inventory_evidence_name}" \
    "$publication_dir/${complete_source_hashes_evidence_name}" <<'PY'
import hashlib
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
generated = pathlib.Path(sys.argv[2])
paths = pathlib.Path(sys.argv[3]).read_text(encoding="utf-8").splitlines()
records = []
for relative in paths:
    if relative.startswith("generated/"):
        source = generated / relative.removeprefix("generated/")
    else:
        source = root / relative
    if not source.is_file():
        raise SystemExit("complete selected source is missing: " + relative)
    records.append(f"{hashlib.sha256(source.read_bytes()).hexdigest()}  {relative}\n")
pathlib.Path(sys.argv[4]).write_text("".join(records), encoding="utf-8")
PY
test -s "$publication_dir/${complete_source_hashes_evidence_name}"

stack_evidence_name=""
stack_evidence_glob=""
stack_evidence_index_name=""
resource_manifest_name=""
application_size_report_name=""
application_size_report_args=()
application_size_probe_evidence_name=""
preprocessed_main_evidence_name=""
resource_gate_report_name=""
resource_contract_evidence_names=()
if [[ "$stack_usage" == "ON" ]]; then
    stack_evidence_name="${artifact_base}.stack-usage"
    stack_evidence_dir="$publication_dir/${stack_evidence_name}"
    shopt -s globstar nullglob
    stack_usage_files=("${build_dir}"/**/*.su)
    shopt -u globstar nullglob
    if [[ ${#stack_usage_files[@]} -eq 0 ]]; then
        printf '%s\n' 'GCC stack-usage mode produced no .su evidence' >&2
        exit 1
    fi
    for stack_usage_file in "${stack_usage_files[@]}"; do
        stack_relative="${stack_usage_file#"${build_dir}/"}"
        mkdir -p "${stack_evidence_dir}/$(dirname "$stack_relative")"
        install -m 0644 "$stack_usage_file" "${stack_evidence_dir}/${stack_relative}"
    done
    stack_evidence_glob="${stack_evidence_dir}/**/*.su"
    shopt -s globstar
    compgen -G "$stack_evidence_glob" > /dev/null
    shopt -u globstar
    stack_evidence_hash="$(python3 - "$stack_evidence_dir" <<'PY'
import hashlib
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
paths = sorted(path for path in root.rglob("*.su") if path.is_file())
if not paths:
    raise SystemExit(1)
records = "".join(
    f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(root).as_posix()}\n"
    for path in paths)
print(hashlib.sha256(records.encode("utf-8")).hexdigest())
PY
)"
    stack_evidence_index_name="${artifact_base}.stack-usage.index"
    python3 - "$stack_evidence_dir" "$publication_dir/${stack_evidence_index_name}" <<'PY'
import hashlib
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
paths = sorted(path for path in root.rglob("*.su") if path.is_file())
if not paths:
    raise SystemExit(1)
records = "".join(
    f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(root).as_posix()}\n"
    for path in paths)
pathlib.Path(sys.argv[2]).write_text(records, encoding="utf-8")
PY
    test -s "$publication_dir/${stack_evidence_index_name}"
    if [[ "$(sha256sum "$publication_dir/${stack_evidence_index_name}" | cut -d' ' -f1)" != \
          "$stack_evidence_hash" ]]; then
        printf '%s\n' 'Stack usage index aggregate differs from stack evidence' >&2
        exit 1
    fi
    if [[ "$wearable_ingress" == "ON" ]]; then
        application_size_report_name="${artifact_base}.mind.application.sizes.v1.json"
        application_size_probe_evidence_name="${artifact_base}.mind.application.size-probe.c"
        install -m 0644 "${repo_root}/tests/application/test_mind_application_resource_sizes.c" \
            "$publication_dir/${application_size_probe_evidence_name}"
        python3 "$resource_checker" \
            --emit-application-size-report "$publication_dir/${application_size_report_name}" \
            --application-size-source \
            "${repo_root}/tests/application/test_mind_application_resource_sizes.c" \
            --compile-commands "$build_dir/compile_commands.json" \
            --full-elf "$source_elf" --full-map "$source_map" \
            --config-header "$generated_config_header"
        test -s "$publication_dir/${application_size_report_name}"
        test -s "$publication_dir/${application_size_probe_evidence_name}"
        application_size_report_args=(
            --application-size-report "$publication_dir/${application_size_report_name}"
            --application-size-source \
            "${repo_root}/tests/application/test_mind_application_resource_sizes.c"
        )
    fi
    resource_manifest_name="${artifact_base}.tron.tavrn.expiry.resources.v1.json"
    python3 "$resource_checker" --emit-resource-manifest "$publication_dir/${resource_manifest_name}" \
        --name "$artifact_base" --target "$target" --feature "$feature" --timer "$timer_profile" \
        --full-elf "$publication_dir/${artifact_base}.elf" --full-map "$publication_dir/${artifact_base}.map" \
        --full-manifest "$config_manifest" --compile-commands "$publication_dir/${compile_commands_evidence_name}" \
        --selected-sources "$publication_dir/${source_inventory_evidence_name}" \
        --disassembly "$publication_dir/${disassembly_evidence_name}" \
        --config-header "$publication_dir/${config_header_evidence_name}" \
        --declared-fixed-state-delta "$resource_declared_fixed_state_delta" \
        "${application_size_report_args[@]}"
    test -s "$publication_dir/${resource_manifest_name}"
fi

write_publication_manifest() {
    local include_candidate_status="$1"

    : > "$manifest_work"
while IFS= read -r manifest_line || [[ -n "$manifest_line" ]]; do
    [[ -n "$manifest_line" ]] || continue
    printf '%s\n' "$manifest_line" >> "$manifest_work"
done < "$config_manifest"
    compile_commands_hash="$(sha256sum "$publication_dir/${compile_commands_evidence_name}" | cut -d' ' -f1)"
{
    printf 'artifact.elf.name=%s.elf\n' "$artifact_base"
    printf 'artifact.elf.sha256=%s\n' "$(sha256sum "$publication_dir/${artifact_base}.elf" | cut -d' ' -f1)"
    printf 'artifact.elf.size=%s\n' "$(wc -c < "$publication_dir/${artifact_base}.elf")"
    printf 'artifact.hex.name=%s.hex\n' "$artifact_base"
    printf 'artifact.hex.sha256=%s\n' "$(sha256sum "$publication_dir/${artifact_base}.hex" | cut -d' ' -f1)"
    printf 'artifact.hex.size=%s\n' "$(wc -c < "$publication_dir/${artifact_base}.hex")"
    printf 'artifact.map.name=%s.map\n' "$artifact_base"
    printf 'artifact.map.sha256=%s\n' "$(sha256sum "$publication_dir/${artifact_base}.map" | cut -d' ' -f1)"
    printf 'artifact.map.size=%s\n' "$(wc -c < "$publication_dir/${artifact_base}.map")"
    printf 'artifact.name=%s\n' "$artifact_base"
    printf 'artifact.sha256=%s\n' "$(sha256sum "$publication_dir/${artifact_base}.elf" | cut -d' ' -f1)"
    printf 'artifact.size=%s\n' "$(wc -c < "$publication_dir/${artifact_base}.elf")"
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
    printf 'build.ninja_file.sha256=%s\n' "$(sha256sum "$publication_dir/${ninja_evidence_name}" | cut -d' ' -f1)"
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
    if [[ "$include_candidate_status" == yes ]]; then
        printf 'candidate.clean_source=%s\n' "$candidate_clean_source"
        printf 'candidate.eligible=%s\n' "$candidate_eligible"
        printf 'candidate.hardware_purpose=%s\n' "$candidate_hardware_purpose"
        printf 'candidate.hook_bench_eligible=%s\n' "$candidate_hook_bench_eligible"
        printf 'candidate.unhooked_acceptance=%s\n' "$candidate_unhooked_acceptance"
    fi
    printf 'source.commit=%s\n' "$commit"
    printf 'source.dirty=%s\n' "$source_dirty"
    printf 'source.worktree_content.scope=git-diff-head-binary-plus-relevant-untracked-v1\n'
    printf 'source.worktree_content.sha256=%s\n' "$worktree_content_sha256"
    printf 'source.inventory.name=%s\n' "$source_inventory_evidence_name"
    printf 'source.inventory.sha256=%s\n' "$source_inventory_hash"
    printf 'source.inventory.size=%s\n' "$(wc -c < "$publication_dir/${source_inventory_evidence_name}")"
    printf 'source.inventory.file_hashes.name=%s\n' "$source_hashes_evidence_name"
    printf 'source.inventory.file_hashes.sha256=%s\n' \
        "$(sha256sum "$publication_dir/${source_hashes_evidence_name}" | cut -d' ' -f1)"
    printf 'source.inventory.file_hashes.size=%s\n' \
        "$(wc -c < "$publication_dir/${source_hashes_evidence_name}")"
    printf 'source.submodule.count=%s\n' "$submodule_count"
    printf '%s' "$submodule_manifest_lines"
    printf 'source.submodule.sha256=%s\n' "$(printf '%s' "$submodule_status" | sha256sum | cut -d' ' -f1)"
    printf 'source.submodule_dirty=%s\n' "$submodule_dirty"
    printf 'source.tree=%s\n' "$tree"
    printf 'evidence.build_ninja.name=%s\n' "$ninja_evidence_name"
    printf 'evidence.build_ninja.sha256=%s\n' "$(sha256sum "$publication_dir/${ninja_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.build_ninja.size=%s\n' "$(wc -c < "$publication_dir/${ninja_evidence_name}")"
    printf 'evidence.cmake_cache.name=%s\n' "$cmake_cache_evidence_name"
    printf 'evidence.cmake_cache.sha256=%s\n' "$(sha256sum "$publication_dir/${cmake_cache_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.cmake_cache.size=%s\n' "$(wc -c < "$publication_dir/${cmake_cache_evidence_name}")"
    printf 'evidence.compile_commands.name=%s\n' "$compile_commands_evidence_name"
    printf 'evidence.compile_commands.sha256=%s\n' "$compile_commands_hash"
    printf 'evidence.compile_commands.size=%s\n' "$(wc -c < "$publication_dir/${compile_commands_evidence_name}")"
    printf 'evidence.ninja_commands.name=%s\n' "$commands_evidence_name"
    printf 'evidence.ninja_commands.sha256=%s\n' "$(sha256sum "$publication_dir/${commands_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.ninja_commands.size=%s\n' "$(wc -c < "$publication_dir/${commands_evidence_name}")"
    printf 'evidence.target_config_header.name=%s\n' "$config_header_evidence_name"
    printf 'evidence.target_config_header.sha256=%s\n' "$(sha256sum "$publication_dir/${config_header_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.target_config_header.size=%s\n' "$(wc -c < "$publication_dir/${config_header_evidence_name}")"
    printf 'evidence.target_config_manifest.name=%s\n' "$config_manifest_evidence_name"
    printf 'evidence.target_config_manifest.sha256=%s\n' "$(sha256sum "$publication_dir/${config_manifest_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.target_config_manifest.size=%s\n' "$(wc -c < "$publication_dir/${config_manifest_evidence_name}")"
    printf 'evidence.disassembly.name=%s\n' "$disassembly_evidence_name"
    printf 'evidence.disassembly.sha256=%s\n' "$(sha256sum "$publication_dir/${disassembly_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.disassembly.size=%s\n' "$(wc -c < "$publication_dir/${disassembly_evidence_name}")"
    printf 'evidence.selected_sources.name=%s\n' "$source_inventory_evidence_name"
    printf 'evidence.selected_sources.sha256=%s\n' "$(sha256sum "$publication_dir/${source_inventory_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.selected_sources.size=%s\n' "$(wc -c < "$publication_dir/${source_inventory_evidence_name}")"
    printf 'evidence.selected_source_hashes.name=%s\n' "$source_hashes_evidence_name"
    printf 'evidence.selected_source_hashes.sha256=%s\n' \
        "$(sha256sum "$publication_dir/${source_hashes_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.selected_source_hashes.size=%s\n' \
        "$(wc -c < "$publication_dir/${source_hashes_evidence_name}")"
    printf 'evidence.complete_selected_sources.name=%s\n' "$complete_source_inventory_evidence_name"
    printf 'evidence.complete_selected_sources.sha256=%s\n' \
        "$(sha256sum "$publication_dir/${complete_source_inventory_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.complete_selected_sources.size=%s\n' \
        "$(wc -c < "$publication_dir/${complete_source_inventory_evidence_name}")"
    printf 'evidence.complete_selected_source_hashes.name=%s\n' \
        "$complete_source_hashes_evidence_name"
    printf 'evidence.complete_selected_source_hashes.sha256=%s\n' \
        "$(sha256sum "$publication_dir/${complete_source_hashes_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.complete_selected_source_hashes.size=%s\n' \
        "$(wc -c < "$publication_dir/${complete_source_hashes_evidence_name}")"
    printf 'evidence.generated_sources.directory.name=%s\n' "$generated_sources_evidence_name"
    printf 'evidence.resource_checker.path=scripts/check_tavrn_expiry_resources.py\n'
    printf 'evidence.resource_checker.name=%s\n' "$resource_checker_evidence_name"
    printf 'evidence.resource_checker.sha256=%s\n' \
        "$(sha256sum "$publication_dir/${resource_checker_evidence_name}" | cut -d' ' -f1)"
    printf 'evidence.resource_checker.size=%s\n' \
        "$(wc -c < "$publication_dir/${resource_checker_evidence_name}")"
    if [[ "$stack_usage" == "ON" ]]; then
        printf 'resource.schema=tron.tavrn.expiry.resources.v1\n'
        printf 'resource.stack_usage=ON\n'
        printf 'resource.su_glob=%s\n' "$stack_evidence_name/**/*.su"
        printf 'resource.su.sha256=%s\n' "$stack_evidence_hash"
        printf 'resource.su.directory.name=%s\n' "$stack_evidence_name"
        printf 'resource.su.index.name=%s\n' "$stack_evidence_index_name"
        printf 'resource.su.index.sha256=%s\n' \
            "$(sha256sum "$publication_dir/${stack_evidence_index_name}" | cut -d' ' -f1)"
        printf 'resource.su.index.size=%s\n' \
            "$(wc -c < "$publication_dir/${stack_evidence_index_name}")"
        printf 'resource.manifest.name=%s\n' "$resource_manifest_name"
        printf 'resource.manifest.sha256=%s\n' "$(sha256sum "$publication_dir/${resource_manifest_name}" | cut -d' ' -f1)"
        printf 'resource.manifest.size=%s\n' "$(wc -c < "$publication_dir/${resource_manifest_name}")"
        printf 'resource.fixed_state.declared_delta_bytes=%s\n' \
            "$resource_declared_fixed_state_delta"
        printf 'resource.fixed_state.inherited_delta_bytes=%s\n' \
            "$resource_inherited_fixed_state_delta"
        printf 'resource.fixed_state.application_delta_bytes=%s\n' \
            "$resource_application_fixed_state_delta"
        printf 'resource.mesh_stack.total_bytes=%s\n' "$resource_mesh_stack_total_bytes"
        printf 'resource.mesh_stack.headroom_bytes=%s\n' "$resource_mesh_stack_headroom_bytes"
        printf 'resource.logger_stack.total_bytes=%s\n' "$resource_logger_stack_total_bytes"
        printf 'resource.logger_stack.headroom_bytes=%s\n' "$resource_logger_stack_headroom_bytes"
        printf 'resource.application_logger_stack.total_bytes=%s\n' "$resource_application_logger_stack_total_bytes"
        printf 'resource.application_logger_stack.headroom_bytes=%s\n' "$resource_application_logger_stack_headroom_bytes"
        printf 'resource.application_uart_stack.total_bytes=%s\n' "$resource_application_uart_stack_total_bytes"
        printf 'resource.application_uart_stack.headroom_bytes=%s\n' "$resource_application_uart_stack_headroom_bytes"
        printf 'resource.application_ui_stack.total_bytes=%s\n' "$resource_application_ui_stack_total_bytes"
        printf 'resource.application_ui_stack.headroom_bytes=%s\n' "$resource_application_ui_stack_headroom_bytes"
        printf 'resource.application_display_stack.total_bytes=%s\n' "$resource_application_display_stack_total_bytes"
        printf 'resource.application_display_stack.headroom_bytes=%s\n' "$resource_application_display_stack_headroom_bytes"
        if [[ -n "$application_size_report_name" ]]; then
            printf 'resource.application_size.name=%s\n' "$application_size_report_name"
            printf 'resource.application_size.sha256=%s\n' \
                "$(sha256sum "$publication_dir/${application_size_report_name}" | cut -d' ' -f1)"
            printf 'resource.application_size.size=%s\n' \
                "$(wc -c < "$publication_dir/${application_size_report_name}")"
            printf 'resource.application_size_probe.name=%s\n' "$application_size_probe_evidence_name"
            printf 'resource.application_size_probe.sha256=%s\n' \
                "$(sha256sum "$publication_dir/${application_size_probe_evidence_name}" | cut -d' ' -f1)"
            printf 'resource.application_size_probe.size=%s\n' \
                "$(wc -c < "$publication_dir/${application_size_probe_evidence_name}")"
        fi
        if [[ -n "$resource_gate_report_name" ]]; then
            printf 'resource.gate_report.name=%s\n' "$resource_gate_report_name"
            printf 'resource.gate_report.sha256=%s\n' \
                "$(sha256sum "$publication_dir/${resource_gate_report_name}" | cut -d' ' -f1)"
            printf 'resource.gate_report.size=%s\n' \
                "$(wc -c < "$publication_dir/${resource_gate_report_name}")"
            printf 'evidence.preprocessed_main.name=%s\n' "$preprocessed_main_evidence_name"
            printf 'evidence.preprocessed_main.sha256=%s\n' \
                "$(sha256sum "$publication_dir/${preprocessed_main_evidence_name}" | cut -d' ' -f1)"
            printf 'evidence.preprocessed_main.size=%s\n' \
                "$(wc -c < "$publication_dir/${preprocessed_main_evidence_name}")"
            printf 'resource.contract.count=%s\n' "${#resource_contract_evidence_names[@]}"
            for resource_contract_index in "${!resource_contract_evidence_names[@]}"; do
                resource_contract_name="${resource_contract_evidence_names[resource_contract_index]}"
                printf 'resource.contract.%s.name=%s\n' "$resource_contract_index" \
                    "$resource_contract_name"
                printf 'resource.contract.%s.sha256=%s\n' "$resource_contract_index" \
                    "$(sha256sum "$publication_dir/${resource_contract_name}" | cut -d' ' -f1)"
                printf 'resource.contract.%s.size=%s\n' "$resource_contract_index" \
                    "$(wc -c < "$publication_dir/${resource_contract_name}")"
            done
        fi
    else
        printf 'resource.stack_usage=OFF\n'
    fi
    printf 'resource.gate=%s\n' "$resource_gate"
} >> "$manifest_work"

duplicate_keys="$(cut -d= -f1 "$manifest_work" | LC_ALL=C sort | uniq -d)"
if [[ -n "$duplicate_keys" ]]; then
    printf 'Manifest generation produced duplicate keys:\n%s\n' "$duplicate_keys" >&2
    exit 1
fi
LC_ALL=C sort "$manifest_work" > "$publication_dir/${artifact_base}.manifest"
}

if [[ -n "$resource_baseline" || "$d2_current_resource_gate" == "ON" ]]; then
    resource_baseline_args=()
    baseline_dir="$(dirname "$resource_baseline")"
    if [[ -n "$resource_baseline" ]]; then
        baseline_map="${baseline_dir}/${timer_tag}.before.map"
        baseline_hash="${baseline_dir}/${timer_tag}.baseline.sha256"
        if [[ ! -f "$baseline_map" || ! -f "$baseline_hash" ]]; then
            printf 'Resource baseline is incomplete for %s: expected %s and %s\n' \
                "$timer_profile" "$baseline_map" "$baseline_hash" >&2
            exit 2
        fi
        resource_baseline_args=(
            --before-map "$baseline_map" --baseline-manifest "$resource_baseline"
            --baseline-sha256 "$baseline_hash"
        )
    else
        resource_baseline_args=(--current-resource-gate)
    fi
    resource_gate="PENDING"
    write_publication_manifest no
    resource_gate_log="${build_dir}/resource-gate.log"
    preprocessed_main_evidence_name="${artifact_base}.main.i"
    resource_mesh_edge_manifest="${repo_root}/tests/protocol/fixtures/tavrn_expiry_resources/required-stack-edges.json"
    if [[ "$repair" == "ON" ]]; then
        resource_mesh_edge_manifest="${repo_root}/tests/protocol/fixtures/tavrn_expiry_resources/required-repair-stack-edges.json"
    fi
    resource_logger_args=()
    resource_application_args=()
    if [[ "$benchmark" == "ON" ]]; then
        resource_logger_args=(
            --logger-stack-root routed_logger_task
            --logger-required-edge-manifest "${repo_root}/tests/protocol/fixtures/tavrn_expiry_resources/required-logger-stack-edges.json"
        )
    fi
    if [[ "$wearable_ingress" == "ON" ]]; then
        shopt -s nullglob
        application_artifact_manifests=("${application_resource_baseline}"/*.manifest)
        shopt -u nullglob
        application_baseline_artifact_manifest=""
        for application_manifest_candidate in "${application_artifact_manifests[@]}"; do
            if [[ "$application_manifest_candidate" != *.build-config.manifest ]]; then
                if [[ -n "$application_baseline_artifact_manifest" ]]; then
                    printf 'Application resource baseline must contain exactly one artifact manifest: %s\n' \
                        "$application_resource_baseline" >&2
                    exit 2
                fi
                application_baseline_artifact_manifest="$application_manifest_candidate"
            fi
        done
        if [[ -z "$application_baseline_artifact_manifest" ]]; then
            printf 'Application resource baseline lacks an artifact manifest: %s\n' \
                "$application_resource_baseline" >&2
            exit 2
        fi
        application_baseline_name=""
        application_baseline_elf_name=""
        application_baseline_map_name=""
        application_baseline_resource_name=""
        application_baseline_build_manifest_name=""
        application_baseline_sources_name=""
        application_manifest_key=""
        application_manifest_value=""
        while IFS='=' read -r application_manifest_key application_manifest_value || \
              [[ -n "$application_manifest_key" ]]; do
            case "$application_manifest_key" in
                artifact.name) application_baseline_name="$application_manifest_value" ;;
                artifact.elf.name) application_baseline_elf_name="$application_manifest_value" ;;
                artifact.map.name) application_baseline_map_name="$application_manifest_value" ;;
                resource.manifest.name) application_baseline_resource_name="$application_manifest_value" ;;
                evidence.target_config_manifest.name)
                    application_baseline_build_manifest_name="$application_manifest_value" ;;
                evidence.selected_sources.name)
                    application_baseline_sources_name="$application_manifest_value" ;;
            esac
        done < "$application_baseline_artifact_manifest"
        if [[ -z "$application_baseline_name" ||
              "$application_baseline_elf_name" != "${application_baseline_name}.elf" ||
              "$application_baseline_map_name" != "${application_baseline_name}.map" ||
              -z "$application_baseline_resource_name" ||
              -z "$application_baseline_build_manifest_name" ||
              -z "$application_baseline_sources_name" ]]; then
            printf 'Application resource baseline artifact manifest is incomplete: %s\n' \
                "$application_baseline_artifact_manifest" >&2
            exit 2
        fi
        application_baseline_map="${application_resource_baseline}/${application_baseline_map_name}"
        application_baseline_manifest="${application_resource_baseline}/${application_baseline_resource_name}"
        application_baseline_build_manifest="${application_resource_baseline}/${application_baseline_build_manifest_name}"
        application_baseline_sources="${application_resource_baseline}/${application_baseline_sources_name}"
        application_baseline_elf="${application_resource_baseline}/${application_baseline_elf_name}"
        for application_baseline_file in "$application_baseline_artifact_manifest" \
            "$application_baseline_map" "$application_baseline_manifest" \
            "$application_baseline_build_manifest" "$application_baseline_sources" \
            "$application_baseline_elf"; do
            if [[ ! -f "$application_baseline_file" ]]; then
                printf 'Application resource baseline is incomplete for %s: missing %s\n' \
                    "$timer_profile" "$application_baseline_file" >&2
                exit 2
            fi
        done
        resource_mesh_edge_manifest="${repo_root}/tests/protocol/fixtures/tavrn_expiry_resources/required-mind-mesh-stack-edges.json"
        resource_application_args=(
            --application-size-report "$publication_dir/${application_size_report_name}"
            --application-size-source "${repo_root}/tests/application/test_mind_application_resource_sizes.c"
            --inherited-fixed-state-delta "$resource_inherited_fixed_state_delta"
            --application-fixed-state-delta "$resource_application_fixed_state_delta"
            --application-before-map "$application_baseline_map"
            --application-baseline-manifest "$application_baseline_manifest"
            --application-baseline-artifact-manifest "$application_baseline_artifact_manifest"
            --application-baseline-seal "$application_resource_baseline_seal"
            --application-baseline-build-manifest "$application_baseline_build_manifest"
            --application-baseline-selected-sources "$application_baseline_sources"
            --application-baseline-elf "$application_baseline_elf"
            --mind-logger-required-edge-manifest "${repo_root}/tests/protocol/fixtures/tavrn_expiry_resources/required-mind-logger-stack-edges.json"
            --mind-uart-required-edge-manifest "${repo_root}/tests/protocol/fixtures/tavrn_expiry_resources/required-mind-uart-stack-edges.json"
            --mind-ui-required-edge-manifest "${repo_root}/tests/protocol/fixtures/tavrn_expiry_resources/required-mind-ui-stack-edges.json"
            --mind-display-required-edge-manifest "${repo_root}/tests/protocol/fixtures/tavrn_expiry_resources/required-mind-display-stack-edges.json"
        )
    fi
    resource_contract_paths=("$resource_mesh_edge_manifest")
    if [[ "$benchmark" == "ON" ]]; then
        resource_contract_paths+=("${repo_root}/tests/protocol/fixtures/tavrn_expiry_resources/required-logger-stack-edges.json")
    fi
    if [[ "$wearable_ingress" == "ON" ]]; then
        resource_contract_paths+=(
            "${repo_root}/tests/protocol/fixtures/tavrn_expiry_resources/required-mind-logger-stack-edges.json"
            "${repo_root}/tests/protocol/fixtures/tavrn_expiry_resources/required-mind-uart-stack-edges.json"
            "${repo_root}/tests/protocol/fixtures/tavrn_expiry_resources/required-mind-ui-stack-edges.json"
            "${repo_root}/tests/protocol/fixtures/tavrn_expiry_resources/required-mind-display-stack-edges.json"
        )
    fi
    resource_contract_evidence_names=()
    for resource_contract_path in "${resource_contract_paths[@]}"; do
        resource_contract_name="${artifact_base}.resource-contract.$(basename "$resource_contract_path")"
        install -m 0644 "$resource_contract_path" "$publication_dir/${resource_contract_name}"
        resource_contract_evidence_names+=("$resource_contract_name")
    done
    set +e
    python3 "$resource_checker" --resource-manifest "$publication_dir/${resource_manifest_name}" \
        --full-elf "$publication_dir/${artifact_base}.elf" --full-map "$publication_dir/${artifact_base}.map" \
        --full-manifest "$publication_dir/${artifact_base}.manifest" \
        "${resource_baseline_args[@]}" --selected-sources "$publication_dir/${source_inventory_evidence_name}" \
        --su-glob "$stack_evidence_glob" --stack-root routed_mesh_task \
        --required-edge-manifest "$resource_mesh_edge_manifest" \
        --resolve-operation-edge 'routed_cycle_operations.router_scheduler_event=routed_cycle_router_scheduler_event' \
        --resolve-operation-edge 'routed_cycle_operations.router_tick=routed_cycle_router_tick' \
        --resolve-operation-edge 'routed_cycle_operations.application_prepare=routed_cycle_application_prepare' \
        --disassembly "$publication_dir/${disassembly_evidence_name}" \
        --compile-commands "$publication_dir/${compile_commands_evidence_name}" \
        --config-header "$publication_dir/${config_header_evidence_name}" \
        --main-source "${repo_root}/app/tavrn_routed_node/src/main.c" \
        --preprocessed-main-out "${publication_dir}/${preprocessed_main_evidence_name}" \
        --require-binding-call "${resource_logger_args[@]}" \
        "${resource_application_args[@]}" >"$resource_gate_log" 2>&1
    resource_gate_status=$?
    set -e
    if [[ $resource_gate_status -eq 0 ]]; then
        resource_gate="PASSED"
    else
        resource_gate="FAILED"
    fi
    if [[ $resource_gate_status -ne 0 ]]; then
        while IFS= read -r resource_gate_line; do
            printf '%s\n' "$resource_gate_line" >&2
        done < "$resource_gate_log"
        printf 'Resource gate failed while staging artifacts: %s\n' \
            "$publication_dir/${artifact_base}.manifest" >&2
        exit "$resource_gate_status"
    fi
    python3 "$resource_checker" --finalize-resource-manifest \
        --resource-manifest "$publication_dir/${resource_manifest_name}" \
        --resource-gate-report "$resource_gate_log" >/dev/null
    resource_gate_report_name="${artifact_base}.resource-gate.txt"
    install -m 0644 "$resource_gate_log" "$publication_dir/${resource_gate_report_name}"
    test -s "$publication_dir/${resource_gate_report_name}"
    test -s "$publication_dir/${preprocessed_main_evidence_name}"
    resource_gate_value() {
        local key="$1"
        local line=""
        local candidate=""
        local count=0

        while IFS= read -r candidate || [[ -n "$candidate" ]]; do
            if [[ "$candidate" == "${key}="* ]]; then
                line="$candidate"
                ((count += 1))
            fi
        done < "$resource_gate_log"
        if [[ $count -ne 1 ]]; then
            printf 'Resource gate output lacks one %s\n' "$key" >&2
            exit 1
        fi
        printf '%s\n' "${line#*=}"
    }
    resource_mesh_stack_total_bytes="$(resource_gate_value MESH_STACK_TOTAL_BYTES)"
    resource_mesh_stack_headroom_bytes="$(resource_gate_value MESH_STACK_HEADROOM_BYTES)"
    if [[ "$benchmark" == "ON" ]]; then
        resource_logger_stack_total_bytes="$(resource_gate_value LOGGER_STACK_TOTAL_BYTES)"
        resource_logger_stack_headroom_bytes="$(resource_gate_value LOGGER_STACK_HEADROOM_BYTES)"
        if [[ ! "$resource_logger_stack_total_bytes" =~ ^[0-9]+$ ]] ||
           [[ ! "$resource_logger_stack_headroom_bytes" =~ ^[0-9]+$ ]] ||
           (( resource_logger_stack_headroom_bytes < 1024 )); then
            printf 'Resource gate logger stack evidence is malformed or below 1024-byte headroom\n' >&2
            exit 1
        fi
    fi
    if [[ "$wearable_ingress" == "ON" ]]; then
        resource_application_logger_stack_total_bytes="$(resource_gate_value ROUTED_LOGGER_TASK_STACK_TOTAL_BYTES)"
        resource_application_logger_stack_headroom_bytes="$(resource_gate_value ROUTED_LOGGER_TASK_STACK_HEADROOM_BYTES)"
        resource_application_uart_stack_total_bytes="$(resource_gate_value MIND_UART_TASK_STACK_TOTAL_BYTES)"
        resource_application_uart_stack_headroom_bytes="$(resource_gate_value MIND_UART_TASK_STACK_HEADROOM_BYTES)"
        resource_application_ui_stack_total_bytes="$(resource_gate_value MIND_UI_TASK_STACK_TOTAL_BYTES)"
        resource_application_ui_stack_headroom_bytes="$(resource_gate_value MIND_UI_TASK_STACK_HEADROOM_BYTES)"
        resource_application_display_stack_total_bytes="$(resource_gate_value DISPLAY_TASK_STACK_TOTAL_BYTES)"
        resource_application_display_stack_headroom_bytes="$(resource_gate_value DISPLAY_TASK_STACK_HEADROOM_BYTES)"
    fi
fi

observe_source_state
if [[ "$candidate" == "ON" &&
      ( "$source_dirty" != "$pre_source_dirty" ||
        "$submodule_dirty" != "$pre_submodule_dirty" ||
         "$commit" != "$pre_commit" || "$tree" != "$pre_tree" ||
         "$worktree_content_sha256" != "$pre_worktree_content_sha256" ) ]]; then
    printf 'Candidate publication refused: source state changed during build/evidence: pre_source_dirty=%s post_source_dirty=%s pre_submodule_dirty=%s post_submodule_dirty=%s pre_commit=%s post_commit=%s pre_tree=%s post_tree=%s pre_worktree_content_sha256=%s post_worktree_content_sha256=%s\n' \
        "$pre_source_dirty" "$source_dirty" "$pre_submodule_dirty" "$submodule_dirty" \
        "$pre_commit" "$commit" "$pre_tree" "$tree" "$pre_worktree_content_sha256" \
        "$worktree_content_sha256" >&2
    exit 1
fi

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
write_publication_manifest yes
if [[ -n "$resource_baseline" || "$d2_current_resource_gate" == "ON" ]]; then
    python3 "$resource_checker" --verify-published-provenance \
        "$publication_dir/${artifact_base}.manifest" \
        >"${build_dir}/published-resource-provenance.log" 2>&1
fi

atomic_publish_candidate_bundle() {
    local staging_dir="$1"
    local final_dir="$2"

    python3 - "$staging_dir" "$final_dir" <<'PY'
import ctypes
import errno
import os
import stat
import sys

staging_dir, final_dir = sys.argv[1:]
unsupported_fsync_errors = {
    errno.EINVAL,
    errno.ENOTSUP,
    getattr(errno, "EOPNOTSUPP", errno.ENOTSUP),
}


def fsync_path(path, *, directory=False):
    flags = os.O_RDONLY
    if directory:
        flags |= os.O_DIRECTORY
    descriptor = os.open(path, flags)
    try:
        try:
            os.fsync(descriptor)
        except OSError as error:
            if error.errno not in unsupported_fsync_errors:
                raise
    finally:
        os.close(descriptor)


for root, directories, files in os.walk(staging_dir):
    for filename in files:
        path = os.path.join(root, filename)
        if stat.S_ISREG(os.stat(path, follow_symlinks=False).st_mode):
            fsync_path(path)

directories_to_sync = []
for root, directories, _ in os.walk(staging_dir):
    directories_to_sync.append(root)
for directory in reversed(directories_to_sync):
    fsync_path(directory, directory=True)

libc = ctypes.CDLL(None, use_errno=True)
try:
    renameat2 = libc.renameat2
except AttributeError:
    raise SystemExit("Candidate publication requires renameat2(RENAME_NOREPLACE)")
renameat2.argtypes = [
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.c_uint,
]
renameat2.restype = ctypes.c_int
if renameat2(-100, os.fsencode(staging_dir), -100, os.fsencode(final_dir), 1) != 0:
    error = ctypes.get_errno()
    if error == errno.EEXIST:
        raise SystemExit(
            "Candidate publication refused: final bundle already exists: %s" % final_dir)
    raise SystemExit(
        "Candidate publication could not atomically finalize bundle %s: %s" %
        (final_dir, os.strerror(error)))

try:
    fsync_path(os.path.dirname(final_dir), directory=True)
except OSError as error:
    print(
        "Warning: candidate bundle was published but output directory fsync failed: %s" %
        error,
        file=sys.stderr,
    )
PY
}

if [[ "$candidate" == "ON" ]]; then
    atomic_publish_candidate_bundle "$publication_dir" "$final_bundle_dir"
    candidate_staging_dir=""
    published_dir="$final_bundle_dir"
else
    shopt -s nullglob dotglob
    publication_entries=("$publication_dir"/*)
    shopt -u nullglob dotglob
    if [[ ${#publication_entries[@]} -eq 0 ]]; then
        printf '%s\n' 'Publication staging directory is empty' >&2
        exit 1
    fi
    for publication_entry in "${publication_entries[@]}"; do
        publication_name="$(basename "$publication_entry")"
        if [[ -e "$out_dir/$publication_name" || -L "$out_dir/$publication_name" ]]; then
            printf 'Publication refused: existing output would be overwritten: %s\n' \
                "$out_dir/$publication_name" >&2
            exit 1
        fi
    done
    mkdir -p "$out_dir"
    for publication_entry in "${publication_entries[@]}"; do
        publication_name="$(basename "$publication_entry")"
        if [[ -d "$publication_entry" ]]; then
            cp -a "$publication_entry" "$out_dir/$publication_name"
        else
            install -m 0644 "$publication_entry" "$out_dir/$publication_name"
        fi
    done
    published_dir="$out_dir"
fi

printf 'Published ELF: %s\n' "$published_dir/${artifact_base}.elf"
printf 'Published HEX: %s\n' "$published_dir/${artifact_base}.hex"
printf 'Published MAP: %s\n' "$published_dir/${artifact_base}.map"
printf 'Commands: %s\n' "$published_dir/${commands_evidence_name}"
printf 'Compile commands: %s\n' "$published_dir/${compile_commands_evidence_name}"
printf 'Build Ninja: %s\n' "$published_dir/${ninja_evidence_name}"
printf 'CMake cache: %s\n' "$published_dir/${cmake_cache_evidence_name}"
printf 'Target config: %s\n' "$published_dir/${config_manifest_evidence_name}"
printf 'Target config header: %s\n' "$published_dir/${config_header_evidence_name}"
printf 'Disassembly: %s\n' "$published_dir/${disassembly_evidence_name}"
printf 'Selected sources: %s\n' "$published_dir/${source_inventory_evidence_name}"
if [[ "$stack_usage" == "ON" ]]; then
    printf 'Stack usage: %s\n' "$published_dir/${stack_evidence_name}"
    printf 'Resource manifest: %s\n' "$published_dir/${resource_manifest_name}"
fi
printf 'Manifest: %s\n' "$published_dir/${artifact_base}.manifest"
