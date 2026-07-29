#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")" && pwd)"
toolchain="$repo_root/cmake/arm-none-eabi-gcc.cmake"
role=""
node_id=""
root_id="0x0001"
network_id="0x5452"
channel="7"
txpower="0x00"
out_dir="$repo_root/artifacts"

usage() {
    cat <<'EOF'
Usage: ./build-rf-role.sh --role root|node --node-id 0xNNNN [options]

Options:
  --root-id 0xNNNN       Configured root ID (default: 0x0001)
  --network-id 0xNNNN    Network ID (default: 0x5452)
  --channel N            RF channel 0..83 (default: 7)
  --txpower 0xNN         Raw supported nRF52833 TXPOWER value (default: 0x00)
  --out DIR              Artifact directory (default: microbit/artifacts)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --role) role="${2:?Missing value for --role}"; shift 2 ;;
        --node-id) node_id="${2:?Missing value for --node-id}"; shift 2 ;;
        --root-id) root_id="${2:?Missing value for --root-id}"; shift 2 ;;
        --network-id) network_id="${2:?Missing value for --network-id}"; shift 2 ;;
        --channel) channel="${2:?Missing value for --channel}"; shift 2 ;;
        --txpower) txpower="${2:?Missing value for --txpower}"; shift 2 ;;
        --out) out_dir="${2:?Missing value for --out}"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ "$role" != "root" && "$role" != "node" ]]; then
    printf '%s\n' '--role must be root or node' >&2
    exit 2
fi
if [[ ! "$node_id" =~ ^0[xX][0-9A-Fa-f]{1,4}$ ]] ||
   [[ ! "$root_id" =~ ^0[xX][0-9A-Fa-f]{1,4}$ ]] ||
   [[ ! "$network_id" =~ ^0[xX][0-9A-Fa-f]{1,4}$ ]]; then
    printf '%s\n' 'node, root, and network IDs must be hexadecimal values fitting 16 bits' >&2
    exit 2
fi
if [[ ! "$channel" =~ ^[0-9]+$ ]] || (( channel > 83 )); then
    printf '%s\n' '--channel must be an integer in 0..83' >&2
    exit 2
fi

node_value=$((node_id))
root_value=$((root_id))
if (( node_value == 0 || node_value == 65535 || root_value == 0 || root_value == 65535 )); then
    printf '%s\n' 'node/root IDs must not be zero or broadcast' >&2
    exit 2
fi
if [[ "$role" == "root" && $node_value -ne $root_value ]]; then
    printf '%s\n' 'root role requires --node-id to equal --root-id' >&2
    exit 2
fi
if [[ "$role" == "node" && $node_value -eq $root_value ]]; then
    printf '%s\n' 'node role requires --node-id to differ from --root-id' >&2
    exit 2
fi

role_flag=0
[[ "$role" == "root" ]] && role_flag=1
canonical_node_id=$(printf '0x%04x' "$node_value")
canonical_root_id=$(printf '0x%04x' "$root_value")
canonical_network_id=$(printf '0x%04x' "$((network_id))")
artifact_base="rf_mesh_node-${role}-${canonical_node_id}"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/tron-rf-role.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
mkdir -p "$out_dir"

cmake -S "$repo_root" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DRF_MESH_NODE_ID="$canonical_node_id" \
    -DRF_MESH_IS_ROOT="$role_flag" \
    -DRF_MESH_ROOT_ID="$canonical_root_id" \
    -DRF_MESH_NETWORK_ID="$canonical_network_id" \
    -DRF_MESH_RF_CHANNEL="$channel" \
    -DRF_MESH_RF_TXPOWER="$txpower"
cmake --build "$build_dir" --target rf_mesh_node --parallel

source_elf="$build_dir/firmware/rf_mesh_node/rf_mesh_node.elf"
test -s "$source_elf"
install -m 0644 "$source_elf" "$out_dir/${artifact_base}.elf"

cat >"$out_dir/${artifact_base}.manifest" <<EOF
role=$role
node_id=$canonical_node_id
root_id=$canonical_root_id
network_id=$canonical_network_id
channel=$channel
txpower=$txpower
protocol_version=2
source_commit=$(git -C "$repo_root" rev-parse HEAD)
source_dirty=$(git -C "$repo_root" status --porcelain --untracked-files=no | grep -q . && printf yes || printf no)
artifact=${artifact_base}.elf
EOF

printf 'Built role artifact: %s\n' "$out_dir/${artifact_base}.elf"
printf 'Manifest: %s\n' "$out_dir/${artifact_base}.manifest"
