#!/usr/bin/env bash
#
# Build a ble_mesh_node image with a pinned node id.
#
# The node's identity is otherwise derived from FICR, which is unique per board
# but unreadable (0x2dc2, 0xf602, ...). Pinning it makes a bench topology
# self-describing in the serial logs.

set -euo pipefail

repo_root="$(cd "$(dirname "$0")" && pwd)"
toolchain="$repo_root/cmake/arm-none-eabi-gcc.cmake"
node_id=""
block_direct_peer="0x0000"
out_dir="$repo_root/artifacts"
ping_interval_ms="2000"
ping_timeout_ms="1500"
ping_root_id="0x0001"
ping_leaf_id="0x0002"

usage() {
    cat <<'EOF'
Usage: ./build-ble-node.sh --node-id 0xNNNN [options]

Options:
  --node-id 0xNNNN         Pinned node id. 0 derives the id from FICR.
  --block-direct-peer ID   Test-only originator whose direct (full-TTL) frames
                           are ignored, forcing a relayed path. 0 disables.
  --out DIR                Artifact directory (default: microbit/artifacts)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --node-id) node_id="${2:?Missing value for --node-id}"; shift 2 ;;
        --block-direct-peer) block_direct_peer="${2:?Missing value for --block-direct-peer}"; shift 2 ;;
        --out) out_dir="${2:?Missing value for --out}"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$node_id" ]]; then
    printf '%s\n' '--node-id is required' >&2
    usage >&2
    exit 2
fi
if [[ ! "$node_id" =~ ^0[xX][0-9A-Fa-f]{1,4}$ ]] ||
   [[ ! "$block_direct_peer" =~ ^0[xX][0-9A-Fa-f]{1,4}$ ]]; then
    printf '%s\n' 'node and blocked peer ids must be hexadecimal values fitting 16 bits' >&2
    exit 2
fi

node_value=$((node_id))
if (( node_value == 65535 )); then
    printf '%s\n' '--node-id must not be the broadcast id 0xffff' >&2
    exit 2
fi

canonical_node_id=$(printf '0x%04x' "$node_value")
canonical_block_direct_peer=$(printf '0x%04x' "$((block_direct_peer))")
if [[ "$canonical_block_direct_peer" != "0x0000" && "$canonical_block_direct_peer" == "$canonical_node_id" ]]; then
    printf '%s\n' '--block-direct-peer must not equal --node-id' >&2
    exit 2
fi
test_hooks=OFF
if [[ "$canonical_block_direct_peer" != "0x0000" ]]; then
    test_hooks=ON
fi
artifact_base="ble_mesh_node-${canonical_node_id}"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/tron-ble-node.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT

if [[ -n "$(git -C "$repo_root" status --porcelain)" ]]; then
    source_dirty=yes
else
    source_dirty=no
fi

mkdir -p "$out_dir"

cmake -S "$repo_root" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DTRON_PHASE1_TARGET=LEGACY \
    -DTRON_NODE_MODE=LEGACY_FLOOD \
    -DTRON_NODE_ID="$canonical_node_id" \
    -DTRON_ENABLE_TEST_HOOKS="$test_hooks" \
    -DTRON_NODE_BLOCK_DIRECT_PEER_ID="$canonical_block_direct_peer"
cmake --build "$build_dir" --target ble_mesh_node --parallel

source_elf="$build_dir/firmware/ble_mesh_node/ble_mesh_node.elf"
test -s "$source_elf"
install -m 0644 "$source_elf" "$out_dir/${artifact_base}.elf"

cat >"$out_dir/${artifact_base}.manifest" <<EOF
node_id=$canonical_node_id
block_direct_peer=$canonical_block_direct_peer
ping_interval_ms=$ping_interval_ms
ping_timeout_ms=$ping_timeout_ms
ping_root_id=$ping_root_id
ping_leaf_id=$ping_leaf_id
source_commit=$(git -C "$repo_root" rev-parse HEAD)
source_dirty=$source_dirty
artifact=${artifact_base}.elf
EOF

printf 'Built node artifact: %s\n' "$out_dir/${artifact_base}.elf"
printf 'Manifest: %s\n' "$out_dir/${artifact_base}.manifest"
