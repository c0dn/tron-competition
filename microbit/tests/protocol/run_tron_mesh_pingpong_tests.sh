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
    "${MICROBIT_ROOT}/app/protocol/tron_mesh_pingpong.c" \
    "${MICROBIT_ROOT}/app/protocol/tron_mesh_packet.c" \
    -o "${BUILD_DIR}/test_tron_mesh_pingpong"

"${BUILD_DIR}/test_tron_mesh_pingpong"

compile_must_fail() {
    if "${CC_BIN}" \
        -std=c99 \
        -Wall \
        -Wextra \
        -Werror \
        -I"${MICROBIT_ROOT}/app/protocol" \
        "$@" \
        -fsyntax-only \
        "${MICROBIT_ROOT}/tests/protocol/test_tron_mesh_pingpong.c" \
        >/dev/null 2>&1; then
        printf 'FAIL compile-time timing guard accepted invalid constants\n' >&2
        exit 1
    fi
}

compile_must_fail \
    -DTRON_MESH_PINGPONG_TIMEOUT_MS=2000u
compile_must_fail \
    -DTRON_MESH_PINGPONG_INTERVAL_MS=0x80000000UL
compile_must_fail \
    -DTRON_MESH_PINGPONG_INTERVAL_MS=0x90000000UL \
    -DTRON_MESH_PINGPONG_TIMEOUT_MS=0x80000000UL

printf 'Compile-time PING/PONG timing guard tests passed.\n'
