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
    "${MICROBIT_ROOT}/app/protocol/tavrn_router.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_mentorship.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_maintenance.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_esc.c" \
    "${MICROBIT_ROOT}/app/protocol/aodv_core.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_link_v2.c" \
    "${MICROBIT_ROOT}/app/protocol/tavrn_wire_v2.c" \
    "${MICROBIT_ROOT}/app/protocol/ble_mesh_tx_queue.c" \
    "${MICROBIT_ROOT}/tests/protocol/support/ble_mesh_scheduler_port.c" \
    "${MICROBIT_ROOT}/app/protocol/tron_timer_config.c" \
    -o "${BUILD_DIR}/test_routed_benchmark"
"${BUILD_DIR}/test_routed_benchmark"

"${CC_BIN}" "${COMMON_FLAGS[@]}" \
    "${MICROBIT_ROOT}/tests/protocol/test_routed_benchmark_observer.c" \
    "${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_benchmark_observer.c" \
    -o "${BUILD_DIR}/test_routed_benchmark_observer"
"${BUILD_DIR}/test_routed_benchmark_observer"

"${CC_BIN}" "${COMMON_FLAGS[@]}" -DROUTED_BENCHMARK_UART_TX_HOST_TEST \
    "${MICROBIT_ROOT}/tests/protocol/test_routed_benchmark_uart_tx.c" \
    "${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_benchmark_uart_tx.c" \
    -o "${BUILD_DIR}/test_routed_benchmark_uart_tx"
"${BUILD_DIR}/test_routed_benchmark_uart_tx"

python3 - "${MICROBIT_ROOT}/app/tavrn_routed_node/src/main.c" \
    "${MICROBIT_ROOT}/app/drivers/display.c" \
    "${MICROBIT_ROOT}/app/tavrn_routed_node/CMakeLists.txt" \
    "${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_benchmark_uart_tx.c" \
    "${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_benchmark_uart_tx.h" <<'PY'
import pathlib
import re
import sys

main_path, display_path, cmake_path, uart_path, uart_header_path = map(
    pathlib.Path, sys.argv[1:])
main = main_path.read_text(encoding="utf-8")
display = display_path.read_text(encoding="utf-8")
cmake = cmake_path.read_text(encoding="utf-8")
uart = uart_path.read_text(encoding="utf-8")
uart_contract = uart + uart_header_path.read_text(encoding="utf-8")
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
health = body("log_benchmark_health")
checkpoint = body("routed_benchmark_health_checkpoint")
reserve = body("router_delivery_reserve")
commit = body("router_delivery_commit")
usermain = body("usermain")
wait_release = body("routed_wait_for_release")
for token in ("routed_benchmark_schedule", "routed_benchmark_destination_prepare",
               "routed_benchmark_record_not_ready_status",
               "routed_benchmark_pending_accepted",
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
for token in ("routed_benchmark_record_submission_status",
              "routed_benchmark_pending_accepted_valid",
              "routed_benchmark_pending_workload"):
    if token not in submit:
        errors.append(f"benchmark submit lacks {token}")
if "routed_next_submit_at = trace.completed_at_ms + TRON_BUILD_LINK_TX_INTERVAL_MS;" not in submit:
    errors.append("nonbenchmark completion-relative submission schedule changed")
for token in ("routed_benchmark_final_pop_record",
               "routed_benchmark_accepted_pop_record", "log_benchmark_final",
               "log_benchmark_accepted",
               "log_benchmark_clock", "log_benchmark_control", "log_benchmark_health"):
    if token not in logger:
        errors.append(f"logger lacks {token}")
for token in ("routed_benchmark_uart_tx_has_headroom",
              "routed_benchmark_uart_tx_settled", "start_benchmark_gtt_emission",
              "next_health_at", "next_control_at"):
    if token not in logger:
        errors.append(f"benchmark logger lacks settled UARTE policy {token}")
if not re.search(r"routed_benchmark_uart_tx_has_headroom\s*\(\s*\).*?"
                 r"routed_benchmark_final_pop_record.*?"
                 r"routed_benchmark_uart_tx_has_headroom\s*\(\s*\).*?"
                 r"routed_benchmark_accepted_pop_record", logger, re.S):
    errors.append("benchmark critical evidence is not guarded by UARTE headroom")
if not re.search(r"critical_record\s*==\s*0.*?routed_benchmark_uart_tx_settled", logger, re.S):
    errors.append("benchmark periodic records can run after a critical dequeue")
benchmark_logger_match = re.search(r"#if TRON_BUILD_BENCHMARK_MODE\s*(?P<body>.*?)\s*#else",
                                   logger, re.S)
benchmark_logger = benchmark_logger_match.group("body") if benchmark_logger_match else ""
if any(call in benchmark_logger for call in ("log_cycle_diagnostic(&routed_logger_record.diagnostic)",
                                             "log_retry_exhausted_event(&routed_logger_record.retry_event)",
                                             "log_rreq_lifecycle(&routed_logged_rreq)")):
    errors.append("benchmark logger retains generic diagnostic formatting")
for token in ("obs_gtt_begin", "obs_gtt_entry", "obs_gtt_end"):
    if token not in main:
        errors.append(f"benchmark source lacks {token}")
observer_records = ("obs_boot", "obs_clock", "obs_accept", "obs_control",
                     "obs_health", "obs_gtt_begin", "obs_gtt_entry", "obs_gtt_end",
                     "obs_final")
for record in observer_records:
    if f'{record} schema=observer-v3 now=' not in main:
        errors.append(f"{record} lacks observer-v3 self-identification")
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
    format_match = re.search(r'tm_printf\(\(UB \*\)"(?P<format>(?:[^"\\]|\\.)*)"', call,
                             re.S)
    if format_match is None:
        errors.append(f"{record} has no literal format for max-line check")
    else:
        literal = bytes(format_match.group("format"), "utf-8").decode("unicode_escape")
        conversions = len(re.findall(r'%(?:[-+ #0]*\d*(?:\.\d+)?[hl]?[diuoxXcsp])', literal))
        if len(literal) + conversions * 64 + 2 >= 8192:
            errors.append(f"{record} can exceed the 8 KiB UARTE logger headroom")
for record, timestamp, event_source in (
        ("obs_accept", "offered_at_ms=%lu", "(UW)event->offered_at_ms"),
        ("obs_accept", "accepted_at_ms=%lu", "(UW)event->accepted_at_ms"),
        ("obs_final", "delivered_at_ms=%lu", "(UW)event->delivered_at_ms")):
    event_prints = [item for item in observer_prints
                    if item.group("record") == record]
    if not event_prints:
        errors.append(f"{record} lacks a copied event timestamp")
    for observer_print in event_prints:
        call = observer_print.group(0)
        if timestamp not in call or event_source not in call:
            errors.append(f"{record} lacks its copied event timestamp")
for record in ("obs_accept", "obs_final"):
    for observer_print in (item for item in observer_prints
                           if item.group("record") == record):
        call = observer_print.group(0)
        if any(field in call for field in ("workload=", "burst=", "sequence=")):
            errors.append(f"{record} repeats derived workload identity fields")
if re.search(r'obs_[a-z_]+\s+now_ms=', main):
    errors.append("observer record still uses now_ms instead of now")
for token in ("ROUTED_BENCHMARK_APP_PAYLOAD_BYTES", "routed_benchmark_decode_payload",
              "app_bytes[6]", "origin_session=%lu", "identity_valid=%u",
              "benchmark_session_high",
              "routed_full_telemetry_snapshot_gtt"):
    if token not in main:
        errors.append(f"benchmark payload/attempt contract lacks {token}")
if "routed_full_telemetry_copy_gtt_entry" in main:
    errors.append("benchmark GTT emission still scans live physical slots")
for legacy in ("routed_benchmark_attempt", "ROUTED_BENCHMARK_RECORD_OFFER",
               "ROUTED_BENCHMARK_RECORD_APPLICATION", "obs_offer", "obs_app"):
    if legacy in main:
        errors.append(f"benchmark source retains v2 OFFER/APPLICATION state: {legacy}")
if not re.search(
        r"snapshot_now\s*=\s*now_ms\(\);.*?"
        r"routed_full_telemetry_snapshot_gtt\(\s*&routed_gtt,\s*snapshot_now,",
        main, re.S):
    errors.append("benchmark GTT query time is not taken at the guarded snapshot")
if "configured_rx_block_adva" not in main:
    errors.append("benchmark source lacks the reciprocal direct-RX block seam")
if "tm_printf" in prepare or "tm_printf" in submit:
    errors.append("benchmark mesh path prints instead of copying logger records")
for token in ("routed_benchmark_logger_storage.accepted",
              "routed_benchmark_logger_storage.final",
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
for token in ("routed_benchmark_accepted_fifo_build_capacity_guard",
              "routed_benchmark_final_fifo_build_capacity_guard",
              "routed_benchmark_accepted_fifo_init",
              "routed_benchmark_final_fifo_init",
              "routed_benchmark_final_accounting_init"):
    if token not in main:
        errors.append(f"benchmark observer-v3 capacity/init contract lacks {token}")
if ("#define ROUTED_LOGGER_TASK_STACK_BYTES TRON_BUILD_ROUTED_LOGGER_TASK_STACK_BYTES" not in main or
        ".stksz = ROUTED_LOGGER_TASK_STACK_BYTES" not in usermain or
        ".stksz = 1536" in usermain):
    errors.append("logger task does not consume the generated stack declaration")
for token in ("routed_benchmark_health_checkpoint", "heartbeat_offered",
               "throughput_not_ready", "accepted_fifo_count", "final_fifo_count",
               "final_commits", "application_reserve_busy",
               "application_commit_busy", "accepted_fifo_faults",
               "final_fifo_faults", "uart_pending_bytes",
               "uart_high_water_bytes", "uart_dropped_bytes",
               "uart_dropped_records", "uart_transport_faults"):
    if token not in health:
        errors.append(f"obs_health lacks observer-v3 field {token}")
for token in ("routed_benchmark_state", "routed_benchmark_accepted_fifo_snapshot",
              "routed_benchmark_final_fifo_snapshot",
              "routed_benchmark_final_accounting", "application_reserve_busy",
              "application_commit_busy", "routed_benchmark_next_record_id"):
    if token not in checkpoint:
        errors.append(f"health checkpoint lacks {token}")
if (checkpoint.find("queue_guard_begin") > checkpoint.find("routed_benchmark_next_record_id") or
        checkpoint.find("routed_benchmark_next_record_id") > checkpoint.find("queue_guard_end")):
    errors.append("health checkpoint does not allocate its record ID under one queue guard")
boot_prints = [item for item in observer_prints if item.group("record") == "obs_boot"]
if (len(boot_prints) != 1 or "started_at_ms=%lu" not in boot_prints[0].group(0) or
        "(UW)started_at_ms" not in boot_prints[0].group(0)):
    errors.append("obs_boot lacks benchmark-state started_at_ms")
critical_final = logger.find("routed_benchmark_final_pop_record")
critical_accepted = logger.find("routed_benchmark_accepted_pop_record")
for later in ("routed_diagnostic_pop", "routed_retry_log_pop", "routed_rreq_pop",
              "local_event_pop", "log_benchmark_clock", "log_benchmark_control",
              "log_benchmark_gtt_next"):
    position = logger.find(later)
    if position != -1 and (critical_final == -1 or critical_accepted == -1 or
                           critical_final > position or critical_accepted > position):
        errors.append(f"benchmark critical FIFO drain can be preempted by {later}")
if ("routed_benchmark_final_fifo" in reserve or
        "ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY" in reserve):
    errors.append("benchmark reserve tests final FIFO fullness")
if not re.search(
        r"benchmark_logger_service_epoch\s*=\s*"
        r"routed_benchmark_logger_service_epoch.*?"
        r"for\s*\(\s*;\s*;\s*\).*?"
        r"tk_clr_flg\s*\(\s*routed_release_flag_id\s*,\s*"
        r"~ROUTED_RELEASE_BIT\s*\).*?"
        r"tk_wai_flg.*?"
        r"release_progressed\s*=\s*routed_benchmark_logger_service_epoch\s*!=\s*"
        r"benchmark_logger_service_epoch",
        wait_release, re.S):
    errors.append("benchmark wait does not require fresh logger service after stale-bit clear")
if (not re.search(r"volatile\s+uint32_t\s+routed_benchmark_logger_service_epoch", main) or
        "routed_benchmark_logger_service_epoch++;" not in logger or
        not re.search(r"routed_benchmark_uart_tx_enqueue.*?"
                      r"routed_benchmark_logger_service_epoch\+\+", main, re.S)):
    errors.append("benchmark logger does not publish bounded service progress")
if not re.search(
        r"routed_benchmark_final_fifo_offer\(.*?"
        r"delivery_callback_end\(\);\s*return TAVRN_ROUTER_DELIVERY_OK;",
        commit, re.S):
    errors.append("benchmark commit does not return OK after final FIFO offer/drop")
for critical in ("routed_benchmark_final_pop_record",
                 "routed_benchmark_accepted_pop_record"):
    critical_body = body(critical)
    take_call = ("routed_benchmark_final_fifo_take" if "final" in critical else
                 "routed_benchmark_accepted_fifo_take")
    if ("record_id_exhausted" not in critical_body or
            critical_body.find("record_id_exhausted") >
            critical_body.find(take_call)):
        errors.append(f"{critical} consumes FIFO evidence after record-ID exhaustion")
    if not (critical_body.find("queue_guard_begin") < critical_body.find(take_call) <
            critical_body.find("routed_benchmark_next_record_id") <
            critical_body.rfind("queue_guard_end")):
        errors.append(f"{critical} does not pop and allocate under one queue guard")
if not re.search(
        r"routed_benchmark_destination_prepare\(.*?"
        r"routed_benchmark_record_not_ready_status\(.*?"
        r"routed_benchmark_pending_accepted_valid\s*=\s*0u;.*?"
        r"ROUTED_CYCLE_APPLICATION_DISABLED",
        prepare, re.S):
    errors.append("FULL not-ready decision can reach a submission")
logger_start = usermain.find("tk_sta_tsk(logger_id")
cyclic_start = usermain.find("tk_sta_cyc(release_cyclic_id")
mesh_start = usermain.find("tk_sta_tsk(mesh_id")
if -1 in (logger_start, cyclic_start, mesh_start) or not (
        logger_start < cyclic_start < mesh_start):
    errors.append("startup order is not logger, cyclic, then mesh")
logger_start_failure = re.search(
    r'if \(tk_sta_tsk\(logger_id, 0\) != E_OK\) \{(?P<body>.*?)\n\s*\}',
    usermain, re.S)
if logger_start_failure is None:
    errors.append("missing logger task start failure path")
else:
    logger_failure_body = logger_start_failure.group("body")
    cleanup_steps = ("tm_printf", "return 1;")
    cleanup_positions = [logger_failure_body.find(step) for step in cleanup_steps]
    if (-1 in cleanup_positions or cleanup_positions != sorted(cleanup_positions) or
            "tk_stp_cyc" in logger_failure_body or "tk_ter_tsk" in logger_failure_body):
        errors.append("logger start failure does not preserve the original cleanup path")
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
if not re.search(r"if\(TRON_BENCHMARK_MODE STREQUAL \"ON\" OR "
                 r"TRON_ENABLE_WEARABLE_INGRESS STREQUAL \"ON\"\)\s*"
                 r"target_link_options\(tavrn_routed_node PRIVATE -Wl,--wrap=tm_snd_dat\)",
                 cmake, re.S):
    errors.append("routed CMake does not select tm_snd_dat wrapping for benchmark or ingress")
if not re.search(r"#if TRON_BUILD_BENCHMARK_MODE.*?"
                 r"void __wrap_tm_snd_dat\(const UB \*buffer, INT size\).*?"
                 r"routed_benchmark_uart_tx_ready.*?"
                 r"__real_tm_snd_dat.*?"
                 r"routed_benchmark_uart_tx_enqueue.*?"
                 r"#elif TRON_BUILD_ROUTED_FULL_TAVRN && TRON_BUILD_ENABLE_WEARABLE_INGRESS",
                 main, re.S):
    errors.append("benchmark async wrapper is not separate from wearable ingress wrapping")
for token in ("ROUTED_BENCHMARK_UART_TX_CAPACITY 16384u", "ROUTED_UART_ENABLE_UARTE 8u",
              "ROUTED_UART_INT_ENDTX", "ROUTED_UART_INTERRUPT_PRIORITY 6",
              "routed_benchmark_uart_tx_publish_barrier", "DisableInt(INTNO(UART0_BASE))",
              "EnableInt(INTNO(UART0_BASE), ROUTED_UART_INTERRUPT_PRIORITY)"):
    if token not in uart_contract:
        errors.append(f"benchmark UARTE transport lacks {token}")
if errors:
    raise SystemExit("; ".join(errors))
PY

printf '%s\n' 'tavrn benchmark tests passed'
