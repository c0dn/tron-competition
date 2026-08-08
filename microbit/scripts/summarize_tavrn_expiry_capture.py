#!/usr/bin/env python3
"""Convert strict routed expiry telemetry and its artifact provenance to JSON."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


SCHEMA = "tron.tavrn.expiry.hardware.capture.v1"
REQUIRED_FIELDS = {
    "now_ms", "pass", "trace_count", "cursor", "soft_selected",
    "hard_selected", "demand_deferred", "departed", "purged", "max_copied",
    "unavailable_count",
    "targeted_controls", "rreq_controls", "expiry_tc_controls",
    "received_evidence", "complete_passes", "max_scheduler_gap_ms",
    "scheduler_fault", "mesh_fault", "router_fault",
}
LINE_PREFIX = "routed expiry_sweep "


class CaptureError(Exception):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_manifest(path: Path, expected_hash: str) -> dict[str, str]:
    if not re.fullmatch(r"[0-9a-f]{64}", expected_hash):
        raise CaptureError("artifact manifest hash must be a lowercase SHA-256")
    if sha256_file(path) != expected_hash:
        raise CaptureError("artifact manifest hash differs from --artifact-manifest-sha256")
    values: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as error:
        raise CaptureError("cannot read artifact manifest: %s" % error) from error
    for line in lines:
        if not line or "=" not in line:
            raise CaptureError("artifact manifest contains a malformed line")
        key, value = line.split("=", 1)
        if not key or key in values:
            raise CaptureError("artifact manifest has an empty or duplicate key")
        values[key] = value
    required = {
        "artifact.elf.sha256": None,
        "feature.level.effective": "FULL_TAVRN",
        "build.timer_profile": "FAST_TEST",
        "hook.enabled": "ON",
        "hook.expiry_full_table": "ON",
    }
    for key, expected in required.items():
        value = values.get(key)
        if value is None or (expected is not None and value != expected):
            raise CaptureError("artifact manifest lacks required %s=%s" % (key, expected))
    if not re.fullmatch(r"[0-9a-f]{64}", values["artifact.elf.sha256"]):
        raise CaptureError("artifact manifest ELF hash is invalid")
    return values


def parse_line(line: str, source: Path, line_number: int) -> dict[str, int] | None:
    if not line.startswith(LINE_PREFIX):
        return None
    values: dict[str, int] = {}
    fields = line[len(LINE_PREFIX):].split()
    if not fields:
        raise CaptureError("%s:%d has no expiry fields" % (source, line_number))
    for field in fields:
        if field.count("=") != 1:
            raise CaptureError("%s:%d has a malformed expiry field" % (source, line_number))
        key, value = field.split("=", 1)
        if key in values or not re.fullmatch(r"[a-z_]+", key) or \
           not re.fullmatch(r"(?:0|[1-9][0-9]*)", value):
            raise CaptureError("%s:%d has an invalid expiry field" % (source, line_number))
        values[key] = int(value, 10)
    if set(values) != REQUIRED_FIELDS:
        missing = ",".join(sorted(REQUIRED_FIELDS - set(values)))
        extra = ",".join(sorted(set(values) - REQUIRED_FIELDS))
        raise CaptureError("%s:%d expiry fields mismatch missing=%s extra=%s" %
                           (source, line_number, missing or "-", extra or "-"))
    return values


def derive_capture(logs: list[Path], cadence_ms: int) -> dict[str, int]:
    records: list[dict[str, int]] = []
    fault_lines = 0
    for path in logs:
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except (OSError, UnicodeDecodeError) as error:
            raise CaptureError("cannot read serial log %s: %s" % (path, error)) from error
        for line_number, line in enumerate(lines, 1):
            record = parse_line(line, path, line_number)
            if record is not None:
                records.append(record)
                continue
            if line.startswith("routed router_fault ") or \
               line.startswith("routed cycle_diagnostic "):
                fault_lines += 1
    if not records:
        raise CaptureError("serial logs contain no routed expiry_sweep telemetry")

    complete_passes = 0
    hard_selected_demand_deferred_passes = 0
    max_scheduler_gap_ms = 0
    unavailable_count = 0
    faults = fault_lines
    targeted_controls = 0
    rreq_controls = 0
    tc_expiry_controls = 0
    previous: dict[str, int] | None = None
    pass_offset: int | None = None
    complete_pass_offset: int | None = None
    for index, record in enumerate(records, 1):
        if pass_offset is None:
            if record["pass"] == 0:
                raise CaptureError("expiry pass numbering must be nonzero")
            pass_offset = record["pass"] - 1
            first_complete = 1 if record["trace_count"] == 16 else 0
            if record["complete_passes"] < first_complete:
                raise CaptureError("reported complete-pass total is invalid")
            complete_pass_offset = record["complete_passes"] - first_complete
        assert complete_pass_offset is not None
        if record["pass"] != pass_offset + index:
            raise CaptureError("expiry pass numbering is missing, repeated, or out of order")
        if record["trace_count"] > 16 or record["cursor"] > 15 or \
           record["max_copied"] > 1:
            raise CaptureError("expiry telemetry violates its bounded table contract")
        if record["trace_count"] == 16:
            complete_passes += 1
            if record["hard_selected"] != 0 and record["demand_deferred"] != 0:
                hard_selected_demand_deferred_passes += 1
        if record["complete_passes"] != complete_pass_offset + complete_passes:
            raise CaptureError("reported complete-pass total does not match raw telemetry")
        if record["scheduler_fault"] or record["mesh_fault"] or record["router_fault"] or \
           record["unavailable_count"]:
            faults += 1
        unavailable_count += record["unavailable_count"]
        max_scheduler_gap_ms = max(max_scheduler_gap_ms, record["max_scheduler_gap_ms"])
        targeted_controls += record["targeted_controls"]
        rreq_controls += record["rreq_controls"]
        tc_expiry_controls += record["expiry_tc_controls"]
        if previous is not None:
            interval = record["now_ms"] - previous["now_ms"]
            if interval < cadence_ms or interval > cadence_ms + 2:
                raise CaptureError("expiry telemetry has a missing or non-cadence due pass")
        previous = record
    assert previous is not None
    duration_ms = records[-1]["now_ms"] - records[0]["now_ms"] + cadence_ms
    if duration_ms < cadence_ms:
        raise CaptureError("expiry telemetry timestamp order is invalid")
    if max_scheduler_gap_ms == 0:
        raise CaptureError("expiry telemetry fabricated a zero scheduler gap")
    return {
        "duration_seconds": duration_ms // 1000,
        "complete_passes": complete_passes,
        "hard_selected_demand_deferred_passes": hard_selected_demand_deferred_passes,
        "max_scheduler_gap_ms": max_scheduler_gap_ms,
        "unavailable_count": unavailable_count,
        "faults": faults,
        "targeted_controls": targeted_controls,
        "rreq_controls": rreq_controls,
        "tc_expiry_controls": tc_expiry_controls,
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial-log", action="append", required=True)
    parser.add_argument("--artifact-manifest", required=True)
    parser.add_argument("--artifact-manifest-sha256", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    try:
        manifest_path = Path(args.artifact_manifest)
        manifest = parse_manifest(manifest_path, args.artifact_manifest_sha256)
        try:
            cadence = int(manifest.get("timer.gtt_maintenance_ms", "0"), 10)
        except ValueError as error:
            raise CaptureError("artifact manifest expiry cadence is not decimal") from error
        if cadence <= 0:
            raise CaptureError("artifact manifest lacks a positive expiry cadence")
        metrics = derive_capture([Path(path) for path in args.serial_log], cadence)
        capture: dict[str, Any] = {
            "schema": SCHEMA,
            **metrics,
            "provenance": {
                "artifact_manifest_name": manifest_path.name,
                "artifact_manifest_sha256": args.artifact_manifest_sha256,
                "artifact_elf_sha256": manifest["artifact.elf.sha256"],
                "feature": manifest["feature.level.effective"],
                "timer": manifest["build.timer_profile"],
                "hooks": manifest["hook.enabled"],
                "expiry_full_table": manifest["hook.expiry_full_table"],
            },
        }
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(capture, indent=2, sort_keys=True) + "\n",
                          encoding="utf-8")
    except CaptureError as error:
        print("FAIL hardware_capture: %s" % error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
