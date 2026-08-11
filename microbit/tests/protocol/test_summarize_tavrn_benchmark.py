#!/usr/bin/env python3
"""Focused fixtures for strict six-board benchmark analysis."""

from __future__ import annotations

import datetime as dt
import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "summarize_tavrn_benchmark.py"
SPEC = importlib.util.spec_from_file_location("summarize_tavrn_benchmark", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

BASE = dt.datetime(2026, 8, 11, tzinfo=dt.timezone.utc)


def timestamp(milliseconds: int) -> str:
    return (BASE + dt.timedelta(milliseconds=milliseconds)).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def telemetry_line(milliseconds: int, payload: str) -> str:
    return f"[{timestamp(milliseconds)} 0.000000] {payload}"


def adva(index: int) -> str:
    return f"{index:02x}:42:de:52:4a:dd"


def metadata() -> dict[str, object]:
    return {
        "schema": MODULE.RUN_SCHEMA,
        "status": "completed",
        "boards": [{"role": role, "uid": f"{index:048x}", "adva": adva(index)}
                   for index, role in enumerate(MODULE.ROLES, 1)],
        "benchmark": {
            "source_role": "A", "destination_role": "C", "origin": "18",
            "destination": "1e", "width": "SID8", "offered_slots": 4,
            "offered_seconds": 4, "drain_seconds": 1,
            "clock_min_span_seconds": 350, "offered_end_utc": timestamp(300000),
        },
        "terminal_counters": {"A": {"retry_exhausted": 0}, "C": {"fault_terminal": 0}},
    }


def board_lines(role: str, outlier: int | None = None, include_final_three: bool = True) -> list[str]:
    events: list[tuple[int, str, int]] = []
    for now in range(0, 380001, 20000):
        host = now + (50 if outlier == now else 0)
        events.append((now, f"routed bench_clock now={now} role={role}", host))
    if role == "A":
        for now, counter, accepted in ((100001, 1, 1), (200001, 2, 1), (300001, 3, 1), (320001, 4, 0)):
            events.append((now,
                           f"routed bench_attempt now={now} role=A slot={counter - 1} counter={counter} "
                           f"accepted={accepted} status={'ACCEPTED' if accepted else 'REJECTED'} "
                           "destination=1e width=SID8", now))
        summary = (381000, "routed bench_summary now=381000 role=A offered=4 attempted=4 accepted=3 "
                   "rejected=1 skipped=0 not_ready=0 attempt_q=0 attempt_q_dropped=0 "
                   "attempt_q_high_water=1 final_q=0")
    elif role == "C":
        finals = [(100121, 1, 7), (200151, 2, 9)]
        if include_final_three:
            finals.append((300501, 3, 8))
        for now, counter, app_len in finals:
            events.append((now, f"routed bench_final now={now} role=C origin=18 destination=1e "
                                f"counter_valid=1 counter={counter} app_len={app_len} peer=dc", now))
        summary = (381000, "routed bench_summary now=381000 role=C offered=0 attempted=0 accepted=0 "
                   "rejected=0 skipped=0 not_ready=0 attempt_q=0 attempt_q_dropped=0 "
                   "attempt_q_high_water=0 final_q=0")
    else:
        summary = (381000, f"routed bench_summary now=381000 role={role} offered=0 attempted=0 accepted=0 "
                   "rejected=0 skipped=0 not_ready=0 attempt_q=0 attempt_q_dropped=0 "
                   "attempt_q_high_water=0 final_q=0")
    events.append((summary[0], summary[1], summary[0]))
    return [telemetry_line(host, text) for _now, text, host in sorted(events)]


class SummarizeTavrnBenchmarkTests(unittest.TestCase):
    def make_logs(self, directory: pathlib.Path, **kwargs: object) -> dict[str, pathlib.Path]:
        paths: dict[str, pathlib.Path] = {}
        for role in MODULE.ROLES:
            path = directory / f"{role}.log"
            path.write_text("\n".join(board_lines(role, **kwargs)) + "\n", encoding="utf-8")
            paths[role] = path
        return paths

    def test_exact_metrics_nearest_rank_and_late_drain(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths = self.make_logs(pathlib.Path(temp))
            for number, role in enumerate(MODULE.ROLES, 1):
                paths[role].write_text(paths[role].read_text(encoding="utf-8").replace(
                    f"role={role}", f"role={number}").replace("width=SID8", "width=1"),
                    encoding="utf-8")
            capture = MODULE.derive_capture(metadata(), paths)
        self.assertEqual(capture["metrics"], {
            "accepted_attempts": 3,
            "delivered_packets": 3,
            "delivered_app_bytes": 24,
            "accepted_pdr": 1.0,
            "offered_delivery_ratio": 0.75,
            "packet_goodput_packets_per_second": 0.75,
            "app_goodput_bytes_per_second": 6.0,
            "one_way_latency": {"p50_ms": 150.0, "p95_ms": 500.0, "p99_ms": 500.0},
        })
        self.assertEqual(capture["counters"]["late_drain_counters"], [3])
        self.assertEqual(capture["counters"]["lost_counters"], [])
        self.assertEqual(MODULE.nearest_rank([1.0, 2.0, 3.0, 4.0], 50), 2.0)
        self.assertEqual(MODULE.nearest_rank([1.0, 2.0, 3.0, 4.0], 95), 4.0)
        self.assertIn("TAVRN six-board benchmark", MODULE.render_markdown(capture))

    def test_drift_and_one_mad_outlier_are_retained_at_ninety_five_percent(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            paths = self.make_logs(pathlib.Path(temp), outlier=380000)
            capture = MODULE.derive_capture(metadata(), paths)
        self.assertEqual(capture["clock_fits"]["A"]["rejected_points"], 1)
        self.assertEqual(capture["clock_fits"]["A"]["retained_points"], 19)
        self.assertEqual(capture["clock_fits"]["A"]["retained_ratio"], 0.95)
        self.assertEqual(capture["clock_fits"]["A"]["slope"], 1.0)

    def test_insufficient_span_and_reboot_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp)
            paths = self.make_logs(folder)
            with self.assertRaisesRegex(MODULE.CaptureError, "clock span"):
                MODULE.derive_capture(metadata(), paths, min_clock_span_seconds=400)
            lines = paths["B"].read_text(encoding="utf-8").splitlines()
            lines.insert(2, telemetry_line(20001, "routed bench_clock now=0 role=B"))
            paths["B"].write_text("\n".join(lines) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(MODULE.CaptureError, "rollback/reboot"):
                MODULE.derive_capture(metadata(), paths)

    def test_duplicate_missing_malformed_and_telemetry_loss_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp)
            paths = self.make_logs(folder)
            lines = paths["A"].read_text(encoding="utf-8").splitlines()
            duplicate = next(line for line in lines if "bench_attempt" in line and "counter=1 " in line)
            duplicate_index = lines.index(duplicate)
            lines.insert(duplicate_index + 1, duplicate)
            paths["A"].write_text("\n".join(lines) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(MODULE.CaptureError, "duplicate accepted"):
                MODULE.derive_capture(metadata(), paths)

            paths = self.make_logs(folder, include_final_three=False)
            capture = MODULE.derive_capture(metadata(), paths)
            self.assertEqual(capture["counters"]["lost_counters"], [3])

            lines = paths["D"].read_text(encoding="utf-8").splitlines()
            lines[0] = "not-a-timestamp routed bench_clock now=0 role=D"
            paths["D"].write_text("\n".join(lines) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(MODULE.CaptureError, "malformed timestamped"):
                MODULE.derive_capture(metadata(), paths)

            paths = self.make_logs(folder)
            lines = paths["E"].read_text(encoding="utf-8").splitlines()
            lines[-1] = lines[-1].replace("attempt_q_dropped=0", "attempt_q_dropped=1")
            paths["E"].write_text("\n".join(lines) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(MODULE.CaptureError, "dropped benchmark telemetry"):
                MODULE.derive_capture(metadata(), paths)

    def test_duplicate_final_role_mismatch_and_host_reversal_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp)
            paths = self.make_logs(folder)
            lines = paths["C"].read_text(encoding="utf-8").splitlines()
            duplicate = next(line for line in lines if "bench_final" in line and "counter=1 " in line)
            duplicate_index = lines.index(duplicate)
            lines.insert(duplicate_index + 1, duplicate)
            paths["C"].write_text("\n".join(lines) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(MODULE.CaptureError, "duplicate final"):
                MODULE.derive_capture(metadata(), paths)

            paths = self.make_logs(folder)
            lines = paths["F"].read_text(encoding="utf-8").splitlines()
            lines[0] = lines[0].replace("role=F", "role=E")
            paths["F"].write_text("\n".join(lines) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(MODULE.CaptureError, "role mismatch"):
                MODULE.derive_capture(metadata(), paths)

            paths = self.make_logs(folder)
            lines = paths["D"].read_text(encoding="utf-8").splitlines()
            lines[2] = lines[2].replace(timestamp(40000), timestamp(1))
            paths["D"].write_text("\n".join(lines) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(MODULE.CaptureError, "host-time reversal"):
                MODULE.derive_capture(metadata(), paths)

    def test_cli_emits_deterministic_json_and_markdown(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp)
            paths = self.make_logs(folder)
            metadata_path = folder / "run.json"
            metadata_path.write_text(json.dumps(metadata()), encoding="utf-8")
            output = folder / "capture.json"
            markdown = folder / "capture.md"
            argv = ["--run-metadata", str(metadata_path), "--output", str(output),
                    "--markdown-output", str(markdown)]
            argv.extend(argument for role, path in sorted(paths.items())
                        for argument in ("--serial-log", f"{role}={path}"))
            self.assertEqual(MODULE.main(argv), 0)
            self.assertEqual(json.loads(output.read_text(encoding="utf-8"))["status"], "PASS")
            self.assertTrue(markdown.read_text(encoding="utf-8").endswith("|\n"))


if __name__ == "__main__":
    unittest.main()
