#!/usr/bin/env bash

set -euo pipefail

source "$(cd "$(dirname "$0")" && pwd)/scripts/common.sh"

device="${1:-}"
baud="${2:-115200}"

if [[ -z "$device" ]]; then
    cat <<'EOF' >&2
Usage: ./serial.sh /dev/ttyACM0 [baud]
EOF
    exit 1
fi

grabserial_bin="$(find_tool grabserial)"
exec "$grabserial_bin" -d "$device" -b "$baud" -t
