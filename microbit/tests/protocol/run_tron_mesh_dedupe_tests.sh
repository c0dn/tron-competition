#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
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
    -I"${MICROBIT_ROOT}/app/protocol" \
    "${MICROBIT_ROOT}/tests/protocol/test_tron_mesh_dedupe.c" \
    "${MICROBIT_ROOT}/app/protocol/tron_mesh_dedupe.c" \
    -o "${BUILD_DIR}/test_tron_mesh_dedupe"

"${BUILD_DIR}/test_tron_mesh_dedupe"
