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

cc \
    -std=c99 \
    -Wall \
    -Wextra \
    -Werror \
    -DWEARABLE_TEST_INJECTOR=1 \
    -DWEARABLE_TEST_INJECTOR_HOST_TEST=1 \
    -I"${MICROBIT_ROOT}/tests/wearable/support" \
    -I"${MICROBIT_ROOT}/app/wearable_app/src" \
    -I"${MICROBIT_ROOT}/app/drivers" \
    -I"${MICROBIT_ROOT}/app/protocol" \
    -I"${REPO_ROOT}/shared" \
    "${MICROBIT_ROOT}/tests/wearable/test_test_injector.c" \
    "${MICROBIT_ROOT}/app/wearable_app/src/fusion.c" \
    "${MICROBIT_ROOT}/app/wearable_app/src/tx_adapter.c" \
    "${MICROBIT_ROOT}/app/wearable_app/src/ble_emit.c" \
    "${MICROBIT_ROOT}/app/wearable_app/src/wearable_test_injector.c" \
    "${MICROBIT_ROOT}/app/protocol/tron_mesh_packet.c" \
    -o "${BUILD_DIR}/test_test_injector"

"${BUILD_DIR}/test_test_injector"

cmake -S "${MICROBIT_ROOT}" -B "${BUILD_DIR}/cmake" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${MICROBIT_ROOT}/cmake/arm-none-eabi-gcc.cmake" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DTRON_PHASE1_TARGET=LEGACY \
    -DTRON_NODE_MODE=LEGACY_FLOOD \
    -DTAVRN_FEATURE_LEVEL=

python3 - "${BUILD_DIR}/cmake/compile_commands.json" \
    "${MICROBIT_ROOT}/app/wearable_app/src" <<'PY'
import json
import sys
from pathlib import Path

compile_commands = json.loads(Path(sys.argv[1]).read_text())
source_dir = Path(sys.argv[2]).resolve()
sources = {str((source_dir / name).resolve()) for name in (
    "main.c", "imath.c", "sensors.c", "fall.c", "sound.c", "fusion.c",
    "ble_emit.c", "tx_adapter.c",
)}
injector_sources = sources | {str((source_dir / "wearable_test_injector.c").resolve())}
injector_target_sources = injector_sources - {str((source_dir / "main.c").resolve())}

def target_commands(target, expected_sources):
    marker = f"{target}.dir/"
    return [entry for entry in compile_commands
            if str(Path(entry["file"]).resolve()) in expected_sources
            and marker in entry["command"]]

production = target_commands("wearable_app", sources)
injector = target_commands("wearable_test_injector", injector_target_sources)
injector_main = target_commands("wearable_test_injector_main",
                                {str((source_dir / "main.c").resolve())})
if {str(Path(entry["file"]).resolve()) for entry in production} != sources:
    raise SystemExit("wearable_app does not compile the complete production source set")
if {str(Path(entry["file"]).resolve()) for entry in injector} != injector_target_sources:
    raise SystemExit("wearable_test_injector does not reuse the production sources plus its hook")
if len(injector_main) != 1:
    raise SystemExit("wearable_test_injector main hook does not compile main.c once")
if any("-DWEARABLE_TEST_INJECTOR=1" in entry["command"] for entry in production):
    raise SystemExit("wearable_app unexpectedly receives the injector macro")
if not all("-DWEARABLE_TEST_INJECTOR=1" in entry["command"]
           for entry in injector + injector_main):
    raise SystemExit("wearable_test_injector is missing its private injector macro")
if "wearable_test_injector.h" not in injector_main[0]["command"]:
    raise SystemExit("wearable_test_injector main is missing its private forced hook header")

print("wearable test injector CMake wiring passed")
PY
