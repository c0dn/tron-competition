#!/usr/bin/env python3
"""Run one continuous, UID-bound six-board TAVRN observation capture.

The runner never rebuilds.  It creates a new immutable run directory, pins the
inventory, plan, artifact, manifest, and explicit tool bytes before touching a
probe, then captures indefinitely until Ctrl-C or an unexpected child exit.
Use explicit absolute paths for pyOCD, grabserial, and uv (for example paths
from ``uv tool dir``); PATH lookup is deliberately not used for run actions.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import os
import re
import select
import signal
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Sequence


class RunError(Exception):
    pass


class TerminationRequest(Exception):
    """A SIGTERM or SIGHUP received while the hardware capture is active."""

    def __init__(self, signum: int) -> None:
        self.signum = signum
        super().__init__(signal.Signals(signum).name)


_ANALYZER_PATH = Path(__file__).with_name("summarize_tavrn_benchmark.py")
try:
    _ANALYZER_BYTES = _ANALYZER_PATH.read_bytes()
except OSError as error:
    raise RunError(f"cannot read analyzer {_ANALYZER_PATH}: {error}") from error
_ANALYZER_SHA256 = hashlib.sha256(_ANALYZER_BYTES).hexdigest()
_ANALYZER_SPEC = importlib.util.spec_from_file_location("tavrn_observation_analyzer", _ANALYZER_PATH)
assert _ANALYZER_SPEC is not None and _ANALYZER_SPEC.loader is not None
analysis = importlib.util.module_from_spec(_ANALYZER_SPEC)
sys.modules[_ANALYZER_SPEC.name] = analysis
exec(compile(_ANALYZER_BYTES, str(_ANALYZER_PATH), "exec"), analysis.__dict__)

PLAN_SCHEMA = "tron.tavrn.observation.plan.v2"
RUN_SCHEMA = analysis.RUN_SCHEMA
ROLES = analysis.ROLES
ROLE_NUMBERS = {role: index for index, role in enumerate(ROLES, 1)}
FICR_DEVICEADDR0 = 0x100000A4
FICR_DEVICEADDR1 = 0x100000A8
UID_RE = re.compile(r"[0-9a-f]{48}")
SHA_RE = re.compile(r"[0-9a-f]{64}")
COMMIT_RE = re.compile(r"[0-9a-f]{40}")
ADVA_RE = re.compile(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}")
BENCH_SCOPE = "BENCH_HOOKED_RESTRICTED"
BENCH_PROFILES = {"AODV_ONLY", "FULL_TAVRN"}
PROFILE_IDENTITY_WIDTH = {"AODV_ONLY": "SID16", "FULL_TAVRN": "SID8"}


@dataclass(frozen=True)
class InventoryBoard:
    uid: str
    adva: str


@dataclass
class CaptureChild:
    board: dict[str, Any]
    process: Any
    handle: Any
    command: list[str]
    log_path: Path
    offset: int = 0


@dataclass(frozen=True)
class CaptureEnd:
    """The single wall-clock boundary shared by live analysis and metadata."""

    host_ms: float
    utc: str


def _format_utc(instant: dt.datetime) -> str:
    return instant.strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def utc_now() -> str:
    return _format_utc(dt.datetime.now(dt.timezone.utc))


def capture_end_instant() -> CaptureEnd:
    """Take the sole authoritative end instant after the UART logs are drained."""
    instant = dt.datetime.now(dt.timezone.utc)
    return CaptureEnd(instant.timestamp() * 1000.0, _format_utc(instant))


def _termination_status(signum: int) -> str:
    return f"terminated_{signal.Signals(signum).name.lower()}"


def _raise_termination_request(signum: int, _frame: Any) -> None:
    raise TerminationRequest(signum)


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
    if not isinstance(value, str) or not ADVA_RE.fullmatch(value.lower()):
        raise RunError("invalid canonical AdvA")
    value = value.lower()
    octets = [int(item, 16) for item in value.split(":")]
    if all(item == 0 for item in octets) or all(item == 0xff for item in octets) or octets[5] & 0xc0 != 0xc0:
        raise RunError("AdvA is not a valid random-static identity")
    return value


def derive_canonical_adva(deviceaddr0: int, deviceaddr1: int) -> str:
    return canonical_adva(":".join(f"{item:02x}" for item in (
        deviceaddr0 & 0xff, (deviceaddr0 >> 8) & 0xff, (deviceaddr0 >> 16) & 0xff,
        (deviceaddr0 >> 24) & 0xff, deviceaddr1 & 0xff, ((deviceaddr1 >> 8) & 0xff) | 0xc0)))


def parse_inventory(path: Path) -> dict[str, InventoryBoard]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as error:
        raise RunError(f"cannot read inventory {path}: {error}") from error
    rows: list[InventoryBoard] = []
    for number, line in enumerate(lines, 1):
        if line == "# probe_uid\tcanonical_adva":
            continue
        if not line:
            raise RunError(f"inventory {path}:{number} is blank")
        values = line.split("\t")
        if len(values) != 2 or not UID_RE.fullmatch(values[0]):
            raise RunError(f"inventory {path}:{number} is malformed")
        rows.append(InventoryBoard(values[0], canonical_adva(values[1])))
    if len(rows) != 6 or len({row.uid for row in rows}) != 6 or len({row.adva for row in rows}) != 6:
        raise RunError("inventory must have exactly six unique UID/AdvA rows")
    return {row.uid: row for row in rows}


def parse_manifest(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as error:
        raise RunError(f"cannot read manifest {path}: {error}") from error
    result: dict[str, str] = {}
    for number, line in enumerate(lines, 1):
        if not line or line.count("=") != 1:
            raise RunError(f"manifest {path}:{number} is malformed")
        key, value = line.split("=", 1)
        if not key or not value or key in result:
            raise RunError(f"manifest {path}:{number} has empty/duplicate key")
        result[key] = value
    return result


def _string(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value:
        raise RunError(f"run plan lacks {name}")
    return value


def _positive(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise RunError(f"run plan has invalid {name}")
    return value


def _hash(value: Any, name: str) -> str:
    value = _string(value, name)
    if not SHA_RE.fullmatch(value):
        raise RunError(f"run plan has invalid {name}")
    return value


def load_plan(path: Path) -> dict[str, Any]:
    try:
        plan = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RunError(f"cannot read run plan {path}: {error}") from error
    if not isinstance(plan, dict) or plan.get("schema") != PLAN_SCHEMA:
        raise RunError("run plan schema is missing or unsupported")
    source, observation, boards = plan.get("source"), plan.get("observation"), plan.get("boards")
    if not isinstance(source, dict) or not isinstance(observation, dict) or not isinstance(boards, list):
        raise RunError("run plan lacks source, observation, or boards")
    for name in ("commit", "tree"):
        if not COMMIT_RE.fullmatch(_string(source.get(name), f"source.{name}")):
            raise RunError(f"run plan has invalid source.{name}")
    profile = _string(plan.get("profile"), "profile")
    if profile not in BENCH_PROFILES:
        raise RunError("run plan profile must be AODV_ONLY or FULL_TAVRN")
    if _string(plan.get("timer_profile"), "timer_profile") != "BALANCED":
        raise RunError("run plan timer_profile must be BALANCED")
    network = plan.get("network")
    if not isinstance(network, dict) or network.get("id") != 1:
        raise RunError("run plan network.id must be 1")
    if _string(plan.get("candidate_scope"), "candidate_scope") != BENCH_SCOPE:
        raise RunError(f"run plan candidate_scope must be {BENCH_SCOPE}")
    if observation.get("source_role") != "A" or observation.get("destination_role") != "C":
        raise RunError("observation must explicitly select source A and destination C")
    for name in ("heartbeat_hz", "throughput_hz", "throughput_start_seconds", "throughput_burst_seconds", "throughput_repeat_seconds", "window_seconds"):
        _positive(observation.get(name), f"observation.{name}")
    if observation["heartbeat_hz"] != 1 or observation["throughput_hz"] != 10 or observation["window_seconds"] != 10 or \
            observation["throughput_start_seconds"] != 60 or observation["throughput_burst_seconds"] != 60 or observation["throughput_repeat_seconds"] != 450:
        raise RunError("observation workload must be 1Hz heartbeat and 10Hz/60s throughput at 60+450s with 10s windows")
    if len(boards) != 6:
        raise RunError("run plan must map exactly six boards")
    roles: set[str] = set()
    uids: set[str] = set()
    advas: set[str] = set()
    serial_paths: set[str] = set()
    resolved_serial_paths: set[Path] = set()
    for board in boards:
        if not isinstance(board, dict):
            raise RunError("run plan has malformed board")
        role = _string(board.get("role"), "board.role")
        if role not in ROLES or role in roles:
            raise RunError("run plan has duplicate/invalid board role")
        roles.add(role)
        uid = _string(board.get("uid"), f"board {role}.uid")
        if not UID_RE.fullmatch(uid) or uid in uids:
            raise RunError(f"run plan has invalid UID for {role}")
        uids.add(uid)
        adva = canonical_adva(_string(board.get("adva"), f"board {role}.adva"))
        if adva in advas:
            raise RunError(f"run plan has duplicate AdvA for {role}")
        advas.add(adva)
        serial = _string(board.get("serial_device"), f"board {role}.serial_device")
        if not serial.startswith("/dev/serial/by-id/") or uid not in serial:
            raise RunError(f"run plan serial device for {role} is not UID-bound")
        try:
            resolved_serial = Path(serial).resolve(strict=False)
        except (OSError, RuntimeError) as error:
            raise RunError(f"run plan cannot resolve serial device for {role}: {error}") from error
        if serial in serial_paths or resolved_serial in resolved_serial_paths:
            raise RunError(f"run plan has duplicate serial device for {role}")
        serial_paths.add(serial)
        resolved_serial_paths.add(resolved_serial)
        artifact = board.get("artifact")
        if not isinstance(artifact, dict):
            raise RunError(f"run plan lacks artifact for {role}")
        for name in ("manifest", "elf", "hex"):
            _string(artifact.get(name), f"board {role}.artifact.{name}")
        for name in ("manifest_sha256", "elf_sha256", "hex_sha256"):
            _hash(artifact.get(name), f"board {role}.artifact.{name}")
        flash = artifact.get("flash_image")
        if not isinstance(flash, dict) or flash.get("format") not in ("elf", "hex"):
            raise RunError(f"run plan lacks flash image for {role}")
        _string(flash.get("path"), f"board {role}.flash.path"); _hash(flash.get("sha256"), f"board {role}.flash.sha256")
        for section in ("manifest_hooks", "manifest_observation"):
            mapping = board.get(section)
            if not isinstance(mapping, dict) or not mapping or any(not isinstance(k, str) or not isinstance(v, str) or not k or not v for k, v in mapping.items()):
                raise RunError(f"run plan lacks explicit {section} for {role}")
    if roles != set(ROLES) or len(uids) != 6 or len(advas) != 6 or len(resolved_serial_paths) != 6:
        raise RunError("run plan does not cover A through F")
    return plan


def _verify(path_text: str, expected: str, label: str) -> Path:
    path = Path(path_text)
    if sha256_file(path) != expected:
        raise RunError(f"{label} hash mismatch")
    return path


def _fixed_manifest_requirements(board: dict[str, Any], plan: dict[str, Any], inventory_hash: str,
                                 advas_by_role: dict[str, str]) -> dict[str, str]:
    """Return the runner-owned acceptance contract, never plan-owned policy."""
    role = board["role"]
    rx_block = (advas_by_role["C"] if role == "A" else
                advas_by_role["A"] if role == "C" else "NOT_CONFIGURED")
    peer_adva = advas_by_role["A"] if role == "C" else advas_by_role["C"]
    identity_width = PROFILE_IDENTITY_WIDTH[plan["profile"]]
    profile_requirements = ({
        "build.behavior": "TAVRN_ROUTED_AODV_ONLY",
        "feature.repair.requested": "OFF",
        "feature.repair.effective": "OFF",
    } if plan["profile"] == "AODV_ONLY" else {
        "build.behavior": "TAVRN_ROUTED_FULL_TAVRN_ESC_K1_MENTORSHIP_ADAPTIVE_HELLO_LOCAL_EXPIRY_TARGETED_FRESHNESS_RREQ_VERIFICATION_TC_METADATA_LOCAL_REPAIR",
        "feature.repair.requested": "ON",
        "feature.repair.effective": "ON",
    })
    artifact = board["artifact"]
    requirements = {
        "build.target": "tavrn_routed_node",
        "build.role": role.lower(),
        "bench.role_number": str(ROLE_NUMBERS[role]),
        "feature.level.effective": plan["profile"],
        "build.timer_profile": plan["timer_profile"],
        "identity.target_probe_uid": board["uid"],
        "identity.adva": canonical_adva(board["adva"]),
        "identity.width": identity_width,
        "source.commit": plan["source"]["commit"],
        "source.tree": plan["source"]["tree"],
        "source.dirty": "no",
        "source.submodule_dirty": "no",
        "candidate.configured": "ON",
        "candidate.scope": BENCH_SCOPE,
        "candidate.clean_source": "yes",
        "candidate.hardware_purpose": "yes",
        "candidate.hook_bench_eligible": "yes",
        "candidate.unhooked_acceptance": "no",
        "bench.mode": "ON",
        "bench.identify_display": "OFF",
        "bench.control_observability": "COMPILE_TIME_OPTIONAL",
        "bench.heartbeat_interval_ms": "1000",
        "bench.burst_interval_ms": "100",
        "bench.burst_start_ms": "60000",
        "bench.burst_duration_ms": "60000",
        "bench.burst_period_ms": "450000",
        "bench.burst_slots": "600",
        "bench.origin_role": "1",
        "bench.destination_role": "3",
        "hook.enabled": "ON",
        "hook.expiry_full_table": "OFF",
        "hook.hack_drop_peer_adva": "NOT_CONFIGURED",
        "hook.hack_drop_count": "0",
        "hook.busy_admission_count": "0",
        "hook.collision_peer_adva": "NOT_CONFIGURED",
        "hook.legacy_alias_input": "NOT_APPLICABLE",
        "hook.legacy_rx_block_peer_id": "NOT_APPLICABLE",
        "hook.rx_block_adva": rx_block,
        "link_test.peer_adva": peer_adva,
        "network.id": "1",
        "identity.inventory.record_count": "6",
        "identity.inventory.full_adva.unique": "yes",
        "identity.inventory.sid16.unique": "yes",
        "identity.inventory.sid16.nonreserved": "yes",
        "identity.inventory.selected_width": identity_width,
        "identity.inventory.sha256": inventory_hash,
        "artifact.elf.sha256": artifact["elf_sha256"],
        "artifact.hex.sha256": artifact["hex_sha256"],
    }
    requirements.update(profile_requirements)
    if plan["profile"] == "FULL_TAVRN":
        requirements.update({"identity.inventory.sid8.unique": "yes",
                             "identity.inventory.sid8.nonreserved": "yes"})
    return requirements


def preflight(plan: dict[str, Any], inventory: dict[str, InventoryBoard], inventory_hash: str) -> list[dict[str, Any]]:
    if not SHA_RE.fullmatch(inventory_hash):
        raise RunError("inventory hash is invalid")
    checked: list[dict[str, Any]] = []
    advas_by_role = {board["role"]: canonical_adva(board["adva"]) for board in plan["boards"]}
    for board in sorted(plan["boards"], key=lambda item: item["role"]):
        role, uid, adva = board["role"], board["uid"], canonical_adva(board["adva"])
        if inventory.get(uid) is None or inventory[uid].adva != adva:
            raise RunError(f"board {role} UID/AdvA mismatch inventory")
        artifact = board["artifact"]
        manifest_path = _verify(artifact["manifest"], artifact["manifest_sha256"], f"board {role} manifest")
        elf = _verify(artifact["elf"], artifact["elf_sha256"], f"board {role} ELF")
        hex_file = _verify(artifact["hex"], artifact["hex_sha256"], f"board {role} HEX")
        flash = artifact["flash_image"]
        flash_path = _verify(flash["path"], flash["sha256"], f"board {role} flash image")
        if flash_path != (elf if flash["format"] == "elf" else hex_file):
            raise RunError(f"board {role} flash image is not the declared artifact")
        manifest = parse_manifest(manifest_path)
        for key, value in _fixed_manifest_requirements(board, plan, inventory_hash, advas_by_role).items():
            if manifest.get(key) != value:
                raise RunError(f"board {role} manifest mismatch {key}")
        # Plan mappings document any further assertions made by the publisher.
        # They can strengthen this fixed contract but never override it.
        for mapping in (board["manifest_hooks"], board["manifest_observation"]):
            for key, value in mapping.items():
                # SID8 is not an AODV identity.  A publisher may report the
                # inventory diagnostic, but it cannot turn an unused SID8
                # collision into an AODV rejection criterion.
                if plan["profile"] == "AODV_ONLY" and key in {
                        "identity.inventory.sid8.unique",
                        "identity.inventory.sid8.nonreserved",
                }:
                    continue
                if manifest.get(key) != value:
                    raise RunError(f"board {role} manifest mismatch {key}")
        checked.append({"role": role, "uid": uid, "adva": adva, "serial_device": board["serial_device"],
                        "artifact": {"manifest": str(manifest_path), "manifest_sha256": artifact["manifest_sha256"],
                                     "elf": str(elf), "elf_sha256": artifact["elf_sha256"],
                                     "hex": str(hex_file), "hex_sha256": artifact["hex_sha256"],
                                     "flash_image": {"format": flash["format"], "path": str(flash_path), "sha256": flash["sha256"]}},
                        "manifest_hooks": dict(sorted(board["manifest_hooks"].items())),
                        "manifest_observation": dict(sorted(board["manifest_observation"].items()))})
    return checked


def ficr_command(pyocd: str, uid: str) -> list[str]:
    return [pyocd, "commander", "--uid", uid, "--target", "nrf52833", "--command", "read32 0x100000a4 8"]


def parse_ficr_words(output: str) -> tuple[int, int]:
    found: dict[int, list[int]] = {}
    for line in output.splitlines():
        lower = line.lower()
        for address in (FICR_DEVICEADDR0, FICR_DEVICEADDR1):
            marker = f"{address:08x}"
            if marker in lower:
                values = [int(item, 16) for item in re.findall(r"(?i)(?:0x)?([0-9a-f]{1,8})", line[lower.index(marker) + 8:])]
                if values:
                    found[address] = values
    first, second = found.get(FICR_DEVICEADDR0, []), found.get(FICR_DEVICEADDR1, [])
    if not first or (len(first) < 2 and not second):
        raise RunError("pyOCD FICR output lacks DEVICEADDR0/1")
    return first[0], first[1] if len(first) > 1 else second[0]


def _checked_run(command: Sequence[str], event: str, events: list[dict[str, Any]],
                 run: Callable[..., subprocess.CompletedProcess[str]]) -> subprocess.CompletedProcess[str]:
    at = utc_now()
    try:
        result = run(list(command), check=False, capture_output=True, text=True)
    except OSError as error:
        events.append({"at": at, "event": event, "command": list(command), "status": "spawn_error", "detail": str(error)})
        raise RunError(f"{event} spawn failed") from error
    events.append({"at": at, "event": event, "command": list(command), "status": "ok" if result.returncode == 0 else "failed", "returncode": result.returncode})
    if result.returncode != 0:
        raise RunError(f"{event} failed exit={result.returncode}")
    return result


def run_ficr_check(board: dict[str, Any], pyocd: str, events: list[dict[str, Any]],
                   run: Callable[..., subprocess.CompletedProcess[str]]) -> None:
    result = _checked_run(ficr_command(pyocd, board["uid"]), f"ficr_{board['role']}", events, run)
    first, second = parse_ficr_words((result.stdout or "") + "\n" + (result.stderr or ""))
    observed = derive_canonical_adva(first, second)
    if observed != board["adva"]:
        raise RunError(f"board {board['role']} FICR AdvA mismatch")


def flash_command(pyocd: str, board: dict[str, Any]) -> list[str]:
    return [pyocd, "load", board["artifact"]["flash_image"]["path"], "--uid", board["uid"], "--target", "nrf52833"]


def halt_command(pyocd: str, uid: str) -> list[str]:
    return [pyocd, "commander", "--uid", uid, "--target", "nrf52833", "--command", "halt"]


def reset_command(pyocd: str, uid: str) -> list[str]:
    return [pyocd, "reset", "--uid", uid, "--target", "nrf52833"]


def grabserial_command(grabserial: str, board: dict[str, Any]) -> list[str]:
    return [grabserial, "-d", board["serial_device"], "-b", "115200", "-T", "-F", "%Y-%m-%dT%H:%M:%S.%fZ"]


def _pin(source: Path, target: Path, expected: str | None = None) -> dict[str, str]:
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copy2(source, target)
    except OSError as error:
        raise RunError(f"cannot pin {source}: {error}") from error
    digest = sha256_file(target)
    if expected is not None and digest != expected:
        raise RunError(f"pinned {source.name} hash changed during startup")
    return {"path": str(target), "sha256": digest}


def pin_inputs(run_dir: Path, inventory: Path, plan: Path, input_hashes: dict[str, str],
               checked: list[dict[str, Any]], pyocd: Path | None, grabserial: Path | None,
               uv: Path | None) -> dict[str, Any]:
    pinned: dict[str, Any] = {
        "inventory": _pin(inventory, run_dir / "inputs" / "inventory.tsv", input_hashes["inventory"]),
        "plan": _pin(plan, run_dir / "inputs" / "plan.json", input_hashes["plan"]),
        "tools": {
            "runner": _pin(Path(__file__), run_dir / "inputs" / "tools" / "run_tavrn_benchmark.py"),
            "analyzer": _pin(_ANALYZER_PATH,
                              run_dir / "inputs" / "tools" / "summarize_tavrn_benchmark.py",
                              _ANALYZER_SHA256),
        },
        "boards": {},
    }
    for label, tool in (("pyocd", pyocd), ("grabserial", grabserial), ("uv", uv)):
        if tool is None:
            pinned["tools"][label] = {"path": "", "status": "not_read"}
        else:
            pinned["tools"][label] = _pin(tool, run_dir / "inputs" / "tools" / label)
    for board in checked:
        role, artifact = board["role"], board["artifact"]
        pinned["boards"][role] = {name: _pin(Path(artifact[name]), run_dir / "inputs" / role / Path(artifact[name]).name, artifact[f"{name}_sha256"])
                                  for name in ("manifest", "elf", "hex")}
        # All later flash commands use the just-verified immutable copy, never
        # a mutable publisher path that could change after preflight.
        for name in ("manifest", "elf", "hex"):
            artifact[name] = pinned["boards"][role][name]["path"]
        artifact["flash_image"]["path"] = artifact[artifact["flash_image"]["format"]]
    return pinned


def _metadata(plan: dict[str, Any], checked: list[dict[str, Any]], pinned: dict[str, Any],
               events: list[dict[str, Any]], status: str, started_utc: str, ended_utc: str) -> dict[str, Any]:
    return {"schema": RUN_SCHEMA, "status": status, "started_utc": started_utc, "ended_utc": ended_utc,
            "source": plan["source"],
            "profile": plan["profile"], "timer_profile": plan["timer_profile"], "candidate_scope": plan["candidate_scope"],
            "observation": plan["observation"], "boards": checked, "pinned": pinned, "events": events}


def pin_observation_store(pinned: dict[str, Any], state: Any) -> None:
    """Retain the normalized SQLite evidence and bind its final hash to metadata."""
    path = getattr(state, "database_path", None)
    if path is None:
        return
    state.flush()
    store_path = Path(path)
    if not store_path.is_file():
        raise RunError(f"observation record store is missing: {store_path}")
    pinned["observation_store"] = {
        "path": str(store_path),
        "sha256": sha256_file(store_path),
        "retention": "retained_run_evidence",
    }


def _tail(child: CaptureChild, state: Any) -> None:
    with child.log_path.open("r", encoding="utf-8") as source:
        source.seek(child.offset)
        chunk = source.read()
        child.offset = source.tell()
    if chunk:
        state.feed(child.board["role"], chunk, str(child.log_path))


def _terminate(children: list[CaptureChild], events: list[dict[str, Any]],
               killpg: Callable[[int, int], None] = os.killpg, timeout: float = 5.0) -> None:
    """Stop each detached capture session, then close all of its log handles."""
    active: list[CaptureChild] = []
    try:
        for child in children:
            if child.process.poll() is not None:
                continue
            # start_new_session=True makes the capture leader's PID its process
            # group ID, including any descendants spawned by grabserial.
            try:
                killpg(child.process.pid, signal.SIGTERM)
            except ProcessLookupError:
                # The child won the poll/kill race.  It is not a second failure
                # and must not get a duplicate terminal event.
                continue
            active.append(child)
            events.append({"at": utc_now(), "event": f"capture_{child.board['role']}",
                           "status": "terminate_requested", "process_group": child.process.pid,
                           "signal": signal.Signals.SIGTERM.name})
        for child in active:
            try:
                returncode = child.process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                try:
                    killpg(child.process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                else:
                    events.append({"at": utc_now(), "event": f"capture_{child.board['role']}",
                                   "status": "kill_requested", "process_group": child.process.pid,
                                   "signal": signal.Signals.SIGKILL.name})
                try:
                    returncode = child.process.wait(timeout=timeout)
                except subprocess.TimeoutExpired as error:
                    raise RunError(f"capture_{child.board['role']} process group did not exit after SIGKILL") from error
            events.append({"at": utc_now(), "event": f"capture_{child.board['role']}",
                           "status": "terminated", "returncode": returncode})
    finally:
        for child in children:
            if not child.handle.closed:
                child.handle.close()


def _unexpected_child_exit(children: list[CaptureChild], events: list[dict[str, Any]],
                           killpg: Callable[[int, int], None] = os.killpg) -> bool:
    failed = False
    for child in children:
        returncode = child.process.poll()
        if returncode is not None:
            events.append({"at": utc_now(), "event": f"capture_{child.board['role']}",
                           "status": "unexpected_exit", "returncode": returncode})
            failed = True
    if failed:
        _terminate(children, events, killpg=killpg)
    return failed


def _write_snapshot(state: Any, output_dir: Path, name: str, charts: bool,
                    chart_renderer: Callable[[Path, str], None] | None) -> dict[str, Any]:
    data = analysis.snapshot(state)
    bundle = output_dir / name
    staged_root: Path | None = None
    try:
        output_dir.mkdir(parents=True, exist_ok=True)
        if bundle.exists():
            raise RunError(f"refuses to overwrite output bundle {bundle}")
        staged_root = Path(tempfile.mkdtemp(prefix=f".{name}.runner.", dir=output_dir))
        staged_bundle = staged_root / name
        # Chart rendering must use the pinned analyzer in a separate subprocess;
        # never use the import-time module's ambient matplotlib installation.
        analysis.write_outputs(data, staged_root, name, charts=False)
        if charts:
            if chart_renderer is None:
                raise RunError("pinned chart renderer is required")
            chart_renderer(staged_bundle, name)
        if bundle.exists():
            raise RunError(f"refuses to overwrite output bundle {bundle}")
        os.replace(staged_bundle, bundle)
        staged_root.rmdir()
        staged_root = None
    except OSError as error:
        raise RunError(f"cannot publish output bundle {bundle}: {error}") from error
    finally:
        if staged_root is not None:
            shutil.rmtree(staged_root, ignore_errors=True)
    return data


def monitor_captures(children: list[CaptureChild], state: Any, output_dir: Path, events: list[dict[str, Any]],
                      snapshot_seconds: float, charts: bool, sleep: Callable[[float], None] = time.sleep,
                      input_ready: Callable[[], bool] | None = None, max_cycles: int | None = None,
                      chart_renderer: Callable[[Path, str], None] | None = None,
                      killpg: Callable[[int, int], None] = os.killpg) -> str:
    """Tail active logs; Ctrl-C is normal, one child exit is a failed run."""
    previous_snapshot = time.monotonic()
    cycles = 0
    provisional_number = 0
    periodic_number = 0
    try:
        while max_cycles is None or cycles < max_cycles:
            cycles += 1
            if _unexpected_child_exit(children, events, killpg=killpg):
                return "failed"
            for child in children:
                _tail(child, state)
            now = time.monotonic()
            requested = input_ready() if input_ready is not None else False
            if input_ready is None and sys.stdin.isatty() and select.select([sys.stdin], [], [], 0)[0]:
                requested = sys.stdin.readline().strip().lower() == "p"
            if requested:
                provisional_number += 1
                name = f"provisional-{provisional_number:04d}"
                _write_snapshot(state, output_dir, name, True, chart_renderer)
                events.append({"at": utc_now(), "event": "provisional_snapshot", "status": "written", "bundle": name})
            if snapshot_seconds > 0 and now - previous_snapshot >= snapshot_seconds:
                periodic_number += 1
                name = f"periodic-{periodic_number:04d}"
                _write_snapshot(state, output_dir, name, charts, chart_renderer)
                events.append({"at": utc_now(), "event": "periodic_snapshot", "status": "written", "bundle": name})
                previous_snapshot = now
            if cycles % 10 == 0:
                status = state.live_status()
                phase = "provisional" if status["provisional"] else "finalized"
                print(f"watch records={status['record_count']} corruption="
                      f"{status['telemetry_corruption_count']} active={','.join(status['active_roles']) or '-'} "
                      f"status={phase}", flush=True)
            sleep(0.2)
    except KeyboardInterrupt:
        if _unexpected_child_exit(children, events, killpg=killpg):
            return "failed"
        _terminate(children, events, killpg=killpg)
        return "completed_by_user"
    return "test_stopped"


def render_pinned_charts(uv: str, analyzer: str, bundle: Path, name: str, events: list[dict[str, Any]],
                         run: Callable[..., subprocess.CompletedProcess[str]]) -> None:
    _checked_run([uv, "run", "--with", "matplotlib==3.11.1", "--no-project", "python", analyzer,
                  "--render-json", str(bundle / f"{name}.json"), "--output-dir", str(bundle)],
                 f"charts_{name}", events, run)


def execute_capture(plan: dict[str, Any], checked: list[dict[str, Any]], pyocd: str, grabserial: str,
                    uv: str, analyzer: str, run_dir: Path, events: list[dict[str, Any]], charts: bool,
                    snapshot_seconds: float,
                    run: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
                    popen: Callable[..., Any] = subprocess.Popen,
                    killpg: Callable[[int, int], None] = os.killpg) -> tuple[str, Any, CaptureEnd]:
    children: list[CaptureChild] = []
    logs = run_dir / "logs"
    state = analysis.ObservationState(
        {"schema": RUN_SCHEMA, "profile": plan["profile"], "observation": plan["observation"],
         "boards": checked}, database_path=run_dir / "observations.sqlite3")
    status = "failed"
    failure: Exception | None = None
    try:
        for board in checked:
            _checked_run(flash_command(pyocd, board), f"flash_{board['role']}", events, run)
            _checked_run(halt_command(pyocd, board["uid"]), f"halt_{board['role']}", events, run)
        logs.mkdir()
        for board in checked:
            log = logs / f"board-{board['role']}.log"
            handle = log.open("w", encoding="utf-8")
            command = grabserial_command(grabserial, board)
            try:
                process = popen(command, stdout=handle, stderr=subprocess.STDOUT, text=True,
                                start_new_session=True)
            except OSError as error:
                handle.close()
                events.append({"at": utc_now(), "event": f"capture_{board['role']}", "command": command,
                               "status": "spawn_error", "detail": str(error)})
                raise RunError(f"capture_{board['role']} spawn failed") from error
            children.append(CaptureChild(board, process, handle, command, log))
            events.append({"at": utc_now(), "event": f"capture_{board['role']}", "command": command, "status": "started", "log": str(log)})
        for board in checked:
            if _unexpected_child_exit(children, events, killpg=killpg):
                status = "failed"
                break
            _checked_run(reset_command(pyocd, board["uid"]), f"run_{board['role']}", events, run)
            if _unexpected_child_exit(children, events, killpg=killpg):
                status = "failed"
                break
        else:
            chart_renderer = lambda bundle, name: render_pinned_charts(uv, analyzer, bundle, name, events, run)
            status = monitor_captures(children, state, run_dir / "outputs", events, snapshot_seconds, charts,
                                      chart_renderer=chart_renderer, killpg=killpg)
    except TerminationRequest as request:
        events.append({"at": utc_now(), "event": "termination_request", "status": "received",
                       "signal": signal.Signals(request.signum).name})
        status = _termination_status(request.signum)
    except (RunError, analysis.CaptureError) as error:
        failure = error
        setattr(error, "observation_state", state)
        raise
    except KeyboardInterrupt:
        if _unexpected_child_exit(children, events, killpg=killpg):
            status = "failed"
        else:
            events.append({"at": utc_now(), "event": "user_interrupt", "status": "received"})
            status = "completed_by_user"
    finally:
        try:
            _terminate(children, events, killpg=killpg)
            for child in children:
                _tail(child, state)
                state.feed(child.board["role"], "", str(child.log_path), final=True)
            end = capture_end_instant()
            state.finalize(end.host_ms)
            for child in children:
                child.board["log"] = {"path": str(child.log_path), "sha256": sha256_file(child.log_path)}
        except (RunError, analysis.CaptureError) as teardown_error:
            setattr(teardown_error, "observation_state", state)
            if failure is None:
                failure = teardown_error
            raise
        finally:
            if failure is not None:
                setattr(failure, "observation_state", state)
                if "end" in locals():
                    setattr(failure, "capture_end", end)
    return status, state, end


def _tool_path(value: str, label: str, require_exists: bool) -> Path:
    path = Path(value)
    if not path.is_absolute():
        raise RunError(f"{label} must be an explicit absolute path")
    if require_exists and (not path.is_file() or not os.access(path, os.X_OK)):
        raise RunError(f"{label} must exist and be executable: {path}")
    return path


def verify_serial_devices(checked: list[dict[str, Any]]) -> None:
    """Prove serial endpoints are distinct, usable character devices before probes."""
    resolved_paths: set[Path] = set()
    raw_paths: set[str] = set()
    for board in checked:
        role = board["role"]
        raw = board["serial_device"]
        if raw in raw_paths:
            raise RunError(f"board {role} has duplicate serial device path")
        raw_paths.add(raw)
        path = Path(raw)
        try:
            resolved = path.resolve(strict=True)
            mode = resolved.stat().st_mode
        except OSError as error:
            raise RunError(f"board {role} serial device is unavailable: {error}") from error
        if not stat.S_ISCHR(mode):
            raise RunError(f"board {role} serial device is not a character device")
        if resolved in resolved_paths:
            raise RunError(f"board {role} serial device resolves to a duplicate device")
        resolved_paths.add(resolved)
        if not os.access(resolved, os.R_OK | os.W_OK):
            raise RunError(f"board {role} serial device is not readable and writable")
        try:
            descriptor = os.open(str(resolved), os.O_RDWR | os.O_NONBLOCK | getattr(os, "O_NOCTTY", 0))
        except OSError as error:
            raise RunError(f"board {role} serial device is not openable read/write: {error}") from error
        else:
            os.close(descriptor)
    if len(resolved_paths) != 6:
        raise RunError("six distinct serial devices are required")


def _write_run_metadata(run_dir: Path, plan: dict[str, Any], checked: list[dict[str, Any]],
                        pinned: dict[str, Any], events: list[dict[str, Any]], status: str,
                        started_utc: str, ended_utc: str | None = None) -> None:
    metadata = _metadata(plan, checked, pinned, events, status, started_utc,
                         utc_now() if ended_utc is None else ended_utc)
    destination = run_dir / "run.json"
    descriptor: int | None = None
    staged: Path | None = None
    try:
        descriptor, staged_name = tempfile.mkstemp(prefix=".run.json.", dir=run_dir, text=True)
        staged = Path(staged_name)
        with os.fdopen(descriptor, "w", encoding="utf-8") as target:
            descriptor = None
            target.write(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
            target.flush()
            os.fsync(target.fileno())
        os.replace(staged, destination)
        staged = None
    except OSError as error:
        raise RunError(f"cannot write run metadata {destination}: {error}") from error
    finally:
        if descriptor is not None:
            os.close(descriptor)
        if staged is not None:
            try:
                staged.unlink()
            except FileNotFoundError:
                pass


def _execute_with_termination_handlers(execution: Callable[[], tuple[str, Any, CaptureEnd]]) -> tuple[str, Any, CaptureEnd]:
    """Run hardware capture with temporary main-thread SIGTERM/SIGHUP handling."""
    if threading.current_thread() is not threading.main_thread():
        return execution()
    previous: list[tuple[int, Any]] = []
    try:
        for signum in (signal.SIGTERM, signal.SIGHUP):
            previous.append((signum, signal.signal(signum, _raise_termination_request)))
        return execution()
    finally:
        for signum, handler in reversed(previous):
            signal.signal(signum, handler)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", required=True); parser.add_argument("--run-plan", required=True)
    parser.add_argument("--run-dir", required=True); parser.add_argument("--pyocd", required=True)
    parser.add_argument("--grabserial", required=True); parser.add_argument("--uv", required=True)
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--dry-run", action="store_true"); parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--snapshot-seconds", type=float, default=0.0); parser.add_argument("--charts", action="store_true")
    args = parser.parse_args(argv)
    run_dir, inventory_path, plan_path = Path(args.run_dir), Path(args.inventory), Path(args.run_plan)
    plan: dict[str, Any] | None = None; checked: list[dict[str, Any]] = []; events: list[dict[str, Any]] = []; pinned: dict[str, Any] = {}
    capture_end: CaptureEnd | None = None
    capture_state: Any | None = None
    started_utc = utc_now()
    try:
        if run_dir.exists():
            raise RunError("run directory already exists; refusing overwrite")
        if args.dry_run and args.preflight_only:
            raise RunError("--dry-run and --preflight-only are mutually exclusive")
        # These must be taken before JSON/TSV parsing, then checked again after
        # copy2 so a mutable publisher cannot swap the accepted input bytes.
        input_hashes = {"inventory": sha256_file(inventory_path), "plan": sha256_file(plan_path)}
        plan = load_plan(plan_path)
        checked = preflight(plan, parse_inventory(inventory_path), input_hashes["inventory"])
        if not args.dry_run and not args.preflight_only and not args.execute:
            raise RunError("refusing hardware action without --execute (or --dry-run/--preflight-only)")
        pyocd = _tool_path(args.pyocd, "--pyocd", not args.dry_run)
        grabserial = _tool_path(args.grabserial, "--grabserial", not args.dry_run)
        uv = _tool_path(args.uv, "--uv", not args.dry_run)
        run_dir.mkdir(parents=True)
        pinned = pin_inputs(run_dir, inventory_path, plan_path, input_hashes, checked,
                            pyocd if pyocd.is_file() else None,
                            grabserial if grabserial.is_file() else None,
                            uv if uv.is_file() else None)
        if args.dry_run:
            dry_pyocd = pinned["tools"]["pyocd"].get("path") or str(pyocd)
            dry_grabserial = pinned["tools"]["grabserial"].get("path") or str(grabserial)
            for board in checked:
                events.extend({"at": utc_now(), "event": f"{event}_{board['role']}", "command": command, "status": "not_run"}
                              for event, command in (("ficr", ficr_command(dry_pyocd, board["uid"])), ("flash", flash_command(dry_pyocd, board)), ("halt", halt_command(dry_pyocd, board["uid"])), ("capture", grabserial_command(dry_grabserial, board)), ("run", reset_command(dry_pyocd, board["uid"]))))
            if args.charts:
                dry_uv = pinned["tools"]["uv"].get("path") or str(uv)
                events.append({"at": utc_now(), "event": "charts_final", "status": "not_run", "command": [
                    dry_uv, "run", "--with", "matplotlib==3.11.1", "--no-project", "python",
                    pinned["tools"]["analyzer"]["path"], "--render-json",
                    str(run_dir / "outputs" / "final" / "final.json"), "--output-dir",
                    str(run_dir / "outputs" / "final")]})
            _write_run_metadata(run_dir, plan, checked, pinned, events, "dry_run", started_utc)
            return 0
        verify_serial_devices(checked)
        pinned_pyocd = pinned["tools"]["pyocd"]["path"]
        pinned_grabserial = pinned["tools"]["grabserial"]["path"]
        pinned_uv = pinned["tools"]["uv"]["path"]
        pinned_analyzer = pinned["tools"]["analyzer"]["path"]
        for board in checked:
            run_ficr_check(board, pinned_pyocd, events, subprocess.run)
        if args.preflight_only:
            _write_run_metadata(run_dir, plan, checked, pinned, events, "preflight_completed", started_utc)
            return 0
        status, capture_state, capture_end = _execute_with_termination_handlers(
            lambda: execute_capture(plan, checked, pinned_pyocd, pinned_grabserial, pinned_uv,
                                     pinned_analyzer, run_dir, events, args.charts, args.snapshot_seconds))
        chart_renderer = lambda bundle, name: render_pinned_charts(pinned_uv, pinned_analyzer, bundle, name,
                                                                      events, subprocess.run)
        final = _write_snapshot(capture_state, run_dir / "outputs", "final", args.charts, chart_renderer)
        if status in {"completed_by_user", "test_stopped"} and final["proving_status"] != "VALID":
            status = ("failed_invalid_evidence" if final["proving_status"] == "INVALID"
                      else "failed_incomplete_evidence")
        pin_observation_store(pinned, capture_state)
        _write_run_metadata(run_dir, plan, checked, pinned, events, status, started_utc, capture_end.utc)
        return 0 if status in {"completed_by_user", "test_stopped"} and final["proving_status"] == "VALID" else 1
    except (RunError, analysis.CaptureError) as error:
        error_end = capture_end or getattr(error, "capture_end", None)
        failed_state = capture_state or getattr(error, "observation_state", None)
        if failed_state is not None:
            capture_state = failed_state
        if plan is not None and run_dir.exists():
            if failed_state is not None:
                pin_observation_store(pinned, failed_state)
            _write_run_metadata(run_dir, plan, checked, pinned, events, "failed", started_utc,
                                error_end.utc if isinstance(error_end, CaptureEnd) else None)
        print(f"FAIL tavrn_observation_run: {error}", file=sys.stderr)
        return 1
    except TerminationRequest as request:
        status = _termination_status(request.signum)
        events.append({"at": utc_now(), "event": "termination_request", "status": "received",
                       "signal": signal.Signals(request.signum).name})
        if plan is not None and run_dir.exists():
            _write_run_metadata(run_dir, plan, checked, pinned, events, status, started_utc,
                                capture_end.utc if capture_end is not None else None)
        print(f"FAIL tavrn_observation_run: {status}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        if plan is not None and run_dir.exists():
            _write_run_metadata(run_dir, plan, checked, pinned, events, "interrupted", started_utc,
                                capture_end.utc if capture_end is not None else None)
        print("FAIL tavrn_observation_run: interrupted", file=sys.stderr)
        return 1
    finally:
        if capture_state is not None:
            capture_state.close()


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
