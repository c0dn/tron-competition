#!/usr/bin/env bash

set -euo pipefail

MICROBIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
CC_BIN="${CC:-cc}"
MODE=""

cleanup() {
    rm -rf "${BUILD_DIR}"
}
trap cleanup EXIT

if [[ $# -ne 1 || ( "$1" != "--telemetry-red" &&
                      "$1" != "--telemetry-green" &&
                      "$1" != "--scheduler-red" &&
                      "$1" != "--scheduler-green" ) ]]; then
    printf 'usage: %s --telemetry-red|--telemetry-green|--scheduler-red|--scheduler-green\n' "$0" >&2
    exit 2
fi

COMMON_FLAGS=(
    -std=c99 -Wall -Wextra -Werror -DBLE_RADIO_HOST_TEST
    -I"${MICROBIT_ROOT}/app/tavrn_routed_node/src"
    -I"${MICROBIT_ROOT}/app/drivers"
    -I"${MICROBIT_ROOT}/app/protocol"
    -I"${MICROBIT_ROOT}/tests/protocol/support"
)
RUNTIME_TEST="${MICROBIT_ROOT}/tests/protocol/test_tavrn_phase4_runtime.c"
SCHEDULER_TEST="${MICROBIT_ROOT}/tests/protocol/test_tavrn_phase4_scheduler_overrun_red.c"
SCHEDULER_CYCLE_SOURCE="${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_cycle.c"
RREQ_TEST="${MICROBIT_ROOT}/tests/protocol/test_tavrn_phase4_rreq_integration.c"
ISOLATION_TEST="${MICROBIT_ROOT}/tests/protocol/test_tavrn_phase4_cycle_isolation.c"
CYCLE_HEADER="${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_cycle.h"
FULL_HEADER="${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_full_telemetry.h"
MAIN_SOURCE="${MICROBIT_ROOT}/app/tavrn_routed_node/src/main.c"
RED_BINDING_SOURCE="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase4_binding_red_fixture.c"
AODV_PRODUCTION_SOURCE="${MICROBIT_ROOT}/app/protocol/aodv_core.c"
ROUTER_PRODUCTION_SOURCE="${MICROBIT_ROOT}/app/protocol/tavrn_router.c"
GTT_SOURCE="${MICROBIT_ROOT}/app/protocol/tavrn_gtt.c"
RREQ_COMMON_SOURCES=(
    "${MICROBIT_ROOT}/app/protocol/tavrn_link_v2.c"
    "${MICROBIT_ROOT}/app/protocol/tavrn_wire_v2.c"
    "${MICROBIT_ROOT}/app/protocol/ble_mesh_tx_queue.c"
    "${MICROBIT_ROOT}/tests/protocol/support/ble_mesh_scheduler_port.c"
    "${MICROBIT_ROOT}/app/protocol/tron_timer_config.c"
)

scheduler_binding_check() {
    local binding_source="$1"

    python3 - "${binding_source}" <<'PY'
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
try:
    source = path.read_text(encoding="utf-8")
except OSError as error:
    print(error, file=sys.stderr)
    sys.exit(2)

def strip_comments_and_strings(text):
    out = []
    index = 0
    state = "code"
    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if state == "code" and char == "/" and next_char == "/":
            out.extend("  ")
            index += 2
            state = "line"
        elif state == "code" and char == "/" and next_char == "*":
            out.extend("  ")
            index += 2
            state = "block"
        elif state == "code" and char in "\"'":
            out.append(" ")
            quote = char
            index += 1
            state = "string:" + quote
        elif state == "code":
            out.append(char)
            index += 1
        elif state == "line":
            out.append("\n" if char == "\n" else " ")
            index += 1
            if char == "\n":
                state = "code"
        elif state == "block":
            if char == "*" and next_char == "/":
                out.extend("  ")
                index += 2
                state = "code"
            else:
                out.append("\n" if char == "\n" else " ")
                index += 1
        elif state.startswith("string:"):
            quote = state[-1]
            out.append("\n" if char == "\n" else " ")
            if char == "\\" and index + 1 < len(text):
                out.append("\n" if text[index + 1] == "\n" else " ")
                index += 2
            else:
                index += 1
                if char == quote:
                    state = "code"
    return "".join(out)

clean = strip_comments_and_strings(source)
errors = []

def function_body(name):
    pattern = re.compile(r"\b" + re.escape(name) + r"\s*\([^;{}]*\)\s*\{")
    match = pattern.search(clean)
    if match is None:
        errors.append(f"missing {name} definition")
        return ""
    start = match.end() - 1
    depth = 0
    for index in range(start, len(clean)):
        if clean[index] == "{":
            depth += 1
        elif clean[index] == "}":
            depth -= 1
            if depth == 0:
                return clean[start + 1:index]
    errors.append(f"{name} body is unterminated")
    return ""

logger_body = function_body("routed_logger_task")
mesh_body = function_body("routed_mesh_task")
healthy_body = function_body("routed_cycle_healthy_yield")
fault_body = function_body("routed_cycle_fault_idle")
wait_body = function_body("routed_wait_for_release")
diagnostic_pending_body = function_body("routed_diagnostic_pending")
diagnostic_pop_body = function_body("routed_diagnostic_pop")
cyclic_body = function_body("routed_release_cyclic")
main_body = function_body("usermain")
scheduler_event_body = function_body("routed_cycle_router_scheduler_event")
scheduler_input_body = function_body("routed_cycle_capture_scheduler_input")
predecode_gate_body = function_body("routed_cycle_mark_predecode_gate")

delay_calls = list(re.finditer(r"\btk_dly_tsk\s*\(", clean))
if len(delay_calls) != 1 or "tk_dly_tsk" not in logger_body:
    errors.append("only routed_logger_task may retain tk_dly_tsk")
for name, body in (("healthy", healthy_body), ("fault", fault_body)):
    if "tk_dly_tsk" in body or not re.search(r"\brouted_wait_for_release\s*\(", body):
        errors.append(f"{name} wait must use routed_wait_for_release")
if not re.search(
        r"\btk_wai_flg\s*\(\s*routed_release_flag_id\s*,\s*"
        r"ROUTED_RELEASE_BIT\s*,\s*TWF_ORW\s*\|\s*TWF_BITCLR\s*,\s*"
        r"&pattern\s*,\s*TMO_FEVR\s*\)", wait_body):
    errors.append("release helper must wait with ORW|BITCLR and TMO_FEVR")
if not re.search(r"\bvolatile\s+uint32_t\s+routed_logger_progress_epoch\s*;", clean):
    errors.append("logger progress epoch must be volatile uint32_t")
for pattern, message in (
    (r"\brouted_diagnostic_pending\s*\(",
     "release helper must check pending diagnostics"),
    (r"\brouted_logger_progress_epoch\b",
     "release helper must observe logger progress"),
    (r"\brouted_cycle_state\s*\.\s*last_scheduler_return_ms\b",
     "release helper must use the scheduler-return epoch"),
    (r"\btron_timer_config\s*\.\s*scheduler_poll_max_ms\b",
     "release helper must use the existing scheduler poll bound"),
):
    if not re.search(pattern, wait_body):
        errors.append(message)
if ("queue_guard_begin" not in diagnostic_pending_body or
        "routed_diagnostic_queue.count" not in diagnostic_pending_body or
        "queue_guard_end" not in diagnostic_pending_body):
    errors.append("diagnostic-pending check must use the production queue guard")
if not re.search(
        r"status\s*=\s*routed_cycle_diagnostic_dequeue\s*\([^;]+;\s*"
        r"if\s*\(\s*status\s*==\s*ROUTED_CYCLE_RESULT_OK\s*\)\s*\{\s*"
        r"routed_logger_progress_epoch\+\+\s*;\s*\}\s*queue_guard_end\s*\(",
        diagnostic_pop_body, re.S):
    errors.append("diagnostic dequeue and progress publication must share the queue guard")
if "routed_logger_progress_epoch++" in logger_body:
    errors.append("logger must not publish diagnostic progress after dispatch is re-enabled")
cyclic_calls = re.findall(r"\b([A-Za-z_]\w*)\s*\(", cyclic_body)
if cyclic_calls != ["tk_set_flg"] or not re.search(
        r"\btk_set_flg\s*\(\s*routed_release_flag_id\s*,\s*"
        r"ROUTED_RELEASE_BIT\s*\)", cyclic_body):
    errors.append("cyclic handler may only set the release bit")
if not re.search(r"\.cycatr\s*=\s*TA_HLNG\s*\|\s*TA_PHS", clean):
    errors.append("release cyclic must preserve phase")
if not re.search(r"tron_timer_config\.scheduler_poll_max_ms\s*<=\s*1u", main_body):
    errors.append("release cyclic must reject poll bounds <= 1")
if not re.search(r"release_period_ms\s*=\s*"
                 r"tron_timer_config\.scheduler_poll_max_ms\s*-\s*1u", main_body):
    errors.append("release period must derive from scheduler_poll_max_ms - 1")
if not re.search(r"release_cyclic\.cyctim\s*=\s*release_period_ms\s*;", main_body):
    errors.append("derived release period must configure cyclic interval")
if not re.search(r"\.cycphs\s*=\s*0u", main_body):
    errors.append("release cyclic must retain its zero phase")

for pattern, message in (
    (r"\brouted_cycle_operations\.healthy_yield\s*=\s*"
     r"routed_cycle_healthy_yield\s*;", "healthy-yield binding is not exact"),
    (r"\brouted_cycle_operations\.fault_idle\s*=\s*"
     r"routed_cycle_fault_idle\s*;", "fault-idle binding is not exact"),
    (r"\.cychdr\s*=\s*\(FP\)\s*routed_release_cyclic\s*,",
     "release cyclic handler binding is not exact"),
):
    if len(re.findall(pattern, clean)) != 1:
        errors.append(message)
if (not re.search(r"\bT_CFLG\s+release_flag\s*=", main_body) or
        len(re.findall(r"\brouted_release_flag_id\s*=\s*"
                       r"tk_cre_flg\s*\(\s*&release_flag\s*\)", main_body)) != 1):
    errors.append("release flag object must be the checked created object")
if (not re.search(r"\bT_CCYC\s+release_cyclic\s*=", main_body) or
        len(re.findall(r"\brelease_cyclic_id\s*=\s*"
                       r"tk_cre_cyc\s*\(\s*&release_cyclic\s*\)", main_body)) != 1 or
        len(re.findall(r"\btk_sta_cyc\s*\(\s*release_cyclic_id\s*\)",
                       main_body)) != 1):
    errors.append("release cyclic object must be the checked created and started object")
release_cyclic_object = re.search(
    r"\bT_CCYC\s+release_cyclic\s*=\s*\{(?P<body>.*?)\};", main_body, re.S)
if (release_cyclic_object is None or not re.search(
        r"\.cychdr\s*=\s*\(FP\)\s*routed_release_cyclic\s*,",
        release_cyclic_object.group("body"))):
    errors.append("checked release cyclic object must bind routed_release_cyclic")

creation_checks = (
    (r"routed_release_flag_id\s*=\s*tk_cre_flg\s*\(\s*&release_flag\s*\)\s*;"
     r"\s*if\s*\(\s*routed_release_flag_id\s*<=\s*0\s*\)",
     "release flag creation is unchecked"),
    (r"release_cyclic_id\s*=\s*tk_cre_cyc\s*\(\s*&release_cyclic\s*\)\s*;"
     r"\s*if\s*\(\s*release_cyclic_id\s*<=\s*0\s*\)",
     "release cyclic creation is unchecked"),
    (r"mesh_id\s*=\s*tk_cre_tsk\s*\(\s*&mesh_task\s*\)\s*;"
     r"\s*if\s*\(\s*mesh_id\s*<=\s*0\s*\)",
     "mesh task creation is unchecked"),
    (r"logger_id\s*=\s*tk_cre_tsk\s*\(\s*&logger_task\s*\)\s*;"
     r"\s*if\s*\(\s*logger_id\s*<=\s*0\s*\)",
     "logger task creation is unchecked"),
)
for pattern, message in creation_checks:
    if not re.search(pattern, main_body):
        errors.append(message)

start_checks = (
    r"if\s*\(\s*tk_sta_tsk\s*\(\s*logger_id\s*,\s*0\s*\)\s*!=\s*E_OK\s*\)",
    r"if\s*\(\s*tk_sta_cyc\s*\(\s*release_cyclic_id\s*\)\s*!=\s*E_OK\s*\)",
    r"if\s*\(\s*tk_sta_tsk\s*\(\s*mesh_id\s*,\s*0\s*\)\s*!=\s*E_OK\s*\)",
)
start_positions = []
for pattern in start_checks:
    match = re.search(pattern, main_body)
    if match is None:
        errors.append("logger, cyclic, and mesh starts must each check E_OK")
        break
    start_positions.append(match.start())
if len(start_positions) == len(start_checks) and start_positions != sorted(start_positions):
    errors.append("startup order must be logger, cyclic, then mesh")

input_fields = (
    r"input_event_type\s*=\s*event->type\s*;",
    r"input_fault\s*=\s*event->fault\s*;",
    r"input_channel\s*=\s*event->channel\s*;",
    r"input_rssi_magnitude_db\s*=\s*event->rssi_magnitude_db\s*;",
    r"input_adv_len\s*=\s*event->adv_len\s*;",
    r"input_present\s*=\s*TAVRN_ROUTER_TRACE_PRESENT\s*;",
)
if any(not re.search(pattern, scheduler_input_body) for pattern in input_fields) or \
        "input_advertiser.bytes" not in scheduler_input_body:
    errors.append("gated RX input capture must preserve the real scheduler event")
capture_call = re.search(
    r"\brouted_cycle_capture_scheduler_input\s*\(\s*&trace\s*,\s*event\s*\)",
    scheduler_event_body)
if capture_call is None:
    errors.append("scheduler event must capture raw input before policy gating")
else:
    gate_positions = [match.start() for match in re.finditer(
        r"\brouted_cycle_mark_predecode_gate\s*\(", scheduler_event_body)]
    if not gate_positions or capture_call.start() > min(gate_positions):
        errors.append("gated RX input capture must precede every policy gate")
predecode_fields = (
    r"wire_decode_present\s*=\s*TAVRN_ROUTER_TRACE_NOT_PRESENT\s*;",
    r"decoded_frame_present\s*=\s*TAVRN_ROUTER_TRACE_NOT_PRESENT\s*;",
    r"link_step_present\s*=\s*TAVRN_ROUTER_TRACE_NOT_PRESENT\s*;",
    r"link_event_present\s*=\s*TAVRN_ROUTER_TRACE_NOT_PRESENT\s*;",
    r"rx_control_present\s*=\s*TAVRN_ROUTER_TRACE_NOT_PRESENT\s*;",
)
if any(not re.search(pattern, predecode_gate_body) for pattern in predecode_fields):
    errors.append("predecode gate must explicitly leave decode and link absent")
if not re.search(
        r"mentorship_status\s*==\s*TAVRN_MENTORSHIP_GATED\s*\)\s*\{\s*"
        r"routed_cycle_mark_predecode_gate\s*\(\s*&trace\s*,\s*"
        r"TAVRN_ROUTER_EVENT_REJOINING\s*\)", scheduler_event_body, re.S):
    errors.append("mentorship gate must report REJOINING rather than zero/OK")

mesh_calls = [name for name in re.findall(r"\b([A-Za-z_]\w*)\s*\(", mesh_body)
              if name not in {"if", "switch", "sizeof"}]
if re.search(r"\b(?:for|while|do)\b", mesh_body) or mesh_calls != ["routed_cycle_run_task"]:
    errors.append("routed_mesh_task must remain the one-call routed_cycle_run_task adapter")

if errors:
    print("; ".join(errors))
    sys.exit(1)
PY
}

if [[ "$1" == "--scheduler-red" || "$1" == "--scheduler-green" ]]; then
    SCHEDULER_MODE="${1#--scheduler-}"
    SCHEDULER_BINARY="${BUILD_DIR}/test_tavrn_phase4_scheduler_overrun_red"
    SCHEDULER_LOG="${BUILD_DIR}/scheduler-${SCHEDULER_MODE}.log"
    SCHEDULER_EXPECTED='FAIL SCHED-YIELD-01: scheduler_return_ms=1574 requested_ms=1 modeled_completion_ms=1577 gap_ms=3 poll_bound_ms=2 detected_phase=HEALTHY_YIELD detected_source=SCHEDULER_RETURN_GAP'
    SCHEDULER_SOURCES=("${SCHEDULER_TEST}" "${SCHEDULER_CYCLE_SOURCE}")

    for source in "${SCHEDULER_SOURCES[@]}"; do
        if [[ ! -f "${source}" ]]; then
            printf 'Phase 4 scheduler %s setup unavailable: expected source %s\n' \
                "${SCHEDULER_MODE}" \
                "${source#"${MICROBIT_ROOT}/"}" >&2
            exit 2
        fi
        if [[ "${source}" == */red_support/* ]]; then
            printf 'Phase 4 scheduler %s rejects RED backend source %s\n' \
                "${SCHEDULER_MODE}" "${source#"${MICROBIT_ROOT}/"}" >&2
            exit 2
        fi
    done
    if ! "${CC_BIN}" "${COMMON_FLAGS[@]}" "${SCHEDULER_SOURCES[@]}" \
            -o "${SCHEDULER_BINARY}"; then
        printf 'Phase 4 scheduler %s compile failed\n' "${SCHEDULER_MODE}" >&2
        exit 2
    fi
    set +e
    "${SCHEDULER_BINARY}" | tee "${SCHEDULER_LOG}"
    scheduler_status=${PIPESTATUS[0]}
    set -e
    scheduler_failure_count="$(grep -c '^FAIL ' "${SCHEDULER_LOG}" || true)"
    scheduler_exact_count="$(grep -Fxc "${SCHEDULER_EXPECTED}" \
        "${SCHEDULER_LOG}" || true)"
    if [[ "${SCHEDULER_MODE}" == "red" ]]; then
        if [[ ${scheduler_status} -ne 1 || "${scheduler_failure_count}" != "1" ||
              "${scheduler_exact_count}" != "1" ]]; then
            printf 'Phase 4 scheduler RED requires one exact assertion failure; got exit=%d fail=%s exact=%s\n' \
                "${scheduler_status}" "${scheduler_failure_count}" \
                "${scheduler_exact_count}" >&2
            exit 2
        fi
        exit 1
    fi
    set +e
    scheduler_binding_message="$(scheduler_binding_check "${MAIN_SOURCE}" 2>&1)"
    scheduler_binding_status=$?
    set -e
    if [[ ${scheduler_binding_status} -ne 0 ]]; then
        printf 'Phase 4 scheduler GREEN binding check failed: %s\n' \
            "${scheduler_binding_message}" >&2
        exit 2
    fi
    if [[ ${scheduler_status} -eq 0 && "${scheduler_failure_count}" == "0" ]]; then
        exit 0
    fi
    if [[ ${scheduler_status} -eq 1 && "${scheduler_failure_count}" == "1" &&
          "${scheduler_exact_count}" == "1" ]]; then
        exit 1
    fi
    printf 'Phase 4 scheduler GREEN malformed result: exit=%d fail=%s exact=%s\n' \
        "${scheduler_status}" "${scheduler_failure_count}" "${scheduler_exact_count}" >&2
    exit 2
fi

MODE="${1#--telemetry-}"

binding_check() {
    local binding_source="$1"

    python3 - "${binding_source}" <<'PY'
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
try:
    source = path.read_text(encoding="utf-8")
except OSError as error:
    print(error, file=sys.stderr)
    sys.exit(2)

def strip_comments_and_strings(text):
    out = []
    index = 0
    state = "code"
    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if state == "code" and char == "/" and next_char == "/":
            out.extend("  ")
            index += 2
            state = "line"
        elif state == "code" and char == "/" and next_char == "*":
            out.extend("  ")
            index += 2
            state = "block"
        elif state == "code" and char in "\"'":
            out.append(" ")
            quote = char
            index += 1
            state = "string:" + quote
        elif state == "code":
            out.append(char)
            index += 1
        elif state == "line":
            out.append("\n" if char == "\n" else " ")
            index += 1
            if char == "\n": state = "code"
        elif state == "block":
            if char == "*" and next_char == "/":
                out.extend("  ")
                index += 2
                state = "code"
            else:
                out.append("\n" if char == "\n" else " ")
                index += 1
        elif state.startswith("string:"):
            quote = state[-1]
            out.append("\n" if char == "\n" else " ")
            if char == "\\" and index + 1 < len(text):
                out.append("\n" if text[index + 1] == "\n" else " ")
                index += 2
            else:
                index += 1
                if char == quote: state = "code"
    return "".join(out)

clean = strip_comments_and_strings(source)
definition = re.compile(
    r"\bLOCAL\s+void\s+routed_mesh_task\s*\(\s*INT\s+stacd\s*,\s*"
    r"void\s*\*\s*exinf\s*\)\s*\{"
)
matches = list(definition.finditer(clean))
errors = []
if '#include "routed_cycle.h"' not in source:
    errors.append("binding source does not include routed_cycle.h")
if len(re.findall(
        r"\bmentorship_config\.sync_dedupe_ms\s*=\s*"
        r"tron_timer_config\.mentor_sync_dedupe_ms\s*;", clean)) != 1:
    errors.append("mentorship SYNC dedupe timer binding is not exact")
if len(matches) != 1:
    errors.append("expected exactly one routed_mesh_task definition")
else:
    start = matches[0].end() - 1
    depth = 0
    end = None
    for index in range(start, len(clean)):
        if clean[index] == "{": depth += 1
        elif clean[index] == "}":
            depth -= 1
            if depth == 0:
                end = index
                break
    if end is None:
        errors.append("routed_mesh_task body is unterminated")
    else:
        body = clean[start + 1:end]
        if re.search(r"\b(?:for|while|do)\b", body):
            errors.append("routed_mesh_task contains a loop")
        calls = re.findall(r"\b([A-Za-z_]\w*)\s*\(", body)
        calls = [name for name in calls if name not in {"if", "switch", "sizeof"}]
        if calls != ["routed_cycle_run_task"]:
            errors.append("routed_mesh_task must contain exactly one routed_cycle_run_task call")
if errors:
    print("; ".join(errors))
    sys.exit(1)
PY
}

if [[ "${MODE}" == "red" ]]; then
    BINDING_SOURCE="${RED_BINDING_SOURCE}"
    CYCLE_SOURCE="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase4_cycle_red_backend.c"
    TELEMETRY_SOURCE="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase4_telemetry_red_backend.c"
    AODV_SOURCE="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase4_aodv_red_production.c"
    ROUTER_SOURCE="${MICROBIT_ROOT}/tests/protocol/red_support/tavrn_phase4_router_red_production.c"
    RUNTIME_SOURCES=("${CYCLE_SOURCE}" "${TELEMETRY_SOURCE}" "${AODV_SOURCE}" "${GTT_SOURCE}")
    ISOLATION_SOURCES=("${CYCLE_SOURCE}" "${AODV_SOURCE}")
else
    BINDING_SOURCE="${MAIN_SOURCE}"
    CYCLE_SOURCE="${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_cycle.c"
    TELEMETRY_SOURCE="${MICROBIT_ROOT}/app/tavrn_routed_node/src/routed_full_telemetry.c"
    AODV_SOURCE="${AODV_PRODUCTION_SOURCE}"
    ROUTER_SOURCE="${ROUTER_PRODUCTION_SOURCE}"
    RUNTIME_SOURCES=("${CYCLE_SOURCE}" "${TELEMETRY_SOURCE}" "${AODV_SOURCE}" "${GTT_SOURCE}")
    ISOLATION_SOURCES=("${CYCLE_SOURCE}" "${AODV_SOURCE}")
fi
RREQ_SOURCES=("${AODV_SOURCE}" "${ROUTER_SOURCE}" "${RREQ_COMMON_SOURCES[@]}" \
              "${CYCLE_SOURCE}")
if [[ "${MODE}" == "red" ]]; then
    RREQ_SOURCES+=("${TELEMETRY_SOURCE}")
fi

for source in "${CYCLE_HEADER}" "${FULL_HEADER}" "${BINDING_SOURCE}" "${RUNTIME_TEST}" \
              "${RREQ_TEST}" "${ISOLATION_TEST}" "${RUNTIME_SOURCES[@]}" \
              "${ISOLATION_SOURCES[@]}" "${RREQ_SOURCES[@]}"; do
    if [[ ! -f "${source}" ]]; then
        printf 'Phase 4 telemetry %s setup unavailable: expected source %s\n' "${MODE}" \
            "${source#"${MICROBIT_ROOT}/"}" >&2
        exit 2
    fi
done

# Deliberately no FULL/GTT source, header, or telemetry backend appears here.
if ! "${CC_BIN}" "${COMMON_FLAGS[@]}" "${ISOLATION_TEST}" \
        "${ISOLATION_SOURCES[@]}" -o "${BUILD_DIR}/test_tavrn_phase4_cycle_isolation"; then
    printf 'Phase 4 telemetry %s cycle-only isolation compile failed\n' "${MODE}" >&2
    exit 2
fi
if ! "${BUILD_DIR}/test_tavrn_phase4_cycle_isolation"; then
    printf 'Phase 4 telemetry %s cycle-only isolation run failed\n' "${MODE}" >&2
    exit 2
fi

if ! "${CC_BIN}" "${COMMON_FLAGS[@]}" "${RUNTIME_TEST}" "${RUNTIME_SOURCES[@]}" \
        -o "${BUILD_DIR}/test_tavrn_phase4_runtime"; then
    printf 'Phase 4 telemetry %s runtime compile failed\n' "${MODE}" >&2
    exit 2
fi
if ! "${CC_BIN}" "${COMMON_FLAGS[@]}" -DTAVRN_PHASE4_RREQ_INTEGRATION \
        "${RREQ_TEST}" "${RREQ_SOURCES[@]}" \
        -o "${BUILD_DIR}/test_tavrn_phase4_rreq"; then
    printf 'Phase 4 telemetry %s RREQ integration compile failed\n' "${MODE}" >&2
    exit 2
fi

set +e
binding_message="$(binding_check "${BINDING_SOURCE}" 2>&1)"
binding_status=$?
set -e
if [[ ${binding_status} -gt 1 ]]; then
    printf 'Phase 4 telemetry %s binding setup failed: %s\n' "${MODE}" \
        "${binding_message}" >&2
    exit 2
fi

if [[ "${MODE}" == "green" ]]; then
    set +e
    "${BUILD_DIR}/test_tavrn_phase4_runtime"
    runtime_status=$?
    "${BUILD_DIR}/test_tavrn_phase4_rreq"
    rreq_status=$?
    set -e
    if [[ ${runtime_status} -gt 1 || ${rreq_status} -gt 1 ]]; then
        printf 'Phase 4 telemetry GREEN test crash/runtime failure\n' >&2
        exit 2
    fi
    if [[ ${binding_status} -ne 0 || ${runtime_status} -ne 0 || ${rreq_status} -ne 0 ]]; then
        [[ ${binding_status} -eq 0 ]] || printf 'FAIL BIND-01: %s\n' "${binding_message}"
        exit 1
    fi
    exit 0
fi

set +e
"${BUILD_DIR}/test_tavrn_phase4_runtime" | tee "${BUILD_DIR}/runtime-red.log"
runtime_status=${PIPESTATUS[0]}
"${BUILD_DIR}/test_tavrn_phase4_rreq" | tee "${BUILD_DIR}/rreq-red.log"
rreq_status=${PIPESTATUS[0]}
set -e
if [[ ${runtime_status} -ne 1 || ${rreq_status} -ne 1 || ${binding_status} -ne 1 ]]; then
    printf 'Phase 4 telemetry RED requires runtime, RREQ, and binding assertion exit 1; got %d/%d/%d\n' \
        "${runtime_status}" "${rreq_status}" "${binding_status}" >&2
    exit 2
fi
printf 'FAIL BIND-01: %s\n' "${binding_message}"
for requirement in BIND-01 BEARER-03 MAINT-03 BUILD-01 GTT-02 GTT-05 \
                   GTT-06 AODV-02 AODV-04; do
    if [[ "${requirement}" == "BIND-01" ]]; then continue; fi
    if ! grep -Fq "FAIL ${requirement}:" "${BUILD_DIR}/runtime-red.log" \
        && ! grep -Fq "FAIL ${requirement}:" "${BUILD_DIR}/rreq-red.log"; then
        printf 'Phase 4 telemetry RED missing required assertion tag %s\n' "${requirement}" >&2
        exit 2
    fi
done
if ! grep -Fq 'tavrn_phase4_runtime RED tests failed:' "${BUILD_DIR}/runtime-red.log" \
    || ! grep -Fq 'tavrn_phase4_rreq RED tests failed:' "${BUILD_DIR}/rreq-red.log"; then
    printf '%s\n' 'Phase 4 telemetry RED missing final assertion summary' >&2
    exit 2
fi
printf 'tavrn_phase4_runtime RED tests failed: runtime=1 rreq=1 binding=1 assertion(s)\n'
exit 1
