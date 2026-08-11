#!/usr/bin/env python3
"""Fail-closed six-board TAVRN benchmark preflight, flash, and capture runner.

The runner accepts a strict inventory TSV and a separate JSON plan.  It never
builds and never selects a board by USB/probe position.  The plan schema is
``tron.tavrn.benchmark.plan.v1`` and has this intentionally explicit shape::

  {
    "schema": "tron.tavrn.benchmark.plan.v1",
    "source": {"commit": "<40 lowercase hex>", "tree": "<40 lowercase hex>"},
    "profile": "FULL_TAVRN", "timer_profile": "BALANCED",
    "candidate_scope": "BENCH_HOOKED_RESTRICTED",
    "benchmark": {"source_role": "A", "destination_role": "C",
      "origin": "...", "destination": "...", "width": "SID8",
      "warmup_seconds": 60, "offered_seconds": 300, "drain_seconds": 35,
      "offered_rate_hz": 4, "interval_ms": 250, "offered_slots": 1200},
    "boards": [{"role": "A", "uid": "...", "adva": "18:42:...",
      "serial_device": "/dev/serial/by-id/...", "artifact": {
        "manifest": "...", "manifest_sha256": "...", "elf": "...",
        "elf_sha256": "...", "hex": "...", "hex_sha256": "...",
        "flash_image": {"format": "hex", "path": "...", "sha256": "..."}},
      "manifest_hooks": {"hook.rx_block_adva": "..."},
      "manifest_benchmark": {"bench.offered_slots": "1200"}}]
  }

Each board manifest is independently hashed and must bind all identities,
profile, timer, candidate scope, source commit/tree, artifact hashes, and the
plan's declared hooks before a pyOCD ``load`` command can be reached.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Sequence


PLAN_SCHEMA = "tron.tavrn.benchmark.plan.v1"
RUN_SCHEMA = "tron.tavrn.benchmark.run.v1"
ROLES = ("A", "B", "C", "D", "E", "F")
ROLE_NUMBERS = {role: index for index, role in enumerate(ROLES, 1)}
FICR_DEVICEADDR0 = 0x100000A4
FICR_DEVICEADDR1 = 0x100000A8
UID_RE = re.compile(r"[0-9a-f]{48}")
SHA256_RE = re.compile(r"[0-9a-f]{64}")
COMMIT_RE = re.compile(r"[0-9a-f]{40}")
ADVA_RE = re.compile(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}")


class RunError(Exception):
    """A prerequisite did not prove the requested run safe to execute."""


@dataclass(frozen=True)
class InventoryBoard:
    uid: str
    adva: str


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(65536), b""):
                digest.update(chunk)
    except OSError as error:
        raise RunError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def canonical_adva(value: str) -> str:
    if not isinstance(value, str):
        raise RunError("AdvA must be a string")
    canonical = value.lower()
    if not ADVA_RE.fullmatch(canonical):
        raise RunError(f"invalid canonical AdvA {value!r}")
    octets = [int(octet, 16) for octet in canonical.split(":")]
    if all(octet == 0 for octet in octets) or all(octet == 0xFF for octet in octets):
        raise RunError("AdvA must not be all zero/all ff")
    if octets[5] & 0xC0 != 0xC0:
        raise RunError("AdvA is not random-static in canonical byte order")
    return canonical


def derive_canonical_adva(deviceaddr0: int, deviceaddr1: int) -> str:
    """Derive repository-canonical AdvA from nRF FICR DEVICEADDR words."""
    if not 0 <= deviceaddr0 <= 0xFFFFFFFF or not 0 <= deviceaddr1 <= 0xFFFFFFFF:
        raise RunError("FICR device address words are outside uint32")
    octets = [
        deviceaddr0 & 0xFF,
        (deviceaddr0 >> 8) & 0xFF,
        (deviceaddr0 >> 16) & 0xFF,
        (deviceaddr0 >> 24) & 0xFF,
        deviceaddr1 & 0xFF,
        ((deviceaddr1 >> 8) & 0xFF) | 0xC0,
    ]
    return canonical_adva(":".join(f"{octet:02x}" for octet in octets))


def parse_inventory(path: Path) -> dict[str, InventoryBoard]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as error:
        raise RunError(f"cannot read inventory {path}: {error}") from error
    rows: list[tuple[str, str]] = []
    for line_number, line in enumerate(lines, 1):
        if not line:
            raise RunError(f"inventory {path}:{line_number} is blank")
        if line.startswith("#"):
            if line != "# probe_uid\tcanonical_adva":
                raise RunError(f"inventory {path}:{line_number} has unexpected header")
            continue
        fields = line.split("\t")
        if len(fields) != 2:
            raise RunError(f"inventory {path}:{line_number} must have UID and AdvA only")
        uid, adva = fields
        if not UID_RE.fullmatch(uid):
            raise RunError(f"inventory {path}:{line_number} has invalid probe UID")
        rows.append((uid, canonical_adva(adva)))
    if len(rows) != 6:
        raise RunError("inventory must contain exactly six board rows")
    if len({uid for uid, _ in rows}) != 6 or len({adva for _, adva in rows}) != 6:
        raise RunError("inventory UIDs and AdvAs must each be unique")
    return {uid: InventoryBoard(uid, adva) for uid, adva in rows}


def parse_kv_manifest(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as error:
        raise RunError(f"cannot read artifact manifest {path}: {error}") from error
    values: dict[str, str] = {}
    for line_number, line in enumerate(lines, 1):
        if not line or line.count("=") != 1:
            raise RunError(f"artifact manifest {path}:{line_number} is malformed")
        key, value = line.split("=", 1)
        if not key or not value or key in values:
            raise RunError(f"artifact manifest {path}:{line_number} has empty/duplicate key")
        values[key] = value
    return values


def _string(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value:
        raise RunError(f"run plan lacks {name}")
    return value


def _positive_int(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise RunError(f"run plan has invalid {name}")
    return value


def _sha(value: Any, name: str) -> str:
    value = _string(value, name)
    if not SHA256_RE.fullmatch(value):
        raise RunError(f"run plan has invalid {name}")
    return value


def _commit(value: Any, name: str) -> str:
    value = _string(value, name)
    if not COMMIT_RE.fullmatch(value):
        raise RunError(f"run plan has invalid {name}")
    return value


def load_plan(path: Path) -> dict[str, Any]:
    try:
        plan = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RunError(f"cannot read run plan {path}: {error}") from error
    if not isinstance(plan, dict) or plan.get("schema") != PLAN_SCHEMA:
        raise RunError("run plan schema is missing or unsupported")
    source = plan.get("source")
    benchmark = plan.get("benchmark")
    boards = plan.get("boards")
    if not isinstance(source, dict) or not isinstance(benchmark, dict) or not isinstance(boards, list):
        raise RunError("run plan lacks source, benchmark, or boards")
    _commit(source.get("commit"), "source.commit")
    _commit(source.get("tree"), "source.tree")
    _string(plan.get("profile"), "profile")
    _string(plan.get("timer_profile"), "timer_profile")
    _string(plan.get("candidate_scope"), "candidate_scope")
    for key in ("source_role", "destination_role", "origin", "destination", "width"):
        _string(benchmark.get(key), f"benchmark.{key}")
    if benchmark["source_role"] != "A" or benchmark["destination_role"] != "C":
        raise RunError("run plan must name A source and C destination")
    for key in ("warmup_seconds", "offered_seconds", "drain_seconds", "offered_rate_hz",
                "interval_ms", "offered_slots"):
        _positive_int(benchmark.get(key), f"benchmark.{key}")
    if benchmark["interval_ms"] * benchmark["offered_rate_hz"] != 1000:
        raise RunError("benchmark interval_ms and offered_rate_hz must exactly describe one second")
    if benchmark["offered_slots"] != benchmark["offered_seconds"] * benchmark["offered_rate_hz"]:
        raise RunError("benchmark offered_slots must equal offered_seconds * offered_rate_hz")
    if len(boards) != 6:
        raise RunError("run plan must contain exactly six boards")
    roles: set[str] = set()
    for board in boards:
        if not isinstance(board, dict):
            raise RunError("run plan contains malformed board")
        role = _string(board.get("role"), "board.role")
        if role not in ROLES or role in roles:
            raise RunError("run plan has duplicate/invalid role")
        roles.add(role)
        uid = _string(board.get("uid"), f"board {role}.uid")
        if not UID_RE.fullmatch(uid):
            raise RunError(f"run plan has invalid UID for {role}")
        canonical_adva(_string(board.get("adva"), f"board {role}.adva"))
        serial_device = _string(board.get("serial_device"), f"board {role}.serial_device")
        if not serial_device.startswith("/dev/serial/by-id/") or uid not in serial_device:
            raise RunError(f"run plan serial device for {role} is not UID-bound by-id path")
        artifact = board.get("artifact")
        if not isinstance(artifact, dict):
            raise RunError(f"run plan lacks artifact for {role}")
        for name in ("manifest", "elf", "hex"):
            _string(artifact.get(name), f"board {role}.artifact.{name}")
        for name in ("manifest_sha256", "elf_sha256", "hex_sha256"):
            _sha(artifact.get(name), f"board {role}.artifact.{name}")
        flash = artifact.get("flash_image")
        if not isinstance(flash, dict) or flash.get("format") not in ("elf", "hex"):
            raise RunError(f"run plan lacks valid flash_image for {role}")
        _string(flash.get("path"), f"board {role}.artifact.flash_image.path")
        _sha(flash.get("sha256"), f"board {role}.artifact.flash_image.sha256")
        hooks = board.get("manifest_hooks")
        if not isinstance(hooks, dict) or not hooks or any(
                not isinstance(key, str) or not isinstance(value, str) or not key or not value
                for key, value in hooks.items()):
            raise RunError(f"run plan lacks explicit manifest_hooks for {role}")
        benchmark_manifest = board.get("manifest_benchmark")
        if not isinstance(benchmark_manifest, dict) or not benchmark_manifest or any(
                not isinstance(key, str) or not isinstance(value, str) or not key or not value
                for key, value in benchmark_manifest.items()):
            raise RunError(f"run plan lacks explicit manifest_benchmark for {role}")
    if roles != set(ROLES):
        raise RunError("run plan does not map roles A through F")
    return plan


def _verify_file_hash(path_text: str, expected: str, name: str) -> Path:
    path = Path(path_text)
    actual = sha256_file(path)
    if actual != expected:
        raise RunError(f"{name} hash mismatch")
    return path


def _manifest_value(manifest: dict[str, str], key: str, expected: str, role: str) -> None:
    if manifest.get(key) != expected:
        actual = manifest.get(key, "MISSING")
        raise RunError(f"board {role} manifest mismatch {key}: expected {expected!r}, got {actual!r}")


def preflight(plan: dict[str, Any], inventory: dict[str, InventoryBoard]) -> list[dict[str, Any]]:
    """Validate every immutable input before any pyOCD load can be issued."""
    checked: list[dict[str, Any]] = []
    seen_uids: set[str] = set()
    seen_advas: set[str] = set()
    for board in sorted(plan["boards"], key=lambda item: item["role"]):
        role = board["role"]
        uid = board["uid"]
        adva = canonical_adva(board["adva"])
        inventory_board = inventory.get(uid)
        if inventory_board is None or inventory_board.adva != adva:
            raise RunError(f"board {role} UID/AdvA does not exactly match inventory")
        if uid in seen_uids or adva in seen_advas:
            raise RunError("run plan board identities are ambiguous")
        seen_uids.add(uid)
        seen_advas.add(adva)
        artifact = board["artifact"]
        manifest_path = _verify_file_hash(artifact["manifest"], artifact["manifest_sha256"],
                                          f"board {role} manifest")
        elf_path = _verify_file_hash(artifact["elf"], artifact["elf_sha256"],
                                     f"board {role} ELF")
        hex_path = _verify_file_hash(artifact["hex"], artifact["hex_sha256"],
                                     f"board {role} HEX")
        flash = artifact["flash_image"]
        flash_path = _verify_file_hash(flash["path"], flash["sha256"],
                                       f"board {role} flash image")
        expected_path = elf_path if flash["format"] == "elf" else hex_path
        expected_hash = artifact["elf_sha256"] if flash["format"] == "elf" else artifact["hex_sha256"]
        if flash_path != expected_path or flash["sha256"] != expected_hash:
            raise RunError(f"board {role} flash image is not the declared published artifact")
        manifest = parse_kv_manifest(manifest_path)
        expected = {
            "build.target": "tavrn_routed_node",
            "build.role": role.lower(),
            "bench.role_number": str(ROLE_NUMBERS[role]),
            "identity.target_probe_uid": uid,
            "identity.adva": adva,
            "feature.level.effective": plan["profile"],
            "build.timer_profile": plan["timer_profile"],
            "candidate.configured": "ON",
            "candidate.scope": plan["candidate_scope"],
            "source.commit": plan["source"]["commit"],
            "source.tree": plan["source"]["tree"],
            "artifact.elf.sha256": artifact["elf_sha256"],
            "artifact.hex.sha256": artifact["hex_sha256"],
        }
        for key, value in expected.items():
            _manifest_value(manifest, key, value, role)
        for key, value in sorted(board["manifest_hooks"].items()):
            _manifest_value(manifest, key, value, role)
        for key, value in sorted(board["manifest_benchmark"].items()):
            _manifest_value(manifest, key, value, role)
        checked.append({
            "role": role,
            "uid": uid,
            "adva": adva,
            "serial_device": board["serial_device"],
            "artifact": {
                "manifest": str(manifest_path), "manifest_sha256": artifact["manifest_sha256"],
                "elf": str(elf_path), "elf_sha256": artifact["elf_sha256"],
                "hex": str(hex_path), "hex_sha256": artifact["hex_sha256"],
                "flash_image": {"format": flash["format"], "path": str(flash_path), "sha256": flash["sha256"]},
            },
            "manifest_hooks": dict(sorted(board["manifest_hooks"].items())),
            "manifest_benchmark": dict(sorted(board["manifest_benchmark"].items())),
        })
    if len(checked) != 6:
        raise RunError("preflight did not validate exactly six boards")
    return checked


def ficr_command(pyocd: str, uid: str) -> list[str]:
    """Return the documented pyOCD Commander read32 invocation for DEVICEADDR0/1."""
    return [pyocd, "commander", "--uid", uid, "--target", "nrf52833",
            "--command", f"read32 0x{FICR_DEVICEADDR0:08x} 8"]


def parse_ficr_words(output: str) -> tuple[int, int]:
    """Read the two words printed by ``read32 0x100000a4 8`` without guessing labels."""
    words_by_address: dict[int, list[int]] = {}
    for line in output.splitlines():
        lowered = line.lower()
        for address in (FICR_DEVICEADDR0, FICR_DEVICEADDR1):
            marker = f"{address:08x}"
            if marker not in lowered:
                continue
            tail = line[lowered.index(marker) + len(marker):]
            parsed = [int(word, 16) for word in re.findall(
                r"(?i)(?:0x)?([0-9a-f]{1,8})", tail)]
            if parsed:
                words_by_address[address] = parsed
    first = words_by_address.get(FICR_DEVICEADDR0, [])
    second = words_by_address.get(FICR_DEVICEADDR1, [])
    if not first:
        raise RunError("pyOCD FICR read did not print DEVICEADDR0")
    values = [first[0], first[1] if len(first) > 1 else (second[0] if second else -1)]
    if values[1] < 0 or any(value > 0xFFFFFFFF for value in values):
        raise RunError("pyOCD FICR read did not print two uint32 DEVICEADDR words")
    return values[0], values[1]


def run_ficr_check(board: dict[str, Any], pyocd: str,
                   run: Callable[..., subprocess.CompletedProcess[str]]) -> dict[str, Any]:
    command = ficr_command(pyocd, board["uid"])
    try:
        completed = run(command, check=False, capture_output=True, text=True)
    except OSError as error:
        raise RunError(f"board {board['role']} could not invoke pyOCD FICR read: {error}") from error
    if completed.returncode != 0:
        raise RunError(f"board {board['role']} pyOCD FICR read failed exit={completed.returncode}")
    word0, word1 = parse_ficr_words((completed.stdout or "") + "\n" + (completed.stderr or ""))
    observed = derive_canonical_adva(word0, word1)
    if observed != board["adva"]:
        raise RunError(f"board {board['role']} FICR AdvA mismatch expected={board['adva']} got={observed}")
    return {"command": command, "deviceaddr0": f"0x{word0:08x}",
            "deviceaddr1": f"0x{word1:08x}", "adva": observed}


def flash_command(pyocd: str, board: dict[str, Any]) -> list[str]:
    return [pyocd, "load", board["artifact"]["flash_image"]["path"], "--uid", board["uid"],
            "--target", "nrf52833"]


def halt_command(pyocd: str, uid: str) -> list[str]:
    return [pyocd, "commander", "--uid", uid, "--target", "nrf52833", "--command", "halt"]


def reset_command(pyocd: str, uid: str) -> list[str]:
    return [pyocd, "reset", "--uid", uid, "--target", "nrf52833"]


def grabserial_command(grabserial: str, board: dict[str, Any], duration_seconds: int) -> list[str]:
    return [grabserial, "-d", board["serial_device"], "-b", "115200", "-T", "-F",
            "%Y-%m-%dT%H:%M:%S.%fZ", "-e", str(duration_seconds)]


def _run_checked(command: Sequence[str], event: str,
                 run: Callable[..., subprocess.CompletedProcess[str]], events: list[dict[str, Any]]) -> None:
    started = utc_now()
    try:
        completed = run(list(command), check=False, capture_output=True, text=True)
    except OSError as error:
        events.append({"at": started, "event": event, "command": list(command), "status": "spawn_error", "detail": str(error)})
        raise RunError(f"{event} could not start: {error}") from error
    status = "ok" if completed.returncode == 0 else "failed"
    events.append({"at": started, "event": event, "command": list(command), "status": status,
                   "returncode": completed.returncode})
    if completed.returncode != 0:
        raise RunError(f"{event} failed exit={completed.returncode}")


def _plan_hash(plan_path: Path) -> str:
    return sha256_file(plan_path)


def build_metadata(plan: dict[str, Any], checked: list[dict[str, Any],], plan_path: Path,
                   inventory_path: Path, status: str, events: list[dict[str, Any]],
                   started: str, ended: str | None = None) -> dict[str, Any]:
    return {
        "schema": RUN_SCHEMA,
        "status": status,
        "started_utc": started,
        "ended_utc": ended,
        "plan": {"path": str(plan_path), "sha256": _plan_hash(plan_path)},
        "inventory": {"path": str(inventory_path), "sha256": sha256_file(inventory_path)},
        "source": dict(sorted(plan["source"].items())),
        "profile": plan["profile"],
        "timer_profile": plan["timer_profile"],
        "candidate_scope": plan["candidate_scope"],
        "benchmark": dict(sorted(plan["benchmark"].items())),
        "boards": checked,
        "events": events,
    }


def _write_metadata(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def execute_capture(plan: dict[str, Any], checked: list[dict[str, Any]], pyocd: str,
                    grabserial: str, output_path: Path,
                    run: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
                    popen: Callable[..., subprocess.Popen[Any]] = subprocess.Popen,
                    sleep: Callable[[float], None] = time.sleep,
                    events: list[dict[str, Any]] | None = None) -> list[dict[str, Any]]:
    """Flash serially, capture six logs concurrently, and leave auditable events."""
    del sleep  # Capture duration is delegated to finite grabserial processes.
    events = events if events is not None else []
    for board in checked:
        _run_checked(flash_command(pyocd, board), f"flash_{board['role']}", run, events)
        _run_checked(halt_command(pyocd, board["uid"]), f"halt_{board['role']}", run, events)
    duration = (plan["benchmark"]["warmup_seconds"] + plan["benchmark"]["offered_seconds"] +
                plan["benchmark"]["drain_seconds"])
    log_dir = output_path.parent / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    processes: list[tuple[dict[str, Any], Path, Any, Any, list[str]]] = []
    try:
        for board in checked:
            log_path = log_dir / f"board-{board['role']}.log"
            command = grabserial_command(grabserial, board, duration)
            handle = log_path.open("w", encoding="utf-8")
            try:
                process = popen(command, stdout=handle, stderr=subprocess.STDOUT, text=True)
            except OSError as error:
                handle.close()
                events.append({"at": utc_now(), "event": f"capture_{board['role']}",
                               "command": command, "status": "spawn_error", "detail": str(error)})
                raise RunError(f"capture_{board['role']} could not start: {error}") from error
            events.append({"at": utc_now(), "event": f"capture_{board['role']}",
                           "command": command, "status": "started", "log": str(log_path)})
            processes.append((board, log_path, process, handle, command))
        for board in checked:
            _run_checked(reset_command(pyocd, board["uid"]), f"run_{board['role']}", run, events)
        timeout = duration + 30
        for board, log_path, process, handle, command in processes:
            try:
                returncode = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired as error:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                events.append({"at": utc_now(), "event": f"capture_{board['role']}",
                               "command": command, "status": "timeout"})
                raise RunError(f"capture_{board['role']} exceeded finite timeout") from error
            finally:
                handle.close()
            status = "ok" if returncode == 0 else "failed"
            events.append({"at": utc_now(), "event": f"capture_{board['role']}", "command": command,
                           "status": status, "returncode": returncode, "log": str(log_path)})
            if returncode != 0:
                raise RunError(f"capture_{board['role']} failed exit={returncode}")
            board["log"] = {"path": str(log_path), "sha256": sha256_file(log_path)}
    finally:
        for _board, _log_path, process, handle, _command in processes:
            if getattr(process, "poll", lambda: 0)() is None:
                process.terminate()
            if not handle.closed:
                handle.close()
    return events


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", required=True, help="strict six-row UID/AdvA TSV")
    parser.add_argument("--run-plan", required=True, help="strict JSON plan")
    parser.add_argument("--output", required=True, help="run metadata JSON path")
    parser.add_argument("--pyocd", default="pyocd")
    parser.add_argument("--grabserial", default="grabserial")
    parser.add_argument("--dry-run", action="store_true", help="validate static inputs and emit planned commands only")
    parser.add_argument("--preflight-only", action="store_true", help="run FICR reads then stop before flash")
    parser.add_argument("--execute", action="store_true", help="allow sequential UID-targeted flash and capture")
    args = parser.parse_args(argv)
    output = Path(args.output)
    plan_path = Path(args.run_plan)
    inventory_path = Path(args.inventory)
    started = utc_now()
    events: list[dict[str, Any]] = []
    plan: dict[str, Any] | None = None
    checked: list[dict[str, Any]] = []
    try:
        if args.dry_run and args.preflight_only:
            raise RunError("--dry-run and --preflight-only are mutually exclusive")
        plan = load_plan(plan_path)
        inventory = parse_inventory(inventory_path)
        checked = preflight(plan, inventory)
        if args.dry_run:
            duration = (plan["benchmark"]["warmup_seconds"] + plan["benchmark"]["offered_seconds"] +
                        plan["benchmark"]["drain_seconds"])
            for board in checked:
                events.extend([
                    {"at": started, "event": f"ficr_{board['role']}", "command": ficr_command(args.pyocd, board["uid"]), "status": "not_run"},
                    {"at": started, "event": f"flash_{board['role']}", "command": flash_command(args.pyocd, board), "status": "not_run"},
                    {"at": started, "event": f"halt_{board['role']}", "command": halt_command(args.pyocd, board["uid"]), "status": "not_run"},
                    {"at": started, "event": f"run_{board['role']}", "command": reset_command(args.pyocd, board["uid"]), "status": "not_run"},
                    {"at": started, "event": f"capture_{board['role']}",
                     "command": grabserial_command(args.grabserial, board, duration), "status": "not_run"},
                ])
            metadata = build_metadata(plan, checked, plan_path, inventory_path, "dry_run", events,
                                      started, utc_now())
            _write_metadata(output, metadata)
            return 0
        if not args.preflight_only and not args.execute:
            raise RunError("refusing hardware action without explicit --execute (or --dry-run/--preflight-only)")
        if shutil.which(args.pyocd) is None:
            raise RunError(f"pyOCD executable not found: {args.pyocd}")
        for board in checked:
            ficr = run_ficr_check(board, args.pyocd, subprocess.run)
            events.append({"at": utc_now(), "event": f"ficr_{board['role']}", "status": "ok", **ficr})
        if args.preflight_only:
            metadata = build_metadata(plan, checked, plan_path, inventory_path, "preflight_completed",
                                      events, started, utc_now())
            _write_metadata(output, metadata)
            return 0
        if shutil.which(args.grabserial) is None:
            raise RunError(f"grabserial executable not found: {args.grabserial}")
        execute_capture(plan, checked, args.pyocd, args.grabserial, output, events=events)
        metadata = build_metadata(plan, checked, plan_path, inventory_path, "completed", events,
                                  started, utc_now())
        _write_metadata(output, metadata)
    except RunError as error:
        if plan is not None:
            try:
                _write_metadata(output, build_metadata(plan, checked, plan_path, inventory_path,
                                                       "failed", events, started, utc_now()))
            except RunError:
                pass
        print(f"FAIL tavrn_benchmark_run: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
