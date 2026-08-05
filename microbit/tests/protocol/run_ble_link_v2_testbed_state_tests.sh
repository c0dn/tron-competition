#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
CC_BIN="${CC:-cc}"

cleanup() {
    rm -rf "${BUILD_DIR}"
}
trap cleanup EXIT

"${CC_BIN}" -std=c99 -Wall -Wextra -Werror -DBLE_RADIO_HOST_TEST \
    -I"${MICROBIT_ROOT}/app/ble_link_v2_testbed/src" \
    -I"${MICROBIT_ROOT}/app/protocol" \
    -I"${MICROBIT_ROOT}/app/drivers" \
    "${MICROBIT_ROOT}/tests/protocol/test_ble_link_v2_testbed_state.c" \
    "${MICROBIT_ROOT}/app/protocol/tron_timer_config.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_wire_v2.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_link_v2.c" \
    "${MICROBIT_ROOT}/app/protocol/ble_mesh_tx_queue.c" \
    "${MICROBIT_ROOT}/tests/protocol/support/ble_mesh_scheduler_port.c" \
    -o "${BUILD_DIR}/test_ble_link_v2_testbed_state"

"${BUILD_DIR}/test_ble_link_v2_testbed_state"
