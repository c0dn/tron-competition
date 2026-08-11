#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
CC_BIN="${CC:-cc}"

cleanup() {
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

COMMON_FLAGS=(
    -std=c99 -Wall -Wextra -Werror -DBLE_RADIO_HOST_TEST
    -I"${MICROBIT_ROOT}/app/tavrn_routed_node/src"
    -I"${MICROBIT_ROOT}/app/drivers"
    -I"${MICROBIT_ROOT}/app/protocol"
    -I"${MICROBIT_ROOT}/tests/protocol/support"
)

"${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/protocol/test_routed_benchmark_isolation.c" \
    "${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_benchmark.c" \
    -o "${BUILD_DIR}/test_routed_benchmark_isolation"
"${BUILD_DIR}/test_routed_benchmark_isolation"

"${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/protocol/test_routed_benchmark.c" \
    "${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_benchmark.c" \
    "${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_benchmark_full.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_full.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_gtt.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_smart_ttl.c" \
    -o "${BUILD_DIR}/test_routed_benchmark"
"${BUILD_DIR}/test_routed_benchmark"

python3 - "${MICROBIT_ROOT}/app/tavrn_routed_node/src/main.c" \
    "${MICROBIT_ROOT}/app/drivers/display.c" <<'PY'
import pathlib
import re
import sys

main_path, display_path = map(pathlib.Path, sys.argv[1:])
main = main_path.read_text(encoding="utf-8")
display = display_path.read_text(encoding="utf-8")
errors = []

def body(name):
    match = re.search(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{", main)
    if not match:
        errors.append(f"missing {name}")
        return ""
    start = match.end() - 1
    depth = 0
    for index in range(start, len(main)):
        if main[index] == "{": depth += 1
        elif main[index] == "}":
            depth -= 1
            if depth == 0: return main[start + 1:index]
    errors.append(f"unterminated {name}")
    return ""

prepare = body("routed_cycle_application_prepare")
submit = body("routed_cycle_router_submit")
logger = body("routed_logger_task")
usermain = body("usermain")
for token in ("routed_benchmark_schedule_due", "routed_benchmark_destination_ready",
              "routed_benchmark_record_not_ready", "routed_benchmark_offer_attempt"):
    if token not in prepare:
        errors.append(f"benchmark prepare lacks {token}")
benchmark_prepare = re.search(
    r"#if TRON_BUILD_BENCHMARK_MODE(?P<body>.*?)#else", prepare, re.S)
if benchmark_prepare is None or "configured_destination.adva.bytes[0]" in benchmark_prepare.group("body"):
    errors.append("FULL benchmark blindly derives destination from AdvA[0]")
for token in ("TRON_BUILD_LINK_INITIATOR == 0", "routed_counters.submitted >=",
              "routed_next_submit_at"):
    if token not in prepare:
        errors.append(f"nonbenchmark prepare parity lacks {token}")
for token in ("routed_benchmark_record_submission", "routed_benchmark_offer_attempt",
              "routed_benchmark_pending_attempt_valid"):
    if token not in submit:
        errors.append(f"benchmark submit lacks {token}")
if "routed_next_submit_at = trace.completed_at_ms + TRON_BUILD_LINK_TX_INTERVAL_MS;" not in submit:
    errors.append("nonbenchmark completion-relative submission schedule changed")
for token in ("routed_benchmark_attempt_pop", "log_benchmark_attempt",
              "log_benchmark_clock", "log_benchmark_summary", "routed bench_final"):
    if token not in logger:
        errors.append(f"logger lacks {token}")
if "tm_printf" in prepare or "tm_printf" in submit:
    errors.append("benchmark mesh path prints instead of copying logger records")
if logger.find("delivery_pop") > logger.find("routed_benchmark_attempt_pop"):
    errors.append("benchmark attempt logging can preempt final-delivery draining")
if (prepare.find("routed_benchmark_destination_ready") >
        prepare.find("routed_benchmark_pending_attempt_valid = 1u")):
    errors.append("FULL not-ready decision can reach a submission")
if ("display_show_benchmark_role(TRON_BUILD_BENCH_ROLE_NUMBER, 1500u);" not in usermain or
        usermain.find("display_show_benchmark_role") > usermain.find("ble_radio_try_init")):
    errors.append("benchmark role scan is not before radio initialization")
benchmark_branch = re.search(
    r"#if TRON_BUILD_BENCHMARK_MODE\s*\n\s*/\*.*?\*/\s*"
    r"display_show_benchmark_role\(TRON_BUILD_BENCH_ROLE_NUMBER, 1500u\);\s*#endif",
    main, re.S)
identify_branch = re.search(
    r"#if TRON_BUILD_BENCH_IDENTIFY_DISPLAY && !TRON_BUILD_BENCHMARK_MODE\s*"
    r"display_init\(\);", main, re.S)
if benchmark_branch is None or identify_branch is None:
    errors.append("benchmark display branch can reach display_init")
for token in ("role_static_x[6]", "role_static_y[6]", "scan_rows(role_digit_glyphs[index], scan_ms)",
              "gpio_high(row_pin[role_static_y[index]])", "gpio_low(col_pin[role_static_x[index]])"):
    if token not in display:
        errors.append(f"static role LED contract lacks {token}")
display_benchmark = re.search(
    r"void\s+display_show_benchmark_role\s*\([^)]*\)\s*\{(?P<body>.*?)\n\}",
    display, re.S)
if display_benchmark is None or "tk_cre_tsk" in display_benchmark.group("body"):
    errors.append("benchmark role indication creates a display task")
if "#if TRON_BUILD_ROUTED_FULL_TAVRN && !TRON_BUILD_BENCHMARK_MODE" not in main:
    errors.append("benchmark FULL logger does not suppress recurring snapshots")
if errors:
    raise SystemExit("; ".join(errors))
PY

printf '%s\n' 'tavrn benchmark tests passed'
