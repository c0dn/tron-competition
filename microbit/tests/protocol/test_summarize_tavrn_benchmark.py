#!/usr/bin/env python3
"""Strict six-stream checks for continuous observer-v2 analysis."""

from __future__ import annotations

import datetime as dt
import hashlib
import importlib.util
import json
import pathlib
import sqlite3
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "observation", ROOT / "scripts" / "summarize_tavrn_benchmark.py")
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

BASE = dt.datetime(2026, 8, 12, tzinfo=dt.timezone.utc)
CAPTURE_END_MS = 120000
HEARTBEAT_OFFERS = 120
THROUGHPUT_SEQUENCES = set(range(600))


def utc(host_ms: int) -> str:
    return (BASE + dt.timedelta(milliseconds=host_ms)).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def line(host_ms: int, payload: str) -> str:
    return f"[{utc(host_ms)} 0.000000] {payload}"


def metadata(profile: str = "FULL_TAVRN", ended_ms: int = CAPTURE_END_MS) -> dict[str, object]:
    return {
        "schema": MODULE.RUN_SCHEMA,
        "status": "completed_by_user",
        "profile": profile,
        "started_utc": utc(0),
        "ended_utc": utc(ended_ms),
        "observation": {
            "source_role": "A",
            "destination_role": "C",
            "heartbeat_hz": 1,
            "throughput_hz": 10,
            "throughput_start_seconds": 60,
            "throughput_burst_seconds": 60,
            "throughput_repeat_seconds": 450,
            "window_seconds": 10,
        },
        "boards": [
            {"role": role, "uid": f"{number:048x}", "adva": f"{number:02x}:42:de:52:4a:dd"}
            for number, role in enumerate(MODULE.ROLES, 1)
        ],
    }


def identity(workload: str, burst: int, sequence: int) -> str:
    value = ((1 if workload == "throughput" else 0) << 23) | (burst << 10) | sequence
    return f"0x{value:06x}"


def wire_record(kind: str, now: int, role: str, session: int, record_id: int,
                extra: str = "") -> str:
    record = (
        f"routed obs_{kind} now={now} role={role} schema=observer-v2 session={session} "
        f"record_id_hi={record_id >> 32} record_id_lo={record_id & 0xffffffff}"
    )
    return f"{record} {extra}" if extra else record


def control_fields(value: int) -> str:
    values = " ".join(f"{field}={value}" for field in sorted(MODULE.CONTROL_COUNTER_FIELDS))
    return f"control_tx_scope=AODV_DISPATCH_PROXY_ONLY {values}"


def health_fields(*, record_id: int | str, skipped: int = 0) -> str:
    fields = {field: 0 for field in MODULE.HEALTH_COUNTER_FIELDS}
    fields["skipped"] = skipped
    values = " ".join(f"{field}={fields[field]}" for field in sorted(fields))
    return f"record_seq={record_id} {values}"


def logical(role: str, profile: str) -> int:
    board = next(board for board in metadata(profile)["boards"] if board["role"] == role)
    octets = str(board["adva"]).split(":")
    return int(octets[0], 16) | (int(octets[1], 16) << 8) if profile == "AODV_ONLY" else int(octets[0], 16)


def stream(
    role: str,
    *,
    profile: str = "FULL_TAVRN",
    heartbeat_offers: int = HEARTBEAT_OFFERS,
    throughput_sequences: set[int] | None = None,
    not_ready: tuple[str, int] | None = None,
    integrity_update: bool = False,
    omitted_clock: set[int] | None = None,
    omitted_control: set[int] | None = None,
) -> str:
    """Build a dense, exact 120-second observer capture for one board."""
    number = MODULE.ROLES.index(role) + 1
    throughput_sequences = THROUGHPUT_SEQUENCES if throughput_sequences is None else throughput_sequences
    omitted_clock = omitted_clock or set()
    omitted_control = omitted_control or set()
    events: list[tuple[int, int, str, str, str]] = []
    order = 0

    def add(host: int, kind: str, extra: str = "", tag: str = "") -> None:
        nonlocal order
        events.append((host, order, kind, tag, extra))
        order += 1

    add(0, "boot", "started_at_ms=0")
    for now in range(0, CAPTURE_END_MS + 1, 1000):
        if now not in omitted_clock:
            add(now, "clock")
    for sample_at in range(0, CAPTURE_END_MS + 1, 10000):
        if sample_at not in omitted_control:
            add(sample_at, "control", control_fields(sample_at // 10000))
        skipped = int(integrity_update and sample_at >= 10000)
        add(sample_at, "health", health_fields(record_id="{record_id}", skipped=skipped))
    for query_at in range(0, CAPTURE_END_MS, 10000):
        if profile == "AODV_ONLY":
            add(query_at, "gtt_begin", "query_at_ms=%d slot_capacity=0 status=NOT_IMPLEMENTED" % query_at)
            add(query_at + 1, "gtt_end", "query_at_ms=%d status=NOT_IMPLEMENTED entry_count=0 nondeparted_count=0" % query_at)
            continue
        add(query_at, "gtt_begin", "query_at_ms=%d slot_capacity=16 retained_entry_count=5" % query_at)
        for slot, other in enumerate(MODULE.ROLES):
            if other == role:
                continue
            board = next(board for board in metadata(profile)["boards"] if board["role"] == other)
            add(query_at + slot + 1, "gtt_entry",
                "query_at_ms=%d slot=%d adva=%s freshness=1 departed=1 last_evidence_ms=%d" %
                (query_at, slot, board["adva"], max(0, query_at - 10)))
        add(query_at + 6, "gtt_end", "query_at_ms=%d status=OK entry_count=5 nondeparted_count=5" % query_at)

    slots = [(sequence * 1000, 1, "heartbeat", 0, sequence)
             for sequence in range(heartbeat_offers)]
    slots.extend((60000 + sequence * 100, 0, "throughput", 0, sequence)
                 for sequence in sorted(throughput_sequences))
    slots.sort()
    if role == "A":
        destination = logical("C", profile)
        width = 1 if profile == "FULL_TAVRN" else 2
        for deadline, _, workload, burst, sequence in slots:
            tag = f"{workload}_{burst}_{sequence}"
            is_not_ready = not_ready == (workload, sequence)
            attempted = 0 if is_not_ready else 1
            accepted = 0 if is_not_ready else 1
            status = MODULE.UINT32_MAX if is_not_ready else 0
            source_destination = 0 if is_not_ready else destination
            source_width = 1 if is_not_ready else width
            event = (
                f"event_at_ms={deadline} deadline_ms={deadline} workload={workload} burst={burst} "
                f"sequence={sequence} payload_id={sequence} identity={identity(workload, burst, sequence)}"
            )
            add(deadline, "offer",
                f"{event} offer={{record_id}} attempted={attempted} accepted=0 status={status} "
                f"destination=0x{source_destination:04x} width={source_width}", tag)
            add(deadline, "app",
                f"{event} offer_record_id_hi=0 offer_record_id_lo={{{tag}}} attempted={attempted} "
                f"accepted={accepted} status={status} destination=0x{source_destination:04x} width={source_width}")
    if role == "C":
        origin, destination = logical("A", profile), logical("C", profile)
        for deadline, _, workload, burst, sequence in slots:
            if not_ready == (workload, sequence):
                continue
            add(deadline + 100, "final",
                f"event_at_ms={deadline + 100} origin_session=1 origin=0x{origin:04x} "
                f"destination=0x{destination:04x} identity_valid=1 workload={workload} burst={burst} "
                f"sequence={sequence} payload_id={sequence} identity={identity(workload, burst, sequence)} "
                "app_kind=127 app_len=7")
    events.sort(key=lambda event: (event[0], event[1]))
    tags = {tag: record_id for record_id, (_, _, _, tag, _) in enumerate(events, 1) if tag}
    return "\n".join(
        line(host, wire_record(kind, host, str(number), number, record_id,
                               extra.format(record_id=record_id, **tags)))
        for record_id, (host, _, kind, _, extra) in enumerate(events, 1)
    ) + "\n"


class ObservationTests(unittest.TestCase):
    def state(self, *, profile: str = "FULL_TAVRN", finalize: bool = True,
              transform: object | None = None, **stream_options: object) -> object:
        state = MODULE.ObservationState(metadata(profile))
        for role in MODULE.ROLES:
            payload = stream(role, profile=profile, **stream_options)
            if transform is not None:
                payload = transform(role, payload)
            state.feed(role, payload[:37], f"{role}.log")
            state.feed(role, payload[37:], f"{role}.log", final=True)
        if finalize:
            state.finalize((BASE + dt.timedelta(milliseconds=CAPTURE_END_MS)).timestamp() * 1000.0)
        return state

    def test_dense_exact_schedule_is_valid(self) -> None:
        data = MODULE.snapshot(self.state())
        self.assertEqual(data["proving_status"], "VALID")
        self.assertEqual(data["schemas"], ["observer-v2"])
        self.assertEqual(len(data["app_packets"]), HEARTBEAT_OFFERS + len(THROUGHPUT_SEQUENCES))
        self.assertEqual(sum(row["offered"] for row in data["app_windows"]), HEARTBEAT_OFFERS)
        self.assertEqual(data["throughput_bursts"][0]["offered"], 600)
        self.assertEqual(data["throughput_bursts"][0]["delivered"], 600)
        self.assertTrue(all(row["status"] == "OK" for row in data["gtt_fleet"]))

    def test_full_gtt_rejects_invalid_departed_enum(self) -> None:
        def invalid_departed(role: str, payload: str) -> str:
            return payload.replace("departed=1", "departed=0", 1) if role == "A" else payload

        with self.assertRaisesRegex(MODULE.CaptureError, "departed outside"):
            self.state(transform=invalid_departed)

    def test_observer_contract_fields_and_low_record_id_half_are_required(self) -> None:
        offer = line(0, wire_record(
            "offer", 0, "1", 1, 2,
            "event_at_ms=0 deadline_ms=0 workload=heartbeat burst=0 sequence=0 payload_id=0 "
            "identity=0x000000 offer=2 attempted=1 accepted=0 status=0 destination=0x0003 width=1"))
        self.assertIsNotNone(MODULE.parse_observation_line(offer))
        self.assertEqual(identity("throughput", (1 << 13) - 1, (1 << 10) - 1),
                         "0xffffff")
        for malformed, expected in (
                (offer.replace(" deadline_ms=0", ""), "missing required fields"),
                (offer.replace(" destination=0x0003", ""), "missing required fields"),
                (offer.replace(" width=1", ""), "missing required fields"),
                (offer.replace(" attempted=1", ""), "missing required fields"),
                (offer.replace("identity=0x000000", "identity=0x000001"), "identity/workload"),
                (offer.replace("burst=0", "burst=8192").replace(
                    "identity=0x000000", "identity=0x200000"), "out-of-range burst")):
            with self.subTest(malformed=malformed), self.assertRaisesRegex(MODULE.CaptureError, expected):
                MODULE.parse_observation_line(malformed)
        boot = line(0, wire_record("boot", 0, "1", 1, 1, "started_at_ms=0"))
        with self.assertRaisesRegex(MODULE.CaptureError, "missing required fields"):
            MODULE.parse_observation_line(boot.replace(" started_at_ms=0", ""))

        state = MODULE.ObservationState(metadata())
        high = 1 << 32
        state.feed("A", line(0, wire_record("boot", 0, "1", 1, high, "started_at_ms=0")) + "\n" +
                   line(1, wire_record("health", 1, "1", 1, high + 1,
                                       health_fields(record_id=1))) + "\n", final=True)
        self.assertEqual(state.health_events[0]["record_seq"], 1)

    def test_only_controlled_terminal_partial_is_right_censored(self) -> None:
        payload = (line(0, wire_record("boot", 0, "1", 1, 1, "started_at_ms=0")) +
                   "\nobs_clock schema=observer-v2")
        strict = MODULE.ObservationState(metadata())
        with self.assertRaisesRegex(MODULE.CaptureError, "partial UART line"):
            strict.feed("A", payload, "A.log", final=True)
        controlled = MODULE.ObservationState(metadata())
        emitted = controlled.feed("A", payload, "A.log", final=True,
                                  allow_terminal_partial=True)
        self.assertEqual(len(emitted), 1)
        self.assertEqual(emitted[0].kind, "boot")

    def test_terminal_censored_source_offer_pair_is_not_corruption(self) -> None:
        run_metadata = metadata()
        next(board for board in run_metadata["boards"] if board["role"] == "A")["log"] = {
            "terminal_censored": True}
        state = MODULE.ObservationState(run_metadata)
        for role in MODULE.ROLES:
            payload = stream(role)
            if role == "A":
                lines = payload.splitlines()
                last_app = max(index for index, item in enumerate(lines) if " obs_app " in item)
                payload = "\n".join(lines[:last_app]) + "\n" + lines[last_app][:40]
                state.feed(role, payload, f"{role}.log", final=True,
                           allow_terminal_partial=True)
            else:
                state.feed(role, payload, f"{role}.log", final=True)
        state.finalize((BASE + dt.timedelta(milliseconds=CAPTURE_END_MS)).timestamp() * 1000.0)
        data = MODULE.snapshot(state)
        self.assertFalse(any(item["reason"] == "missing_source_app"
                             for item in data["invalid_intervals"]))

    def test_source_attempt_contract_allows_full_not_ready_but_not_aodv_not_ready(self) -> None:
        data = MODULE.snapshot(self.state(not_ready=("heartbeat", 0)))
        self.assertEqual(data["proving_status"], "VALID")
        self.assertEqual(sum(row["not_ready"] for row in data["app_windows"]), 1)
        rejected = MODULE.snapshot(self.state(profile="AODV_ONLY", not_ready=("heartbeat", 0)))
        self.assertEqual(rejected["proving_status"], "INVALID")
        self.assertTrue(any(item["reason"] == "invalid_not_ready_source_attempt"
                            for item in rejected["invalid_intervals"]))

        def mismatched_app(role: str, payload: str) -> str:
            return payload.replace("attempted=1 accepted=1 status=0 destination",
                                   "attempted=0 accepted=1 status=0 destination", 1) \
                if role == "A" else payload

        mismatch = MODULE.snapshot(self.state(transform=mismatched_app))
        self.assertEqual(mismatch["proving_status"], "INVALID")
        self.assertTrue(any(item["reason"] == "source_app_identity_mismatch"
                            for item in mismatch["invalid_intervals"]))

    def test_schedule_rejects_early_missing_wrong_deadline_and_destination_width(self) -> None:
        def early(role: str, payload: str) -> str:
            return payload.replace(
                "event_at_ms=60000 deadline_ms=60000 workload=throughput",
                "event_at_ms=59999 deadline_ms=60000 workload=throughput", 1) if role == "A" else payload

        def wrong_deadline(role: str, payload: str) -> str:
            return payload.replace("deadline_ms=60000 workload=throughput",
                                   "deadline_ms=60001 workload=throughput") if role == "A" else payload

        def wrong_destination_width(role: str, payload: str) -> str:
            return payload.replace("destination=0x0003 width=1", "destination=0x0004 width=2") \
                if role == "A" else payload

        cases = (
            (self.state(transform=early), "stale_or_early_source_offer"),
            (self.state(throughput_sequences=THROUGHPUT_SEQUENCES - {300}),
             "throughput_sequence_gap_or_repetition"),
            (self.state(transform=wrong_deadline), "source_offer_deadline_mismatch"),
            (self.state(transform=wrong_destination_width), "invalid_source_destination_width"),
        )
        for state, reason in cases:
            with self.subTest(reason=reason):
                data = MODULE.snapshot(state)
                self.assertEqual(data["proving_status"], "INVALID")
                self.assertTrue(any(item["reason"] == reason for item in data["invalid_intervals"]))

    def test_finalized_capture_needs_complete_first_burst_and_120_heartbeats(self) -> None:
        incomplete_heartbeat = MODULE.snapshot(self.state(heartbeat_offers=HEARTBEAT_OFFERS - 1))
        self.assertEqual(incomplete_heartbeat["proving_status"], "INCOMPLETE")
        self.assertIn("incomplete_contiguous_heartbeat_offers:A", incomplete_heartbeat["incomplete_reasons"])
        incomplete_burst = MODULE.snapshot(self.state(throughput_sequences=set(range(599))))
        self.assertEqual(incomplete_burst["proving_status"], "INCOMPLETE")
        self.assertIn("incomplete_throughput_burst_0:A", incomplete_burst["incomplete_reasons"])

    def test_tight_clock_and_control_cadence_limits_are_enforced(self) -> None:
        clock = MODULE.snapshot(self.state(omitted_clock={60000}))
        self.assertEqual(clock["proving_status"], "INCOMPLETE")
        self.assertIn("clock_cadence_gap:A", clock["incomplete_reasons"])
        control = MODULE.snapshot(self.state(omitted_control={60000}))
        self.assertEqual(control["proving_status"], "INCOMPLETE")
        self.assertIn("control_cadence_gap:A", control["incomplete_reasons"])

    def test_integrity_evidence_invalidates_without_disallowing_rejected_pdr(self) -> None:
        data = MODULE.snapshot(self.state(integrity_update=True))
        self.assertEqual(data["proving_status"], "INVALID")
        self.assertTrue(any(item["reason"] == "telemetry_integrity:skipped"
                            for item in data["invalid_intervals"]))

    def test_aodv_sid16_source_destination_and_not_implemented_gtt_are_valid(self) -> None:
        data = MODULE.snapshot(self.state(profile="AODV_ONLY"))
        self.assertEqual(data["proving_status"], "VALID")
        self.assertEqual(data["gtt_fleet"], [{"window_start_s": 0, "status": "N/A_AODV_GTT",
                                              "invalid": False, "invalid_roles": []}])

    def test_replay_and_pinned_matplotlib_output(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp)
            run_metadata = metadata()
            paths: dict[str, pathlib.Path] = {}
            for role in MODULE.ROLES:
                path = folder / f"{role}.log"
                path.write_text(stream(role), encoding="utf-8")
                paths[role] = path
                board = next(board for board in run_metadata["boards"] if board["role"] == role)
                board["log"] = {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            data = MODULE.snapshot(MODULE.replay(run_metadata, paths))
            self.assertEqual(data["proving_status"], "VALID")
            snapshot_path = folder / "snapshot.json"
            snapshot_path.write_text(json.dumps(data), encoding="utf-8")
            bundle = folder / "chart-stage"
            bundle.mkdir()
            self.assertEqual(MODULE.main(["--render-json", str(snapshot_path), "--output-dir", str(bundle)]), 0)
            self.assertTrue((bundle / "heartbeat.png").is_file())

    def test_sqlite_store_round_trips_records_flushes_and_closes(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp) / "observations.sqlite3"
            store = MODULE.SQLiteObservationRecordStore(path)
            record = MODULE.ObservationRecord(
                "clock", "A", "1", "observer-v2", 42, 4_294_967_338, 4_294_967_337,
                1_786_003_200_123.125,
                {"now": "42", "role": "A", "unicode": "π", "empty": ""},
                "synthetic.log", 17)
            store.append(record)
            self.assertEqual(list(store), [record])
            self.assertEqual(len(store), 1)
            store.flush()
            connection = sqlite3.connect(path)
            try:
                self.assertEqual(connection.execute(
                    "SELECT COUNT(*) FROM observation_records").fetchone()[0], 1)
            finally:
                connection.close()
            store.close()
            self.assertTrue(store.closed)

    def test_sqlite_snapshot_matches_in_memory_and_flushes_before_scan(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            options: dict[str, object] = {
                "heartbeat_offers": 5,
                "throughput_sequences": {0, 1, 2},
            }
            memory = MODULE.ObservationState(metadata())
            durable = MODULE.ObservationState(metadata(), pathlib.Path(temp) / "observations.sqlite3")
            for role in MODULE.ROLES:
                payload = stream(role, **options)
                memory.feed(role, payload, f"{role}.log", final=True)
                durable.feed(role, payload, f"{role}.log", final=True)
            end_ms = (BASE + dt.timedelta(milliseconds=CAPTURE_END_MS)).timestamp() * 1000.0
            memory.finalize(end_ms)
            durable.finalize(end_ms)
            with mock.patch.object(durable, "flush", wraps=durable.flush) as flush:
                self.assertEqual(MODULE.snapshot(durable), MODULE.snapshot(memory))
            flush.assert_called_once()
            self.assertEqual(durable.retained_record_count, 0)
            self.assertEqual(durable.gtt_pairs, [])
            self.assertEqual(durable.gtt_snapshots, [])
            self.assertEqual(durable.health_events, [])
            durable.close()

    def test_sqlite_state_keeps_100000_records_off_heap_and_live_status_constant(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            state = MODULE.ObservationState(metadata(), pathlib.Path(temp) / "observations.sqlite3")

            def fields(now: int, record_id: int) -> dict[str, str]:
                return {
                    "now": str(now), "role": "A", "schema": "observer-v2", "session": "1",
                    "record_id_hi": "0", "record_id_lo": str(record_id),
                    "_record_id": str(record_id),
                }

            boot = fields(0, 1)
            boot["started_at_ms"] = "0"
            state._ingest("boot", 0.0, boot, "synthetic", 1)
            for record_id in range(2, 100_002):
                state._ingest("clock", float(record_id), fields(record_id, record_id),
                              "synthetic", record_id)

            self.assertEqual(len(state.records), 100_001)
            self.assertEqual(state.retained_record_count, 0)
            self.assertEqual(state.health_events, [])
            self.assertEqual(state.gtt_pairs, [])
            self.assertEqual(state.gtt_snapshots, [])
            with mock.patch.object(MODULE.SQLiteObservationRecordStore, "__iter__",
                                   side_effect=AssertionError("live status scanned records")):
                status = state.live_status()
            self.assertEqual(status["record_count"], 100_001)
            self.assertEqual(status["active_roles"], ["A"])
            self.assertFalse(status["telemetry_corruption"])
            self.assertTrue(status["provisional"])
            state.flush()
            connection = sqlite3.connect(state.database_path)
            try:
                self.assertEqual(connection.execute(
                    "SELECT COUNT(*) FROM observation_records").fetchone()[0], 100_001)
            finally:
                connection.close()
            state.close()
            self.assertTrue(state.closed)


if __name__ == "__main__":
    unittest.main()
