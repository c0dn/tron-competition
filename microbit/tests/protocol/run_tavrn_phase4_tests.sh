#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
CC_BIN="${CC:-cc}"

cleanup() {
    rm -rf "${BUILD_DIR}"
}
trap cleanup EXIT

if [[ $# -ne 1 || ( "$1" != "--esc" && "$1" != "--bootstrap" &&
                     "$1" != "--esc-red" && "$1" != "--bootstrap-red" &&
                     "$1" != "--green" ) ]]; then
    printf 'usage: %s --esc|--bootstrap|--esc-red|--bootstrap-red|--green\n' "$0" >&2
    exit 2
fi

if [[ "$1" == "--esc-red" ]]; then
    TEST_MODE="--esc"
    REQUIREMENTS=(ESC-01 ESC-02 ESC-03)
    SUMMARY='tavrn_phase4 ESC RED tests failed: 3 assertion(s)'
elif [[ "$1" == "--bootstrap-red" ]]; then
    TEST_MODE="--bootstrap"
    REQUIREMENTS=(BOOT-01 BOOT-02 BOOT-03 BOOT-04 BOOT-05 BOOT-06)
    SUMMARY='tavrn_phase4 BOOTSTRAP RED tests failed: 6 assertion(s)'
fi

COMMON_FLAGS=(
    -std=c99
    -Wall
    -Wextra
    -Werror
    -DBLE_RADIO_HOST_TEST
    -I"${MICROBIT_ROOT}/app/drivers"
    -I"${MICROBIT_ROOT}/app/protocol"
    -I"${MICROBIT_ROOT}/tests/protocol/support"
    -I"${MICROBIT_ROOT}/tests/protocol/red_support"
)
SOURCES=(
    "${MICROBIT_ROOT}/tests/protocol/test_tavrn_phase4.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_wire_v2.c"
    "${MICROBIT_ROOT}/app/protocol/aodv_core.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_link_v2.c"
    "${MICROBIT_ROOT}/app/protocol/ble_mesh_tx_queue.c"
    "${MICROBIT_ROOT}/tests/protocol/support/ble_mesh_scheduler_port.c"
    "${MICROBIT_ROOT}/app/protocol/tron_timer_config.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_gtt.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_smart_ttl.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_full.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_router.c"
)

if [[ "$1" == "--green" || "$1" == "--esc" || "$1" == "--bootstrap" ]]; then
    COMMON_FLAGS+=( -DTAVRN_ESC_MENTORSHIP_API )
    SOURCES+=(
        "${MICROBIT_ROOT}/app/protocol/tavrn_esc.c"
        "${MICROBIT_ROOT}/app/protocol/tavrn_mentorship.c"
        "${MICROBIT_ROOT}/app/protocol/tavrn_maintenance.c"
    )
else
    SOURCES+=("${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase4_esc_mentor_red_backend.c")
fi

for source in "${SOURCES[@]}" \
              "${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase4_esc_mentor_contract.h"; do
    if [[ ! -f "${source}" ]]; then
        printf 'Phase 4 %s setup unavailable: expected source %s\n' \
            "$1" "${source#"${MICROBIT_ROOT}/"}" >&2
        exit 2
    fi
done

if ! "${CC_BIN}" "${COMMON_FLAGS[@]}" "${SOURCES[@]}" \
        -o "${BUILD_DIR}/test_tavrn_phase4"; then
    printf 'Phase 4 %s compile failed\n' "$1" >&2
    exit 2
fi

if [[ "$1" == "--green" ]]; then
    "${BUILD_DIR}/test_tavrn_phase4" --esc
    "${BUILD_DIR}/test_tavrn_phase4" --bootstrap
    exit 0
fi

if [[ "$1" == "--esc" || "$1" == "--bootstrap" ]]; then
    "${BUILD_DIR}/test_tavrn_phase4" "$1"
    exit 0
fi

set +e
"${BUILD_DIR}/test_tavrn_phase4" "${TEST_MODE}" | tee "${BUILD_DIR}/red.log"
test_status=${PIPESTATUS[0]}
set -e

failure_count="$(grep -c '^FAIL ' "${BUILD_DIR}/red.log" || true)"
if [[ ${test_status} -ne 1 || "${failure_count}" != "${#REQUIREMENTS[@]}" ]]; then
    printf 'Phase 4 %s RED requires exactly %d assertion failures; got exit=%d fail=%s\n' \
        "$1" "${#REQUIREMENTS[@]}" "${test_status}" "${failure_count}" >&2
    exit 2
fi
for requirement in "${REQUIREMENTS[@]}"; do
    if [[ "$(grep -c "^FAIL ${requirement}:" "${BUILD_DIR}/red.log" || true)" != "1" ]]; then
        printf 'Phase 4 %s RED requires one exact %s failure\n' \
            "$1" "${requirement}" >&2
        exit 2
    fi
done
if [[ "$(grep -Fxc "${SUMMARY}" "${BUILD_DIR}/red.log" || true)" != "1" ]]; then
    printf 'Phase 4 %s RED missing exact final assertion summary\n' "$1" >&2
    exit 2
fi
exit 1
