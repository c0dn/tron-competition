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
    "${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_full_telemetry.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_full.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_gtt.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_smart_ttl.c" \
    -o "${BUILD_DIR}/test_routed_benchmark"
"${BUILD_DIR}/test_routed_benchmark"

"${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/protocol/test_routed_benchmark_observer.c" \
    "${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_benchmark_observer.c" \
    -o "${BUILD_DIR}/test_routed_benchmark_observer"
"${BUILD_DIR}/test_routed_benchmark_observer"

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
control = body("log_benchmark_control")
usermain = body("usermain")
for token in ("routed_benchmark_schedule", "routed_benchmark_destination_ready",
              "routed_benchmark_record_not_ready_status", "routed_benchmark_offer_attempt",
              "TRON_BUILD_BENCH_ROLE_NUMBER != 1u"):
    if token not in prepare:
        errors.append(f"benchmark prepare lacks {token}")
benchmark_prepare = re.search(
    r"#if TRON_BUILD_BENCHMARK_MODE(?P<body>.*?)#else", prepare, re.S)
if (benchmark_prepare is None or
        "configured_destination.adva.bytes[0]" in benchmark_prepare.group("body") or
        "TRON_BUILD_LINK_TRANSACTION_TARGET" in benchmark_prepare.group("body")):
    errors.append("FULL benchmark blindly derives destination from AdvA[0]")
for token in ("TRON_BUILD_LINK_INITIATOR == 0", "routed_counters.submitted >=",
               "routed_next_submit_at"):
    if token not in prepare:
        errors.append(f"nonbenchmark prepare parity lacks {token}")
for token in ("routed_benchmark_record_submission_status", "routed_benchmark_offer_attempt",
               "routed_benchmark_pending_attempt_valid"):
    if token not in submit:
        errors.append(f"benchmark submit lacks {token}")
if "routed_next_submit_at = trace.completed_at_ms + TRON_BUILD_LINK_TX_INTERVAL_MS;" not in submit:
    errors.append("nonbenchmark completion-relative submission schedule changed")
for token in ("routed_benchmark_attempt_pop", "log_benchmark_attempt",
               "log_benchmark_clock", "log_benchmark_control", "log_benchmark_health",
               "obs_final"):
    if token not in logger:
        errors.append(f"logger lacks {token}")
for token in ("obs_gtt_begin", "obs_gtt_entry", "obs_gtt_end"):
    if token not in main:
        errors.append(f"benchmark source lacks {token}")
observer_records = ("obs_boot", "obs_clock", "obs_offer", "obs_app", "obs_control",
                    "obs_health", "obs_gtt_begin", "obs_gtt_entry", "obs_gtt_end",
                    "obs_final")
for record in observer_records:
    if f'{record} schema=observer-v2 now=' not in main:
        errors.append(f"{record} lacks observer-v2 self-identification")
observer_prints = list(re.finditer(
    r'tm_printf\(\(UB \*\)"(?P<record>obs_[a-z_]+).*?\);', main, re.S))
for record in observer_records:
    if not any(item.group("record") == record for item in observer_prints):
        errors.append(f"{record} is not emitted by tm_printf")
for observer_print in observer_prints:
    record = observer_print.group("record")
    call = observer_print.group(0)
    preceding = main[max(0, observer_print.start() - 100):observer_print.start()]
    if not re.search(r'emission_now\s*=\s*now_ms\(\);\s*$', preceding):
        errors.append(f"{record} does not obtain now immediately before printing")
    if not re.search(r'\\n",\s*\(UW\)emission_now\s*,', call):
        errors.append(f"{record} common now is not the emission timestamp")
for record, event_source in (("obs_offer", "(UW)attempt->event_at_ms"),
                              ("obs_app", "(UW)attempt->event_at_ms"),
                              ("obs_final", "(UW)delivery->delivered_at_ms")):
    event_prints = [item for item in observer_prints
                    if item.group("record") == record]
    if not event_prints:
        errors.append(f"{record} lacks an emitted event_at_ms timestamp")
    for observer_print in event_prints:
        call = observer_print.group(0)
        if "event_at_ms=%lu" not in call or event_source not in call:
            errors.append(f"{record} lacks its copied event_at_ms timestamp")
if re.search(r'obs_[a-z_]+\s+now_ms=', main):
    errors.append("observer record still uses now_ms instead of now")
for token in ("ROUTED_BENCHMARK_APP_PAYLOAD_BYTES", "routed_benchmark_decode_payload",
              "app_bytes[7]", "origin_session=%lu", "attempted = 0u",
              "attempted = 1u", "benchmark_session_high",
              "routed_full_telemetry_snapshot_gtt"):
    if token not in main:
        errors.append(f"benchmark payload/attempt contract lacks {token}")
if "routed_full_telemetry_copy_gtt_entry" in main:
    errors.append("benchmark GTT emission still scans live physical slots")
if "configured_rx_block_adva" not in main:
    errors.append("benchmark source lacks the reciprocal direct-RX block seam")
if "tm_printf" in prepare or "tm_printf" in submit:
    errors.append("benchmark mesh path prints instead of copying logger records")
for token in ("routed_benchmark_logger_storage.delivery",
              "routed_benchmark_logger_storage.local_event"):
    if token not in logger:
        errors.append(f"benchmark logger scratch is not static: {token}")
for local in ("routed_benchmark_observer_t observer;", "tavrn_link_counters_t link;",
              "aodv_counters_t aodv;", "tavrn_router_counters_t router;"):
    if local in control:
        errors.append(f"large control snapshot returned to logger stack: {local}")
if ("routed_benchmark_logger_storage_t" not in main or
        "static routed_benchmark_logger_storage_t routed_benchmark_logger_storage;" not in main):
    errors.append("benchmark-only static logger storage is absent")
if ("#define ROUTED_LOGGER_TASK_STACK_BYTES TRON_BUILD_ROUTED_LOGGER_TASK_STACK_BYTES" not in main or
        ".stksz = ROUTED_LOGGER_TASK_STACK_BYTES" not in usermain or
        ".stksz = 1536" in usermain):
    errors.append("logger task does not consume the generated stack declaration")
boot_prints = [item for item in observer_prints if item.group("record") == "obs_boot"]
if (len(boot_prints) != 1 or "started_at_ms=%lu" not in boot_prints[0].group(0) or
        "(UW)started_at_ms" not in boot_prints[0].group(0)):
    errors.append("obs_boot lacks benchmark-state started_at_ms")
if logger.find("delivery_pop") > logger.find("routed_benchmark_attempt_pop"):
    errors.append("benchmark attempt logging can preempt final-delivery draining")
if (prepare.find("routed_benchmark_destination_ready") >
        prepare.find("routed_benchmark_pending_attempt_valid = 1u")):
    errors.append("FULL not-ready decision can reach a submission")
if (usermain.find("tk_sta_cyc(release_cyclic_id") >
        usermain.find("tk_sta_tsk(mesh_id") or
        usermain.find("tk_sta_tsk(mesh_id") >
        usermain.find("tk_sta_tsk(logger_id")):
    errors.append("logger can start before cyclic and mesh routing startup")
logger_start_failure = re.search(
    r'if \(tk_sta_tsk\(logger_id, 0\) != E_OK\) \{(?P<body>.*?)\n\s*\}',
    usermain, re.S)
if logger_start_failure is None:
    errors.append("missing logger task start failure path")
else:
    logger_failure_body = logger_start_failure.group("body")
    cleanup_steps = ("tm_printf", "(void)tk_stp_cyc(release_cyclic_id);",
                     "(void)tk_ter_tsk(mesh_id);", "return 1;")
    cleanup_positions = [logger_failure_body.find(step) for step in cleanup_steps]
    if -1 in cleanup_positions or cleanup_positions != sorted(cleanup_positions):
        errors.append("logger start failure does not stop cyclic and terminate mesh")
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
display_benchmark = re.search(
    r"void\s+display_show_benchmark_role\s*\([^)]*\)\s*\{(?P<body>.*?)\n\}",
    display, re.S)
if (display_benchmark is None or "tk_cre_tsk" in display_benchmark.group("body") or
        "all_off();" not in display_benchmark.group("body") or
        "gpio_low(row_pin" in display_benchmark.group("body")):
    errors.append("benchmark role indication does not leave the matrix off")
if "#if TRON_BUILD_ROUTED_FULL_TAVRN && !TRON_BUILD_BENCHMARK_MODE" not in main:
    errors.append("benchmark FULL logger does not suppress recurring snapshots")
if errors:
    raise SystemExit("; ".join(errors))
PY

printf '%s\n' 'tavrn benchmark tests passed'
