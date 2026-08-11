#!/usr/bin/env python3
"""Focused lifecycle tests for the continuous six-board runner."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import pathlib
import signal
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("runner", ROOT / "scripts" / "run_tavrn_benchmark.py")
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def adva(number: int) -> str:
    return f"{number:02x}:42:de:52:4a:dd"


class FakeProcess:
    _next_pid = 100_000
    _by_pid: dict[int, "FakeProcess"] = {}

    def __init__(self, exit_code: int | None = None, ignore_sigterm: bool = False) -> None:
        self.exit_code = exit_code
        self.terminated = False
        self.ignore_sigterm = ignore_sigterm
        self.signals: list[int] = []
        self.pid = FakeProcess._next_pid
        FakeProcess._next_pid += 1
        FakeProcess._by_pid[self.pid] = self

    def poll(self) -> int | None:
        return self.exit_code

    def wait(self, timeout: int | None = None) -> int:
        if self.exit_code is None:
            raise subprocess.TimeoutExpired("grabserial", timeout)
        return self.exit_code

    @classmethod
    def killpg(cls, pgid: int, signum: int) -> None:
        process = cls._by_pid[pgid]
        process.signals.append(signum)
        if signum == signal.SIGTERM:
            process.terminated = True
            if not process.ignore_sigterm:
                process.exit_code = 0
        elif signum == signal.SIGKILL:
            process.exit_code = -9


class RunnerTests(unittest.TestCase):
    def fixture(self, folder: pathlib.Path, profile: str = "FULL_TAVRN") -> tuple[pathlib.Path, pathlib.Path, dict[str, object]]:
        if profile not in MODULE.BENCH_PROFILES:
            raise ValueError("unsupported fixture profile")
        inventory = folder / "inventory.tsv"
        inventory.write_text("# probe_uid\tcanonical_adva\n" + "\n".join(
            f"{number:048x}\t{adva(number)}" for number in range(1, 7)) + "\n", encoding="utf-8")
        inventory_hash = digest(inventory)
        full = profile == "FULL_TAVRN"
        identity_width = "SID8" if full else "SID16"
        behavior = ("TAVRN_ROUTED_FULL_TAVRN_ESC_K1_MENTORSHIP_ADAPTIVE_HELLO_LOCAL_EXPIRY_TARGETED_FRESHNESS_RREQ_VERIFICATION_TC_METADATA_LOCAL_REPAIR"
                    if full else "TAVRN_ROUTED_AODV_ONLY")
        repair = "ON" if full else "OFF"
        sid8_status = "yes" if full else "no"
        boards: list[dict[str, object]] = []
        for number, role in enumerate(MODULE.ROLES, 1):
            uid = f"{number:048x}"; elf = folder / f"{role}.elf"; hex_file = folder / f"{role}.hex"
            elf.write_bytes(f"elf-{role}".encode()); hex_file.write_bytes(f"hex-{role}".encode())
            manifest = folder / f"{role}.manifest"
            rx_block = adva(3) if role == "A" else adva(1) if role == "C" else "NOT_CONFIGURED"
            peer_adva = adva(1) if role == "C" else adva(3)
            manifest.write_text("\n".join(("build.target=tavrn_routed_node", f"build.role={role.lower()}",
                f"bench.role_number={number}", f"identity.target_probe_uid={uid}", f"identity.adva={adva(number)}",
                f"identity.width={identity_width}", f"feature.level.effective={profile}", "build.timer_profile=BALANCED",
                f"build.behavior={behavior}", f"feature.repair.requested={repair}", f"feature.repair.effective={repair}",
                "source.commit=" + "a" * 40, "source.tree=" + "b" * 40, "source.dirty=no", "source.submodule_dirty=no",
                "candidate.configured=ON", "candidate.scope=BENCH_HOOKED_RESTRICTED", "candidate.clean_source=yes",
                "candidate.hardware_purpose=yes", "candidate.hook_bench_eligible=yes", "candidate.unhooked_acceptance=no",
                "bench.mode=ON", "bench.identify_display=OFF", "bench.control_observability=COMPILE_TIME_OPTIONAL",
                "bench.heartbeat_interval_ms=1000", "bench.burst_interval_ms=100", "bench.burst_start_ms=60000",
                "bench.burst_duration_ms=60000", "bench.burst_period_ms=450000", "bench.burst_slots=600",
                "bench.origin_role=1", "bench.destination_role=3", "hook.enabled=ON", "hook.expiry_full_table=OFF",
                "hook.hack_drop_peer_adva=NOT_CONFIGURED", "hook.hack_drop_count=0", "hook.busy_admission_count=0",
                "hook.collision_peer_adva=NOT_CONFIGURED", "hook.legacy_alias_input=NOT_APPLICABLE",
                "hook.legacy_rx_block_peer_id=NOT_APPLICABLE", f"hook.rx_block_adva={rx_block}",
                f"link_test.peer_adva={peer_adva}", "network.id=1",
                "identity.inventory.record_count=6", "identity.inventory.full_adva.unique=yes",
                "identity.inventory.sid16.unique=yes", "identity.inventory.sid16.nonreserved=yes",
                f"identity.inventory.sid8.unique={sid8_status}", f"identity.inventory.sid8.nonreserved={sid8_status}",
                f"identity.inventory.selected_width={identity_width}", f"identity.inventory.sha256={inventory_hash}",
                f"artifact.elf.sha256={digest(elf)}", f"artifact.hex.sha256={digest(hex_file)}", "obs.mode=ON")) + "\n", encoding="utf-8")
            boards.append({"role": role, "uid": uid, "adva": adva(number), "serial_device": f"/dev/serial/by-id/{uid}",
                           "artifact": {"manifest": str(manifest), "manifest_sha256": digest(manifest), "elf": str(elf),
                                        "elf_sha256": digest(elf), "hex": str(hex_file), "hex_sha256": digest(hex_file),
                                        "flash_image": {"format": "hex", "path": str(hex_file), "sha256": digest(hex_file)}},
                            "manifest_hooks": {"hook.rx_block_adva": rx_block}, "manifest_observation": {"obs.mode": "ON"}})
        plan = {"schema": MODULE.PLAN_SCHEMA, "source": {"commit": "a" * 40, "tree": "b" * 40},
                "profile": profile, "timer_profile": "BALANCED", "network": {"id": 1},
                "candidate_scope": "BENCH_HOOKED_RESTRICTED",
                "observation": {"source_role": "A", "destination_role": "C", "heartbeat_hz": 1,
                                "throughput_hz": 10, "throughput_start_seconds": 60, "throughput_burst_seconds": 60, "throughput_repeat_seconds": 450,
                                "window_seconds": 10}, "boards": boards}
        plan_path = folder / "plan.json"; plan_path.write_text(json.dumps(plan), encoding="utf-8")
        return inventory, plan_path, plan

    @staticmethod
    def replace_manifest_value(plan: dict[str, object], role: str, old: str, new: str) -> None:
        boards = plan["boards"]
        assert isinstance(boards, list)
        board = next(item for item in boards if isinstance(item, dict) and item["role"] == role)
        artifact = board["artifact"]
        assert isinstance(artifact, dict)
        manifest = pathlib.Path(str(artifact["manifest"]))
        contents = manifest.read_text(encoding="utf-8")
        if old not in contents:
            raise AssertionError(f"fixture manifest lacks {old}")
        manifest.write_text(contents.replace(old, new), encoding="utf-8")
        artifact["manifest_sha256"] = digest(manifest)

    def test_preflight_and_noninteractive_dry_run_construct_unbounded_commands(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); inventory, plan_path, _ = self.fixture(folder)
            checked = MODULE.preflight(MODULE.load_plan(plan_path), MODULE.parse_inventory(inventory), digest(inventory))
            self.assertEqual(len(checked), 6)
            run_dir = folder / "run"
            result = MODULE.main(["--inventory", str(inventory), "--run-plan", str(plan_path), "--run-dir", str(run_dir),
                                  "--pyocd", "/missing/pyocd", "--grabserial", "/missing/grabserial",
                                  "--uv", "/missing/uv", "--dry-run"])
            data = json.loads((run_dir / "run.json").read_text(encoding="utf-8"))
            inventory_hash, plan_hash = digest(inventory), digest(plan_path)
        self.assertEqual(result, 0); self.assertEqual(data["status"], "dry_run")
        self.assertIn("started_utc", data); self.assertIn("ended_utc", data)
        self.assertEqual(data["pinned"]["inventory"]["sha256"], inventory_hash)
        self.assertEqual(data["pinned"]["plan"]["sha256"], plan_hash)
        self.assertEqual(data["pinned"]["tools"]["analyzer"]["sha256"], MODULE._ANALYZER_SHA256)
        self.assertEqual(MODULE._ANALYZER_SHA256, hashlib.sha256(MODULE._ANALYZER_BYTES).hexdigest())
        commands = [event["command"] for event in data["events"]]
        capture = next(command for command in commands if command[0] == "/missing/grabserial")
        self.assertIn("-T", capture); self.assertIn("%Y-%m-%dT%H:%M:%S.%fZ", capture); self.assertNotIn("-e", capture)

    def test_no_overwrite_and_explicit_execute_gate(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); inventory, plan, _ = self.fixture(folder)
            occupied = folder / "occupied"; occupied.mkdir()
            self.assertEqual(MODULE.main(["--inventory", str(inventory), "--run-plan", str(plan), "--run-dir", str(occupied),
                                           "--pyocd", "/x", "--grabserial", "/y", "--uv", "/z", "--dry-run"]), 1)
            self.assertEqual(MODULE.main(["--inventory", str(inventory), "--run-plan", str(plan), "--run-dir", str(folder / "new"),
                                           "--pyocd", "/x", "--grabserial", "/y", "--uv", "/z"]), 1)

    def test_ficr_derivation_and_mismatch(self) -> None:
        self.assertEqual(MODULE.derive_canonical_adva(0x52DE4218, 0x1D4A), "18:42:de:52:4a:dd")
        self.assertEqual(MODULE.parse_ficr_words("0x100000a4: 52de4218\n0x100000a8: 00001d4a\n"), (0x52DE4218, 0x1D4A))
        events: list[dict[str, object]] = []
        board = {"role": "A", "uid": "1" * 48, "adva": "18:42:de:52:4a:dd"}
        def ok(*_args: object, **_kwargs: object) -> subprocess.CompletedProcess[str]:
            return subprocess.CompletedProcess([], 0, "0x100000a4: 52de4218 00001d4a", "")
        MODULE.run_ficr_check(board, "/tool/pyocd", events, ok)
        board["adva"] = "19:42:de:52:4a:dd"
        with self.assertRaisesRegex(MODULE.RunError, "FICR"):
            MODULE.run_ficr_check(board, "/tool/pyocd", events, ok)

    def test_plan_requires_distinct_identities_and_resolved_serial_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); _, plan_path, plan = self.fixture(folder)
            boards = plan["boards"]
            assert isinstance(boards, list)
            first, second = boards[0], boards[1]
            assert isinstance(first, dict) and isinstance(second, dict)
            second["uid"] = first["uid"]; second["serial_device"] = first["serial_device"]
            plan_path.write_text(json.dumps(plan), encoding="utf-8")
            with self.assertRaisesRegex(MODULE.RunError, "UID"):
                MODULE.load_plan(plan_path)

            _, plan_path, plan = self.fixture(folder)
            boards = plan["boards"]
            first, second = boards[0], boards[1]
            assert isinstance(first, dict) and isinstance(second, dict)
            first["serial_device"] = f"/dev/serial/by-id/{first['uid']}/../shared"
            second["serial_device"] = f"/dev/serial/by-id/{second['uid']}/../shared"
            plan_path.write_text(json.dumps(plan), encoding="utf-8")
            with self.assertRaisesRegex(MODULE.RunError, "duplicate serial"):
                MODULE.load_plan(plan_path)

    def test_fixed_manifest_contract_cannot_be_replaced_by_plan_mapping(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); inventory, plan_path, plan = self.fixture(folder)
            boards = plan["boards"]
            assert isinstance(boards, list) and isinstance(boards[0], dict)
            board = boards[0]
            artifact = board["artifact"]
            assert isinstance(artifact, dict)
            manifest = pathlib.Path(str(artifact["manifest"]))
            manifest.write_text(manifest.read_text(encoding="utf-8").replace(
                "candidate.clean_source=yes", "candidate.clean_source=no"), encoding="utf-8")
            artifact["manifest_sha256"] = digest(manifest)
            hooks = board["manifest_hooks"]
            assert isinstance(hooks, dict)
            hooks["candidate.clean_source"] = "no"
            plan_path.write_text(json.dumps(plan), encoding="utf-8")
            with self.assertRaisesRegex(MODULE.RunError, "candidate.clean_source"):
                MODULE.preflight(MODULE.load_plan(plan_path), MODULE.parse_inventory(inventory), digest(inventory))

    def test_fixed_manifest_profile_network_peer_and_sid8_contract(self) -> None:
        full_behavior = "TAVRN_ROUTED_FULL_TAVRN_ESC_K1_MENTORSHIP_ADAPTIVE_HELLO_LOCAL_EXPIRY_TARGETED_FRESHNESS_RREQ_VERIFICATION_TC_METADATA_LOCAL_REPAIR"
        cases = (
            ("A", "link_test.peer_adva=" + adva(3), "link_test.peer_adva=" + adva(1), "link_test.peer_adva"),
            ("C", "hook.rx_block_adva=" + adva(1), "hook.rx_block_adva=NOT_CONFIGURED", "hook.rx_block_adva"),
            ("A", "network.id=1", "network.id=2", "network.id"),
            ("A", "build.behavior=" + full_behavior, "build.behavior=TAVRN_ROUTED_AODV_ONLY", "build.behavior"),
            ("A", "feature.repair.requested=ON", "feature.repair.requested=OFF", "feature.repair.requested"),
            ("A", "feature.repair.effective=ON", "feature.repair.effective=OFF", "feature.repair.effective"),
            ("A", "identity.inventory.sid8.unique=yes", "identity.inventory.sid8.unique=no",
             "identity.inventory.sid8.unique"),
        )
        for role, old, new, expected in cases:
            with self.subTest(requirement=expected), tempfile.TemporaryDirectory() as temp:
                folder = pathlib.Path(temp); inventory, plan_path, plan = self.fixture(folder)
                self.replace_manifest_value(plan, role, old, new)
                plan_path.write_text(json.dumps(plan), encoding="utf-8")
                with self.assertRaisesRegex(MODULE.RunError, expected):
                    MODULE.preflight(MODULE.load_plan(plan_path), MODULE.parse_inventory(inventory), digest(inventory))

        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); _, plan_path, plan = self.fixture(folder)
            plan["timer_profile"] = "FAST_TEST"
            plan_path.write_text(json.dumps(plan), encoding="utf-8")
            with self.assertRaisesRegex(MODULE.RunError, "timer_profile"):
                MODULE.load_plan(plan_path)
            plan["timer_profile"] = "BALANCED"; plan["network"] = {"id": 2}
            plan_path.write_text(json.dumps(plan), encoding="utf-8")
            with self.assertRaisesRegex(MODULE.RunError, "network.id"):
                MODULE.load_plan(plan_path)

    def test_aodv_manifest_requires_aodv_behavior_without_sid8_gate(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); inventory, plan_path, plan = self.fixture(folder, "AODV_ONLY")
            boards = plan["boards"]
            assert isinstance(boards, list)
            for board in boards:
                assert isinstance(board, dict)
                observation = board["manifest_observation"]
                assert isinstance(observation, dict)
                observation.update({"identity.inventory.sid8.unique": "yes",
                                    "identity.inventory.sid8.nonreserved": "yes"})
            plan_path.write_text(json.dumps(plan), encoding="utf-8")
            checked = MODULE.preflight(MODULE.load_plan(plan_path), MODULE.parse_inventory(inventory), digest(inventory))
            self.assertEqual(len(checked), 6)

            self.replace_manifest_value(plan, "A", "build.behavior=TAVRN_ROUTED_AODV_ONLY",
                                        "build.behavior=TAVRN_ROUTED_FULL_TAVRN_ESC_K1_MENTORSHIP_ADAPTIVE_HELLO_LOCAL_EXPIRY_TARGETED_FRESHNESS_RREQ_VERIFICATION_TC_METADATA_LOCAL_REPAIR")
            plan_path.write_text(json.dumps(plan), encoding="utf-8")
            with self.assertRaisesRegex(MODULE.RunError, "build.behavior"):
                MODULE.preflight(MODULE.load_plan(plan_path), MODULE.parse_inventory(inventory), digest(inventory))

    def test_serial_and_tool_preflight_are_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); regular = folder / "serial"; regular.write_text("not a tty", encoding="utf-8")
            checked = [{"role": role, "serial_device": str(regular)} for role in MODULE.ROLES]
            with self.assertRaisesRegex(MODULE.RunError, "not a character device"):
                MODULE.verify_serial_devices(checked)
            tool = folder / "tool"; tool.write_text("#!/bin/sh\n", encoding="utf-8")
            with self.assertRaisesRegex(MODULE.RunError, "executable"):
                MODULE._tool_path(str(tool), "--uv", True)

    def test_pinning_preserves_mode_and_rejects_changed_preparse_plan(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp)
            source = folder / "tool"; source.write_text("#!/bin/sh\n", encoding="utf-8"); source.chmod(0o751)
            target = folder / "pinned" / "tool"
            MODULE._pin(source, target)
            self.assertEqual(stat.S_IMODE(target.stat().st_mode), stat.S_IMODE(source.stat().st_mode))

            inventory, plan_path, _ = self.fixture(folder)
            original_load_plan = MODULE.load_plan

            def changed_after_hash(path: pathlib.Path) -> dict[str, object]:
                loaded = original_load_plan(path)
                path.write_text(path.read_text(encoding="utf-8") + "\n", encoding="utf-8")
                return loaded

            with mock.patch.object(MODULE, "load_plan", side_effect=changed_after_hash):
                result = MODULE.main(["--inventory", str(inventory), "--run-plan", str(plan_path),
                                      "--run-dir", str(folder / "changed"), "--pyocd", "/missing/pyocd",
                                      "--grabserial", "/missing/grabserial", "--uv", "/missing/uv", "--dry-run"])
            self.assertEqual(result, 1)

            checked = MODULE.preflight(MODULE.load_plan(plan_path), MODULE.parse_inventory(inventory), digest(inventory))
            analyzer = folder / "analyzer.py"; analyzer.write_text("version = 1\n", encoding="utf-8")
            expected_analyzer_hash = digest(analyzer)
            analyzer.write_text("version = 2\n", encoding="utf-8")
            with mock.patch.object(MODULE, "_ANALYZER_PATH", analyzer), \
                    mock.patch.object(MODULE, "_ANALYZER_SHA256", expected_analyzer_hash):
                with self.assertRaisesRegex(MODULE.RunError, "analyzer.py hash changed"):
                    MODULE.pin_inputs(folder / "changed-analyzer", inventory, plan_path,
                                      {"inventory": digest(inventory), "plan": digest(plan_path)}, checked,
                                      None, None, None)

    def test_pinned_chart_command_and_finalized_terminal_status(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); bundle = folder / "final"; bundle.mkdir()
            (bundle / "final.json").write_text("{}", encoding="utf-8")
            commands: list[list[str]] = []

            def ok(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                commands.append(command)
                return subprocess.CompletedProcess(command, 0, "", "")

            MODULE.render_pinned_charts("/pinned/uv", "/pinned/analyzer.py", bundle, "final", [], ok)
            self.assertEqual(commands, [["/pinned/uv", "run", "--with", "matplotlib==3.11.1", "--no-project",
                                         "python", "/pinned/analyzer.py", "--render-json", str(bundle / "final.json"),
                                         "--output-dir", str(bundle)]])

            inventory, plan_path, _ = self.fixture(folder)
            tools = []
            for name in ("pyocd", "grabserial", "uv"):
                tool = folder / name; tool.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8"); tool.chmod(0o755)
                tools.append(tool)
            run_dir = folder / "run"
            state = MODULE.analysis.ObservationState({"boards": [], "profile": "FULL_TAVRN", "observation": {}})
            end = MODULE.CaptureEnd(1_786_003_200_123.0, "2026-08-12T00:00:00.123000Z")
            state.finalize(end.host_ms)

            def final_before_metadata(*_args: object, **_kwargs: object) -> dict[str, str]:
                self.assertFalse((run_dir / "run.json").exists())
                return {"proving_status": "INCOMPLETE"}

            with mock.patch.object(MODULE, "verify_serial_devices"), \
                    mock.patch.object(MODULE, "run_ficr_check"), \
                    mock.patch.object(MODULE, "execute_capture", return_value=("completed_by_user", state, end)), \
                    mock.patch.object(MODULE, "_write_snapshot", side_effect=final_before_metadata):
                result = MODULE.main(["--inventory", str(inventory), "--run-plan", str(plan_path), "--run-dir", str(run_dir),
                                      "--pyocd", str(tools[0]), "--grabserial", str(tools[1]), "--uv", str(tools[2]), "--execute"])
            self.assertEqual(result, 1)
            self.assertIsNotNone(state.finalized_host_ms)
            self.assertEqual(json.loads((run_dir / "run.json").read_text(encoding="utf-8"))["status"],
                             "failed_incomplete_evidence")

            valid_run_dir = folder / "valid-run"
            valid_state = MODULE.analysis.ObservationState({"boards": [], "profile": "FULL_TAVRN", "observation": {}})
            valid_end = MODULE.CaptureEnd(1_786_003_201_123.0, "2026-08-12T00:00:01.123000Z")
            valid_state.finalize(valid_end.host_ms)
            with mock.patch.object(MODULE, "verify_serial_devices"), \
                    mock.patch.object(MODULE, "run_ficr_check"), \
                    mock.patch.object(MODULE, "execute_capture", return_value=("completed_by_user", valid_state, valid_end)), \
                    mock.patch.object(MODULE, "_write_snapshot", return_value={"proving_status": "VALID"}):
                result = MODULE.main(["--inventory", str(inventory), "--run-plan", str(plan_path), "--run-dir", str(valid_run_dir),
                                      "--pyocd", str(tools[0]), "--grabserial", str(tools[1]), "--uv", str(tools[2]), "--execute"])
            self.assertEqual(result, 0)
            self.assertIsNotNone(valid_state.finalized_host_ms)
            self.assertEqual(json.loads((valid_run_dir / "run.json").read_text(encoding="utf-8"))["status"],
                             "completed_by_user")

            invalid_run_dir = folder / "invalid-run"
            invalid_state = MODULE.analysis.ObservationState({"boards": [], "profile": "FULL_TAVRN", "observation": {}})
            invalid_end = MODULE.CaptureEnd(1_786_003_202_123.0, "2026-08-12T00:00:02.123000Z")
            invalid_state.finalize(invalid_end.host_ms)
            with mock.patch.object(MODULE, "verify_serial_devices"), \
                    mock.patch.object(MODULE, "run_ficr_check"), \
                    mock.patch.object(MODULE, "execute_capture", return_value=("completed_by_user", invalid_state, invalid_end)), \
                    mock.patch.object(MODULE, "_write_snapshot", return_value={"proving_status": "INVALID"}):
                result = MODULE.main(["--inventory", str(inventory), "--run-plan", str(plan_path), "--run-dir", str(invalid_run_dir),
                                      "--pyocd", str(tools[0]), "--grabserial", str(tools[1]), "--uv", str(tools[2]), "--execute"])
            self.assertEqual(result, 1)
            self.assertIsNotNone(invalid_state.finalized_host_ms)
            self.assertEqual(json.loads((invalid_run_dir / "run.json").read_text(encoding="utf-8"))["status"],
                             "failed_invalid_evidence")

    def test_snapshot_chart_failure_leaves_no_named_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); output_dir = folder / "outputs"; inventory, plan_path, _ = self.fixture(folder)
            plan = MODULE.load_plan(plan_path)
            checked = MODULE.preflight(plan, MODULE.parse_inventory(inventory), digest(inventory))
            state = MODULE.analysis.ObservationState({"schema": MODULE.RUN_SCHEMA, "boards": checked,
                                                      "profile": plan["profile"], "observation": plan["observation"]})

            def failed_renderer(_bundle: pathlib.Path, _name: str) -> None:
                raise MODULE.RunError("chart renderer failed")

            with self.assertRaisesRegex(MODULE.RunError, "chart renderer failed"):
                MODULE._write_snapshot(state, output_dir, "final", True, failed_renderer)
            self.assertFalse((output_dir / "final").exists())
            self.assertEqual(list(output_dir.iterdir()), [])

    def test_capture_children_start_new_sessions(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); inventory, plan_path, _ = self.fixture(folder)
            plan = MODULE.load_plan(plan_path)
            checked = MODULE.preflight(plan, MODULE.parse_inventory(inventory), digest(inventory))
            run_dir = folder / "run"; run_dir.mkdir()
            events: list[dict[str, object]] = []; launches: list[dict[str, object]] = []

            def ok(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                return subprocess.CompletedProcess(command, 0, "", "")

            def launch(_command: list[str], **kwargs: object) -> FakeProcess:
                launches.append(kwargs)
                return FakeProcess()

            with mock.patch.object(MODULE, "monitor_captures", return_value="test_stopped"):
                status, state, _end = MODULE.execute_capture(plan, checked, "/pinned/pyocd", "/pinned/grabserial",
                                                              "/pinned/uv", "/pinned/analyzer.py", run_dir, events,
                                                              False, 0.0, run=ok, popen=launch,
                                                              killpg=FakeProcess.killpg)
            self.assertEqual(status, "test_stopped")
            self.assertEqual(len(launches), len(MODULE.ROLES))
            self.assertTrue(all(call.get("start_new_session") is True for call in launches))
            state.close()

    def test_live_and_offline_share_the_exact_capture_end_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); inventory, plan_path, _ = self.fixture(folder)
            plan = MODULE.load_plan(plan_path)
            checked = MODULE.preflight(plan, MODULE.parse_inventory(inventory), digest(inventory))
            run_dir = folder / "run"; run_dir.mkdir()
            events: list[dict[str, object]] = []; processes: list[FakeProcess] = []
            instant = MODULE.dt.datetime(2026, 8, 12, 0, 0, 0, 123000, tzinfo=MODULE.dt.timezone.utc)
            end = MODULE.CaptureEnd(instant.timestamp() * 1000.0, MODULE._format_utc(instant))

            def ok(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                return subprocess.CompletedProcess(command, 0, "", "")

            def launch(_command: list[str], **_kwargs: object) -> FakeProcess:
                process = FakeProcess(); processes.append(process)
                return process

            with mock.patch.object(MODULE, "monitor_captures", return_value="test_stopped"), \
                    mock.patch.object(MODULE, "capture_end_instant", return_value=end):
                status, live, observed_end = MODULE.execute_capture(
                    plan, checked, "/pinned/pyocd", "/pinned/grabserial", "/pinned/uv", "/pinned/analyzer.py",
                    run_dir, events, False, 0.0, run=ok, popen=launch, killpg=FakeProcess.killpg)

            metadata = MODULE._metadata(plan, checked, {}, events, status,
                                        "2026-08-12T00:00:00.000000Z", observed_end.utc)
            paths = {board["role"]: pathlib.Path(board["log"]["path"]) for board in checked}
            offline = MODULE.analysis.replay(metadata, paths)
            self.assertEqual(status, "test_stopped")
            self.assertEqual(observed_end, end)
            self.assertEqual(live.finalized_host_ms, end.host_ms)
            self.assertEqual(offline.finalized_host_ms, live.finalized_host_ms)
            self.assertEqual(MODULE.analysis.snapshot(offline)["evidence_complete"],
                             MODULE.analysis.snapshot(live)["evidence_complete"])
            self.assertTrue(all(process.signals == [signal.SIGTERM] for process in processes))
            live.close()

    def test_chart_delay_does_not_change_capture_end_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); inventory, plan_path, _ = self.fixture(folder)
            tools = []
            for name in ("pyocd", "grabserial", "uv"):
                tool = folder / name; tool.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8"); tool.chmod(0o755)
                tools.append(tool)
            run_dir = folder / "run"
            instant = MODULE.dt.datetime(2026, 8, 12, 0, 0, 1, 123000, tzinfo=MODULE.dt.timezone.utc)
            end = MODULE.CaptureEnd(instant.timestamp() * 1000.0, MODULE._format_utc(instant))
            state = MODULE.analysis.ObservationState({"boards": [], "profile": "FULL_TAVRN", "observation": {}})
            state.finalize(end.host_ms)
            delayed = []

            def render_after_delay(*_args: object, **_kwargs: object) -> dict[str, str]:
                delayed.append(True)
                return {"proving_status": "VALID"}

            with mock.patch.object(MODULE, "verify_serial_devices"), \
                    mock.patch.object(MODULE, "run_ficr_check"), \
                    mock.patch.object(MODULE, "execute_capture", return_value=("completed_by_user", state, end)), \
                    mock.patch.object(MODULE, "_write_snapshot", side_effect=render_after_delay):
                result = MODULE.main(["--inventory", str(inventory), "--run-plan", str(plan_path), "--run-dir", str(run_dir),
                                      "--pyocd", str(tools[0]), "--grabserial", str(tools[1]), "--uv", str(tools[2]), "--execute"])
            self.assertEqual(result, 0)
            self.assertEqual(delayed, [True])
            self.assertEqual(json.loads((run_dir / "run.json").read_text(encoding="utf-8"))["ended_utc"], end.utc)

    def test_sigterm_and_sighup_unwind_capture_and_close_logs(self) -> None:
        for signum in (signal.SIGTERM, signal.SIGHUP):
            with self.subTest(signal=signal.Signals(signum).name), tempfile.TemporaryDirectory() as temp:
                folder = pathlib.Path(temp); inventory, plan_path, _ = self.fixture(folder)
                plan = MODULE.load_plan(plan_path)
                checked = MODULE.preflight(plan, MODULE.parse_inventory(inventory), digest(inventory))
                run_dir = folder / "run"; run_dir.mkdir()
                events: list[dict[str, object]] = []; processes: list[FakeProcess] = []; handles = []

                def ok(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                    return subprocess.CompletedProcess(command, 0, "", "")

                def launch(_command: list[str], **kwargs: object) -> FakeProcess:
                    handles.append(kwargs["stdout"])
                    process = FakeProcess(); processes.append(process)
                    return process

                with mock.patch.object(MODULE, "monitor_captures", side_effect=MODULE.TerminationRequest(signum)):
                    status, state, _end = MODULE.execute_capture(
                        plan, checked, "/pinned/pyocd", "/pinned/grabserial", "/pinned/uv", "/pinned/analyzer.py",
                        run_dir, events, False, 0.0, run=ok, popen=launch, killpg=FakeProcess.killpg)
                self.assertEqual(status, f"terminated_{signal.Signals(signum).name.lower()}")
                self.assertIsNotNone(state.finalized_host_ms)
                self.assertTrue(all(process.signals == [signal.SIGTERM] for process in processes))
                self.assertTrue(all(handle.closed for handle in handles))
                self.assertIn({"event": "termination_request", "status": "received",
                                "signal": signal.Signals(signum).name},
                              [{key: value for key, value in event.items() if key != "at"} for event in events])
                state.close()

    def test_termination_uses_process_groups_and_escalates_only_active_children(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); events: list[dict[str, object]] = []
            active_log = folder / "active.log"; active_log.write_text("", encoding="utf-8")
            exited_log = folder / "exited.log"; exited_log.write_text("", encoding="utf-8")
            active = FakeProcess(ignore_sigterm=True); exited = FakeProcess(exit_code=0)
            active_handle = active_log.open("a", encoding="utf-8")
            exited_handle = exited_log.open("a", encoding="utf-8")
            children = [
                MODULE.CaptureChild({"role": "A"}, active, active_handle, ["grab"], active_log),
                MODULE.CaptureChild({"role": "B"}, exited, exited_handle, ["grab"], exited_log),
            ]
            MODULE._terminate(children, events, killpg=FakeProcess.killpg, timeout=0.0)
            self.assertEqual(active.signals, [signal.SIGTERM, signal.SIGKILL])
            self.assertEqual(exited.signals, [])
            self.assertEqual([event["status"] for event in events],
                             ["terminate_requested", "kill_requested", "terminated"])
            self.assertTrue(active_handle.closed); self.assertTrue(exited_handle.closed)

    def test_main_restores_signal_handlers_and_writes_atomic_terminal_metadata(self) -> None:
        for signum in (signal.SIGTERM, signal.SIGHUP):
            with self.subTest(signal=signal.Signals(signum).name), tempfile.TemporaryDirectory() as temp:
                folder = pathlib.Path(temp); inventory, plan_path, _ = self.fixture(folder)
                tools = []
                for name in ("pyocd", "grabserial", "uv"):
                    tool = folder / name; tool.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8"); tool.chmod(0o755)
                    tools.append(tool)
                run_dir = folder / "run"; previous = {signal.SIGTERM: object(), signal.SIGHUP: object()}
                installed: dict[int, object] = {}; calls: list[tuple[int, object]] = []

                def set_handler(number: int, handler: object) -> object:
                    calls.append((number, handler))
                    if handler is MODULE._raise_termination_request:
                        installed[number] = handler
                        return previous[number]
                    self.assertIs(handler, previous[number])
                    return handler

                def request_during_capture(*_args: object, **_kwargs: object) -> None:
                    handler = installed[signum]
                    assert callable(handler)
                    handler(signum, None)

                with mock.patch.object(MODULE, "verify_serial_devices"), \
                        mock.patch.object(MODULE, "run_ficr_check"), \
                        mock.patch.object(MODULE, "execute_capture", side_effect=request_during_capture), \
                        mock.patch.object(MODULE.signal, "signal", side_effect=set_handler), \
                        mock.patch.object(MODULE.os, "replace", wraps=MODULE.os.replace) as replace:
                    result = MODULE.main(["--inventory", str(inventory), "--run-plan", str(plan_path), "--run-dir", str(run_dir),
                                          "--pyocd", str(tools[0]), "--grabserial", str(tools[1]), "--uv", str(tools[2]), "--execute"])
                self.assertEqual(result, 1)
                self.assertEqual([number for number, _handler in calls],
                                 [signal.SIGTERM, signal.SIGHUP, signal.SIGHUP, signal.SIGTERM])
                self.assertEqual(replace.call_count, 1)
                staged, destination = replace.call_args.args
                self.assertEqual(destination, run_dir / "run.json")
                self.assertEqual(staged.parent, run_dir)
                self.assertEqual(json.loads((run_dir / "run.json").read_text(encoding="utf-8"))["status"],
                                 f"terminated_{signal.Signals(signum).name.lower()}")

    def test_run_metadata_uses_sibling_atomic_replace(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); inventory, plan_path, _ = self.fixture(folder)
            plan = MODULE.load_plan(plan_path)
            checked = MODULE.preflight(plan, MODULE.parse_inventory(inventory), digest(inventory))
            run_dir = folder / "run"; run_dir.mkdir()
            with mock.patch.object(MODULE.os, "replace", wraps=MODULE.os.replace) as replace:
                MODULE._write_run_metadata(run_dir, plan, checked, {}, [], "dry_run", "2026-08-12T00:00:00.000000Z")
            self.assertEqual(replace.call_count, 1)
            staged, destination = replace.call_args.args
            self.assertEqual(destination, run_dir / "run.json")
            self.assertEqual(staged.parent, run_dir)
            self.assertTrue(staged.name.startswith(".run.json."))
            self.assertEqual(json.loads((run_dir / "run.json").read_text(encoding="utf-8"))["status"], "dry_run")
            self.assertEqual(sorted(path.name for path in run_dir.iterdir()), ["run.json"])

    def test_live_snapshot_keeps_children_active_and_ctrl_c_finalizes(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); inventory, plan_path, _ = self.fixture(folder)
            plan = MODULE.load_plan(plan_path)
            checked = MODULE.preflight(plan, MODULE.parse_inventory(inventory), digest(inventory))
            state = MODULE.analysis.ObservationState({"schema": MODULE.RUN_SCHEMA, "boards": checked,
                                                      "profile": plan["profile"], "observation": plan["observation"]})
            children = []
            for role in MODULE.ROLES:
                log = folder / f"{role}.log"; log.write_text("", encoding="utf-8")
                children.append(MODULE.CaptureChild({"role": role}, FakeProcess(), log.open("a", encoding="utf-8"), ["grab"], log))
            events: list[dict[str, object]] = []
            rendered: list[tuple[pathlib.Path, str]] = []

            def render(bundle: pathlib.Path, name: str) -> None:
                self.assertTrue((bundle / f"{name}.json").is_file())
                self.assertEqual(bundle.parent.parent, folder / "outputs")
                rendered.append((bundle, name))

            status = MODULE.monitor_captures(children, state, folder / "outputs", events, 0, False,
                                             sleep=lambda _value: None, input_ready=lambda: True, max_cycles=1,
                                             chart_renderer=render)
            self.assertEqual(status, "test_stopped")
            self.assertTrue(all(child.process.poll() is None for child in children))
            self.assertEqual(len(rendered), 1); self.assertEqual(rendered[0][1], "provisional-0001")
            self.assertTrue((folder / "outputs" / "provisional-0001" / "provisional-0001.json").is_file())
            def interrupt(_value: float) -> None:
                raise KeyboardInterrupt
            status = MODULE.monitor_captures(children, state, folder / "outputs", events, 0, False,
                                             sleep=interrupt, max_cycles=2, killpg=FakeProcess.killpg)
            self.assertEqual(status, "completed_by_user"); self.assertTrue(all(child.process.terminated for child in children))

    def test_watch_line_uses_live_status_without_full_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp)
            state = MODULE.analysis.ObservationState(
                {"boards": [], "profile": "FULL_TAVRN", "observation": {}})
            children = []
            for role in MODULE.ROLES:
                log = folder / f"{role}.log"
                log.write_text("", encoding="utf-8")
                children.append(MODULE.CaptureChild(
                    {"role": role}, FakeProcess(), log.open("a", encoding="utf-8"), ["grab"], log))
            with mock.patch.object(MODULE.analysis, "snapshot",
                                   side_effect=AssertionError("watch called full snapshot")):
                status = MODULE.monitor_captures(
                    children, state, folder / "outputs", [], 0, False,
                    sleep=lambda _value: None, max_cycles=10, killpg=FakeProcess.killpg)
            self.assertEqual(status, "test_stopped")
            for child in children:
                child.handle.close()

    def test_runner_retains_hashes_and_closes_sqlite_evidence_on_success_and_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp)
            inventory, plan_path, _ = self.fixture(folder)
            tools = []
            for name in ("pyocd", "grabserial", "uv"):
                tool = folder / name
                tool.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
                tool.chmod(0o755)
                tools.append(tool)

            def run_case(name: str, snapshot: object) -> tuple[int, object, pathlib.Path, dict[str, object]]:
                run_dir = folder / name
                captured: dict[str, object] = {}
                end = MODULE.CaptureEnd(1_786_003_200_123.0, "2026-08-12T00:00:00.123000Z")

                def capture(*_args: object, **_kwargs: object) -> tuple[str, object, object]:
                    state = MODULE.analysis.ObservationState(
                        {"boards": [], "profile": "FULL_TAVRN", "observation": {}},
                        database_path=run_dir / "observations.sqlite3")
                    state.finalize(end.host_ms)
                    captured["state"] = state
                    return "completed_by_user", state, end

                snapshot_patch = (mock.patch.object(MODULE, "_write_snapshot", side_effect=snapshot)
                                  if isinstance(snapshot, Exception) else
                                  mock.patch.object(MODULE, "_write_snapshot", return_value=snapshot))
                with mock.patch.object(MODULE, "verify_serial_devices"), \
                        mock.patch.object(MODULE, "run_ficr_check"), \
                        mock.patch.object(MODULE, "execute_capture", side_effect=capture), \
                        snapshot_patch:
                    result = MODULE.main([
                        "--inventory", str(inventory), "--run-plan", str(plan_path),
                        "--run-dir", str(run_dir), "--pyocd", str(tools[0]),
                        "--grabserial", str(tools[1]), "--uv", str(tools[2]), "--execute"])
                data = json.loads((run_dir / "run.json").read_text(encoding="utf-8"))
                return result, captured["state"], run_dir, data

            result, state, run_dir, data = run_case("success", {"proving_status": "VALID"})
            self.assertEqual(result, 0)
            evidence = data["pinned"]["observation_store"]
            self.assertEqual(evidence["retention"], "retained_run_evidence")
            self.assertEqual(evidence["path"], str(run_dir / "observations.sqlite3"))
            self.assertEqual(evidence["sha256"], digest(run_dir / "observations.sqlite3"))
            self.assertTrue(state.closed)

            result, state, run_dir, data = run_case("failure", MODULE.RunError("final bundle failed"))
            self.assertEqual(result, 1)
            self.assertEqual(data["status"], "failed")
            self.assertEqual(data["pinned"]["observation_store"]["sha256"],
                             digest(run_dir / "observations.sqlite3"))
            self.assertTrue(state.closed)

    def test_child_failure_terminates_peers(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); state = MODULE.analysis.ObservationState({"boards": [], "profile": "FULL_TAVRN", "observation": {}})
            children = []
            for index, role in enumerate(MODULE.ROLES):
                log = folder / f"{role}.log"; log.write_text("", encoding="utf-8")
                children.append(MODULE.CaptureChild({"role": role}, FakeProcess(1 if index == 0 else None), log.open("a", encoding="utf-8"), ["grab"], log))
            status = MODULE.monitor_captures(children, state, folder / "outputs", [], 0, False,
                                             sleep=lambda _value: None, max_cycles=1, killpg=FakeProcess.killpg)
            self.assertEqual(status, "failed"); self.assertTrue(all(child.process.poll() is not None for child in children))

    def test_ctrl_c_concurrent_with_child_exit_is_a_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pathlib.Path(temp); state = MODULE.analysis.ObservationState({"boards": [], "profile": "FULL_TAVRN", "observation": {}})
            children = []
            for role in MODULE.ROLES:
                log = folder / f"{role}.log"; log.write_text("", encoding="utf-8")
                children.append(MODULE.CaptureChild({"role": role}, FakeProcess(), log.open("a", encoding="utf-8"), ["grab"], log))

            def interrupted(_value: float) -> None:
                children[0].process.exit_code = 9
                raise KeyboardInterrupt

            events: list[dict[str, object]] = []
            status = MODULE.monitor_captures(children, state, folder / "outputs", events, 0, False,
                                             sleep=interrupted, max_cycles=2, killpg=FakeProcess.killpg)
            self.assertEqual(status, "failed")
            self.assertTrue(any(event.get("status") == "unexpected_exit" for event in events))


if __name__ == "__main__":
    unittest.main()
