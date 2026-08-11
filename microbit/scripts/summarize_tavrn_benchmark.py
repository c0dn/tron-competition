#!/usr/bin/env python3
"""Fail-closed analysis for one six-board TAVRN benchmark capture.

The input metadata is emitted by ``run_tavrn_benchmark.py``.  A benchmark
plan must identify A/C's logical origin/destination because a counter alone is
not an identity.  Serial logs are supplied explicitly as ``ROLE=PATH`` pairs,
which deliberately prevents an accidental probe-order based association.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import re
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


SCHEMA = "tron.tavrn.benchmark.capture.v1"
RUN_SCHEMA = "tron.tavrn.benchmark.run.v1"
ROLES = ("A", "B", "C", "D", "E", "F")
DEFAULT_MIN_CLOCK_SPAN_SECONDS = 350
TIMESTAMP_RE = re.compile(
    r"^\[?(?P<value>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{6}Z)"
    r"(?:\s+[^\]]+)?\]?\s+(?P<payload>routed\s+bench_[^\r\n]*)$"
)
FIELD_RE = re.compile(r"[a-z_]+=[^\s=]+")
UINT_RE = re.compile(r"(?:0|[1-9][0-9]*)")
ROLE_RE = re.compile(r"(?:[A-F]|[1-6])")
WORD_RE = re.compile(r"[A-Za-z0-9_.:-]+")

REQUIRED_FIELDS: dict[str, set[str]] = {
    "clock": {"now", "role"},
    "attempt": {"now", "role", "slot", "counter", "accepted", "status", "destination", "width"},
    "final": {"now", "role", "origin", "destination", "counter_valid", "counter", "app_len", "peer"},
    "summary": {"now", "role", "offered", "attempted", "accepted", "rejected", "skipped", "not_ready", "attempt_q", "attempt_q_dropped", "attempt_q_high_water", "final_q"},
}


class CaptureError(Exception):
    """The capture does not provide enough unambiguous evidence."""


@dataclass(frozen=True)
class BenchRecord:
    kind: str
    host_ms: float
    fields: dict[str, str]
    source: Path
    line_number: int


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _error(path: Path, line_number: int, message: str) -> CaptureError:
    return CaptureError(f"{path}:{line_number} {message}")


def _parse_timestamp(value: str, path: Path, line_number: int) -> float:
    try:
        parsed = dt.datetime.strptime(value, "%Y-%m-%dT%H:%M:%S.%fZ")
    except ValueError as error:
        raise _error(path, line_number, "has a malformed absolute timestamp") from error
    return parsed.replace(tzinfo=dt.timezone.utc).timestamp() * 1000.0


def _parse_uint(value: str, key: str, path: Path, line_number: int) -> int:
    if not UINT_RE.fullmatch(value):
        raise _error(path, line_number, f"has non-decimal {key}")
    number = int(value, 10)
    if number > 0xFFFFFFFF:
        raise _error(path, line_number, f"has out-of-range {key}")
    return number


def parse_bench_line(line: str, path: Path, line_number: int) -> BenchRecord | None:
    """Parse one timestamped bench record, rejecting lookalikes and extras."""
    if "routed bench_" not in line:
        return None
    match = TIMESTAMP_RE.fullmatch(line)
    if match is None:
        raise _error(path, line_number, "has malformed timestamped benchmark telemetry")
    payload = match.group("payload")
    payload_parts = payload.split(" ", 2)
    if len(payload_parts) != 3 or payload_parts[0] != "routed" or \
            not payload_parts[1].startswith("bench_"):
        raise _error(path, line_number, "has malformed benchmark telemetry")
    kind = payload_parts[1][len("bench_"):]
    rendered_fields = payload_parts[2]
    if kind not in REQUIRED_FIELDS:
        raise _error(path, line_number, f"has unknown benchmark record {kind}")
    fields: dict[str, str] = {}
    split_fields = rendered_fields.split()
    if not split_fields or any(not FIELD_RE.fullmatch(field) for field in split_fields):
        raise _error(path, line_number, "has malformed benchmark field")
    for field in split_fields:
        key, value = field.split("=", 1)
        if key in fields:
            raise _error(path, line_number, f"has duplicate benchmark field {key}")
        fields[key] = value
    if set(fields) != REQUIRED_FIELDS[kind]:
        missing = ",".join(sorted(REQUIRED_FIELDS[kind] - set(fields))) or "-"
        extra = ",".join(sorted(set(fields) - REQUIRED_FIELDS[kind])) or "-"
        raise _error(path, line_number, f"benchmark fields mismatch missing={missing} extra={extra}")
    if not ROLE_RE.fullmatch(fields["role"]):
        raise _error(path, line_number, "has invalid benchmark role")
    for key in REQUIRED_FIELDS[kind] - {"role", "status", "destination", "width", "origin", "peer"}:
        _parse_uint(fields[key], key, path, line_number)
    for key in {"status", "destination", "width", "origin", "peer"} & REQUIRED_FIELDS[kind]:
        if not WORD_RE.fullmatch(fields[key]):
            raise _error(path, line_number, f"has invalid {key}")
    if kind == "attempt" and _parse_uint(fields["accepted"], "accepted", path, line_number) not in (0, 1):
        raise _error(path, line_number, "has accepted outside 0..1")
    if kind == "final" and _parse_uint(fields["counter_valid"], "counter_valid", path, line_number) not in (0, 1):
        raise _error(path, line_number, "has counter_valid outside 0..1")
    return BenchRecord(kind, _parse_timestamp(match.group("value"), path, line_number), fields,
                       path, line_number)


def parse_log(path: Path, expected_role: str) -> list[BenchRecord]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as error:
        raise CaptureError(f"cannot read serial log {path}: {error}") from error
    records: list[BenchRecord] = []
    previous_host: float | None = None
    previous_now: int | None = None
    summary_seen = False
    for line_number, line in enumerate(lines, 1):
        record = parse_bench_line(line, path, line_number)
        if record is None:
            continue
        if summary_seen:
            raise _error(path, line_number, "has telemetry after terminal bench_summary")
        expected_role_values = {expected_role, str(ROLES.index(expected_role) + 1)}
        if record.fields["role"] not in expected_role_values:
            raise _error(path, line_number, f"role mismatch expected={expected_role}")
        # Firmware records the fixed role number (1=A through 6=F); normalise
        # it at the capture boundary so every downstream join is role-stable.
        record.fields["role"] = expected_role
        now = _parse_uint(record.fields["now"], "now", path, line_number)
        if previous_host is not None and record.host_ms < previous_host:
            raise _error(path, line_number, "has host-time reversal")
        if previous_now is not None and now < previous_now:
            raise _error(path, line_number, "has firmware rollback/reboot")
        previous_host = record.host_ms
        previous_now = now
        records.append(record)
        summary_seen = record.kind == "summary"
    if not records:
        raise CaptureError(f"serial log {path} contains no benchmark telemetry")
    return records


def _required_string(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value:
        raise CaptureError(f"run metadata lacks unambiguous {name}")
    return value


def _required_positive_int(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise CaptureError(f"run metadata has invalid {name}")
    return value


def load_metadata(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CaptureError(f"cannot read run metadata {path}: {error}") from error
    if not isinstance(data, dict) or data.get("schema") != RUN_SCHEMA:
        raise CaptureError("run metadata schema is missing or unsupported")
    if data.get("status") != "completed":
        raise CaptureError("run metadata does not prove a completed capture")
    boards = data.get("boards")
    if not isinstance(boards, list) or len(boards) != len(ROLES):
        raise CaptureError("run metadata must contain exactly six boards")
    seen_roles: set[str] = set()
    seen_uids: set[str] = set()
    seen_advas: set[str] = set()
    for board in boards:
        if not isinstance(board, dict):
            raise CaptureError("run metadata has malformed board")
        role = _required_string(board.get("role"), "board role")
        uid = _required_string(board.get("uid"), "board uid")
        adva = _required_string(board.get("adva"), "board AdvA")
        if role not in ROLES or role in seen_roles or uid in seen_uids or adva in seen_advas:
            raise CaptureError("run metadata has ambiguous board identities")
        seen_roles.add(role)
        seen_uids.add(uid)
        seen_advas.add(adva)
    if seen_roles != set(ROLES):
        raise CaptureError("run metadata does not map roles A through F")
    benchmark = data.get("benchmark")
    if not isinstance(benchmark, dict):
        raise CaptureError("run metadata lacks benchmark settings")
    for key in ("source_role", "destination_role", "origin", "destination", "width"):
        _required_string(benchmark.get(key), f"benchmark.{key}")
    if benchmark["source_role"] != "A" or benchmark["destination_role"] != "C":
        raise CaptureError("benchmark metadata must identify A as source and C as destination")
    for key in ("offered_slots", "offered_seconds", "drain_seconds"):
        _required_positive_int(benchmark.get(key), f"benchmark.{key}")
    clock_span = benchmark.get("clock_min_span_seconds", DEFAULT_MIN_CLOCK_SPAN_SECONDS)
    _required_positive_int(clock_span, "benchmark.clock_min_span_seconds")
    return data


def _median(values: Iterable[float]) -> float:
    values_list = sorted(values)
    if not values_list:
        raise CaptureError("cannot calculate a median of no values")
    return float(statistics.median(values_list))


def nearest_rank(values: Iterable[float], percentile: int) -> float:
    """Return the standard nearest-rank percentile (percentile is 1..100)."""
    ordered = sorted(values)
    if not ordered or not 1 <= percentile <= 100:
        raise CaptureError("nearest-rank percentile has invalid inputs")
    return ordered[math.ceil(percentile * len(ordered) / 100) - 1]


def _linear_fit(points: list[tuple[float, float]]) -> tuple[float, float, list[float]]:
    if len(points) < 2:
        raise CaptureError("clock has fewer than two points")
    mean_x = sum(point[0] for point in points) / len(points)
    mean_y = sum(point[1] for point in points) / len(points)
    denominator = sum((point[0] - mean_x) ** 2 for point in points)
    if denominator == 0:
        raise CaptureError("clock points have no firmware-time spread")
    slope = sum((point[0] - mean_x) * (point[1] - mean_y) for point in points) / denominator
    intercept = mean_y - slope * mean_x
    residuals = [point[1] - (intercept + slope * point[0]) for point in points]
    return intercept, slope, residuals


def fit_clock(points: list[tuple[float, float]], min_span_ms: int) -> dict[str, Any]:
    """Centered least squares plus one deterministic MAD rejection pass.

    ``points`` are ``(firmware_now_ms, host_absolute_ms)``.  The reported
    intercept is converted to a centered representation so its large epoch
    value never affects the least-squares calculation.
    """
    if len(points) < 2:
        raise CaptureError("clock has fewer than two telemetry points")
    ordered = list(points)
    intercept, slope, residuals = _linear_fit(ordered)
    median_residual = _median(residuals)
    mad = _median(abs(value - median_residual) for value in residuals)
    mad_threshold = max(5.0, 3.0 * 1.4826 * mad)
    retained = [point for point, residual in zip(ordered, residuals)
                if abs(residual - median_residual) <= mad_threshold]
    if len(retained) < 2 or len(retained) / len(ordered) < 0.95:
        raise CaptureError("clock MAD rejection retained fewer than 95% of telemetry points")
    intercept, slope, residuals = _linear_fit(retained)
    absolute_residuals = [abs(value) for value in residuals]
    span_ms = max(point[0] for point in retained) - min(point[0] for point in retained)
    if span_ms < min_span_ms:
        raise CaptureError(f"clock span {span_ms:.3f}ms is below required {min_span_ms}ms")
    p95_residual = nearest_rank(absolute_residuals, 95)
    max_residual = max(absolute_residuals)
    if not 0.995 <= slope <= 1.005:
        raise CaptureError(f"clock slope {slope:.9f} is outside 0.995..1.005")
    if p95_residual > 5.0 or max_residual > 20.0:
        raise CaptureError("clock residual limits exceeded")
    center_now = sum(point[0] for point in retained) / len(retained)
    center_host = sum(point[1] for point in retained) / len(retained)
    return {
        "slope": slope,
        "center_now_ms": center_now,
        "center_host_ms": center_host,
        "intercept_ms": intercept,
        "total_points": len(ordered),
        "retained_points": len(retained),
        "rejected_points": len(ordered) - len(retained),
        "retained_ratio": len(retained) / len(ordered),
        "span_ms": span_ms,
        "residual_p95_ms": p95_residual,
        "residual_max_ms": max_residual,
    }


def estimate_host_ms(fit: dict[str, Any], now_ms: int) -> float:
    return float(fit["intercept_ms"]) + float(fit["slope"]) * now_ms


def _summary_record(records: list[BenchRecord], role: str) -> BenchRecord:
    summaries = [record for record in records if record.kind == "summary"]
    if len(summaries) != 1:
        raise CaptureError(f"board {role} must have exactly one terminal bench_summary")
    summary = summaries[0]
    if _parse_uint(summary.fields["attempt_q_dropped"], "attempt_q_dropped",
                   summary.source, summary.line_number) != 0:
        raise CaptureError(f"board {role} reports dropped benchmark telemetry")
    return summary


def _round(value: float) -> float:
    return round(value, 6)


def _validated_log_map(items: list[str]) -> dict[str, Path]:
    mapped: dict[str, Path] = {}
    for item in items:
        role, separator, path = item.partition("=")
        if separator != "=" or role not in ROLES or not path or role in mapped:
            raise CaptureError("--serial-log must be a unique ROLE=PATH with roles A through F")
        mapped[role] = Path(path)
    if set(mapped) != set(ROLES):
        raise CaptureError("exactly six --serial-log ROLE=PATH inputs are required")
    return mapped


def _validate_log_provenance(metadata: dict[str, Any], log_paths: dict[str, Path]) -> None:
    by_role = {board["role"]: board for board in metadata["boards"]}
    for role, path in log_paths.items():
        recorded = by_role[role].get("log")
        if recorded is None:
            continue
        if not isinstance(recorded, dict):
            raise CaptureError(f"run metadata board {role} has malformed log provenance")
        expected_hash = recorded.get("sha256")
        if expected_hash is not None:
            if not isinstance(expected_hash, str) or not re.fullmatch(r"[0-9a-f]{64}", expected_hash):
                raise CaptureError(f"run metadata board {role} has invalid log hash")
            if sha256_file(path) != expected_hash:
                raise CaptureError(f"serial log hash differs from run metadata for board {role}")


def _safe_counter_map(value: Any, name: str) -> dict[str, Any]:
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise CaptureError(f"{name} must be an object")

    def walk(item: Any, path: str) -> Any:
        if isinstance(item, bool):
            raise CaptureError(f"{name}.{path} must not be boolean")
        if isinstance(item, int):
            if item < 0:
                raise CaptureError(f"{name}.{path} must be nonnegative")
            return item
        if isinstance(item, dict):
            return {key: walk(subvalue, f"{path}.{key}" if path else key)
                    for key, subvalue in sorted(item.items())
                    if isinstance(key, str) and key}
        raise CaptureError(f"{name}.{path} must be an integer or object")

    if any(not isinstance(key, str) or not key for key in value):
        raise CaptureError(f"{name} has invalid key")
    return walk(value, "")


def _matches_width(expected: str, observed: str) -> bool:
    """Accept the plan's manifest spelling and firmware's compact enum value."""
    return observed == expected or {
        "SID8": "1",
        "SID16": "2",
    }.get(expected) == observed


def derive_capture(metadata: dict[str, Any], log_paths: dict[str, Path],
                   min_clock_span_seconds: int | None = None) -> dict[str, Any]:
    """Derive strict, deterministic six-board metrics from raw captures."""
    _validate_log_provenance(metadata, log_paths)
    benchmark = metadata["benchmark"]
    configured_span = _required_positive_int(
        benchmark.get("clock_min_span_seconds", DEFAULT_MIN_CLOCK_SPAN_SECONDS),
        "benchmark.clock_min_span_seconds")
    if min_clock_span_seconds is not None:
        configured_span = min_clock_span_seconds
    if configured_span <= 0:
        raise CaptureError("clock minimum span must be positive")
    records_by_role = {role: parse_log(path, role) for role, path in sorted(log_paths.items())}
    clocks: dict[str, dict[str, Any]] = {}
    summaries: dict[str, BenchRecord] = {}
    for role, records in records_by_role.items():
        clock_points = [(_parse_uint(record.fields["now"], "now", record.source, record.line_number),
                         record.host_ms)
                        for record in records if record.kind == "clock"]
        clocks[role] = fit_clock(clock_points, configured_span * 1000)
        summaries[role] = _summary_record(records, role)

    source_role = benchmark["source_role"]
    destination_role = benchmark["destination_role"]
    expected_origin = benchmark["origin"]
    expected_destination = benchmark["destination"]
    expected_width = benchmark["width"]
    attempt_records = [record for record in records_by_role[source_role] if record.kind == "attempt"]
    final_records = [record for record in records_by_role[destination_role] if record.kind == "final"]
    for role, records in records_by_role.items():
        if role != source_role and any(record.kind == "attempt" for record in records):
            raise CaptureError(f"board {role} emitted source benchmark attempts")
        if role != destination_role and any(record.kind == "final" for record in records):
            raise CaptureError(f"board {role} emitted destination benchmark finals")

    accepted: dict[int, BenchRecord] = {}
    for record in attempt_records:
        if record.fields["destination"] != expected_destination or \
                not _matches_width(expected_width, record.fields["width"]):
            raise _error(record.source, record.line_number, "has attempt destination/width mismatch")
        if _parse_uint(record.fields["accepted"], "accepted", record.source, record.line_number) != 1:
            continue
        counter = _parse_uint(record.fields["counter"], "counter", record.source, record.line_number)
        if counter in accepted:
            raise _error(record.source, record.line_number, "has duplicate accepted app counter")
        accepted[counter] = record

    finals: dict[int, BenchRecord] = {}
    for record in final_records:
        if record.fields["origin"] != expected_origin or record.fields["destination"] != expected_destination:
            raise _error(record.source, record.line_number, "has final origin/destination mismatch")
        if _parse_uint(record.fields["counter_valid"], "counter_valid", record.source, record.line_number) != 1:
            raise _error(record.source, record.line_number, "has counter-invalid final delivery")
        counter = _parse_uint(record.fields["counter"], "counter", record.source, record.line_number)
        if counter in finals:
            raise _error(record.source, record.line_number, "has duplicate final app counter")
        finals[counter] = record

    source_summary = summaries[source_role]
    offered = _parse_uint(source_summary.fields["offered"], "offered",
                          source_summary.source, source_summary.line_number)
    attempted = _parse_uint(source_summary.fields["attempted"], "attempted",
                            source_summary.source, source_summary.line_number)
    accepted_summary = _parse_uint(source_summary.fields["accepted"], "accepted",
                                   source_summary.source, source_summary.line_number)
    if offered != _required_positive_int(benchmark["offered_slots"], "benchmark.offered_slots"):
        raise CaptureError("source offered counter differs from planned offered_slots")
    if attempted != len(attempt_records) or accepted_summary != len(accepted):
        raise CaptureError("source compact counters disagree with attempt telemetry")
    if attempted < accepted_summary:
        raise CaptureError("source compact counters are internally inconsistent")

    matched = sorted(set(accepted) & set(finals))
    lost = sorted(set(accepted) - set(finals))
    unmatched_final = sorted(set(finals) - set(accepted))
    latencies: list[float] = []
    delivered_bytes = 0
    late: list[int] = []
    offered_end_value = benchmark.get("offered_end_utc")
    offered_end_ms: float | None = None
    if offered_end_value is not None:
        if not isinstance(offered_end_value, str):
            raise CaptureError("benchmark.offered_end_utc must be a timestamp")
        offered_end_ms = _parse_timestamp(offered_end_value, Path("run-metadata"), 0)
    for counter in matched:
        attempt = accepted[counter]
        final = finals[counter]
        sent_host_ms = estimate_host_ms(clocks[source_role], _parse_uint(
            attempt.fields["now"], "now", attempt.source, attempt.line_number))
        delivered_host_ms = estimate_host_ms(clocks[destination_role], _parse_uint(
            final.fields["now"], "now", final.source, final.line_number))
        latency = delivered_host_ms - sent_host_ms
        if latency < 0:
            raise CaptureError(f"counter {counter} has negative fitted one-way latency")
        latencies.append(latency)
        delivered_bytes += _parse_uint(final.fields["app_len"], "app_len", final.source, final.line_number)
        if offered_end_ms is not None and delivered_host_ms > offered_end_ms:
            late.append(counter)
    offered_seconds = _required_positive_int(benchmark["offered_seconds"], "benchmark.offered_seconds")
    latency_percentiles = ({"p50_ms": _round(nearest_rank(latencies, 50)),
                            "p95_ms": _round(nearest_rank(latencies, 95)),
                            "p99_ms": _round(nearest_rank(latencies, 99))}
                           if latencies else {"p50_ms": None, "p95_ms": None, "p99_ms": None})
    terminal_counters = _safe_counter_map(
        metadata.get("terminal_counters", metadata.get("compact_final_counters")),
        "terminal_counters")
    return {
        "schema": SCHEMA,
        "status": "PASS",
        "benchmark": {
            "source_role": source_role,
            "destination_role": destination_role,
            "origin": expected_origin,
            "destination": expected_destination,
            "width": expected_width,
            "offered_slots": offered,
            "offered_seconds": offered_seconds,
            "drain_seconds": _required_positive_int(benchmark["drain_seconds"], "benchmark.drain_seconds"),
        },
        "metrics": {
            "accepted_attempts": len(accepted),
            "delivered_packets": len(matched),
            "delivered_app_bytes": delivered_bytes,
            "accepted_pdr": _round(len(matched) / len(accepted)) if accepted else None,
            "offered_delivery_ratio": _round(len(matched) / offered),
            "packet_goodput_packets_per_second": _round(len(matched) / offered_seconds),
            "app_goodput_bytes_per_second": _round(delivered_bytes / offered_seconds),
            "one_way_latency": latency_percentiles,
        },
        "counters": {
            "lost_counters": lost,
            "unmatched_accepted_counters": lost,
            "unmatched_final_counters": unmatched_final,
            "late_drain_counters": late,
            "terminal_counters": terminal_counters,
        },
        "clock_fits": {role: {key: _round(value) if isinstance(value, float) else value
                               for key, value in sorted(fit.items())}
                       for role, fit in sorted(clocks.items())},
        "compact_summaries": {
            role: {key: _parse_uint(value, key, summary.source, summary.line_number)
                   if key != "role" else value
                   for key, value in sorted(summary.fields.items())}
            for role, summary in sorted(summaries.items())
        },
        "provenance": {
            "run_metadata_schema": metadata["schema"],
            "run_metadata_name": metadata.get("run_metadata_name", ""),
        },
    }


def render_markdown(capture: dict[str, Any]) -> str:
    metrics = capture["metrics"]
    counters = capture["counters"]
    lines = [
        "# TAVRN six-board benchmark summary",
        "",
        "| Metric | Value |",
        "| --- | ---: |",
        f"| Accepted PDR | {metrics['accepted_pdr']!s} |",
        f"| Offered delivery ratio | {metrics['offered_delivery_ratio']!s} |",
        f"| Packet goodput (packets/s) | {metrics['packet_goodput_packets_per_second']!s} |",
        f"| App goodput (bytes/s) | {metrics['app_goodput_bytes_per_second']!s} |",
        f"| One-way p50/p95/p99 (ms) | {metrics['one_way_latency']['p50_ms']}/"
        f"{metrics['one_way_latency']['p95_ms']}/{metrics['one_way_latency']['p99_ms']} |",
        f"| Lost accepted counters | {len(counters['lost_counters'])} |",
        f"| Late drain counters | {len(counters['late_drain_counters'])} |",
        "",
        "| Board | Slope | Retained | Span (ms) | p95/max residual (ms) |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for role, fit in sorted(capture["clock_fits"].items()):
        lines.append(
            f"| {role} | {fit['slope']:.6f} | {fit['retained_points']}/{fit['total_points']} | "
            f"{fit['span_ms']:.3f} | {fit['residual_p95_ms']:.3f}/{fit['residual_max_ms']:.3f} |")
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-metadata", required=True)
    parser.add_argument("--serial-log", action="append", required=True, metavar="ROLE=PATH")
    parser.add_argument("--output", required=True, help="deterministic JSON result path")
    parser.add_argument("--markdown-output", required=True, help="concise Markdown result path")
    parser.add_argument("--min-clock-span-seconds", type=int,
                        help="override the metadata/default production minimum")
    args = parser.parse_args(argv)
    try:
        log_paths = _validated_log_map(args.serial_log)
        metadata = load_metadata(Path(args.run_metadata))
        capture = derive_capture(metadata, log_paths, args.min_clock_span_seconds)
        output = Path(args.output)
        markdown_output = Path(args.markdown_output)
        output.parent.mkdir(parents=True, exist_ok=True)
        markdown_output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(capture, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        markdown_output.write_text(render_markdown(capture), encoding="utf-8")
    except CaptureError as error:
        print(f"FAIL tavrn_benchmark_capture: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
