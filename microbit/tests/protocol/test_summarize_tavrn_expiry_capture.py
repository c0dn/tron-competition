#!/usr/bin/env python3
"""Focused pass/fail fixtures for the expiry hardware-capture summarizer."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile


def sha256_file(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    fixtures = root / "tests" / "protocol" / "fixtures" / "tavrn_expiry_capture"
    summarizer = root / "scripts" / "summarize_tavrn_expiry_capture.py"
    checker_path = root / "scripts" / "check_tavrn_expiry_resources.py"
    manifest = fixtures / "artifact.manifest"
    manifest_hash = sha256_file(manifest)
    with tempfile.TemporaryDirectory() as directory:
        output = pathlib.Path(directory) / "capture.json"
        command = [sys.executable, str(summarizer), "--serial-log",
                   str(fixtures / "pass.log"), "--artifact-manifest", str(manifest),
                   "--artifact-manifest-sha256", manifest_hash, "--output", str(output)]
        if subprocess.run(command, check=False).returncode != 0 or not output.is_file():
            return 1
        capture = json.loads(output.read_text(encoding="utf-8"))
        expected = {
            "schema": "tron.tavrn.expiry.hardware.capture.v1",
            "duration_seconds": 7,
            "complete_passes": 3,
            "hard_selected_demand_deferred_passes": 2,
            "max_scheduler_gap_ms": 2,
            "unavailable_count": 0,
            "faults": 0,
            "targeted_controls": 0,
            "rreq_controls": 0,
            "tc_expiry_controls": 0,
        }
        if any(capture.get(key) != value for key, value in expected.items()):
            return 1
        midstream_output = pathlib.Path(directory) / "midstream-capture.json"
        midstream = command.copy()
        midstream[midstream.index(str(fixtures / "pass.log"))] = str(
            fixtures / "pass-midstream.log")
        midstream[midstream.index(str(output))] = str(midstream_output)
        if subprocess.run(midstream, check=False).returncode != 0:
            return 1
        midstream_capture = json.loads(midstream_output.read_text(encoding="utf-8"))
        if any(midstream_capture.get(key) != value for key, value in expected.items()):
            return 1
        provenance = capture.get("provenance")
        if provenance != {
            "artifact_manifest_name": manifest.name,
            "artifact_manifest_sha256": manifest_hash,
            "artifact_elf_sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            "feature": "FULL_TAVRN",
            "timer": "FAST_TEST",
            "hooks": "ON",
            "expiry_full_table": "ON",
        }:
            return 1
        invalid = [sys.executable, str(summarizer), "--serial-log",
                   str(fixtures / "fail-malformed.log"), "--artifact-manifest", str(manifest),
                   "--artifact-manifest-sha256", manifest_hash, "--output", str(output)]
        if subprocess.run(invalid, check=False, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode != 1:
            return 1
        zero_gap = [sys.executable, str(summarizer), "--serial-log",
                    str(fixtures / "fail-zero-gap.log"), "--artifact-manifest",
                    str(manifest), "--artifact-manifest-sha256", manifest_hash,
                    "--output", str(output)]
        if subprocess.run(zero_gap, check=False, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode != 1:
            return 1
        bad_hash = command.copy()
        bad_hash[bad_hash.index(manifest_hash)] = "0" * 64
        if subprocess.run(bad_hash, check=False, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode != 1:
            return 1
        spec = importlib.util.spec_from_file_location("expiry_checker", checker_path)
        if spec is None or spec.loader is None:
            return 1
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        loaded = module.load_capture(output, manifest)
        if loaded["complete_passes"] != 3 or \
                loaded["hard_selected_demand_deferred_passes"] != 2:
            return 1
        unavailable_output = pathlib.Path(directory) / "unavailable-capture.json"
        unavailable = [sys.executable, str(summarizer), "--serial-log",
                       str(fixtures / "fail-unavailable.log"), "--artifact-manifest",
                       str(manifest), "--artifact-manifest-sha256", manifest_hash,
                       "--output", str(unavailable_output)]
        if subprocess.run(unavailable, check=False).returncode != 0:
            return 1
        unavailable_capture = module.load_capture(unavailable_output)
        if unavailable_capture["unavailable_count"] != 1 or \
                unavailable_capture["faults"] != 1:
            return 1
        try:
            module.validate_capture({"capture": unavailable_capture})
        except module.CheckFailure:
            return 0
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
