#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPO_ROOT="$(cd "${MICROBIT_ROOT}/.." && pwd)"
BUILD_DIR="$(mktemp -d)"
CC_BIN="${CC:-cc}"

cleanup() {
    rm -rf "${BUILD_DIR}"
}
trap cleanup EXIT

COMMON_FLAGS=(
    -std=c99 -Wall -Wextra -Werror
    -I"${MICROBIT_ROOT}/tests/wearable/support"
    -I"${MICROBIT_ROOT}/app/mind_application"
    -I"${MICROBIT_ROOT}/app/tavrn_routed_node/src"
    -I"${MICROBIT_ROOT}/app/drivers"
    -I"${MICROBIT_ROOT}/app/protocol"
    -I"${REPO_ROOT}/shared"
)

"${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/application/test_mind_application_wire.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_application_wire.c" \
    -o "${BUILD_DIR}/test_mind_application_wire"
"${BUILD_DIR}/test_mind_application_wire"

"${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/application/test_mind_application_ingress.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_application_wire.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_application_ingress.c" \
    "${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_cycle.c" \
    -o "${BUILD_DIR}/test_mind_application_ingress"
"${BUILD_DIR}/test_mind_application_ingress"

"${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/application/test_mind_root_plane.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_application_wire.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_topology_adapter.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_root_plane.c" \
    -o "${BUILD_DIR}/test_mind_root_plane"
"${BUILD_DIR}/test_mind_root_plane"

"${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/application/test_mind_event_forwarder.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_application_wire.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_application_ingress.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_topology_adapter.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_root_plane.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_root_inbox.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_event_forwarder.c" \
    -o "${BUILD_DIR}/test_mind_event_forwarder"
"${BUILD_DIR}/test_mind_event_forwarder"

"${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/application/test_mind_log_formatter.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_command.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_log.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_log_formatter.c" \
    -o "${BUILD_DIR}/test_mind_log_formatter"
"${BUILD_DIR}/test_mind_log_formatter"

"${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/application/test_mind_gtt_response.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_gtt_response.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_log.c" \
    -o "${BUILD_DIR}/test_mind_gtt_response"
"${BUILD_DIR}/test_mind_gtt_response"

"${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/application/test_mind_phase5_provenance.c" \
    -o "${BUILD_DIR}/test_mind_phase5_provenance"
"${BUILD_DIR}/test_mind_phase5_provenance"

"${CC_BIN}" "${COMMON_FLAGS[@]}" -DBLE_RADIO_HOST_TEST \
    "${MICROBIT_ROOT}/tests/application/test_mind_router_inbox_integration.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_application_wire.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_root_inbox.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_wire_v2.c" \
    "${MICROBIT_ROOT}/app/protocol/aodv_core.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_link_v2.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_router.c" \
    "${MICROBIT_ROOT}/app/protocol/ble_mesh_tx_queue.c" \
    "${MICROBIT_ROOT}/app/protocol/tron_timer_config.c" \
    "${MICROBIT_ROOT}/tests/protocol/support/ble_mesh_scheduler_port.c" \
    -o "${BUILD_DIR}/test_mind_router_inbox_integration"
"${BUILD_DIR}/test_mind_router_inbox_integration"

"${CC_BIN}" "${COMMON_FLAGS[@]}" -DMIND_APPLICATION_HOST_TEST \
    "${MICROBIT_ROOT}/tests/application/test_mind_root_integration.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_application_wire.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_application_ingress.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_topology_adapter.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_root_plane.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_event_forwarder.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_command.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_log.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_log_formatter.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_root_inbox.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_root_coordinator.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_uart.c" \
    -o "${BUILD_DIR}/test_mind_root_integration"
"${BUILD_DIR}/test_mind_root_integration"

"${CC_BIN}" "${COMMON_FLAGS[@]}" -DMIND_APPLICATION_HOST_TEST -DMIND_AUDIO_HOST_REGISTERS \
    "${MICROBIT_ROOT}/tests/application/test_mind_command_ui.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_command.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_application_wire.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_topology_adapter.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_root_plane.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_log.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_audio.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_ui.c" \
    -o "${BUILD_DIR}/test_mind_command_ui"
"${BUILD_DIR}/test_mind_command_ui"

"${CC_BIN}" "${COMMON_FLAGS[@]}" -DMIND_APPLICATION_HOST_TEST \
    "${MICROBIT_ROOT}/tests/application/test_mind_uart.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_command.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_uart.c" \
    "${MICROBIT_ROOT}/app/mind_application/mind_root_inbox.c" \
    -o "${BUILD_DIR}/test_mind_uart"
"${BUILD_DIR}/test_mind_uart"

# The resource checker compiles this same probe with the published ARM command
# line and reads its symbols as target-ABI sizeof evidence.  Compile it here as
# a host ABI/header closure check as well.
"${CC_BIN}" "${COMMON_FLAGS[@]}" -c \
    "${MICROBIT_ROOT}/tests/application/test_mind_application_resource_sizes.c" \
    -o "${BUILD_DIR}/test_mind_application_resource_sizes.o"
