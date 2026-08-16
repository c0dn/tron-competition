#!/usr/bin/env python3
"""Incremental/offline analyzer for continuous six-board TAVRN observations.

This module is deliberately importable by ``run_tavrn_benchmark.py``: the live
watch and an offline replay feed exactly the same :class:`ObservationState`.
Firmware records are timestamped by grabserial and use ``routed obs_*``
prefixes.  No record is terminal; records may repeat indefinitely.

``now`` is the monotonic timestamp at which firmware emits a record.  Delivery
and source events also carry their own ``event_at_ms`` timestamp.  Keeping
those clocks separate prevents logger delay from becoming a claimed one-way
latency while retaining record-order checks on ``now``.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import math
import re
import sqlite3
import statistics
import sys
import shutil
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable


SCHEMA = "tron.tavrn.observation.capture.v2"
RUN_SCHEMA = "tron.tavrn.observation.run.v2"
MATPLOTLIB_PIN = "matplotlib==3.11.1"
OBSERVER_SCHEMA = "observer-v2"
ROLES = ("A", "B", "C", "D", "E", "F")
HALF_RANGE = 0x80000000
UINT32_MAX = 0xFFFFFFFF
HEARTBEAT_WINDOW_SECONDS = 10.0
THROUGHPUT_BURST_SECONDS = 60.0
CLOCK_CADENCE_MS = 1500.0
TELEMETRY_CADENCE_MS = 12000.0
HEARTBEAT_MINIMUM_OFFERS = 120
THROUGHPUT_BURST_SLOT_COUNT = 600
HEARTBEAT_INTERVAL_MS = 1000
THROUGHPUT_START_MS = 60000
THROUGHPUT_BURST_PERIOD_MS = 450000
THROUGHPUT_INTERVAL_MS = 100
NOT_READY_STATUS = UINT32_MAX
AODV_ACCEPTED_STATUSES = {0, 1}
AODV_STATUS_MAX = 8
TIMESTAMP_RE = re.compile(
    r"^\[?(?P<value>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{6}Z)"
    r"(?:\s+[^\]]+)?\]?\s+(?P<payload>(?:routed\s+)?obs_[^\r\n]*)$"
)
FIELD_RE = re.compile(r"[a-z][a-z0-9_]*=[^\s=]+")
UINT_RE = re.compile(r"(?:0|[1-9][0-9]*)")
WORD_RE = re.compile(r"[A-Za-z0-9_.:@+-]+")
IDENTITY_RE = re.compile(r"0x[0-9a-f]{6}")
ADVA_RE = re.compile(r"[0-9a-f]{2}(?::[0-9a-f]{2}){5}")

CONTROL_COUNTER_FIELDS = {
    *(f"rx_control_proxy_{kind}" for kind in ("rreq", "rrep", "rerr", "hello", "sync_offer", "sync_pull", "sync_data", "tc_update", "rrep_ack")),
    *(f"rx_control_proxy_bytes_{kind}" for kind in ("rreq", "rrep", "rerr", "hello", "sync_offer", "sync_pull", "sync_data", "tc_update", "rrep_ack")),
    *(f"aodv_dispatch_enqueue_proxy_{kind}" for kind in ("rreq", "rrep", "rerr", "hello", "sync_offer", "sync_pull", "sync_data", "tc_update", "rrep_ack")),
    *(f"aodv_dispatch_enqueue_proxy_bytes_{kind}" for kind in ("rreq", "rrep", "rerr", "hello", "sync_offer", "sync_pull", "sync_data", "tc_update", "rrep_ack")),
    "scheduler_rx_adv_proxy", "scheduler_tx_done_proxy", "scheduler_tx_failed_proxy",
    "scheduler_fault_proxy", "scheduler_rx_ch37_proxy", "scheduler_rx_ch38_proxy",
    "scheduler_rx_ch39_proxy", "link_tx_admitted_proxy", "link_tx_done_proxy",
    "link_tx_failed_proxy", "retry_due", "retry_exhausted", "aodv_rreq_duplicate",
    "aodv_rreq_rate_limited", "aodv_action_backpressure", "router_failure_invariant",
}
HEALTH_COUNTER_FIELDS = {
    "offered", "accepted", "rejected", "not_ready", "skipped", "heartbeat_skipped",
    "throughput_skipped", "event_q", "event_q_high_water", "event_q_dropped",
    "attempt_queue_faults", "guard_faults", "snapshot_faults", "final_q",
    "diagnostic_dropped", "rreq_dropped", "retry_log_dropped", "trace_over_budget",
    "trace_fault_latched", "mesh_fault", "router_fault",
}
HEALTH_GAUGES = {"event_q", "final_q"}

REQUIRED: dict[str, set[str]] = {
    "boot": {"now", "role", "schema", "session", "started_at_ms"},
    "clock": {"now", "role", "schema", "session"},
    "offer": {"now", "role", "schema", "session", "event_at_ms", "identity", "sequence",
                "workload", "burst", "offer", "payload_id", "deadline_ms", "destination", "width",
                "attempted", "accepted", "status"},
    "final": {"now", "role", "schema", "session", "origin_session", "origin", "destination",
               "event_at_ms", "identity", "sequence", "identity_valid", "workload", "burst",
               "payload_id", "app_kind", "app_len"},
    "app": {"now", "role", "schema", "session", "event_at_ms", "identity", "sequence",
              "offer_record_id_hi", "offer_record_id_lo", "deadline_ms", "workload", "burst",
              "payload_id", "destination", "width", "attempted", "accepted", "status"},
    "gtt_begin": {"now", "role", "schema", "session", "query_at_ms", "slot_capacity"},
    "gtt_entry": {"now", "role", "schema", "session", "query_at_ms", "slot", "adva",
                  "freshness", "departed", "last_evidence_ms"},
    "gtt_end": {"now", "role", "schema", "session", "query_at_ms", "status", "entry_count",
                "nondeparted_count"},
    "control": {"now", "role", "schema", "session", "control_tx_scope", *CONTROL_COUNTER_FIELDS},
    "health": {"now", "role", "schema", "session", "record_seq", *HEALTH_COUNTER_FIELDS},
}
RECORD_ID_FIELDS = {"record_id_hi", "record_id_lo"}
NUMERIC_FIELDS: dict[str, set[str]] = {
    "boot": {"started_at_ms"}, "clock": set(),
    "offer": {"event_at_ms", "deadline_ms", "sequence", "offer", "payload_id", "attempted",
              "accepted", "width", "burst", "status"},
    "final": {"event_at_ms", "sequence", "origin_session", "identity_valid", "burst", "payload_id", "app_kind", "app_len"},
    "app": {"event_at_ms", "deadline_ms", "sequence", "offer_record_id_hi", "offer_record_id_lo",
            "burst", "payload_id", "width", "attempted", "accepted", "status"},
    "gtt_begin": {"query_at_ms", "slot_capacity"},
    "gtt_entry": {"query_at_ms", "slot", "departed", "last_evidence_ms"},
    "gtt_end": {"query_at_ms", "entry_count", "nondeparted_count"},
    "control": CONTROL_COUNTER_FIELDS,
    "health": HEALTH_COUNTER_FIELDS,
}
META_FIELDS = {"now", "role", "schema", "session", "record_seq", "control_tx_scope", "_record_id", *RECORD_ID_FIELDS}
INTEGRITY_COUNTER_MARKERS = ("drop", "skip", "fault", "mesh", "router")
BUNDLE_NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*")


class CaptureError(Exception):
    """Observation evidence is malformed or cannot be associated safely."""


@dataclass(frozen=True)
class ObservationRecord:
    kind: str
    role: str
    session: str
    schema: str
    now: int
    now_unwrapped: int
    event_at_unwrapped: int | None
    host_ms: float
    fields: dict[str, str]
    source: str
    line_number: int


class SQLiteObservationRecordStore:
    """Append-only, transaction-batched durable storage for observation records.

    Rows are read back in the monotonic ingestion order assigned by SQLite.  The
    JSON column contains the arbitrary wire-field map; all other
    :class:`ObservationRecord` fields have typed columns.  No pickle data is
    ever written to a run directory.
    """

    COMMIT_BATCH_WRITES = 256

    def __init__(self, path: Path | str) -> None:
        self.path = Path(path)
        self._closed = False
        self._pending_writes = 0
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self._connection = sqlite3.connect(self.path)
            self._connection.execute("""
                CREATE TABLE IF NOT EXISTS observation_records (
                    ingestion_id INTEGER PRIMARY KEY,
                    kind TEXT NOT NULL,
                    role TEXT NOT NULL,
                    session TEXT NOT NULL,
                    schema_name TEXT NOT NULL,
                    now_value INTEGER NOT NULL,
                    now_unwrapped INTEGER NOT NULL,
                    event_at_unwrapped INTEGER,
                    host_ms REAL NOT NULL,
                    fields_json TEXT NOT NULL,
                    source TEXT NOT NULL,
                    line_number INTEGER NOT NULL
                )
            """)
            self._connection.execute("""
                CREATE TABLE IF NOT EXISTS observation_invalid_intervals (
                    interval_id INTEGER PRIMARY KEY,
                    role TEXT NOT NULL,
                    start_ms REAL NOT NULL,
                    end_ms REAL NOT NULL,
                    reason TEXT NOT NULL
                )
            """)
            self._record_count = int(self._connection.execute(
                "SELECT COUNT(*) FROM observation_records").fetchone()[0])
            self._invalid_interval_count = int(self._connection.execute(
                "SELECT COUNT(*) FROM observation_invalid_intervals").fetchone()[0])
            self._connection.commit()
        except (OSError, sqlite3.Error) as error:
            raise CaptureError(f"cannot open observation record store {self.path}: {error}") from error

    def __len__(self) -> int:
        return self._record_count

    @property
    def closed(self) -> bool:
        return self._closed

    @property
    def invalid_interval_count(self) -> int:
        return self._invalid_interval_count

    def append(self, record: ObservationRecord) -> None:
        if self._closed:
            raise CaptureError("cannot append to a closed observation record store")
        try:
            self._connection.execute(
                """INSERT INTO observation_records (
                       kind, role, session, schema_name, now_value, now_unwrapped,
                       event_at_unwrapped, host_ms, fields_json, source, line_number
                   ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
                (record.kind, record.role, record.session, record.schema, record.now,
                 record.now_unwrapped, record.event_at_unwrapped, record.host_ms,
                 json.dumps(record.fields, ensure_ascii=False, separators=(",", ":")),
                 record.source, record.line_number))
            self._record_count += 1
            self._commit_if_batch_complete()
        except sqlite3.Error as error:
            raise CaptureError(f"cannot append observation record to {self.path}: {error}") from error

    def __iter__(self) -> Iterable[ObservationRecord]:
        if self._closed:
            raise CaptureError("cannot iterate a closed observation record store")
        try:
            cursor = self._connection.execute(
                """SELECT kind, role, session, schema_name, now_value, now_unwrapped,
                          event_at_unwrapped, host_ms, fields_json, source, line_number
                   FROM observation_records ORDER BY ingestion_id""")
            for row in cursor:
                fields = json.loads(row[8])
                if not isinstance(fields, dict) or any(not isinstance(key, str) or
                                                       not isinstance(value, str)
                                                       for key, value in fields.items()):
                    raise CaptureError(f"observation record store {self.path} has invalid fields")
                yield ObservationRecord(str(row[0]), str(row[1]), str(row[2]), str(row[3]),
                                        int(row[4]), int(row[5]),
                                        int(row[6]) if row[6] is not None else None,
                                        float(row[7]), fields, str(row[9]), int(row[10]))
        except (sqlite3.Error, json.JSONDecodeError) as error:
            raise CaptureError(f"cannot read observation record store {self.path}: {error}") from error

    def append_invalid_interval(self, interval: dict[str, Any]) -> None:
        if self._closed:
            raise CaptureError("cannot append to a closed observation record store")
        try:
            self._connection.execute(
                """INSERT INTO observation_invalid_intervals (role, start_ms, end_ms, reason)
                   VALUES (?, ?, ?, ?)""",
                (str(interval["role"]), float(interval["start_ms"]),
                 float(interval["end_ms"]), str(interval["reason"])))
            self._invalid_interval_count += 1
            self._commit_if_batch_complete()
        except (KeyError, TypeError, ValueError, sqlite3.Error) as error:
            raise CaptureError(f"cannot append invalid interval to {self.path}: {error}") from error

    def invalid_intervals(self) -> Iterable[dict[str, Any]]:
        if self._closed:
            raise CaptureError("cannot iterate a closed observation record store")
        try:
            for row in self._connection.execute(
                    """SELECT role, start_ms, end_ms, reason
                       FROM observation_invalid_intervals ORDER BY interval_id"""):
                yield {"role": str(row[0]), "start_ms": float(row[1]),
                       "end_ms": float(row[2]), "reason": str(row[3])}
        except sqlite3.Error as error:
            raise CaptureError(f"cannot read invalid intervals from {self.path}: {error}") from error

    def flush(self) -> None:
        if self._closed:
            raise CaptureError("cannot flush a closed observation record store")
        try:
            self._connection.commit()
            self._pending_writes = 0
        except sqlite3.Error as error:
            raise CaptureError(f"cannot flush observation record store {self.path}: {error}") from error

    def _commit_if_batch_complete(self) -> None:
        self._pending_writes += 1
        if self._pending_writes >= self.COMMIT_BATCH_WRITES:
            self.flush()

    def close(self) -> None:
        if self._closed:
            return
        try:
            self._connection.commit()
            self._connection.close()
        except sqlite3.Error as error:
            raise CaptureError(f"cannot close observation record store {self.path}: {error}") from error
        self._closed = True


@dataclass
class _RoleClock:
    session: str | None = None
    previous_now: int | None = None
    epoch: int = 0
    boot_required: bool = True
    previous_host_ms: float | None = None
    last_health_seq: int | None = None
    last_health_host_ms: float | None = None
    last_integrity_values: dict[str, int] = field(default_factory=dict)
    last_control_values: dict[str, int] = field(default_factory=dict)
    last_health_values: dict[str, int] = field(default_factory=dict)
    last_record_id: int | None = None


@dataclass
class _GttOpen:
    record: ObservationRecord
    entries: list[ObservationRecord] = field(default_factory=list)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _error(source: str, line_number: int, message: str) -> CaptureError:
    return CaptureError(f"{source}:{line_number} {message}")


def _timestamp(value: str, source: str, line_number: int) -> float:
    try:
        parsed = dt.datetime.strptime(value, "%Y-%m-%dT%H:%M:%S.%fZ")
    except ValueError as error:
        raise _error(source, line_number, "has malformed UTC timestamp") from error
    return parsed.replace(tzinfo=dt.timezone.utc).timestamp() * 1000.0


def _uint(value: str, field_name: str, source: str, line_number: int) -> int:
    if not UINT_RE.fullmatch(value):
        raise _error(source, line_number, f"has non-decimal {field_name}")
    number = int(value, 10)
    if number > UINT32_MAX:
        raise _error(source, line_number, f"has out-of-range {field_name}")
    return number


def _uint64(value: str, field_name: str, source: str, line_number: int) -> int:
    if not UINT_RE.fullmatch(value):
        raise _error(source, line_number, f"has non-decimal {field_name}")
    number = int(value, 10)
    if number > 0xFFFFFFFFFFFFFFFF:
        raise _error(source, line_number, f"has out-of-range {field_name}")
    return number


def _role(value: str, expected_role: str | None, source: str, line_number: int) -> str:
    normalized = value if value in ROLES else (ROLES[ord(value) - ord("1")]
                                                if len(value) == 1 and value in "123456" else "")
    if not normalized or (expected_role is not None and normalized != expected_role):
        raise _error(source, line_number, f"has role mismatch expected={expected_role or 'A..F'}")
    return normalized


def _record_id(fields: dict[str, str], source: str, line_number: int) -> int:
    return (_uint(fields["record_id_hi"], "record_id_hi", source, line_number) << 32) | \
        _uint(fields["record_id_lo"], "record_id_lo", source, line_number)


def _validate_event_identity(fields: dict[str, str], source: str, line_number: int) -> None:
    """Validate the firmware's exact compact 24-bit benchmark identity layout."""
    burst = _uint(fields["burst"], "burst", source, line_number)
    sequence = _uint(fields["sequence"], "sequence", source, line_number)
    payload_id = _uint(fields["payload_id"], "payload_id", source, line_number)
    if burst >= (1 << 13):
        raise _error(source, line_number, "has out-of-range burst")
    if sequence >= (1 << 10) or payload_id >= (1 << 10):
        raise _error(source, line_number, "has out-of-range sequence/payload_id")
    if sequence != payload_id:
        raise _error(source, line_number, "has sequence/payload_id mismatch")
    identity_text = fields["identity"]
    if IDENTITY_RE.fullmatch(identity_text) is None:
        raise _error(source, line_number, "has malformed identity")
    workload = fields["workload"]
    expected = ((1 if workload == "throughput" else 0) << 23) | (burst << 10) | payload_id
    if int(identity_text[2:], 16) != expected:
        raise _error(source, line_number, "has identity/workload/burst/sequence mismatch")


def _validate_strict_counters(kind: str, fields: dict[str, str], source: str,
                              line_number: int) -> None:
    """Reject opaque or non-uint32 control/health fields instead of dropping them."""
    allowed_words = {"control_tx_scope"} if kind == "control" else set()
    for name, value in fields.items():
        if name in META_FIELDS or name in allowed_words:
            continue
        _uint(value, name, source, line_number)


def parse_observation_line(line: str, source: str = "stream", line_number: int = 1,
                           expected_role: str | None = None) -> tuple[str, float, dict[str, str]] | None:
    """Parse one complete wire line.  Partial-line buffering lives in state."""
    if "obs_" not in line:
        return None
    match = TIMESTAMP_RE.fullmatch(line)
    if match is None:
        raise _error(source, line_number, "has malformed timestamped observation")
    parts = match.group("payload").split()
    if parts and parts[0] == "routed":
        parts.pop(0)
    if len(parts) < 2 or not parts[0].startswith("obs_"):
        raise _error(source, line_number, "has malformed observation prefix")
    kind = parts.pop(0)[4:]
    if kind not in REQUIRED:
        raise _error(source, line_number, f"has unsupported observation kind {kind}")
    rendered = parts
    if not rendered or any(not FIELD_RE.fullmatch(item) for item in rendered):
        raise _error(source, line_number, "has malformed observation field")
    fields: dict[str, str] = {}
    for item in rendered:
        key, value = item.split("=", 1)
        if key in fields:
            raise _error(source, line_number, f"has duplicate field {key}")
        fields[key] = value
    missing = REQUIRED[kind] - set(fields)
    if missing:
        raise _error(source, line_number, f"is missing required fields {','.join(sorted(missing))}")
    missing_record_id = RECORD_ID_FIELDS - set(fields)
    if missing_record_id:
        raise _error(source, line_number, "has incomplete record ID")
    if fields["schema"] != OBSERVER_SCHEMA:
        raise _error(source, line_number, f"has unsupported observation schema {fields['schema']}")
    if kind in ("offer", "app", "final"):
        if fields["workload"] == "0":
            fields["workload"] = "heartbeat"
        elif fields["workload"] == "1":
            fields["workload"] = "throughput"
    for name in {"now", "session", *RECORD_ID_FIELDS, *NUMERIC_FIELDS[kind]}:
        _uint(fields[name], name, source, line_number)
    fields["_record_id"] = str(_record_id(fields, source, line_number))
    if kind in ("offer", "app", "final"):
        _validate_event_identity(fields, source, line_number)
    if kind in ("offer", "app") and (_uint(fields["accepted"], "accepted", source, line_number) not in (0, 1)):
        raise _error(source, line_number, "has accepted outside 0..1")
    if kind in ("offer", "app") and _uint(fields["attempted"], "attempted", source, line_number) not in (0, 1):
        raise _error(source, line_number, "has attempted/accepted outside 0..1")
    if kind == "final" and _uint(fields["identity_valid"], "identity_valid", source, line_number) not in (0, 1):
        raise _error(source, line_number, "has identity_valid outside 0..1")
    if kind == "gtt_entry" and _uint(fields["departed"], "departed", source, line_number) not in (1, 2):
        raise _error(source, line_number, "has departed outside FALSE=1/TRUE=2")
    if kind == "gtt_entry":
        if ADVA_RE.fullmatch(fields["adva"]) is None:
            raise _error(source, line_number, "has non-canonical GTT AdvA")
        freshness = _uint(fields["freshness"], "freshness", source, line_number)
        if freshness not in (1, 2, 3, 4):
            raise _error(source, line_number, "has freshness outside 1..4")
    if kind == "control":
        if fields["control_tx_scope"] != "AODV_DISPATCH_PROXY_ONLY":
            raise _error(source, line_number, "has unsupported control_tx_scope")
        _validate_strict_counters(kind, fields, source, line_number)
    if kind == "health":
        _uint64(fields["record_seq"], "record_seq", source, line_number)
        _validate_strict_counters(kind, fields, source, line_number)
    if fields["workload"] not in ("heartbeat", "throughput") if kind in ("offer", "app", "final") else False:
        raise _error(source, line_number, "has unsupported workload")
    for name in ("schema", "status", "workload", "origin", "destination", "adva", "freshness"):
        if name in fields and not WORD_RE.fullmatch(fields[name]):
            raise _error(source, line_number, f"has invalid {name}")
    _role(fields["role"], expected_role, source, line_number)
    return kind, _timestamp(match.group("value"), source, line_number), fields


def nearest_rank(values: Iterable[float], percentile: int) -> float:
    ordered = sorted(values)
    if not ordered or not 1 <= percentile <= 100:
        raise CaptureError("nearest-rank percentile has invalid inputs")
    return ordered[math.ceil(len(ordered) * percentile / 100) - 1]


def _fit(points: list[tuple[float, float]]) -> dict[str, Any]:
    if len(points) < 2:
        return {"valid": False, "reason": "fewer_than_two_points", "total_points": len(points)}
    mean_x = sum(point[0] for point in points) / len(points)
    mean_y = sum(point[1] for point in points) / len(points)
    denominator = sum((point[0] - mean_x) ** 2 for point in points)
    if denominator == 0:
        return {"valid": False, "reason": "no_firmware_time_spread", "total_points": len(points)}
    slope = sum((x - mean_x) * (y - mean_y) for x, y in points) / denominator
    intercept = mean_y - slope * mean_x
    residuals = [y - (intercept + slope * x) for x, y in points]
    median = float(statistics.median(residuals))
    mad = float(statistics.median(abs(value - median) for value in residuals))
    limit = max(5.0, 3.0 * 1.4826 * mad)
    retained = [point for point, residual in zip(points, residuals) if abs(residual - median) <= limit]
    if len(retained) < 2:
        return {"valid": False, "reason": "mad_rejected_all_points", "total_points": len(points)}
    mean_x = sum(point[0] for point in retained) / len(retained)
    mean_y = sum(point[1] for point in retained) / len(retained)
    denominator = sum((x - mean_x) ** 2 for x, _ in retained)
    slope = sum((x - mean_x) * (y - mean_y) for x, y in retained) / denominator
    intercept = mean_y - slope * mean_x
    residuals = [y - (intercept + slope * x) for x, y in retained]
    absolute = [abs(value) for value in residuals]
    return {
        "valid": 0.995 <= slope <= 1.005 and nearest_rank(absolute, 95) <= 5.0 and max(absolute) <= 20.0,
        "reason": "ok" if 0.995 <= slope <= 1.005 and nearest_rank(absolute, 95) <= 5.0 and max(absolute) <= 20.0
        else "slope_or_residual_limit",
        "slope": slope, "intercept_ms": intercept, "total_points": len(points),
        "retained_points": len(retained), "rejected_points": len(points) - len(retained),
        "span_ms": max(x for x, _ in retained) - min(x for x, _ in retained),
        "residual_p95_ms": nearest_rank(absolute, 95), "residual_max_ms": max(absolute),
    }


def _counter_fields(record: ObservationRecord) -> dict[str, int]:
    """Return explicitly emitted numeric counters, never record identity/meta."""
    result: dict[str, int] = {}
    for key, value in record.fields.items():
        if key in META_FIELDS:
            continue
        if UINT_RE.fullmatch(value):
            result[key] = _uint(value, key, record.source, record.line_number)
    return result


class ObservationState:
    """One append-only incremental model for live capture and offline replay."""

    def __init__(self, metadata: dict[str, Any] | None = None,
                 database_path: Path | str | None = None) -> None:
        self.metadata = metadata or {}
        self.record_store = (SQLiteObservationRecordStore(database_path)
                             if database_path is not None else None)
        self.records: list[ObservationRecord] | SQLiteObservationRecordStore = (
            self.record_store if self.record_store is not None else [])
        self.buffers: dict[str, str] = {role: "" for role in ROLES}
        self.line_numbers: dict[str, int] = {role: 0 for role in ROLES}
        self.role_state: dict[str, _RoleClock] = {role: _RoleClock() for role in ROLES}
        self.schemas: set[str] = set()
        self._invalid_intervals: list[dict[str, Any]] = []
        self._invalid_interval_count = (self.record_store.invalid_interval_count
                                        if self.record_store is not None else 0)
        self.health_events: list[dict[str, Any]] = []
        self.gtt_open: dict[tuple[str, str], _GttOpen] = {}
        self.gtt_snapshots: list[dict[str, Any]] = []
        self.gtt_pairs: list[dict[str, Any]] = []
        self.finalized_host_ms: float | None = None
        self._latest_host_ms: float | None = None

    @property
    def database_path(self) -> Path | None:
        return self.record_store.path if self.record_store is not None else None

    @property
    def closed(self) -> bool:
        return self.record_store.closed if self.record_store is not None else False

    @property
    def invalid_intervals(self) -> list[dict[str, Any]]:
        """Materialize durable corruption intervals only for a full snapshot."""
        if self.record_store is None:
            return self._invalid_intervals
        return list(self.record_store.invalid_intervals())

    @property
    def retained_record_count(self) -> int:
        """Records intentionally retained as Python objects, excluding the store."""
        if self.record_store is None:
            return len(self.records)
        return sum(len(current.entries) + 1 for current in self.gtt_open.values())

    def _add_invalid_interval(self, interval: dict[str, Any]) -> None:
        self._invalid_interval_count += 1
        if self.record_store is None:
            self._invalid_intervals.append(interval)
        else:
            self.record_store.append_invalid_interval(interval)

    def flush(self) -> None:
        """Commit pending durable records before a scan or evidence hash."""
        if self.record_store is not None:
            self.record_store.flush()

    def close(self) -> None:
        """Flush and close optional durable storage after terminal metadata."""
        if self.record_store is not None:
            self.record_store.close()

    def live_status(self) -> dict[str, Any]:
        """Return the bounded, O(1) status used by the live watch line.

        This deliberately does not calculate proving status: that requires a
        full record-store scan and belongs to explicit/periodic/final bundles.
        """
        provisional = self.finalized_host_ms is None
        return {
            "record_count": len(self.records),
            "corruption": self._invalid_interval_count > 0,
            "corruption_count": self._invalid_interval_count,
            "telemetry_corruption": self._invalid_interval_count > 0,
            "telemetry_corruption_count": self._invalid_interval_count,
            "active_roles": sorted(role for role, role_state in self.role_state.items()
                                   if role_state.session is not None),
            "finalized": not provisional,
            "provisional": provisional,
            "incomplete": provisional,
        }

    def feed(self, role: str, chunk: str, source: str | None = None, final: bool = False,
             allow_terminal_partial: bool = False) -> list[ObservationRecord]:
        if role not in ROLES:
            raise CaptureError("incremental feed has invalid role")
        if self.finalized_host_ms is not None:
            raise CaptureError("cannot feed observations after finalize")
        source = source or role
        complete = self.buffers[role] + chunk
        if final and complete and not complete.endswith("\n") and not allow_terminal_partial:
            raise _error(source, self.line_numbers[role] + complete.count("\n") + 1,
                         "ends with partial UART line")
        parts = complete.split("\n")
        self.buffers[role] = "" if final else parts.pop()
        if final:
            parts.pop()
        emitted: list[ObservationRecord] = []
        for line in parts:
            self.line_numbers[role] += 1
            parsed = parse_observation_line(line.rstrip("\r"), source, self.line_numbers[role], role)
            if parsed is not None:
                emitted.append(self._ingest(*parsed, source, self.line_numbers[role]))
        open_keys = [key for key in self.gtt_open if key[0] == role]
        if final and open_keys:
            if not allow_terminal_partial:
                raise _error(source, self.line_numbers[role] + 1, "ends with partial GTT snapshot")
            for key in open_keys:
                self.gtt_open.pop(key)
        return emitted

    def finalize(self, host_ms: float) -> None:
        """Close a capture at its authoritative host timestamp.

        This deliberately only records the boundary.  ``snapshot`` applies the
        same completeness checks for live and offline use, so a live caller may
        inspect an incomplete state before it knows the final boundary.
        """
        if not isinstance(host_ms, (int, float)) or not math.isfinite(float(host_ms)):
            raise CaptureError("capture end host time is invalid")
        endpoint = float(host_ms)
        self.flush()
        latest = self._latest_host_ms if self._latest_host_ms is not None else endpoint
        if endpoint < latest:
            raise CaptureError("capture end precedes observed record")
        if self.finalized_host_ms is not None and self.finalized_host_ms != endpoint:
            raise CaptureError("capture end is already finalized")
        self.finalized_host_ms = endpoint

    def feed_file(self, role: str, path: Path,
                  allow_terminal_partial: bool = False) -> list[ObservationRecord]:
        try:
            return self.feed(role, path.read_text(encoding="utf-8"), str(path), final=True,
                             allow_terminal_partial=allow_terminal_partial)
        except (OSError, UnicodeDecodeError) as error:
            raise CaptureError(f"cannot read serial log {path}: {error}") from error

    def _ingest(self, kind: str, host_ms: float, fields: dict[str, str], source: str,
                line_number: int) -> ObservationRecord:
        role = _role(fields["role"], None, source, line_number)
        session = fields["session"]
        state = self.role_state[role]
        now = _uint(fields["now"], "now", source, line_number)
        previous_host = state.previous_host_ms
        if state.previous_host_ms is not None and host_ms < state.previous_host_ms:
            raise _error(source, line_number, "has host-time reversal")
        if kind == "boot":
            if state.session == session:
                raise _error(source, line_number, "repeats active boot session")
            state.session = session
            state.previous_now = now
            state.epoch = 0
            state.boot_required = False
            state.last_health_seq = None
            state.last_health_host_ms = None
            state.last_integrity_values = {}
            state.last_control_values = {}
            state.last_health_values = {}
            state.last_record_id = None
        else:
            if state.boot_required or state.session != session:
                raise _error(source, line_number, "has record without matching obs_boot session")
            assert state.previous_now is not None
            if now < state.previous_now:
                if state.previous_now - now > HALF_RANGE:
                    state.epoch += UINT32_MAX + 1
                else:
                    raise _error(source, line_number, "has reboot/rollback without obs_boot")
            state.previous_now = now
        state.previous_host_ms = host_ms
        record_id = _record_id(fields, source, line_number)
        if state.last_record_id is not None and record_id != state.last_record_id + 1:
            self._add_invalid_interval({"role": role, "start_ms": previous_host or host_ms,
                                        "end_ms": host_ms, "reason": "record_sequence_gap"})
        state.last_record_id = record_id
        if self.schemas and fields["schema"] not in self.schemas:
            raise _error(source, line_number, "changes observation schema within one capture")
        self.schemas.add(fields["schema"])
        now_unwrapped = state.epoch + now
        event_at_unwrapped: int | None = None
        if kind in {"offer", "app", "final"}:
            event_at = _uint(fields["event_at_ms"], "event_at_ms", source, line_number)
            relative = (event_at - now) & UINT32_MAX
            if relative == HALF_RANGE:
                raise _error(source, line_number, "has event_at_ms at uint32 half-range from now")
            if relative > HALF_RANGE:
                relative -= UINT32_MAX + 1
            event_at_unwrapped = now_unwrapped + relative
        record = ObservationRecord(kind, role, session, fields["schema"], now, now_unwrapped,
                                    event_at_unwrapped, host_ms, dict(fields), source, line_number)
        self.records.append(record)
        self._latest_host_ms = max(self._latest_host_ms, host_ms) if self._latest_host_ms is not None else host_ms
        if kind == "control":
            self._cumulative_counters(record, state, "control")
        elif kind == "health":
            self._health(record, state)
        elif kind == "gtt_begin":
            key = (role, session)
            if key in self.gtt_open:
                raise _error(source, line_number, "starts GTT snapshot before prior obs_gtt_end")
            self.gtt_open[key] = _GttOpen(record)
        elif kind == "gtt_entry":
            current = self.gtt_open.get((role, session))
            if current is None:
                raise _error(source, line_number, "has obs_gtt_entry without obs_gtt_begin")
            current.entries.append(record)
        elif kind == "gtt_end":
            current = self.gtt_open.pop((role, session), None)
            if current is None:
                raise _error(source, line_number, "has obs_gtt_end without obs_gtt_begin")
            full_snapshot = self._complete_gtt_snapshot(current, record)
            if self.record_store is None:
                self.gtt_pairs.append({"role": role, "session": session, "begin": current.record,
                                       "end": record, "entries": current.entries,
                                       "status": record.fields["status"]})
                if full_snapshot:
                    self.gtt_snapshots.append({"role": role, "session": session,
                                                "begin": current.record, "end": record,
                                                "entries": current.entries})
        return record

    def _complete_gtt_snapshot(self, current: _GttOpen,
                               end: ObservationRecord) -> bool:
        begin = current.record
        query_at = _uint(begin.fields["query_at_ms"], "query_at_ms", begin.source, begin.line_number)
        if _uint(end.fields["query_at_ms"], "query_at_ms", end.source, end.line_number) != query_at:
            raise _error(end.source, end.line_number, "has GTT end query_at_ms mismatch")
        if end.fields["status"] == "NOT_IMPLEMENTED":
            if self.metadata.get("profile") != "AODV_ONLY" or \
                    begin.fields.get("status") != "NOT_IMPLEMENTED" or current.entries or \
                    _uint(begin.fields["slot_capacity"], "slot_capacity",
                          begin.source, begin.line_number) != 0 or \
                    _uint(end.fields["entry_count"], "entry_count",
                          end.source, end.line_number) != 0 or \
                    _uint(end.fields["nondeparted_count"], "nondeparted_count",
                          end.source, end.line_number) != 0:
                raise _error(end.source, end.line_number,
                             "has invalid NOT_IMPLEMENTED GTT evidence")
            return False
        if end.fields["status"] != "OK":
            raise _error(end.source, end.line_number, f"has rejected GTT snapshot status={end.fields['status']}")
        if self.metadata.get("profile") != "FULL_TAVRN":
            raise _error(end.source, end.line_number, "has FULL GTT snapshot outside FULL_TAVRN")
        capacity = _uint(begin.fields["slot_capacity"], "slot_capacity", begin.source, begin.line_number)
        expected_entries = _uint(end.fields["entry_count"], "entry_count", end.source, end.line_number)
        expected_nondeparted = _uint(end.fields["nondeparted_count"], "nondeparted_count", end.source, end.line_number)
        if "retained_entry_count" not in begin.fields or _uint(begin.fields["retained_entry_count"], "retained_entry_count",
                                                                begin.source, begin.line_number) != expected_entries:
            raise _error(end.source, end.line_number, "has missing or mismatched retained_entry_count")
        if expected_entries != len(current.entries) or expected_entries > capacity:
            raise _error(end.source, end.line_number, "has partial GTT snapshot entry_count")
        slots: set[int] = set()
        subjects: set[str] = set()
        nondeparted = 0
        for entry in current.entries:
            if _uint(entry.fields["query_at_ms"], "query_at_ms", entry.source, entry.line_number) != query_at:
                raise _error(entry.source, entry.line_number, "has GTT entry query_at_ms mismatch")
            slot = _uint(entry.fields["slot"], "slot", entry.source, entry.line_number)
            if slot >= capacity or slot in slots:
                raise _error(entry.source, entry.line_number, "has duplicate or out-of-range GTT slot")
            slots.add(slot)
            subject = entry.fields["adva"].lower()
            if subject in subjects:
                raise _error(entry.source, entry.line_number, "has duplicate GTT subject")
            subjects.add(subject)
            departed = _uint(entry.fields["departed"], "departed",
                             entry.source, entry.line_number)
            if departed not in (1, 2):
                raise _error(entry.source, entry.line_number,
                             "has invalid GTT departed enum")
            if departed == 1:
                nondeparted += 1
        if expected_nondeparted != nondeparted:
            raise _error(end.source, end.line_number, "has partial GTT snapshot nondeparted_count")
        return True

    def _cumulative_counters(self, record: ObservationRecord, state: _RoleClock,
                             kind: str) -> None:
        values = _counter_fields(record)
        previous = state.last_control_values if kind == "control" else state.last_health_values
        for name, value in values.items():
            if kind == "health" and name in HEALTH_GAUGES:
                continue
            if name in previous and value < previous[name]:
                raise _error(record.source, record.line_number,
                             f"has decreasing cumulative {kind} counter {name}")
        if kind == "control":
            state.last_control_values = values
        else:
            state.last_health_values = values

    def _health(self, record: ObservationRecord, state: _RoleClock) -> None:
        sequence = _uint64(record.fields["record_seq"], "record_seq", record.source, record.line_number)
        if sequence != (_record_id(record.fields, record.source, record.line_number) & UINT32_MAX):
            raise _error(record.source, record.line_number,
                         "has record_seq different from low 32 bits of record ID")
        self._cumulative_counters(record, state, "health")
        previous_host = state.last_health_host_ms or record.host_ms
        numbers = _counter_fields(record)
        integrity = {key: value for key, value in numbers.items()
                     if any(marker in key.lower() for marker in INTEGRITY_COUNTER_MARKERS)}
        for key, value in integrity.items():
            previous = state.last_integrity_values.get(key, 0)
            if value > previous:
                self._add_invalid_interval({"role": record.role, "start_ms": previous_host,
                                            "end_ms": record.host_ms,
                                            "reason": f"telemetry_integrity:{key}"})
        state.last_integrity_values = integrity
        state.last_health_seq = sequence
        state.last_health_host_ms = record.host_ms
        if self.record_store is None:
            self.health_events.append({"role": record.role, "session": record.session,
                                       "host_ms": record.host_ms, "record_seq": sequence, **numbers})

    def clock_fits(self) -> dict[str, dict[str, Any]]:
        result: dict[str, dict[str, Any]] = {}
        for role in ROLES:
            sessions: dict[str, list[tuple[float, float]]] = {}
            for record in self.records:
                if record.role == role and record.kind == "clock":
                    sessions.setdefault(record.session, []).append((record.now_unwrapped, record.host_ms))
            result[role] = {session: _fit(points) for session, points in sorted(sessions.items())}
        return result

    def host_from_firmware(self, record: ObservationRecord, fits: dict[str, dict[str, Any]]) -> float | None:
        fit = fits.get(record.role, {}).get(record.session)
        if not fit or not fit.get("valid"):
            return None
        return float(fit["intercept_ms"]) + float(fit["slope"]) * record.now_unwrapped

    def host_from_event(self, record: ObservationRecord, fits: dict[str, dict[str, Any]]) -> float | None:
        fit = fits.get(record.role, {}).get(record.session)
        if record.event_at_unwrapped is None or not fit or not fit.get("valid"):
            return None
        return float(fit["intercept_ms"]) + float(fit["slope"]) * record.event_at_unwrapped

    def completed_gtt_pairs(self) -> list[dict[str, Any]]:
        """Return completed GTT pairs, rebuilding them from durable records.

        In-memory callers retain the legacy derived lists.  The disk-backed
        path reconstructs this snapshot-only view so repeated GTT entries do
        not remain as duplicate Python objects during continuous capture.
        """
        if self.record_store is None:
            return self.gtt_pairs
        open_snapshots: dict[tuple[str, str], _GttOpen] = {}
        pairs: list[dict[str, Any]] = []
        for record in self.records:
            key = (record.role, record.session)
            if record.kind == "gtt_begin":
                open_snapshots[key] = _GttOpen(record)
            elif record.kind == "gtt_entry":
                current = open_snapshots.get(key)
                if current is not None:
                    current.entries.append(record)
            elif record.kind == "gtt_end":
                current = open_snapshots.pop(key, None)
                if current is not None:
                    pairs.append({"role": record.role, "session": record.session,
                                  "begin": current.record, "end": record,
                                  "entries": current.entries,
                                  "status": record.fields["status"]})
        return pairs


def _metadata_roles(metadata: dict[str, Any]) -> tuple[str, str]:
    observation = metadata.get("observation", metadata.get("benchmark", {}))
    if not isinstance(observation, dict):
        observation = {}
    source_role = observation.get("source_role", "A")
    destination_role = observation.get("destination_role", "C")
    if source_role != "A" or destination_role != "C":
        raise CaptureError("observation roles must be source A and destination C")
    return source_role, destination_role


def _profile(metadata: dict[str, Any]) -> str:
    profile = metadata.get("profile")
    if profile not in {"AODV_ONLY", "FULL_TAVRN"}:
        raise CaptureError("run metadata profile must be AODV_ONLY or FULL_TAVRN")
    return str(profile)


def _boards_by_role(metadata: dict[str, Any]) -> dict[str, dict[str, Any]]:
    boards = metadata.get("boards")
    if not isinstance(boards, list) or len(boards) != len(ROLES):
        raise CaptureError("run metadata must contain exactly six boards")
    result: dict[str, dict[str, Any]] = {}
    for board in boards:
        if not isinstance(board, dict) or board.get("role") not in ROLES or board["role"] in result:
            raise CaptureError("run metadata must map roles A through F exactly")
        adva = board.get("adva")
        if not isinstance(adva, str) or ADVA_RE.fullmatch(adva) is None:
            raise CaptureError(f"run metadata board {board['role']} has invalid canonical AdvA")
        result[board["role"]] = board
    if set(result) != set(ROLES):
        raise CaptureError("run metadata must map roles A through F exactly")
    return result


def _logical_id(board: dict[str, Any], profile: str) -> int:
    adva = str(board["adva"])
    octets = [int(value, 16) for value in adva.split(":")]
    return octets[0] | (octets[1] << 8) if profile == "AODV_ONLY" else octets[0]


def _window_start(host_ms: float, origin_ms: float, seconds: int = 10) -> int:
    return int(math.floor((host_ms - origin_ms) / (seconds * 1000))) * seconds


def _event_key(record: ObservationRecord, session: str | None = None) -> tuple[str, str, str, int]:
    return (session if session is not None else record.session, record.fields["workload"], record.fields["burst"],
            _uint(record.fields["payload_id"], "payload_id", record.source, record.line_number))


def _latency_summary(values: list[float]) -> dict[str, float | None]:
    if not values:
        return {"p50_ms": None, "p95_ms": None, "p99_ms": None}
    return {"p50_ms": round(nearest_rank(values, 50), 6), "p95_ms": round(nearest_rank(values, 95), 6),
            "p99_ms": round(nearest_rank(values, 99), 6)}


def _bool_field(record: ObservationRecord, name: str) -> int:
    return _uint(record.fields[name], name, record.source, record.line_number)


def _invalid_roles(start_ms: float, end_ms: float, invalid: list[dict[str, Any]]) -> list[str]:
    return sorted({str(item["role"]) for item in invalid
                   if float(item["start_ms"]) <= end_ms and float(item["end_ms"]) >= start_ms})


def _uint32_elapsed(later: int, earlier: int) -> int:
    return (later - earlier) & UINT32_MAX


def _expected_offer_deadline(started_at_ms: int, record: ObservationRecord) -> tuple[int, int]:
    """Return the exact fixed deadline and slot interval for a source offer."""
    sequence = _uint(record.fields["sequence"], "sequence", record.source, record.line_number)
    burst = _uint(record.fields["burst"], "burst", record.source, record.line_number)
    if record.fields["workload"] == "heartbeat":
        global_sequence = (burst << 10) | sequence
        return ((started_at_ms + global_sequence * HEARTBEAT_INTERVAL_MS) & UINT32_MAX,
                HEARTBEAT_INTERVAL_MS)
    return ((started_at_ms + THROUGHPUT_START_MS + burst * THROUGHPUT_BURST_PERIOD_MS +
             sequence * THROUGHPUT_INTERVAL_MS) & UINT32_MAX, THROUGHPUT_INTERVAL_MS)


def _app_metrics(state: ObservationState, fits: dict[str, dict[str, Any]], origin_ms: float) -> tuple[
        list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]]]:
    source_role, destination_role = _metadata_roles(state.metadata)
    if source_role not in ROLES or destination_role not in ROLES or source_role == destination_role:
        raise CaptureError("observation source/destination roles must be distinct A..F roles")
    offers = [record for record in state.records if record.kind == "offer" and record.role == source_role]
    apps = [record for record in state.records if record.kind == "app" and record.role == source_role]
    finals = [record for record in state.records if record.kind == "final" and record.role == destination_role]
    issues: list[dict[str, Any]] = []

    def invalidate(record: ObservationRecord, reason: str) -> None:
        issues.append({"role": record.role, "start_ms": record.host_ms, "end_ms": record.host_ms,
                       "reason": reason})

    for record in state.records:
        if record.kind in {"offer", "app"} and record.role != source_role:
            invalidate(record, f"unexpected_{record.kind}_role")
        if record.kind == "final" and record.role != destination_role:
            invalidate(record, "unexpected_final_role")

    profile = _profile(state.metadata)
    boards = _boards_by_role(state.metadata)
    expected_source_destination = _logical_id(boards[destination_role], profile)
    expected_source_width = 1 if profile == "FULL_TAVRN" else 2
    expected_source_destination_text = f"0x{expected_source_destination:04x}"

    def validate_source_attempt(record: ObservationRecord) -> None:
        attempted = _bool_field(record, "attempted")
        accepted = _bool_field(record, "accepted")
        status = _uint(record.fields["status"], "status", record.source, record.line_number)
        if attempted:
            if record.fields["destination"] != expected_source_destination_text or \
                    _uint(record.fields["width"], "width", record.source, record.line_number) != expected_source_width:
                invalidate(record, "invalid_source_destination_width")
            if record.kind == "offer":
                if accepted != 0 or status != 0:
                    invalidate(record, "invalid_offer_submission_status")
            elif status > AODV_STATUS_MAX or accepted != int(status in AODV_ACCEPTED_STATUSES):
                invalidate(record, "invalid_app_submission_status")
            return
        if profile != "FULL_TAVRN" or record.fields["destination"] != "0x0000" or \
                _uint(record.fields["width"], "width", record.source, record.line_number) != 1 or \
                accepted != 0 or status != NOT_READY_STATUS:
            invalidate(record, "invalid_not_ready_source_attempt")

    for record in [*offers, *apps]:
        validate_source_attempt(record)

    boot_started_at: dict[str, int] = {}
    for record in state.records:
        if record.kind == "boot" and record.role == source_role:
            boot_started_at[record.session] = _uint(record.fields["started_at_ms"], "started_at_ms",
                                                    record.source, record.line_number)
    expected_heartbeat: dict[str, int] = {}
    expected_throughput: dict[str, tuple[int, int]] = {}
    previous_deadline: dict[str, int] = {}
    previous_event_at: dict[str, int] = {}
    for offer in offers:
        started_at_ms = boot_started_at.get(offer.session)
        if started_at_ms is None:
            invalidate(offer, "source_offer_without_started_at")
            continue
        expected_deadline, interval_ms = _expected_offer_deadline(started_at_ms, offer)
        deadline = _uint(offer.fields["deadline_ms"], "deadline_ms", offer.source, offer.line_number)
        event_at = _uint(offer.fields["event_at_ms"], "event_at_ms", offer.source, offer.line_number)
        if deadline != expected_deadline:
            invalidate(offer, "source_offer_deadline_mismatch")
        elapsed = _uint32_elapsed(event_at, deadline)
        if elapsed >= HALF_RANGE or elapsed >= interval_ms:
            invalidate(offer, "stale_or_early_source_offer")
        if offer.session in previous_deadline:
            deadline_delta = _uint32_elapsed(deadline, previous_deadline[offer.session])
            event_delta = _uint32_elapsed(event_at, previous_event_at[offer.session])
            if deadline_delta >= HALF_RANGE or event_delta >= HALF_RANGE:
                invalidate(offer, "stale_or_catch_up_source_order")
        previous_deadline[offer.session] = deadline
        previous_event_at[offer.session] = event_at
        sequence = _uint(offer.fields["sequence"], "sequence", offer.source, offer.line_number)
        burst = _uint(offer.fields["burst"], "burst", offer.source, offer.line_number)
        if offer.fields["workload"] == "heartbeat":
            global_sequence = (burst << 10) | sequence
            expected = expected_heartbeat.get(offer.session, 0)
            if global_sequence != expected:
                invalidate(offer, "heartbeat_identity_not_contiguous")
            expected_heartbeat[offer.session] = global_sequence + 1
            continue
        if sequence >= THROUGHPUT_BURST_SLOT_COUNT:
            invalidate(offer, "throughput_sequence_out_of_range")
        expected_burst, expected_sequence = expected_throughput.get(offer.session, (0, 0))
        if (burst, sequence) != (expected_burst, expected_sequence):
            invalidate(offer, "throughput_sequence_gap_or_repetition")
        expected_throughput[offer.session] = ((burst + 1, 0) if sequence == THROUGHPUT_BURST_SLOT_COUNT - 1
                                               else (burst, sequence + 1))

    offers_by_id: dict[tuple[str, int], ObservationRecord] = {}
    for offer in offers:
        offer_id = _record_id(offer.fields, offer.source, offer.line_number)
        key = (offer.session, offer_id)
        if key in offers_by_id:
            invalidate(offer, "duplicate_offer_record_id")
            continue
        offers_by_id[key] = offer
    apps_by_offer_id: dict[tuple[str, int], ObservationRecord] = {}
    for app in apps:
        offer_id = _record_id({"record_id_hi": app.fields["offer_record_id_hi"],
                                "record_id_lo": app.fields["offer_record_id_lo"]}, app.source, app.line_number)
        key = (app.session, offer_id)
        offer = offers_by_id.get(key)
        if offer is None:
            invalidate(app, "unmatched_source_app")
            continue
        if key in apps_by_offer_id:
            invalidate(app, "duplicate_source_app")
            continue
        matching_fields = ("session", "workload", "burst", "sequence", "payload_id", "identity",
                           "deadline_ms", "destination", "width", "attempted")
        if _event_key(app) != _event_key(offer) or any(
                app.fields[name] != offer.fields[name] for name in matching_fields):
            invalidate(app, "source_app_identity_mismatch")
            continue
        apps_by_offer_id[key] = app
    accepted: dict[tuple[str, str, str, int], ObservationRecord] = {}
    censored_offer_ids: set[tuple[str, int]] = set()
    source_log = boards[source_role].get("log")
    source_terminal_censored = bool(
        isinstance(source_log, dict) and source_log.get("terminal_censored"))
    for record in offers:
        record_key = (record.session, _record_id(record.fields, record.source, record.line_number))
        app = apps_by_offer_id.get(record_key)
        if app is None:
            boundary_delta = ((state.finalized_host_ms - record.host_ms)
                              if state.finalized_host_ms is not None else float("inf"))
            if source_terminal_censored and record is offers[-1] and 0.0 <= boundary_delta <= 1000.0:
                censored_offer_ids.add(record_key)
                continue
            invalidate(record, "missing_source_app")
            continue
        if _bool_field(app, "accepted") == 1:
            key = _event_key(record)
            if key in accepted:
                invalidate(record, "duplicate_accepted_offer")
                continue
            accepted[key] = record
    final_groups: dict[tuple[str, str, str, int], list[ObservationRecord]] = {}
    for record in finals:
        final_groups.setdefault(_event_key(record, record.fields["origin_session"]), []).append(record)
    delivered: dict[tuple[str, str, str, int], ObservationRecord] = {}
    expected_origin: int | None = None
    expected_destination: int | None = None
    if final_groups:
        profile = _profile(state.metadata)
        boards = _boards_by_role(state.metadata)
        expected_origin = _logical_id(boards["A"], profile)
        expected_destination = _logical_id(boards["C"], profile)
    for key, candidates in final_groups.items():
        if len(candidates) != 1:
            for record in candidates:
                invalidate(record, "duplicate_final")
            continue
        final = candidates[0]
        if _bool_field(final, "identity_valid") != 1:
            invalidate(final, "invalid_final_identity")
            continue
        if _uint(final.fields["app_kind"], "app_kind", final.source, final.line_number) != 127 or \
                _uint(final.fields["app_len"], "app_len", final.source, final.line_number) != 7:
            invalidate(final, "invalid_final_app_contract")
            continue
        assert expected_origin is not None and expected_destination is not None
        if final.fields["origin"] != f"0x{expected_origin:04x}" or \
                final.fields["destination"] != f"0x{expected_destination:04x}":
            invalidate(final, "invalid_final_origin_destination")
            continue
        if key not in accepted:
            invalidate(final, "unmatched_final")
            continue
        delivered[key] = final
    packets: list[dict[str, Any]] = []
    for offer in offers:
        if (offer.session, _record_id(offer.fields, offer.source, offer.line_number)) in censored_offer_ids:
            continue
        key = _event_key(offer)
        final = delivered.get(key)
        app = apps_by_offer_id.get((offer.session, _record_id(offer.fields, offer.source, offer.line_number)))
        offered_host = state.host_from_event(offer, fits)
        final_host = state.host_from_event(final, fits) if final else None
        latency = final_host - offered_host if final_host is not None and offered_host is not None else None
        if latency is not None and latency < 0:
            raise _error(final.source, final.line_number, "has negative fitted latency")
        packets.append({
            "role": offer.role, "session": offer.session, "workload": offer.fields["workload"],
            "burst": offer.fields["burst"], "offer": _uint(offer.fields["offer"], "offer", offer.source, offer.line_number),
            "payload_id": key[3], "offered_host_ms": round(offered_host, 6) if offered_host is not None else None,
            "attempted": _bool_field(offer, "attempted"), "accepted": _bool_field(app, "accepted") if app else 0,
            "status": app.fields["status"] if app else "MISSING_APP", "delivered": int(final is not None),
            "delivered_host_ms": round(final_host, 6) if final_host is not None else None,
            "latency_ms": round(latency, 6) if latency is not None else None,
            "app_bytes": _uint(final.fields["app_len"], "app_len", final.source, final.line_number) if final else 0,
        })
    for key, candidates in final_groups.items():
        if key not in accepted:
            final = candidates[0]
            packets.append({"role": source_role, "session": key[0], "workload": key[1], "burst": key[2],
                            "offer": None, "payload_id": key[3], "offered_host_ms": None, "attempted": None,
                            "accepted": None, "status": "UNMATCHED_FINAL", "delivered": 0,
                            "delivered_host_ms": round(state.host_from_event(final, fits), 6)
                            if state.host_from_event(final, fits) is not None else None,
                            "latency_ms": None, "app_bytes": 0})
    packets.sort(key=lambda item: (item["workload"], item["burst"], item["payload_id"],
                                   item["offer"] is None, item["offer"] or -1))
    heartbeat: dict[int, list[dict[str, Any]]] = {}
    bursts: dict[str, list[dict[str, Any]]] = {}
    for packet in packets:
        if packet["offered_host_ms"] is None:
            continue
        if packet["workload"] == "heartbeat":
            heartbeat.setdefault(_window_start(float(packet["offered_host_ms"]), origin_ms), []).append(packet)
        elif packet["workload"] == "throughput":
            bursts.setdefault(str(packet["burst"]), []).append(packet)
    windows = [_metric_row("heartbeat", str(start), start, rows,
                           origin_ms + start * 1000.0, HEARTBEAT_WINDOW_SECONDS, state.invalid_intervals + issues)
               for start, rows in sorted(heartbeat.items())]
    burst_rows: list[dict[str, Any]] = []
    for burst, rows in sorted(bursts.items()):
        starts = [float(item["offered_host_ms"]) for item in rows]
        burst_rows.append(_metric_row("throughput", burst, _window_start(min(starts), origin_ms), rows,
                                      min(starts), THROUGHPUT_BURST_SECONDS, state.invalid_intervals + issues))
    return packets, windows, burst_rows, issues


def _metric_row(workload: str, label: str, window: int, rows: list[dict[str, Any]], period_start_ms: float,
                duration: float, invalid: list[dict[str, Any]]) -> dict[str, Any]:
    offered = len(rows)
    attempted = sum(int(item["attempted"] or 0) for item in rows)
    accepted = sum(int(item["accepted"] or 0) for item in rows)
    delivered = sum(int(item["delivered"] or 0) for item in rows)
    app_bytes = sum(int(item["app_bytes"] or 0) for item in rows)
    latencies = [float(item["latency_ms"]) for item in rows if item["latency_ms"] is not None]
    starts = [float(item["offered_host_ms"]) for item in rows if item["offered_host_ms"] is not None]
    ends = [float(item["delivered_host_ms"]) for item in rows if item["delivered_host_ms"] is not None]
    not_ready = sum(1 for item in rows if item["status"].upper() in {"NOT_READY", "4294967295"})
    rejected = sum(1 for item in rows if item["accepted"] == 0)
    invalid_roles = _invalid_roles(period_start_ms, period_start_ms + duration * 1000.0, invalid)
    return {
        "workload": workload, "label": label, "window_start_s": window, "duration_seconds": round(duration, 6),
        "offered": offered, "attempted": attempted, "accepted": accepted, "delivered": delivered,
        "accepted_pdr": round(delivered / accepted, 6) if accepted else None,
        "offered_pdr": round(delivered / offered, 6) if offered else None,
        "packet_goodput_packets_per_second": round(delivered / duration, 6),
        "app_goodput_bytes_per_second": round(app_bytes / duration, 6),
        "first_delivery_ms": round(min(ends) - min(starts), 6) if starts and ends else None,
        "not_ready": not_ready, "rejected": rejected, "invalid": bool(invalid_roles), "invalid_roles": invalid_roles,
        **_latency_summary(latencies),
    }


def _subject_role(value: str, boards: dict[str, dict[str, Any]]) -> str | None:
    for role, board in boards.items():
        if value in {role.lower(), str(ROLES.index(role) + 1), str(board.get("adva", "")).lower()}:
            return role
    return None


def _gtt_metrics(state: ObservationState, origin_ms: float, invalid: list[dict[str, Any]],
                 gtt_pairs: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]]]:
    profile = _profile(state.metadata)
    if profile == "AODV_ONLY":
        return [], [{"window_start_s": 0, "status": "N/A_AODV_GTT",
                     "invalid": False, "invalid_roles": []}], []
    snapshots = [pair for pair in gtt_pairs if pair["status"] == "OK"]
    if not snapshots:
        return [], [], []
    boards = _boards_by_role(state.metadata)
    entries_rows: list[dict[str, Any]] = []
    fleet_rows: list[dict[str, Any]] = []
    by_window: dict[int, list[dict[str, Any]]] = {}
    previous: dict[str, set[str]] = {}
    for snapshot in sorted(snapshots, key=lambda item: (item["end"].host_ms, item["role"])):
        role = snapshot["role"]
        truth = set(ROLES) - {role}
        known: set[str] = set()
        fresh: set[str] = set()
        ages: list[int] = []
        for entry in snapshot["entries"]:
            raw_subject = entry.fields["adva"].lower()
            subject = _subject_role(raw_subject, boards)
            observed_subject = subject if subject is not None else f"unknown:{raw_subject}"
            if subject == role:
                continue
            known.add(observed_subject)
            freshness = _uint(entry.fields["freshness"], "freshness", entry.source, entry.line_number)
            stale = entry.fields["departed"] == "2" or freshness != 1
            if not stale:
                fresh.add(observed_subject)
            query_at = _uint(entry.fields["query_at_ms"], "query_at_ms", entry.source, entry.line_number)
            last_evidence = _uint(entry.fields["last_evidence_ms"], "last_evidence_ms",
                                  entry.source, entry.line_number)
            age_value = (query_at - last_evidence) & UINT32_MAX
            if age_value >= HALF_RANGE:
                raise _error(entry.source, entry.line_number,
                             "has last_evidence_ms after snapshot query_at_ms")
            age = str(age_value)
            ages.append(age_value)
            entries_rows.append({"window_start_s": _window_start(snapshot["end"].host_ms, origin_ms),
                                   "role": role, "session": snapshot["session"], "subject": raw_subject,
                                   "subject_role": subject or "",
                                   "fresh": int(not stale), "evidence_age_ms": age,
                                   "snapshot_host_ms": round(snapshot["end"].host_ms, 6)})
        overlap = known & truth
        union = known | truth
        churn = len(known ^ previous.get(role, set())) if role in previous else 0
        previous[role] = known
        invalid_roles = _invalid_roles(snapshot["begin"].host_ms, snapshot["end"].host_ms, invalid)
        row = {"window_start_s": _window_start(snapshot["end"].host_ms, origin_ms), "role": role,
               "precision": round(len(overlap) / len(known), 6) if known else None,
               "recall": round(len(overlap) / len(truth), 6) if truth else None,
               "jaccard": round(len(overlap) / len(union), 6) if union else 1.0,
               "truth_stale_ratio": round(len(truth - fresh) / len(truth), 6) if truth else 0.0,
               "stale_ratio": round(len(known - fresh) / len(known), 6) if known else None,
               "evidence_age_mean_ms": round(sum(ages) / len(ages), 6) if ages else None,
                "churn": churn, "known": len(known), "truth": len(truth), "known_set": sorted(known),
                "invalid": bool(invalid_roles), "invalid_roles": invalid_roles,
                "snapshot_start_host_ms": round(snapshot["begin"].host_ms, 6),
                "snapshot_end_host_ms": round(snapshot["end"].host_ms, 6)}
        by_window.setdefault(row["window_start_s"], []).append(row)
    for window, rows in sorted(by_window.items()):
        # GTT truth excludes the reporting node itself.  Restore that known
        # self-membership solely for a fair pairwise fleet-agreement comparison.
        present_sets = [set(row["known_set"]) | {row["role"]} for row in rows]
        pairs = []
        for index, first in enumerate(present_sets):
            for second in present_sets[index + 1:]:
                pairs.append(len(first & second) / len(first | second) if first | second else 1.0)
        reporting_roles = {str(row["role"]) for row in rows}
        six_unique_roles = reporting_roles == set(ROLES) and len(rows) == len(ROLES)
        fleet_start = min(float(row["snapshot_start_host_ms"]) for row in rows)
        fleet_end = max(float(row["snapshot_end_host_ms"]) for row in rows)
        invalid_roles = _invalid_roles(fleet_start, fleet_end, invalid)
        invalid_roles = sorted(set(invalid_roles).union(
            *(set(row["invalid_roles"]) for row in rows)))
        fleet_rows.append({"window_start_s": window,
                           "status": "OK" if six_unique_roles else "INCOMPLETE_FLEET",
                           "nodes": len(reporting_roles), "reporting_roles": sorted(reporting_roles),
                           "precision": _mean(row["precision"] for row in rows),
                           "recall": _mean(row["recall"] for row in rows),
                           "jaccard": _mean(row["jaccard"] for row in rows),
                           "truth_stale_ratio": _mean(row["truth_stale_ratio"] for row in rows),
                           "stale_ratio": _mean(row["stale_ratio"] for row in rows),
                           "evidence_age_mean_ms": _mean(row["evidence_age_mean_ms"] for row in rows),
                           "churn": sum(int(row["churn"]) for row in rows),
                            "pairwise_agreement": round(sum(pairs) / len(pairs), 6) if pairs else None,
                            "invalid": bool(invalid_roles), "invalid_roles": invalid_roles,
                            "converged": int(six_unique_roles and all(
                               row["precision"] == 1.0 and row["recall"] == 1.0 for row in rows))})
    return entries_rows, fleet_rows, [row for rows in by_window.values() for row in rows]


def _mean(values: Iterable[float | None]) -> float | None:
    present = [float(value) for value in values if value is not None]
    return round(sum(present) / len(present), 6) if present else None


def _control_metrics(state: ObservationState, origin_ms: float,
                     invalid: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    previous: dict[tuple[str, str], ObservationRecord] = {}
    for record in sorted((item for item in state.records if item.kind == "control"), key=lambda item: item.host_ms):
        key = (record.role, record.session)
        prior = previous.get(key)
        previous[key] = record
        if prior is None:
            continue
        elapsed = (record.host_ms - prior.host_ms) / 1000.0
        if elapsed <= 0:
            continue
        current_values, prior_values = _counter_fields(record), _counter_fields(prior)
        for name, value in sorted(current_values.items()):
            if name not in prior_values:
                continue
            delta = value - prior_values[name]
            kind = "bytes" if "byte" in name else ("scheduler_proxy" if "sched" in name else "messages")
            invalid_roles = _invalid_roles(prior.host_ms, record.host_ms, invalid)
            rows.append({"window_start_s": _window_start(prior.host_ms, origin_ms), "role": record.role,
                          "session": record.session, "counter": name, "kind": kind, "delta": delta,
                          "elapsed_seconds": round(elapsed, 6), "rate_per_second": round(delta / elapsed, 6),
                          "interval_start_host_ms": round(prior.host_ms, 6),
                          "interval_end_host_ms": round(record.host_ms, 6),
                          "invalid": bool(invalid_roles), "invalid_roles": invalid_roles,
                          "discovery_proxy": int("rreq" in name or "discovery" in name or "ring" in name),
                         "retry_proxy": int("retry" in name), "backpressure_proxy": int("backpressure" in name or "queue" in name),
                         "fault_proxy": int("fault" in name)})
    return rows


def _app_cumulative(state: ObservationState, origin_ms: float) -> list[dict[str, Any]]:
    """Application events are identities, not cumulative counters."""
    return []


def _health_metrics(state: ObservationState, origin_ms: float) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    events = state.health_events if state.record_store is None else (
        {"role": record.role, "session": record.session, "host_ms": record.host_ms,
         "record_seq": _uint64(record.fields["record_seq"], "record_seq", record.source,
                               record.line_number), **_counter_fields(record)}
        for record in state.records if record.kind == "health")
    invalid_intervals = state.invalid_intervals
    for event in events:
        invalid_roles = _invalid_roles(float(event["host_ms"]), float(event["host_ms"]),
                                       invalid_intervals)
        rows.append({"window_start_s": _window_start(float(event["host_ms"]), origin_ms), **event,
                     "invalid": bool(invalid_roles), "invalid_roles": invalid_roles})
    return rows


def _cadence_reason(role: str, kind: str, boot: ObservationRecord,
                    samples: list[ObservationRecord], end_ms: float, limit_ms: float) -> str | None:
    if not samples:
        return f"missing_{kind}:{role}"
    anchors = [boot.host_ms, *(record.host_ms for record in samples), end_ms]
    if any(later - earlier > limit_ms for earlier, later in zip(anchors, anchors[1:])):
        return f"{kind}_cadence_gap:{role}"
    return None


def _incomplete_evidence_reasons(state: ObservationState, fits: dict[str, dict[str, Any]],
                                 packets: list[dict[str, Any]],
                                 gtt_pairs: list[dict[str, Any]]) -> list[str]:
    if state.finalized_host_ms is None:
        return ["capture_not_finalized"]
    reasons: list[str] = []
    end_ms = state.finalized_host_ms
    profile = _profile(state.metadata)
    source_role, _ = _metadata_roles(state.metadata)
    for role in ROLES:
        session = state.role_state[role].session
        if session is None:
            reasons.append(f"missing_boot:{role}")
            continue
        session_records = [record for record in state.records
                           if record.role == role and record.session == session]
        boots = [record for record in session_records if record.kind == "boot"]
        if len(boots) != 1:
            reasons.append(f"missing_boot:{role}")
            continue
        boot = boots[0]
        fit = fits.get(role, {}).get(session)
        if not fit or not fit.get("valid"):
            reasons.append(f"invalid_clock_fit:{role}")
        clock_reason = _cadence_reason(role, "clock", boot,
                                       [record for record in session_records if record.kind == "clock"],
                                       end_ms, CLOCK_CADENCE_MS)
        if clock_reason:
            reasons.append(clock_reason)
        for kind in ("control", "health"):
            reason = _cadence_reason(role, kind, boot,
                                     [record for record in session_records if record.kind == kind],
                                     end_ms, TELEMETRY_CADENCE_MS)
            if reason:
                reasons.append(reason)
        pairs = [pair for pair in gtt_pairs if pair["role"] == role and pair["session"] == session]
        gtt_samples = [pair["end"] for pair in pairs]
        reason = _cadence_reason(role, "gtt", boot, gtt_samples, end_ms, TELEMETRY_CADENCE_MS)
        if reason:
            reasons.append(reason)
        if profile == "FULL_TAVRN":
            if not any(pair["status"] == "OK" for pair in pairs):
                reasons.append(f"missing_full_gtt_snapshot:{role}")
        elif not any(pair["status"] == "NOT_IMPLEMENTED" for pair in pairs):
            reasons.append(f"missing_aodv_gtt_pair:{role}")
    source_session = state.role_state[source_role].session
    source_offers = [record for record in state.records
                     if record.kind == "offer" and record.role == source_role and
                     record.session == source_session]
    heartbeat_offers = [record for record in source_offers
                        if record.fields["workload"] == "heartbeat"]
    heartbeat_sequences = [
        (_uint(record.fields["burst"], "burst", record.source, record.line_number) << 10) |
        _uint(record.fields["sequence"], "sequence", record.source, record.line_number)
        for record in heartbeat_offers]
    if heartbeat_sequences[:HEARTBEAT_MINIMUM_OFFERS] != list(range(HEARTBEAT_MINIMUM_OFFERS)):
        reasons.append(f"incomplete_contiguous_heartbeat_offers:{source_role}")
    throughput_zero_sequences = {
        _uint(record.fields["sequence"], "sequence", record.source, record.line_number)
        for record in source_offers
        if record.fields["workload"] == "throughput" and
        _uint(record.fields["burst"], "burst", record.source, record.line_number) == 0
    }
    if throughput_zero_sequences != set(range(THROUGHPUT_BURST_SLOT_COUNT)):
        reasons.append(f"incomplete_throughput_burst_0:{source_role}")
    source_packets = [packet for packet in packets if packet["role"] == source_role and
                      packet["offer"] is not None and packet["session"] == source_session]
    if not source_packets:
        reasons.append(f"missing_source_offer:{source_role}")
    if not any(packet["status"] != "MISSING_APP" for packet in source_packets):
        reasons.append(f"missing_source_app:{source_role}")
    return sorted(set(reasons))


def snapshot(state: ObservationState) -> dict[str, Any]:
    """Build a deterministic summary without consuming or mutating live state."""
    state.flush()
    if state.gtt_open:
        # An open snapshot is normal during live operation and intentionally not
        # treated as a malformed capture until the next begin/end contradicts it.
        pass
    origin = min((record.host_ms for record in state.records), default=0.0)
    fits = state.clock_fits()
    packets, heartbeat, bursts, correlation_invalid = _app_metrics(state, fits, origin)
    all_invalid_intervals = state.invalid_intervals + correlation_invalid
    gtt_pairs = state.completed_gtt_pairs()
    gtt_entries, gtt_fleet, gtt_nodes = _gtt_metrics(state, origin, all_invalid_intervals, gtt_pairs)
    control = _control_metrics(state, origin, all_invalid_intervals)
    app_cumulative = _app_cumulative(state, origin)
    health = _health_metrics(state, origin)
    telemetry_corrupt = bool(all_invalid_intervals)
    incomplete_reasons = _incomplete_evidence_reasons(state, fits, packets, gtt_pairs)
    if telemetry_corrupt and incomplete_reasons:
        status = "INVALID_TELEMETRY_INCOMPLETE_EVIDENCE"
    elif telemetry_corrupt:
        status = "INVALID_TELEMETRY"
    elif incomplete_reasons:
        status = "INCOMPLETE_EVIDENCE"
    else:
        status = "VALID"
    return {
        "schema": SCHEMA, "status": status, "record_count": len(state.records),
        "schemas": sorted(state.schemas), "invalid_intervals": sorted(all_invalid_intervals, key=lambda item: (item["role"], item["start_ms"], item["reason"])),
        "clock_fits": fits, "app_packets": packets, "app_windows": heartbeat,
        "throughput_bursts": bursts, "gtt_entries": gtt_entries, "gtt_fleet": gtt_fleet,
        "gtt_nodes": gtt_nodes, "control_deltas": control, "app_cumulative": app_cumulative, "telemetry_health": health,
        "telemetry_corruption": telemetry_corrupt, "incomplete_reasons": incomplete_reasons,
        "evidence_complete": not incomplete_reasons, "finalized_host_ms": state.finalized_host_ms,
        "proving_status": "INVALID" if telemetry_corrupt else
        ("INCOMPLETE" if incomplete_reasons else "VALID"),
    }


def render_markdown(data: dict[str, Any]) -> str:
    heartbeat = data["app_windows"][-1] if data["app_windows"] else {}
    lines = ["# TAVRN live observation", "", f"Status: **{data['proving_status']}**", "",
             "| Item | Value |", "| --- | ---: |",
             f"| Parsed records | {data['record_count']} |",
             f"| Invalid telemetry intervals | {len(data['invalid_intervals'])} |",
             f"| Heartbeat offered/accepted/delivered | {heartbeat.get('offered', 0)}/{heartbeat.get('accepted', 0)}/{heartbeat.get('delivered', 0)} |",
             f"| Heartbeat accepted/offered PDR | {heartbeat.get('accepted_pdr')}/{heartbeat.get('offered_pdr')} |",
             f"| Heartbeat p50/p95/p99 latency (ms) | {heartbeat.get('p50_ms')}/{heartbeat.get('p95_ms')}/{heartbeat.get('p99_ms')} |",
             f"| Throughput bursts | {len(data['throughput_bursts'])} |",
             f"| GTT fleet samples | {len(data['gtt_fleet'])} |",
             f"| Control delta rows | {len(data['control_deltas'])} |",
             "", "Clock fits:", ""]
    for role in ROLES:
        sessions = data["clock_fits"].get(role, {})
        summary = ", ".join(f"{session}:{fit.get('reason')}" for session, fit in sorted(sessions.items())) or "none"
        lines.append(f"- {role}: {summary}")
    return "\n".join(lines) + "\n"


def _write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    fields = sorted({key for row in rows for key in row})
    with path.open("w", encoding="utf-8", newline="") as target:
        writer = csv.DictWriter(target, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: row.get(key, "") for key in fields})


def generate_pngs(data: dict[str, Any], output_dir: Path) -> tuple[bool, str | None]:
    """Write optional dashboard PNGs using only pinned optional matplotlib."""
    try:
        import matplotlib
        if matplotlib.__version__ != "3.11.1":
            return False, f"requires matplotlib==3.11.1, found {matplotlib.__version__}"
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except (ImportError, RuntimeError) as error:
        return False, f"{MATPLOTLIB_PIN} unavailable: {error}; use uv run --with {MATPLOTLIB_PIN} --no-project python ..."
    try:
        def plot_rows(rows: list[dict[str, Any]], x: str, series: list[str], path: str, title: str) -> None:
            figure, axis = plt.subplots(figsize=(8, 3))
            for name in series:
                axis.plot([item.get(x, 0) for item in rows], [item.get(name) for item in rows], marker="o", label=name)
            for interval in data["invalid_intervals"]:
                axis.axvspan((interval["start_ms"] - _origin(data)) / 1000.0,
                            (interval["end_ms"] - _origin(data)) / 1000.0, color="red", alpha=0.12)
            if path == "throughput.png":
                for row in rows:
                    axis.axvspan(row.get("window_start_s", 0), row.get("window_start_s", 0) + row.get("duration_seconds", 0), color="gold", alpha=0.12)
            axis.set_title(title); axis.legend(); axis.grid(True, alpha=0.25); figure.tight_layout()
            figure.savefig(output_dir / path); plt.close(figure)
        plot_rows(data["app_windows"], "window_start_s", ["accepted_pdr", "offered_pdr", "p95_ms"], "heartbeat.png", "Heartbeat PDR and latency")
        plot_rows(data["throughput_bursts"], "window_start_s", ["packet_goodput_packets_per_second", "offered_pdr", "p95_ms"], "throughput.png", "Throughput burst goodput/PDR/latency")
        plot_rows(data["gtt_fleet"], "window_start_s", ["precision", "recall", "jaccard"], "gtt.png", "GTT fleet precision/recall/Jaccard")
        plot_rows(data["control_deltas"], "window_start_s", ["rate_per_second"], "control.png", "Control message/byte rates (scheduler values are proxies)")
        health_figure, health_axis = plt.subplots(figsize=(8, 3))
        health_axis.plot([item.get("window_start_s", 0) for item in data["telemetry_health"]],
                         [item.get("record_seq", 0) for item in data["telemetry_health"]], marker="o", label="record sequence")
        for label, selector in (("retry proxy", "retry_proxy"), ("fault proxy", "fault_proxy")):
            rows = [item for item in data["control_deltas"] if item.get(selector) == 1]
            health_axis.plot([item["window_start_s"] for item in rows], [item["rate_per_second"] for item in rows], marker="x", label=label)
        health_axis.set_title("Retries/faults/telemetry health (scheduler events are proxies)"); health_axis.legend(); health_axis.grid(True, alpha=0.25); health_figure.tight_layout()
        health_figure.savefig(output_dir / "health.png"); plt.close(health_figure)
        figure, axis = plt.subplots(figsize=(8, 3))
        heat = data["gtt_nodes"]
        windows = sorted({int(row["window_start_s"]) for row in heat})
        if not windows:
            windows = [0]
        matrix = [[next((float(row["recall"]) for row in heat if row["role"] == role and row["window_start_s"] == window and row["recall"] is not None), float("nan"))
                   for window in windows] for role in ROLES]
        image = axis.imshow(matrix, aspect="auto", vmin=0, vmax=1, cmap="viridis")
        axis.set_yticks(range(len(ROLES)), ROLES); axis.set_xticks(range(len(windows)), windows)
        axis.set_title("GTT node recall heatmap"); figure.colorbar(image, ax=axis, label="recall"); figure.tight_layout()
        figure.savefig(output_dir / "gtt_heatmap.png"); plt.close(figure)
    except Exception as error:  # chart errors must never terminate capture
        return False, str(error)
    return True, None


def _origin(data: dict[str, Any]) -> float:
    values = [item.get("offered_host_ms") for item in data.get("app_packets", []) if item.get("offered_host_ms") is not None]
    return float(min(values)) if values else 0.0


def write_outputs(data: dict[str, Any], output_dir: Path, name: str = "watch", charts: bool = False) -> dict[str, Any]:
    if not BUNDLE_NAME_RE.fullmatch(name):
        raise CaptureError("output bundle name is invalid")
    bundle = output_dir / name
    staged: Path | None = None
    try:
        output_dir.mkdir(parents=True, exist_ok=True)
        if bundle.exists():
            raise CaptureError(f"refuses to overwrite output bundle {bundle}")
        staged = Path(tempfile.mkdtemp(prefix=f".{name}.", dir=output_dir))
        result = dict(data)
        if charts:
            okay, reason = generate_pngs(result, staged)
            result["png_status"] = "generated" if okay else "unavailable"
            if reason:
                result["png_error"] = reason
        (staged / f"{name}.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        (staged / f"{name}.md").write_text(render_markdown(result), encoding="utf-8")
        for filename, key in (("app_packets.csv", "app_packets"), ("app_windows.csv", "app_windows"),
                              ("throughput_bursts.csv", "throughput_bursts"), ("gtt_entries.csv", "gtt_entries"),
                              ("gtt_nodes.csv", "gtt_nodes"), ("gtt_fleet.csv", "gtt_fleet"), ("control_deltas.csv", "control_deltas"),
                              ("app_cumulative.csv", "app_cumulative"),
                              ("telemetry_health.csv", "telemetry_health")):
            _write_csv(staged / filename, result[key])
        if bundle.exists():
            raise CaptureError(f"refuses to overwrite output bundle {bundle}")
        staged.rename(bundle)
        staged = None
        return result
    except OSError as error:
        raise CaptureError(f"cannot write output bundle {bundle}: {error}") from error
    finally:
        if staged is not None:
            shutil.rmtree(staged, ignore_errors=True)


def load_snapshot_json(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CaptureError(f"cannot read snapshot JSON {path}: {error}") from error
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        raise CaptureError("snapshot JSON schema is missing or unsupported")
    return data


def render_snapshot_pngs(data: dict[str, Any], bundle_dir: Path) -> None:
    """Render PNGs into a pre-existing runner stage without replaying logs."""
    if not bundle_dir.is_dir():
        raise CaptureError(f"chart bundle stage does not exist: {bundle_dir}")
    png_names = {"heartbeat.png", "throughput.png", "gtt.png", "control.png", "health.png", "gtt_heatmap.png"}
    if any((bundle_dir / name).exists() for name in png_names):
        raise CaptureError(f"refuses to overwrite chart files in bundle {bundle_dir}")
    okay, reason = generate_pngs(data, bundle_dir)
    if not okay:
        raise CaptureError(reason or "cannot render snapshot PNGs")


def _metadata_end_host_ms(metadata: dict[str, Any]) -> float:
    ended = metadata.get("ended_utc")
    if not isinstance(ended, str):
        raise CaptureError("run metadata must have ended_utc")
    return _timestamp(ended, "run metadata", 0)


def _validate_metadata(data: dict[str, Any]) -> dict[str, Any]:
    if data.get("schema") != RUN_SCHEMA:
        raise CaptureError("run metadata schema is missing or unsupported")
    _boards_by_role(data)
    _profile(data)
    _metadata_roles(data)
    _metadata_end_host_ms(data)
    return data


def load_metadata(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CaptureError(f"cannot read run metadata {path}: {error}") from error
    if not isinstance(data, dict):
        raise CaptureError("run metadata schema is missing or unsupported")
    return _validate_metadata(data)


def _log_map(metadata: dict[str, Any], supplied: list[str]) -> dict[str, Path]:
    paths: dict[str, Path] = {}
    for item in supplied:
        role, marker, name = item.partition("=")
        if marker != "=" or role not in ROLES or not name or role in paths:
            raise CaptureError("--serial-log must be unique ROLE=PATH")
        paths[role] = Path(name)
    if not paths:
        for board in metadata["boards"]:
            log = board.get("log") if isinstance(board, dict) else None
            if isinstance(log, dict) and isinstance(log.get("path"), str):
                paths[board["role"]] = Path(log["path"])
    if set(paths) != set(ROLES):
        raise CaptureError("six ROLE=PATH logs are required or must be present in run metadata")
    return paths


def _verify_log_hashes(metadata: dict[str, Any], paths: dict[str, Path]) -> None:
    boards = _boards_by_role(metadata)
    for role, path in paths.items():
        log = boards[role].get("log")
        if not isinstance(log, dict):
            raise CaptureError(f"run metadata board {role} has mandatory log provenance")
        expected = log.get("sha256")
        if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise CaptureError(f"run metadata board {role} has invalid log hash")
        try:
            actual = sha256_file(path)
        except OSError as error:
            raise CaptureError(f"cannot hash serial log {path}: {error}") from error
        if actual != expected:
            raise CaptureError(f"serial log hash differs from run metadata for board {role}")


def replay(metadata: dict[str, Any], paths: dict[str, Path]) -> ObservationState:
    _validate_metadata(metadata)
    if set(paths) != set(ROLES):
        raise CaptureError("offline replay requires exactly six ROLE=PATH logs")
    _verify_log_hashes(metadata, paths)
    state = ObservationState(metadata)
    boards = _boards_by_role(metadata)
    for role, path in sorted(paths.items()):
        log = boards[role].get("log")
        allow_terminal_partial = bool(isinstance(log, dict) and log.get("terminal_censored"))
        state.feed_file(role, path, allow_terminal_partial=allow_terminal_partial)
    state.finalize(_metadata_end_host_ms(metadata))
    return state


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--run-metadata")
    source.add_argument("--render-json", metavar="SNAPSHOT_JSON")
    parser.add_argument("--serial-log", action="append", default=[], metavar="ROLE=PATH")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--name", default="offline")
    parser.add_argument("--charts", action="store_true", help="optional matplotlib==3.11.1 PNGs")
    args = parser.parse_args(argv)
    try:
        if args.render_json:
            if args.serial_log:
                raise CaptureError("--serial-log cannot be used with --render-json")
            render_snapshot_pngs(load_snapshot_json(Path(args.render_json)), Path(args.output_dir))
        else:
            metadata = load_metadata(Path(args.run_metadata))
            state = replay(metadata, _log_map(metadata, args.serial_log))
            write_outputs(snapshot(state), Path(args.output_dir), args.name, args.charts)
    except CaptureError as error:
        print(f"FAIL tavrn_observation: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
