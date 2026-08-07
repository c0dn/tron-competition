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
