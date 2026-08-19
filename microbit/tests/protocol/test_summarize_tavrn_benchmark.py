#!/usr/bin/env python3
"""Focused six-stream checks for strict observer-v3 checkpoint analysis."""

from __future__ import annotations

import datetime as dt
import hashlib
import importlib.util
import json
import pathlib
import re
import sqlite3
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "observation", ROOT / "scripts" / "summarize_tavrn_benchmark.py")
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

BASE = dt.datetime(2026, 8, 12, tzinfo=dt.timezone.utc)
CAPTURE_END_MS = 70_000


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
        "observation": {"source_role": "A", "destination_role": "C"},
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
    common = (f"routed obs_{kind} schema=observer-v3 now={now} role={role} session={session} "
              f"record_id_hi={record_id >> 32} record_id_lo={record_id & 0xffffffff}")
    return f"{common} {extra}" if extra else common


def control_fields(value: int = 0) -> str:
    counters = " ".join(f"{field}={value}" for field in sorted(MODULE.CONTROL_COUNTER_FIELDS))
    return f"control_tx_scope=AODV_DISPATCH_PROXY_ONLY {counters}"


def health_fields(**overrides: int) -> str:
    values = {field: 0 for field in MODULE.HEALTH_COUNTER_FIELDS}
    values.update(overrides)
    return " ".join(f"{field}={values[field]}" for field in sorted(values))


def logical(role: str, profile: str) -> int:
    board = next(board for board in metadata(profile)["boards"] if board["role"] == role)
    octets = str(board["adva"]).split(":")
    return int(octets[0], 16) | (int(octets[1], 16) << 8) if profile == "AODV_ONLY" else int(octets[0], 16)


def stream(role: str, profile: str = "FULL_TAVRN") -> str:
    """Build a compact, complete observer-v3 prefix for one board."""
    number = MODULE.ROLES.index(role) + 1
    events: list[tuple[int, int, str, str]] = []
    order = 0

    def add(host: int, kind: str, extra: str = "") -> None:
        nonlocal order
        events.append((host, order, kind, extra))
        order += 1

    add(0, "boot", "started_at_ms=0 feature=FULL_TAVRN" if profile == "FULL_TAVRN" else
        "started_at_ms=0 feature=AODV_ONLY")
    for now in (0, 1000, 2000, 8000):
        add(now, "clock")
    add(100, "control", control_fields())
    add(7000, "control", control_fields(1))
    if profile == "FULL_TAVRN":
        add(3000, "gtt_begin", "query_at_ms=3000 slot_capacity=16 retained_entry_count=5")
        for slot, other in enumerate(item for item in MODULE.ROLES if item != role):
            board = next(board for board in metadata(profile)["boards"] if board["role"] == other)
            add(3001 + slot, "gtt_entry",
                f"query_at_ms=3000 slot={slot} adva={board['adva']} serial=1 serial_state=1 "
                "hop_count=1 hop_state=1 freshness=1 departed=1 last_evidence_ms=2999")
        add(3007, "gtt_end", "query_at_ms=3000 status=OK entry_count=5 nondeparted_count=5")
    else:
        add(3000, "gtt_begin", "query_at_ms=3000 slot_capacity=0 status=NOT_IMPLEMENTED")
        add(3001, "gtt_end", "query_at_ms=3000 status=NOT_IMPLEMENTED entry_count=0 nondeparted_count=0")
    if role == "A":
        accepted = (("heartbeat", 0, 0), ("heartbeat", 0, 1), ("heartbeat", 0, 2),
                    ("throughput", 0, 0), ("throughput", 0, 1))
        for offset, (workload, burst, sequence) in enumerate(accepted):
            offered = sequence * 1000 if workload == "heartbeat" else 60_000 + sequence * 100
            add(offered + 2, "accept",
                f"offered_at_ms={offered} accepted_at_ms={offered + 1} "
                f"identity={identity(workload, burst, sequence)} submit_status={offset % 2}")
        for at in (61_000, 66_000):
            add(at, "health", health_fields(
                offered=8, accepted=5, rejected=3, not_ready=1, skipped=2,
                heartbeat_offered=4, heartbeat_accepted=3, heartbeat_rejected=1,
                heartbeat_not_ready=1, heartbeat_skipped=1, throughput_offered=4,
                throughput_accepted=2, throughput_rejected=2, throughput_not_ready=0,
                throughput_skipped=1))
    elif role == "C":
        origin, destination = logical("A", profile), logical("C", profile)
        delivered = (("heartbeat", 0, 0), ("heartbeat", 0, 1), ("heartbeat", 0, 2),
                     ("throughput", 0, 0), ("throughput", 0, 1))
        for offset, (workload, burst, sequence) in enumerate(delivered):
            at = 5000 + sequence * 10 if workload == "heartbeat" else 62_000 + sequence * 10
            add(at, "final",
                f"delivered_at_ms={at} origin_session=1 identity_valid=1 "
                f"identity={identity(workload, burst, sequence)} logical_origin=0x{origin:04x} "
                f"logical_destination=0x{destination:04x} app_kind=127 app_len=7")
        add(66_000, "health", health_fields(final_commits=5, valid_heartbeat_finals=3,
                                              valid_throughput_finals=2))
    else:
        add(9000, "health", health_fields())
    events.sort()
    return "\n".join(
        line(host, wire_record(kind, host, str(number), number, record_id, extra))
        for record_id, (host, _, kind, extra) in enumerate(events, 1)
    ) + "\n"


class ObservationTests(unittest.TestCase):
    def state(self, *, profile: str = "FULL_TAVRN", finalize: bool = True,
              transform: object | None = None, terminal_roles: set[str] | None = None,
              database_path: pathlib.Path | None = None) -> object:
        run_metadata = metadata(profile)
        terminal_roles = terminal_roles or set()
        for board in run_metadata["boards"]:
            if board["role"] in terminal_roles:
                board["log"] = {"terminal_censored": True}
        state = MODULE.ObservationState(run_metadata, database_path)
        for role in MODULE.ROLES:
            payload = stream(role, profile)
            if transform is not None:
                payload = transform(role, payload)
            split = min(37, len(payload))
            state.feed(role, payload[:split], f"{role}.log")
            state.feed(role, payload[split:], f"{role}.log", final=True,
                       allow_terminal_partial=role in terminal_roles)
        if finalize:
            state.finalize((BASE + dt.timedelta(milliseconds=CAPTURE_END_MS)).timestamp() * 1000.0)
        return state

    def snapshot(self, **kwargs: object) -> dict[str, object]:
        return MODULE.snapshot(self.state(**kwargs))

    def test_dense_v3_capture_has_exact_checkpoint_totals(self) -> None:
        data = self.snapshot()
        self.assertEqual(data["proving_status"], "VALID")
        self.assertEqual(data["schemas"], ["observer-v3"])
        self.assertEqual(data["validity"]["application"]["status"], "VALID")
        self.assertEqual(data["validity"]["transport"]["status"], "VALID")
        self.assertEqual(len(data["app_packets"]), 5)
        self.assertTrue(all(row["row_semantics"] == "accepted_event" and row["accepted"] == 1 and
                            row["attempted"] == 1 and row["status"] in (0, 1)
                            for row in data["app_packets"]))
        totals = data["application_totals"]
        self.assertEqual(totals["combined"], {
            "offered": 8, "accepted": 5, "rejected": 3, "not_ready": 1, "skipped": 2,
            "delivered": 5, "accepted_pdr": 1.0, "offered_pdr": 0.625})
        self.assertEqual(totals["heartbeat"]["delivered"], 3)
        self.assertEqual(totals["throughput"]["delivered"], 2)
        self.assertTrue(all(row["offered"] is None and row["rejected"] is None and
                            row["not_ready"] is None and row["offered_pdr"] is None
                            for row in [*data["app_windows"], *data["throughput_bursts"]]))

    def test_strict_accept_final_health_grammar_and_identity_derivation(self) -> None:
        accept = line(0, wire_record("accept", 0, "1", 1, 2,
            "offered_at_ms=0 accepted_at_ms=1 identity=0x800401 submit_status=1"))
        kind, _, fields = MODULE.parse_observation_line(accept)
        self.assertEqual(kind, "accept")
        self.assertEqual({name: fields[name] for name in ("workload", "burst", "sequence", "payload_id")},
                         {"workload": "throughput", "burst": "1", "sequence": "1", "payload_id": "1"})
        health = line(0, wire_record("health", 0, "1", 1, 3, health_fields()))
        final = line(0, wire_record("final", 0, "3", 1, 4,
            "delivered_at_ms=1 origin_session=1 identity_valid=1 identity=0x000000 "
            "logical_origin=0x0001 logical_destination=0x0003 app_kind=127 app_len=7"))
        for malformed, expected in (
                (accept.replace(" submit_status=1", " submit_status=2"), "submit_status outside"),
                (accept.replace(" accepted_at_ms=1", ""), "missing required fields"),
                (accept.replace("identity=0x800401", "identity=0x80040a extra=1"), "unexpected"),
                (final.replace(" logical_destination=0x0003", ""), "missing required fields"),
                (final.replace("logical_origin=0x0001", "logical_origin=0x001"), "malformed logical_origin"),
                (health.replace(" accepted_fifo_count=0", ""), "missing required fields"),
                (health + " extra=1", "unexpected")):
            with self.subTest(malformed=malformed), self.assertRaisesRegex(MODULE.CaptureError, expected):
                MODULE.parse_observation_line(malformed)

    def test_gtt_wire_grammar_requires_all_firmware_fields(self) -> None:
        complete = line(3001, wire_record(
            "gtt_entry", 3001, "1", 1, 8,
            "query_at_ms=3000 slot=0 adva=02:42:de:52:4a:dd serial=1 serial_state=1 "
            "hop_count=1 hop_state=1 freshness=1 departed=1 last_evidence_ms=2999"))
        self.assertEqual(MODULE.parse_observation_line(complete)[0], "gtt_entry")
        for malformed in (complete.replace(" serial=1", ""),
                          complete.replace(" hop_state=1", " hop_state=2"),
                          complete + " extra=1"):
            with self.subTest(malformed=malformed), self.assertRaises(MODULE.CaptureError):
                MODULE.parse_observation_line(malformed)

    def test_source_and_destination_checkpoint_equations_are_strict(self) -> None:
        def source_mismatch(role: str, payload: str) -> str:
            return payload.replace("accepted=5", "accepted=4") if role == "A" else payload

        def destination_mismatch(role: str, payload: str) -> str:
            return payload.replace("final_commits=5", "final_commits=4") if role == "C" else payload

        for transform, reason in ((source_mismatch, "source_checkpoint_accepted_equation"),
                                  (destination_mismatch, "destination_checkpoint_final_equation")):
            with self.subTest(reason=reason):
                data = self.snapshot(transform=transform)
                self.assertEqual(data["proving_status"], "INVALID")
                self.assertIn(reason, data["validity"]["application"]["reasons"])

    def test_checkpoint_identity_workload_counts_are_strict(self) -> None:
        def source_mismatch(role: str, payload: str) -> str:
            return payload.replace("heartbeat_accepted=3", "heartbeat_accepted=2") if role == "A" else payload

        def destination_mismatch(role: str, payload: str) -> str:
            return payload.replace("valid_heartbeat_finals=3", "valid_heartbeat_finals=2") if role == "C" else payload

        for transform, reason in (
                (source_mismatch, "source_checkpoint_heartbeat_accepted_identity_count"),
                (destination_mismatch, "destination_checkpoint_heartbeat_identity_count")):
            with self.subTest(reason=reason):
                data = self.snapshot(transform=transform)
                self.assertEqual(data["proving_status"], "INVALID")
                self.assertIn(reason, data["validity"]["application"]["reasons"])

    def test_three_frontiers_censor_post_cutoff_tail_without_biasing_pdr(self) -> None:
        def renumber(lines: list[str]) -> str:
            return "\n".join(re.sub(
                r"record_id_hi=\d+ record_id_lo=\d+",
                f"record_id_hi=0 record_id_lo={record_id}", item)
                for record_id, item in enumerate(lines, 1)) + "\n"

        def transform(role: str, payload: str) -> str:
            lines = payload.splitlines()
            if role == "A":
                support = max(index for index, item in enumerate(lines) if " obs_health " in item)
                lines[support] = lines[support].replace("accepted=5", "accepted=6").replace(
                    "offered=8", "offered=9").replace(
                    "heartbeat_accepted=3 heartbeat_not_ready=1 heartbeat_offered=4 heartbeat_rejected=1",
                    "heartbeat_accepted=4 heartbeat_not_ready=1 heartbeat_offered=5 heartbeat_rejected=1")
                lines.insert(support, line(63_000, wire_record(
                    "accept", 63_000, "1", 1, 0,
                    "offered_at_ms=3000 accepted_at_ms=3001 identity=0x000003 submit_status=0")))
                return renumber(lines)
            if role == "C":
                health = next(index for index, item in enumerate(lines) if " obs_health " in item)
                lines[health] = lines[health].replace("final_commits=5", "final_commits=6").replace(
                    "valid_heartbeat_finals=3", "valid_heartbeat_finals=4")
                lines.insert(health, line(64_000, wire_record(
                    "final", 64_000, "3", 3, 0,
                    "delivered_at_ms=64000 origin_session=1 identity_valid=1 identity=0x000003 "
                    "logical_origin=0x0001 logical_destination=0x0003 app_kind=127 app_len=7")))
                return renumber(lines)
            return payload

        data = self.snapshot(transform=transform)
        self.assertEqual(data["proving_status"], "VALID")
        self.assertEqual(len(data["app_packets"]), 5)
        self.assertEqual(data["application_totals"]["combined"]["accepted"], 5)
        self.assertEqual(data["application_totals"]["combined"]["delivered"], 5)
        self.assertEqual(data["application_totals"]["combined"]["accepted_pdr"], 1.0)
        self.assertEqual(data["selected_checkpoints"].keys(),
                         {"source_cutoff", "destination_frontier", "source_support"})
        self.assertEqual(data["selected_checkpoints"]["source_cutoff"]["host_ms"],
                         (BASE + dt.timedelta(milliseconds=61_000)).timestamp() * 1000.0)
        self.assertEqual(data["selected_checkpoints"]["destination_frontier"]["host_ms"],
                         (BASE + dt.timedelta(milliseconds=66_000)).timestamp() * 1000.0)

    def test_no_deadline_settled_frontier_is_incomplete(self) -> None:
        def transform(role: str, payload: str) -> str:
            if role != "C":
                return payload
            return payload.replace(utc(66_000), utc(65_000)).replace("now=66000", "now=65000")

        data = self.snapshot(transform=transform)
        self.assertEqual(data["proving_status"], "INCOMPLETE")
        self.assertIn("no_settled_application_frontier_tuple",
                      data["validity"]["application"]["reasons"])

    def test_uart_delay_cannot_fake_a_settled_frontier(self) -> None:
        def transform(role: str, payload: str) -> str:
            if role != "C":
                return payload
            return payload.replace(utc(66_000), utc(70_000)).replace(
                "obs_health schema=observer-v3 now=66000",
                "obs_health schema=observer-v3 now=63000")

        data = self.snapshot(transform=transform)
        self.assertEqual(data["proving_status"], "INCOMPLETE")
        self.assertIn("no_settled_application_frontier_tuple",
                      data["validity"]["application"]["reasons"])

    def test_destination_reboot_after_cutoff_cannot_supply_frontier(self) -> None:
        def transform(role: str, payload: str) -> str:
            if role != "C":
                return payload
            lines = [item for item in payload.splitlines() if " obs_health " not in item]
            additions = [
                line(63_000, wire_record("boot", 0, "3", 33, 1,
                                         "started_at_ms=0 feature=FULL_TAVRN")),
                line(63_001, wire_record("clock", 0, "3", 33, 2)),
                line(64_000, wire_record("clock", 1000, "3", 33, 3)),
                line(65_000, wire_record("clock", 2000, "3", 33, 4)),
                line(66_000, wire_record("clock", 3000, "3", 33, 5)),
                line(66_001, wire_record("health", 3001, "3", 33, 6,
                                         health_fields())),
            ]
            return "\n".join(sorted([*lines, *additions])) + "\n"

        data = self.snapshot(transform=transform)
        self.assertEqual(data["proving_status"], "INCOMPLETE")
        self.assertIn("no_settled_application_frontier_tuple",
                      data["validity"]["application"]["reasons"])

    def test_destination_session_must_span_the_accepted_prefix(self) -> None:
        def transform(role: str, payload: str) -> str:
            if role != "C":
                return payload
            lines = [item for item in payload.splitlines()
                     if " obs_health " not in item and
                     not (" obs_final " in item and "identity=0x8" in item)]
            additions = [
                line(60_000, wire_record("boot", 0, "3", 33, 1,
                                         "started_at_ms=0 feature=FULL_TAVRN")),
                line(60_001, wire_record("clock", 0, "3", 33, 2)),
                line(62_000, wire_record("clock", 2000, "3", 33, 3)),
                line(64_000, wire_record("clock", 4000, "3", 33, 4)),
                line(66_000, wire_record("clock", 6000, "3", 33, 5)),
                line(66_001, wire_record("health", 6001, "3", 33, 6,
                                         health_fields())),
            ]
            return "\n".join(sorted([*lines, *additions])) + "\n"

        data = self.snapshot(transform=transform)
        self.assertEqual(data["proving_status"], "INCOMPLETE")
        self.assertIn("no_settled_application_frontier_tuple",
                      data["validity"]["application"]["reasons"])

    def test_accepted_timestamp_and_schedule_contract_is_strict(self) -> None:
        def offer_to_accept(role: str, payload: str) -> str:
            return payload.replace("offered_at_ms=0 accepted_at_ms=1", "offered_at_ms=100 accepted_at_ms=1", 1) \
                if role == "A" else payload

        def accept_to_emission(role: str, payload: str) -> str:
            return payload.replace("obs_accept schema=observer-v3 now=2", "obs_accept schema=observer-v3 now=0", 1) \
                if role == "A" else payload

        def throughput_sequence(role: str, payload: str) -> str:
            return payload.replace("identity=0x800001", "identity=0x800258", 1) \
                if role in {"A", "C"} else payload

        for transform, reason in ((offer_to_accept, "accepted_timestamp_offer_to_accept"),
                                  (accept_to_emission, "accepted_timestamp_accept_to_emission"),
                                  (throughput_sequence, "throughput_sequence_out_of_range")):
            with self.subTest(reason=reason):
                data = self.snapshot(transform=transform)
                self.assertEqual(data["proving_status"], "INVALID")
                self.assertIn(reason, data["validity"]["application"]["reasons"])

    def test_earlier_fifo_backlog_is_superseded_by_a_clean_drained_checkpoint(self) -> None:
        def transform(role: str, payload: str) -> str:
            if role != "A":
                return payload
            lines = payload.splitlines()
            index = [index for index, item in enumerate(lines) if " obs_accept " in item][2]
            lines.insert(index, line(2001, wire_record(
                "health", 2001, "1", 1, 0, health_fields(
                    offered=3, accepted=3, heartbeat_offered=3, heartbeat_accepted=3,
                    accepted_fifo_count=1, accepted_fifo_high_water=1))))
            return "\n".join(re.sub(
                r"record_id_hi=\d+ record_id_lo=\d+",
                f"record_id_hi=0 record_id_lo={record_id}", item)
                for record_id, item in enumerate(lines, 1)) + "\n"

        data = self.snapshot(transform=transform)
        self.assertEqual(data["proving_status"], "VALID")
        self.assertEqual(data["selected_checkpoints"]["source_cutoff"]["host_ms"],
                         (BASE + dt.timedelta(milliseconds=61_000)).timestamp() * 1000.0)

    def test_critical_fifo_fault_and_busy_counters_invalidate_application(self) -> None:
        cases = (
            ("A", "accepted_fifo_dropped=0", "accepted_fifo_dropped=1"),
            ("C", "final_fifo_dropped=0", "final_fifo_dropped=1"),
            ("A", "accepted_fifo_count=0", "accepted_fifo_count=1"),
            ("C", "final_fifo_count=0", "final_fifo_count=1"),
            ("A", "accepted_fifo_faults=0", "accepted_fifo_faults=1"),
            ("C", "guard_faults=0", "guard_faults=1"),
            ("A", "snapshot_faults=0", "snapshot_faults=1"),
            ("C", "application_reserve_busy=0", "application_reserve_busy=1"),
            ("A", "application_commit_busy=0", "application_commit_busy=1"),
        )
        for target, old, new in cases:
            def transform(role: str, payload: str, target: str = target, old: str = old, new: str = new) -> str:
                return payload.replace(old, new) if role == target else payload

            with self.subTest(counter=old):
                self.assertEqual(self.snapshot(transform=transform)["proving_status"], "INVALID")

    def test_transport_checkpoint_requires_every_role_to_be_clean(self) -> None:
        for target, old, new in (
                ("A", "uart_pending_bytes=0", "uart_pending_bytes=1"),
                ("B", "uart_dropped_bytes=0", "uart_dropped_bytes=1"),
                ("D", "uart_dropped_records=0", "uart_dropped_records=1"),
                ("F", "uart_transport_faults=0", "uart_transport_faults=1")):
            def transform(role: str, payload: str, target: str = target,
                          old: str = old, new: str = new) -> str:
                return payload.replace(old, new) if role == target else payload

            with self.subTest(counter=old):
                data = self.snapshot(transform=transform)
                self.assertEqual(data["proving_status"], "INVALID")
                self.assertEqual(data["validity"]["transport"]["status"], "INVALID")

        def no_role_checkpoint(role: str, payload: str) -> str:
            return "\n".join(item for item in payload.splitlines()
                             if " obs_health " not in item) + "\n" if role == "E" else payload

        data = self.snapshot(transform=no_role_checkpoint)
        self.assertEqual(data["proving_status"], "INCOMPLETE")
        self.assertIn("missing_transport_checkpoint:E",
                      data["validity"]["transport"]["reasons"])

    def test_duplicate_and_unmatched_identities_invalidate_application(self) -> None:
        def duplicate_accepted(role: str, payload: str) -> str:
            return payload.replace("identity=0x000001", "identity=0x000000", 1) if role == "A" else payload

        def duplicate_final(role: str, payload: str) -> str:
            return payload.replace("identity=0x000001", "identity=0x000000", 1) if role == "C" else payload

        def unmatched_final(role: str, payload: str) -> str:
            return payload.replace("identity=0x000002", "identity=0x000003", 1) if role == "C" else payload

        for transform, reason in ((duplicate_accepted, "duplicate_accepted_identity"),
                                  (duplicate_final, "duplicate_final_identity"),
                                  (unmatched_final, "unmatched_final_identity")):
            with self.subTest(reason=reason):
                data = self.snapshot(transform=transform)
                self.assertEqual(data["proving_status"], "INVALID")
                self.assertIn(reason, data["validity"]["application"]["reasons"])

    def test_terminal_partial_events_after_complete_checkpoints_are_right_censored(self) -> None:
        cases = (("A", "routed obs_accept schema=observer-v3"),
                 ("C", "routed obs_final schema=observer-v3"),
                 ("A", "routed obs_health schema=observer-v3"))
        for role, suffix in cases:
            def transform(current: str, payload: str, role: str = role, suffix: str = suffix) -> str:
                return payload + suffix if current == role else payload

            with self.subTest(suffix=suffix):
                data = self.snapshot(transform=transform, terminal_roles={role})
                self.assertEqual(data["proving_status"], "VALID")
                self.assertEqual(len(data["app_packets"]), 5)

    def test_record_id_gap_before_checkpoint_invalidates_but_gap_after_is_censored(self) -> None:
        def before(role: str, payload: str) -> str:
            if role != "A":
                return payload
            lines = payload.splitlines()
            index = next(index for index, item in enumerate(lines) if " obs_accept " in item)
            lines[index] = re.sub(r"record_id_lo=(\d+)",
                                  lambda match: f"record_id_lo={int(match.group(1)) + 1}", lines[index])
            return "\n".join(lines) + "\n"

        invalid = self.snapshot(transform=before)
        self.assertEqual(invalid["proving_status"], "INVALID")
        self.assertIn("source_record_id_gap_before_checkpoint",
                      invalid["validity"]["application"]["reasons"])

        def after(role: str, payload: str) -> str:
            if role != "A":
                return payload
            record_id = len(payload.splitlines()) + 2
            return payload + line(69_000, wire_record("clock", 69_000, "1", 1, record_id)) + "\n"

        censored = self.snapshot(transform=after)
        self.assertEqual(censored["proving_status"], "VALID")
        self.assertTrue(censored["telemetry_corruption"])

    def test_non_one_first_record_id_and_wrong_event_roles_invalidate_application(self) -> None:
        def first_record_id_gap(role: str, payload: str) -> str:
            if role != "A":
                return payload
            return re.sub(r"record_id_hi=0 record_id_lo=(\d+)",
                          lambda match: f"record_id_hi=0 record_id_lo={int(match.group(1)) + 1}",
                          payload)

        data = self.snapshot(transform=first_record_id_gap)
        self.assertEqual(data["proving_status"], "INVALID")
        self.assertIn("source_record_id_gap_before_checkpoint",
                      data["validity"]["application"]["reasons"])

        def wrong_roles(role: str, payload: str) -> str:
            record_id = len(payload.splitlines()) + 1
            if role == "B":
                return payload + line(69_000, wire_record(
                    "accept", 69_000, "2", 2, record_id,
                    "offered_at_ms=69000 accepted_at_ms=69001 identity=0x000000 submit_status=0")) + "\n"
            if role == "F":
                return payload + line(69_000, wire_record(
                    "final", 69_000, "6", 6, record_id,
                    "delivered_at_ms=69000 origin_session=1 identity_valid=1 identity=0x000000 "
                    "logical_origin=0x0001 logical_destination=0x0003 app_kind=127 app_len=7")) + "\n"
            return payload

        data = self.snapshot(transform=wrong_roles)
        self.assertEqual(data["proving_status"], "INVALID")
        self.assertIn("unexpected_accept_role", data["validity"]["application"]["reasons"])
        self.assertIn("unexpected_final_role", data["validity"]["application"]["reasons"])

    def test_auxiliary_relay_failures_do_not_invalidate_application(self) -> None:
        def transform(role: str, payload: str) -> str:
            if role == "B":
                return "\n".join(item for item in payload.splitlines() if " obs_control " not in item) + "\n"
            if role == "D":
                return payload.replace("diagnostic_dropped=0", "diagnostic_dropped=1")
            return payload

        data = self.snapshot(transform=transform)
        self.assertEqual(data["proving_status"], "VALID")
        self.assertEqual(data["validity"]["application"]["status"], "VALID")
        self.assertNotEqual(data["validity"]["control"]["status"], "VALID")

    def test_aodv_gtt_and_full_gtt_validation_are_preserved(self) -> None:
        self.assertEqual(self.snapshot(profile="AODV_ONLY")["proving_status"], "VALID")

        def invalid_departed(role: str, payload: str) -> str:
            return payload.replace("departed=1", "departed=0", 1) if role == "A" else payload

        with self.assertRaisesRegex(MODULE.CaptureError, "departed outside"):
            self.state(transform=invalid_departed)

        def invalid_capacity(role: str, payload: str) -> str:
            return payload.replace("slot_capacity=16", "slot_capacity=15", 1) \
                if role == "A" else payload

        with self.assertRaisesRegex(MODULE.CaptureError,
                                    "invalid FULL GTT slot_capacity"):
            self.state(transform=invalid_capacity)

    def test_invalid_gtt_snapshot_does_not_discard_application_evidence(self) -> None:
        def transform(role: str, payload: str) -> str:
            if role != "A":
                return payload
            lines = [item for item in payload.splitlines() if " obs_gtt_entry " not in item]
            begin = next(index for index, item in enumerate(lines) if " obs_gtt_begin " in item)
            lines[begin] = lines[begin].replace("retained_entry_count=5",
                                                "retained_entry_count=0")
            end = next(index for index, item in enumerate(lines) if " obs_gtt_end " in item)
            lines[end] = lines[end].replace("status=OK entry_count=5 nondeparted_count=5",
                                             "status=INVALID entry_count=0 nondeparted_count=0")
            return "\n".join(re.sub(
                r"record_id_hi=\d+ record_id_lo=\d+",
                f"record_id_hi=0 record_id_lo={record_id}", item)
                for record_id, item in enumerate(lines, 1)) + "\n"

        data = self.snapshot(transform=transform)
        self.assertEqual(data["proving_status"], "VALID")
        self.assertEqual(data["validity"]["gtt"]["status"], "INVALID")
        self.assertIn("invalid_gtt_snapshot:A", data["validity"]["gtt"]["reasons"])

        def stale_invalid_snapshot(role: str, payload: str) -> str:
            if role != "A":
                return payload
            lines = [item for item in payload.splitlines() if " obs_gtt_entry " not in item]
            end = next(index for index, item in enumerate(lines) if " obs_gtt_end " in item)
            lines[end] = lines[end].replace("status=OK entry_count=5 nondeparted_count=5",
                                             "status=INVALID entry_count=0 nondeparted_count=0")
            return "\n".join(re.sub(
                r"record_id_hi=\d+ record_id_lo=\d+",
                f"record_id_hi=0 record_id_lo={record_id}", item)
                for record_id, item in enumerate(lines, 1)) + "\n"

        with self.assertRaisesRegex(MODULE.CaptureError,
                                    "invalid INVALID GTT evidence"):
            self.state(transform=stale_invalid_snapshot)

    def test_replay_and_plot_rendering_are_preserved(self) -> None:
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

    def test_sqlite_store_and_snapshot_parity(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp)
            path = folder / "observations.sqlite3"
            store = MODULE.SQLiteObservationRecordStore(path)
            record = MODULE.ObservationRecord(
                "clock", "A", "1", "observer-v3", 42, 4_294_967_338, None,
                1_786_003_200_123.125,
                {"now": "42", "role": "A", "unicode": "π", "empty": ""}, "synthetic.log", 17)
            store.append(record)
            self.assertEqual(list(store), [record])
            store.flush()
            connection = sqlite3.connect(path)
            try:
                self.assertEqual(connection.execute("SELECT COUNT(*) FROM observation_records").fetchone()[0], 1)
            finally:
                connection.close()
            store.close()

            memory = self.state()
            durable = self.state(database_path=folder / "durable.sqlite3")
            self.assertEqual(MODULE.snapshot(durable), MODULE.snapshot(memory))
            self.assertEqual(durable.retained_record_count, 0)
            durable.close()


if __name__ == "__main__":
    unittest.main()
