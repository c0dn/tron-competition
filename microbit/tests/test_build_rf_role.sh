#!/usr/bin/env bash
set -euo pipefail

microbit_dir="$(cd "$(dirname "$0")/.." && pwd)"
out_dir="$(mktemp -d)"
trap 'rm -rf "$out_dir"' EXIT

expect_fail() {
    if "$@" >/dev/null 2>&1; then
        printf 'FAIL expected rejection: %q' "$1" >&2
        printf ' %q' "${@:2}" >&2
        printf '\n' >&2
        exit 1
    fi
}

expect_fail "$microbit_dir/build-rf-role.sh"
expect_fail "$microbit_dir/build-rf-role.sh" --role node --node-id 0x0001 --out "$out_dir"
expect_fail "$microbit_dir/build-rf-role.sh" --role root --node-id 0x1001 --out "$out_dir"

"$microbit_dir/build-rf-role.sh" --role root --node-id 0x0001 --out "$out_dir"
"$microbit_dir/build-rf-role.sh" --role node --node-id 0x1001 --out "$out_dir"

test -s "$out_dir/rf_mesh_node-root-0x0001.elf"
test -s "$out_dir/rf_mesh_node-node-0x1001.elf"
grep -q '^role=root$' "$out_dir/rf_mesh_node-root-0x0001.manifest"
grep -q '^node_id=0x0001$' "$out_dir/rf_mesh_node-root-0x0001.manifest"
grep -q '^role=node$' "$out_dir/rf_mesh_node-node-0x1001.manifest"
grep -q '^node_id=0x1001$' "$out_dir/rf_mesh_node-node-0x1001.manifest"

printf 'RF role build tests passed\n'
