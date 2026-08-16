#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPO_ROOT="$(cd "${MICROBIT_ROOT}/.." && pwd)"
BUILD_DIR="$(mktemp -d)"

cleanup() {
    rm -rf "${BUILD_DIR}"
}
trap cleanup EXIT

cc \
    -std=c99 \
    -Wall \
    -Wextra \
    -Werror \
    -DWEARABLE_HOST_TEST \
    -I"${MICROBIT_ROOT}/app/wearable_app/src" \
    -I"${REPO_ROOT}/shared" \
    "${MICROBIT_ROOT}/tests/wearable/test_tx_adapter.c" \
    "${MICROBIT_ROOT}/app/wearable_app/src/tx_adapter.c" \
    -o "${BUILD_DIR}/test_tx_adapter"

"${BUILD_DIR}/test_tx_adapter"

cc \
    -std=c99 \
    -Wall \
    -Wextra \
    -Werror \
    -I"${MICROBIT_ROOT}/tests/wearable/support" \
    -I"${MICROBIT_ROOT}/app/wearable_app/src" \
    -I"${MICROBIT_ROOT}/app/drivers" \
    -I"${MICROBIT_ROOT}/app/protocol" \
    -I"${REPO_ROOT}/shared" \
    "${MICROBIT_ROOT}/tests/wearable/test_ble_emit.c" \
    "${MICROBIT_ROOT}/app/wearable_app/src/ble_emit.c" \
    "${MICROBIT_ROOT}/app/protocol/tron_mesh_packet.c" \
    -o "${BUILD_DIR}/test_ble_emit"

"${BUILD_DIR}/test_ble_emit"
