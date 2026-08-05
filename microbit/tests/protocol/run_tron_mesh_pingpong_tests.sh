#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
CC_BIN="${CC:-cc}"

cleanup() {
    rm -rf "${BUILD_DIR}"
}
trap cleanup EXIT

"${CC_BIN}" \
    -std=c99 \
    -Wall \
    -Wextra \
    -Werror \
    -I"${MICROBIT_ROOT}/app/protocol" \
    "${MICROBIT_ROOT}/tests/protocol/test_tron_mesh_pingpong.c" \
    "${MICROBIT_ROOT}/app/protocol/tron_timer_config.c" \
    "${MICROBIT_ROOT}/app/protocol/tron_mesh_pingpong.c" \
    "${MICROBIT_ROOT}/app/protocol/tron_mesh_packet.c" \
    -o "${BUILD_DIR}/test_tron_mesh_pingpong"

"${BUILD_DIR}/test_tron_mesh_pingpong"

printf 'Frozen timer-object PING/PONG timing tests passed.\n'
