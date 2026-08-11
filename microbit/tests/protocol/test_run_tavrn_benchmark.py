#!/usr/bin/env python3
"""Focused fail-closed fixtures for the six-board benchmark runner."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "run_tavrn_benchmark.py"
SPEC = importlib.util.spec_from_file_location("run_tavrn_benchmark", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def adva(index: int) -> str:
    return f"{index:02x}:42:de:52:4a:dd"


class RunTavrnBenchmarkTests(unittest.TestCase):
    def make_fixture(self, folder: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path, dict[str, object]]:
        inventory = folder / "inventory.tsv"
        inventory.write_text("# probe_uid\tcanonical_adva\n" + "\n".join(
            f"{index:048x}\t{adva(index)}" for index in range(1, 7)) + "\n", encoding="utf-8")
        boards: list[dict[str, object]] = []
        for index, role in enumerate(MODULE.ROLES, 1):
            uid = f"{index:048x}"
            elf = folder / f"{role}.elf"
            hex_file = folder / f"{role}.hex"
            elf.write_bytes(f"elf-{role}".encode())
            hex_file.write_bytes(f"hex-{role}".encode())
            manifest = folder / f"{role}.manifest"
            manifest.write_text("\n".join([
                "build.target=tavrn_routed_node", f"build.role={role.lower()}",
                f"bench.role_number={index}", f"identity.target_probe_uid={uid}",
                f"identity.adva={adva(index)}", "feature.level.effective=FULL_TAVRN",
                "build.timer_profile=BALANCED", "candidate.configured=ON",
                "candidate.scope=BENCH_HOOKED_RESTRICTED", "source.commit=" + "a" * 40,
                "source.tree=" + "b" * 40, f"artifact.elf.sha256={digest(elf)}",
                f"artifact.hex.sha256={digest(hex_file)}", "hook.rx_block_adva=NOT_CONFIGURED",
                "bench.offered_slots=2",
            ]) + "\n", encoding="utf-8")
            boards.append({
                "role": role, "uid": uid, "adva": adva(index),
                "serial_device": f"/dev/serial/by-id/{uid}",
                "artifact": {
                    "manifest": str(manifest), "manifest_sha256": digest(manifest),
                    "elf": str(elf), "elf_sha256": digest(elf),
                    "hex": str(hex_file), "hex_sha256": digest(hex_file),
                    "flash_image": {"format": "hex", "path": str(hex_file), "sha256": digest(hex_file)},
                },
                "manifest_hooks": {"hook.rx_block_adva": "NOT_CONFIGURED"},
                "manifest_benchmark": {"bench.offered_slots": "2"},
            })
        plan = {
            "schema": MODULE.PLAN_SCHEMA, "source": {"commit": "a" * 40, "tree": "b" * 40},
            "profile": "FULL_TAVRN", "timer_profile": "BALANCED",
            "candidate_scope": "BENCH_HOOKED_RESTRICTED",
            "benchmark": {"source_role": "A", "destination_role": "C", "origin": "18",
                          "destination": "1e", "width": "SID8", "warmup_seconds": 1,
                          "offered_seconds": 2, "drain_seconds": 1, "offered_rate_hz": 1,
                          "interval_ms": 1000, "offered_slots": 2},
            "boards": boards,
        }
        plan_path = folder / "plan.json"
        plan_path.write_text(json.dumps(plan), encoding="utf-8")
        return inventory, plan_path, plan

    def test_preflight_validates_all_published_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            inventory_path, plan_path, _plan = self.make_fixture(pathlib.Path(temp))
            checked = MODULE.preflight(MODULE.load_plan(plan_path), MODULE.parse_inventory(inventory_path))
        self.assertEqual([board["role"] for board in checked], list(MODULE.ROLES))
        self.assertEqual(checked[0]["artifact"]["flash_image"]["format"], "hex")

    def test_manifest_hash_role_uid_adva_and_hook_mismatches_fail_closed(self) -> None:
        cases = (
            ("manifest_sha256", "0" * 64, "manifest hash mismatch"),
            ("role", "B", "duplicate/invalid role"),
            ("uid", "f" * 48, "UID/AdvA does not exactly match inventory"),
            ("adva", "18:42:de:52:4a:dd", "UID/AdvA does not exactly match inventory"),
            ("hook", "ON", "manifest mismatch hook.rx_block_adva"),
        )
        for kind, value, expected in cases:
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temp:
                inventory_path, plan_path, plan = self.make_fixture(pathlib.Path(temp))
                board = plan["boards"][0]
                assert isinstance(board, dict)
                if kind == "manifest_sha256":
                    board["artifact"]["manifest_sha256"] = value
                elif kind == "hook":
                    board["manifest_hooks"]["hook.rx_block_adva"] = value
                else:
                    board[kind] = value
                    if kind == "uid":
                        board["serial_device"] = f"/dev/serial/by-id/{value}"
                plan_path.write_text(json.dumps(plan), encoding="utf-8")
                with self.assertRaisesRegex(MODULE.RunError, expected):
                    MODULE.preflight(MODULE.load_plan(plan_path), MODULE.parse_inventory(inventory_path))

    def test_dry_run_records_exact_uid_commands_without_tools(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp)
            inventory, plan, _ = self.make_fixture(folder)
            output = folder / "run.json"
            result = MODULE.main(["--inventory", str(inventory), "--run-plan", str(plan),
                                  "--output", str(output), "--dry-run", "--pyocd", "fake-pyocd",
                                  "--grabserial", "fake-grabserial"])
            data = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(result, 0)
        self.assertEqual(data["status"], "dry_run")
        commands = [event["command"] for event in data["events"]]
        self.assertIn(["fake-pyocd", "commander", "--uid", f"{1:048x}", "--target", "nrf52833",
                       "--command", "read32 0x100000a4 8"], commands)
        self.assertIn(["fake-grabserial", "-d", f"/dev/serial/by-id/{1:048x}", "-b", "115200",
                       "-T", "-F", "%Y-%m-%dT%H:%M:%S.%fZ", "-e", "4"], commands)
        self.assertIn("-T", MODULE.grabserial_command("grabserial", data["boards"][0], 4))
        self.assertIn("%Y-%m-%dT%H:%M:%S.%fZ", MODULE.grabserial_command("grabserial", data["boards"][0], 4))

    def test_hardware_path_requires_explicit_execute(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp)
            inventory, plan, _ = self.make_fixture(folder)
            output = folder / "run.json"
            self.assertEqual(MODULE.main(["--inventory", str(inventory), "--run-plan", str(plan),
                                          "--output", str(output)]), 1)
            self.assertEqual(json.loads(output.read_text(encoding="utf-8"))["status"], "failed")

    def test_ficr_derivation_and_runner_mismatch(self) -> None:
        self.assertEqual(MODULE.derive_canonical_adva(0x52DE4218, 0x00001D4A), "18:42:de:52:4a:dd")
        self.assertEqual(MODULE.parse_ficr_words("0x100000a4: 0x52de4218 0x00001d4a\n"),
                         (0x52DE4218, 0x1D4A))
        self.assertEqual(MODULE.parse_ficr_words("0x100000a4: 52de4218 00001d4a\n"),
                         (0x52DE4218, 0x1D4A))
        self.assertEqual(MODULE.parse_ficr_words("0x100000a4: 52de4218\n0x100000a8: 00001d4a\n"),
                         (0x52DE4218, 0x1D4A))
        board = {"role": "A", "uid": "1" * 48, "adva": "18:42:de:52:4a:dd"}

        def runner(*_args: object, **_kwargs: object) -> subprocess.CompletedProcess[str]:
            return subprocess.CompletedProcess([], 0, "0x100000a4: 0x52de4218 0x00001d4a\n", "")

        self.assertEqual(MODULE.run_ficr_check(board, "pyocd", runner)["adva"], board["adva"])
        board["adva"] = "19:42:de:52:4a:dd"
        with self.assertRaisesRegex(MODULE.RunError, "FICR AdvA mismatch"):
            MODULE.run_ficr_check(board, "pyocd", runner)

    def test_capture_process_failure_is_not_silenced(self) -> None:
        class FailedProcess:
            def wait(self, timeout: int | None = None) -> int:
                del timeout
                return 1

            def poll(self) -> int:
                return 1

            def terminate(self) -> None:
                pass

        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp)
            inventory, plan_path, _ = self.make_fixture(folder)
            checked = MODULE.preflight(MODULE.load_plan(plan_path), MODULE.parse_inventory(inventory))

            def runner(*_args: object, **_kwargs: object) -> subprocess.CompletedProcess[str]:
                return subprocess.CompletedProcess([], 0, "", "")

            def popen(*_args: object, **_kwargs: object) -> FailedProcess:
                return FailedProcess()

            with self.assertRaisesRegex(MODULE.RunError, "capture_A failed exit=1"):
                MODULE.execute_capture(MODULE.load_plan(plan_path), checked, "pyocd", "grabserial",
                                       folder / "run.json", run=runner, popen=popen)


if __name__ == "__main__":
    unittest.main()
