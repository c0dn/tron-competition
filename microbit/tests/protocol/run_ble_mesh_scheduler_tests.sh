#!/usr/bin/env bash
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MICROBIT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR=${TMPDIR:-/tmp/opencode}/ble_mesh_scheduler_tests
CC_BIN=${CC:-cc}

mkdir -p "$BUILD_DIR"

"$CC_BIN" \
  -std=c99 \
  -Wall \
  -Wextra \
  -Werror \
  -DBLE_RADIO_HOST_TEST \
  -I"$MICROBIT_DIR/app/protocol" \
  -I"$MICROBIT_DIR/app/drivers" \
  "$MICROBIT_DIR/app/protocol/ble_mesh_scheduler.c" \
  "$SCRIPT_DIR/test_ble_mesh_scheduler.c" \
  -o "$BUILD_DIR/test_ble_mesh_scheduler"

"$CC_BIN" \
  -std=c99 \
  -Wall \
  -Wextra \
  -Werror \
  -DBLE_RADIO_HOST_TEST \
  -DTEST_BLE_RADIO_DRIVER \
  -I"$MICROBIT_DIR/app/drivers" \
  "$MICROBIT_DIR/app/drivers/ble_radio.c" \
  "$SCRIPT_DIR/test_ble_mesh_scheduler.c" \
  -o "$BUILD_DIR/test_ble_radio_driver"

"$BUILD_DIR/test_ble_radio_driver"
"$BUILD_DIR/test_ble_mesh_scheduler"
