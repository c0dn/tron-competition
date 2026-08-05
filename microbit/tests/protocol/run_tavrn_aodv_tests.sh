#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
CC_BIN="${CC:-cc}"
MODE="production"

cleanup() {
    rm -rf "${BUILD_DIR}"
}
trap cleanup EXIT

if [[ $# -gt 1 || ( $# -eq 1 && "$1" != "--red" ) ]]; then
    printf 'usage: %s [--red]\n' "$0" >&2
    exit 2
fi
if [[ $# -eq 1 ]]; then
    MODE="red"
fi

SOURCES=(
    "${MICROBIT_ROOT}/app/protocol/tavrn_wire_v2.c"
)
if [[ "${MODE}" == "red" ]]; then
    SOURCES+=("${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_aodv_red_backend.c")
else
    SOURCES+=("${MICROBIT_ROOT}/app/protocol/aodv_core.c")
fi

for source in \
    "${MICROBIT_ROOT}/app/protocol/aodv_core.h" \
    "${MICROBIT_ROOT}/tests/protocol/test_tavrn_aodv.c" \
    "${SOURCES[@]}"; do
    if [[ ! -f "${source}" ]]; then
        printf 'AODV setup unavailable: expected source %s\n' \
            "${source#"${MICROBIT_ROOT}/"}" >&2
        exit 2
    fi
done

if ! "${CC_BIN}" \
    -std=c99 \
    -Wall \
    -Wextra \
    -Werror \
    -I"${MICROBIT_ROOT}/app/protocol" \
    "${MICROBIT_ROOT}/tests/protocol/test_tavrn_aodv.c" \
    "${SOURCES[@]}" \
    -o "${BUILD_DIR}/test_tavrn_aodv"; then
    printf 'AODV %s compile failed\n' "${MODE}" >&2
    exit 2
fi

if [[ "${MODE}" != "red" ]]; then
    "${BUILD_DIR}/test_tavrn_aodv"
    exit $?
fi

set +e
"${BUILD_DIR}/test_tavrn_aodv" | tee "${BUILD_DIR}/aodv-red.log"
test_status=${PIPESTATUS[0]}
set -e

if [[ ${test_status} -ne 1 ]]; then
    printf 'AODV RED expected assertion exit 1, got %d\n' "${test_status}" >&2
    exit 2
fi

for requirement in \
    SERIAL-02 SERIAL-03 SERIAL-04 \
    AODV-01 AODV-02 AODV-03 AODV-04 AODV-05 AODV-06 AODV-07; do
    if ! grep -Fq "FAIL ${requirement}:" "${BUILD_DIR}/aodv-red.log"; then
        printf 'AODV RED missing required assertion tag %s\n' "${requirement}" >&2
        exit 2
    fi
done
if ! grep -Fq 'tavrn_aodv RED tests failed:' "${BUILD_DIR}/aodv-red.log"; then
    printf '%s\n' 'AODV RED missing final assertion summary' >&2
    exit 2
fi

exit 1
