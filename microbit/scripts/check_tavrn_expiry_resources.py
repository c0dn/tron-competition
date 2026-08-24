#!/usr/bin/env python3
"""Fail-closed resource evidence checker for the future TAVRN expiry slice.

This program deliberately has no firmware policy decisions.  It checks the
evidence emitted by the build and the frozen, versioned JSON records used by
the resource gate.  ``--expect-fail`` still exits non-zero: it proves that the
fixture reaches its named failure class rather than turning arbitrary command
or parse errors into a passing test.
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import re
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


SCHEMA = "tron.tavrn.expiry.resources.v1"
EDGE_SCHEMA = "tron.tavrn.expiry.required-stack-edges.v2"
INDIRECT_EDGE_SCHEMA = "tron.tavrn.expiry.required-stack-edges.v3"
APPLICATION_SIZE_SCHEMA = "tron.mind.application.resource-sizes.v1"
D2_PAIRED_ACCEPTANCE_SCHEMA = "tron.tavrn.d2.application-incremental.v1"
WORKTREE_CONTENT_DIGEST_SCOPE = "git-diff-head-binary-plus-relevant-untracked-v1"
INACTIVE_INDIRECT_TARGET = "__inactive__"
CAPTURE_SCHEMA = "tron.tavrn.expiry.hardware.capture.v1"
CAPTURE_PROVENANCE_KEYS = {
    "artifact_manifest_name", "artifact_manifest_sha256", "artifact_elf_sha256",
    "feature", "timer", "hooks", "expiry_full_table",
}
CAPTURE_BUILD_CONFIG_DIFFERENCES = {
    "build.stack_usage", "hook.enabled", "hook.expiry_full_table",
}
CAPTURE_REQUIRED_BUILD_CONFIG_DIFFERENCES = {
    "hook.enabled", "hook.expiry_full_table",
}
STACK_LIMIT = 4096
STACK_HEADROOM = 1024
RAM_MINIMUM = 8192
FIXED_DELTA_MAXIMUM = 512
REQUIRED_ROUTED_MESH_TASK_STACK_BYTES = 4864
REQUIRED_ROUTED_INITIAL_TASK_STACK_BYTES = 4096
REQUIRED_ROUTED_RUNTIME_RAM_RESERVE_BYTES = 13360
REQUIRED_ROUTED_LOGGER_TASK_STACK_BYTES = 1840
MIND_AUXILIARY_TASK_STACK_BYTES = 512
MIND_AUXILIARY_TASK_HEADROOM = 128
MIND_TASK_SYSTEM_STACK_BYTES = 128
MIND_TASK_STACK_ALIGNMENT_BYTES = 8
APPLICATION_FIXED_STATE_LINKER_PLACEMENT_BYTES = 14

MIND_APPLICATION_SOURCES = frozenset({
    "app/mind_application/mind_application_wire.c",
    "app/mind_application/mind_application_ingress.c",
    "app/mind_application/mind_event_forwarder.c",
    "app/mind_application/mind_topology_adapter.c",
    "app/mind_application/mind_root_plane.c",
    "app/mind_application/mind_root_coordinator.c",
    "app/mind_application/mind_root_inbox.c",
    "app/mind_application/mind_command.c",
    "app/mind_application/mind_log.c",
    "app/mind_application/mind_log_formatter.c",
    "app/mind_application/mind_gtt_response.c",
    "app/mind_application/mind_uart.c",
    "app/mind_application/mind_audio.c",
    "app/mind_application/mind_ui.c",
})
D2_APPLICATION_SOURCE_ADDITIONS = MIND_APPLICATION_SOURCES | {
    "app/drivers/display.c",
}
MIND_APPLICATION_CAPACITIES = {
    "root": 16,
    "campaign": 16,
    "ack": 16,
    "event": 16,
    "ingress_seen": 16,
    "ingress_queue": 8,
    "final_inbox": 8,
    "logger": 8,
    "uart_rx_ring": 32,
    "command_mailbox": 8,
}
MIND_APPLICATION_PRODUCTION_COMPONENTS = {
    "routed_mind_ingress": "ingress",
    "routed_mind_root_coordinator": "root_coordinator",
    "routed_mind_root_inbox": "root_inbox",
    "routed_mind_uart": "uart",
    "routed_mind_ui": "ui",
    "routed_mind_log_queue": "log_queue",
    "routed_mind_logged_record": "log_record",
    "routed_mind_pending_request": "root_coordinator_request",
    "routed_mind_last_submit_status": "aodv_status",
    "routed_mind_phase5_first_invalid": "phase5_provenance",
    "routed_mind_gtt_response": "gtt_response",
    "routed_logger_dispatch_epoch": "logger_dispatch_epoch",
    "routed_logger_progress_wake_armed": "logger_progress_wake_armed",
    "routed_cycle_fault_logged": "cycle_fault_logged",
    "mind_uart_active": "pointer",
    "mind_audio_sample": "audio_sample",
    "display_framebuffer": "display_framebuffer",
}
MIND_APPLICATION_STATIC_STACK_COMPONENTS = {
    "mind_uart_task_stack": "TRON_BUILD_MIND_UART_TASK_STATIC_BUFFER_BYTES",
    "mind_ui_task_stack": "TRON_BUILD_MIND_UI_TASK_STATIC_BUFFER_BYTES",
    "display_task_stack": "TRON_BUILD_MIND_DISPLAY_TASK_STATIC_BUFFER_BYTES",
}
MIND_APPLICATION_TASK_SOURCES = {
    "mind_uart_task_stack": (
        "app/mind_application/mind_uart.c", "mind_uart_start_task",
        "TRON_BUILD_MIND_UART_TASK_STACK_BYTES"),
    "mind_ui_task_stack": (
        "app/mind_application/mind_ui.c", "mind_ui_start",
        "TRON_BUILD_MIND_UI_TASK_STACK_BYTES"),
    "display_task_stack": (
        "app/drivers/display.c", "display_init_with_priority",
        "TRON_BUILD_MIND_DISPLAY_TASK_STACK_BYTES"),
}

# The supplied manifest selects the measured path but is not allowed to weaken
# the routed FULL root contract.  These edges cover both routed-cycle callbacks
# and the maintenance binding's targeted owner branch.
REQUIRED_ROOTED_STACK_EDGES = frozenset({
    ("routed_mesh_task", "routed_cycle_run_task", "call", "always"),
    ("routed_cycle_run_task", "routed_cycle_run_once", "call", "always"),
    ("routed_cycle_run_once", "routed_cycle_operations.router_scheduler_event",
     "indirect", "always"),
    ("routed_cycle_run_once", "routed_cycle_operations.router_tick", "indirect", "always"),
    ("routed_cycle_router_tick", "tavrn_full_maintenance_binding_tick", "call", "binding"),
    ("tavrn_full_maintenance_binding_tick", "tavrn_maintenance_targeted_owner_tick",
     "call", "binding"),
})

REQUIRED_LOGGER_STACK_EDGES = frozenset({
    ("routed_logger_task", "log_benchmark_control", "call", "always"),
    ("log_benchmark_control", "tm_printf", "call", "always"),
})

RESOURCE_KEYS = {
    "schema", "name", "target", "feature", "timer", "ram", "fixed_state",
    "stack", "heap", "binding", "capture", "provenance", "expected_failure",
    "preprocessed_main",
}


class CheckFailure(Exception):
    def __init__(self, code: str, message: str) -> None:
        self.code = code
        super().__init__(message)


@dataclass(frozen=True)
class MapUsage:
    data_bytes: int
    bss_bytes: int
    allocated_bytes: int
    unallocated_bytes: int


@dataclass(frozen=True)
class StackFrame:
    identity: str
    bare_name: str
    frame_bytes: int


@dataclass(frozen=True)
class IndirectCallSite:
    """One register-indirect BLX instruction in the production disassembly."""

    caller: str
    ordinal: int
    address: int
    register: str


@dataclass(frozen=True)
class IndirectCallBinding:
    """A source-derived target for one indirect call in source order."""

    caller: str
    target: str | None


@dataclass(frozen=True)
class RoutedRuntimeDeclaration:
    initial_task_stack_bytes: int
    runtime_ram_reserve_bytes: int
    logger_task_stack_bytes: int


@dataclass(frozen=True)
class StackReport:
    root: str
    stack_bytes: int
    chain: tuple[StackFrame, ...]
    total_bytes: int
    headroom_bytes: int


@dataclass(frozen=True)
class RuntimeRamReport:
    map_unallocated_ram_bytes: int
    runtime_ram_reserve_bytes: int
    post_reserve_ram_bytes: int


@dataclass(frozen=True)
class InitialTaskStackReport:
    initial_task_stack_bytes: int
    static_frame_bytes: int
    logical_headroom_bytes: int


@dataclass(frozen=True)
class ApplicationFixedStateReport:
    baseline_allocated_bytes: int
    production_allocated_bytes: int
    measured_delta_bytes: int
    declared_delta_bytes: int
    aggregate_new_static_bytes: int
    replaced_static_bytes: int
    linker_placement_bytes: int


@dataclass(frozen=True)
class BaselineEvidence:
    baseline_of_record: str
    before_source_inventory_sha256: str
    after_source_inventory_sha256: str
    source_inventory_drift: str


@dataclass(frozen=True)
class RunResult:
    verdict: str
    baseline: BaselineEvidence | None
    stack_chain: str | None
    runtime_ram: RuntimeRamReport | None
    initial_task_stack: InitialTaskStackReport | None
    logger_stack: StackReport | None
    application_stacks: tuple[StackReport, ...]
    application_fixed_state: ApplicationFixedStateReport | None


class StackFrames:
    """Stack-use records retaining source identity and conservative bare lookup."""

    def __init__(self) -> None:
        self._by_identity: dict[str, StackFrame] = {}
        self._by_bare: dict[str, list[StackFrame]] = {}
        self._declared: dict[str, StackFrame] = {}
        self._used_declared: set[str] = set()

    def add(self, source: str, function: str, frame_bytes: int) -> None:
        identity = "%s:%s" % (source, function)
        previous = self._by_identity.get(identity)
        # GCC may emit several records for one identity after IPA.  Never select
        # the smaller frame from that set.
        if previous is None or frame_bytes > previous.frame_bytes:
            self._by_identity[identity] = StackFrame(identity, function, frame_bytes)

    def finish(self) -> None:
        self._by_bare = {}
        for frame in self._by_identity.values():
            self._by_bare.setdefault(frame.bare_name, []).append(frame)

    def declare(self, symbol: str, frame_bytes: int) -> None:
        if symbol in self._declared:
            raise CheckFailure("stack", "duplicate declared .su-less leaf: %s" % symbol)
        self._declared[symbol] = StackFrame("declared:" + symbol, symbol, frame_bytes)

    def is_declared(self, symbol: str) -> bool:
        return symbol in self._declared

    def require_declared_coverage(self) -> None:
        unused = sorted(set(self._declared) - self._used_declared)
        if unused:
            raise CheckFailure("stack", "declared .su-less leaves are not active: %s" %
                               ",".join(unused))

    def resolve(self, symbol: str) -> StackFrame:
        # A cloned symbol is a distinct active callee.  It must have its own .su
        # record; falling back to its unsuffixed parent would undercount it.
        if "." in symbol:
            candidates = [frame for frame in self._by_identity.values()
                          if frame.identity.endswith(":" + symbol)]
            if len(candidates) != 1:
                raise CheckFailure("stack", "active cloned callee lacks unique .su evidence: %s" % symbol)
            return candidates[0]
        candidates = self._by_bare.get(symbol, [])
        if not candidates:
            declared = self._declared.get(symbol)
            if declared is not None:
                self._used_declared.add(symbol)
                return declared
            raise CheckFailure("stack", "active callee lacks .su evidence: %s" % symbol)
        if len(candidates) == 1:
            return candidates[0]
        # The disassembler does not carry source-file identity for a local
        # static symbol.  Conservatively charge the largest observed frame.
        maximum = max(frame.frame_bytes for frame in candidates)
        return StackFrame("%s:<ambiguous-max>" % symbol, symbol, maximum)

    def resolve_source_function(self, source: Path, function: str) -> StackFrame:
        """Resolve one exact source/function frame, never a same-named fallback."""
        source_resolved = source.resolve()
        candidates: list[StackFrame] = []
        for frame in self._by_identity.values():
            frame_source, separator, frame_function = frame.identity.rpartition(":")
            if not separator or frame_function != function:
                continue
            try:
                if Path(frame_source).resolve() == source_resolved:
                    candidates.append(frame)
            except OSError:
                continue
        if len(candidates) != 1:
            raise CheckFailure(
                "stack", "production %s:%s has %d .su frames; expected exactly one" %
                (source, function, len(candidates)))
        return candidates[0]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checker_source_root() -> Path:
    """Return the checked-in microbit source root containing this checker."""
    return Path(__file__).resolve().parents[1]


def git_toplevel() -> Path:
    """Discover, and contain ourselves within, the active Git worktree."""
    anchor = checker_source_root()
    try:
        completed = subprocess.run(
            ["git", "-C", str(anchor), "rev-parse", "--show-toplevel"],
            capture_output=True, text=True, check=False)
    except OSError as error:
        raise CheckFailure("provenance", "cannot discover Git worktree root: %s" % error)
    if completed.returncode != 0:
        raise CheckFailure("provenance", "checker source is not inside a Git worktree: " +
                           completed.stderr.strip())
    try:
        root = Path(completed.stdout.strip()).resolve(strict=True)
        anchor.relative_to(root)
    except (OSError, ValueError) as error:
        raise CheckFailure("provenance", "discovered Git root does not contain checker source: %s" % error)
    return root


def worktree_content_digest() -> str:
    """Digest dirty tracked content plus relevant untracked build inputs.

    ``HEAD^{tree}`` identifies only committed content and ``source.dirty=yes``
    carries no content identity.  Preserve the binary Git patch for every
    tracked worktree/index delta and add the bytes of untracked source,
    configuration, and test inputs.  Build/output/temp trees are deliberately
    excluded so publication does not hash its own output.
    """
    try:
        repo_root = git_toplevel()
        tracked_result = subprocess.run(
            ["git", "-C", str(repo_root), "diff", "--no-ext-diff", "--binary",
             "--full-index", "HEAD", "--"], capture_output=True, check=False)
        untracked = subprocess.run(
            ["git", "-C", str(repo_root), "ls-files", "--others", "--exclude-standard", "-z"],
            capture_output=True, check=False)
    except OSError as error:
        raise CheckFailure("provenance", "cannot inspect worktree content: %s" % error)
    if tracked_result.returncode != 0:
        raise CheckFailure("provenance", "cannot inspect tracked worktree content: " +
                           tracked_result.stderr.decode("utf-8", errors="replace").strip())
    if untracked.returncode != 0:
        raise CheckFailure("provenance", "cannot list untracked worktree inputs: " +
                           untracked.stderr.decode("utf-8", errors="replace").strip())
    digest = hashlib.sha256()
    tracked = tracked_result.stdout

    def add_record(kind: bytes, value: bytes) -> None:
        digest.update(len(kind).to_bytes(4, "big"))
        digest.update(kind)
        digest.update(len(value).to_bytes(8, "big"))
        digest.update(value)

    add_record(b"scope", WORKTREE_CONTENT_DIGEST_SCOPE.encode("ascii"))
    add_record(b"tracked", tracked)
    excluded_parts = frozenset({
        ".git", ".opencode", "__pycache__", "artifacts", "build", "cmake-build-debug",
        "cmake-build-release", "node_modules", "out", "output", "temp", "tmp",
    })
    relevant_suffixes = frozenset({
        ".c", ".cc", ".cpp", ".h", ".hpp", ".cmake", ".in", ".json", ".py", ".sh",
        ".txt", ".yaml", ".yml",
    })
    relevant_names = frozenset({"CMakeLists.txt", "Makefile", ".gitignore", ".gitmodules"})
    records: list[tuple[bytes, bytes]] = []
    for raw in untracked.stdout.split(b"\0"):
        if not raw:
            continue
        relative = raw.decode("utf-8", errors="surrogateescape")
        candidate = Path(relative)
        if candidate.is_absolute() or ".." in candidate.parts or not candidate.parts:
            raise CheckFailure("provenance", "untracked worktree input path is invalid")
        if any(part in excluded_parts for part in candidate.parts):
            continue
        if candidate.name not in relevant_names and candidate.suffix.lower() not in relevant_suffixes:
            continue
        absolute = repo_root / candidate
        try:
            if not absolute.is_file():
                continue
            records.append((raw, absolute.read_bytes()))
        except OSError as error:
            raise CheckFailure("provenance", "cannot read untracked worktree input %s: %s" %
                               (relative, error))
    for relative, contents in sorted(records):
        add_record(b"untracked-path", relative)
        add_record(b"untracked-content", contents)
    return digest.hexdigest()


def strict_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key %r" % key)
        result[key] = value
    return result


def canonical_stack_chain(chain: Any, label: str) -> str:
    """Serialize a gate stack chain in the one accepted compact JSON form."""
    if not isinstance(chain, (list, tuple)):
        raise CheckFailure("provenance", "%s must be a JSON array" % label)
    normalized: list[dict[str, Any]] = []
    for index, frame in enumerate(chain):
        if isinstance(frame, StackFrame):
            function = frame.identity
            frame_bytes = frame.frame_bytes
        elif isinstance(frame, dict):
            if set(frame) != {"function", "frame_bytes"}:
                raise CheckFailure("provenance", "%s[%d] has invalid fields" % (label, index))
            function = frame["function"]
            frame_bytes = frame["frame_bytes"]
        else:
            raise CheckFailure("provenance", "%s[%d] must be an object" % (label, index))
        if not isinstance(function, str) or not function:
            raise CheckFailure("provenance", "%s[%d].function must be nonempty" % (label, index))
        if isinstance(frame_bytes, bool) or not isinstance(frame_bytes, int) or frame_bytes <= 0:
            raise CheckFailure("provenance", "%s[%d].frame_bytes must be a positive integer" %
                               (label, index))
        normalized.append({"function": function, "frame_bytes": frame_bytes})
    return json.dumps(normalized, sort_keys=True, separators=(",", ":"))


def canonical_gate_chain_value(path: Path, key: str, label: str) -> str:
    """Require one canonical stack-chain gate line and return its raw value."""
    try:
        values = [line.removeprefix(key + "=") for line in
                  path.read_text(encoding="utf-8").splitlines()
                  if line.startswith(key + "=")]
    except OSError as error:
        raise CheckFailure("provenance", "cannot read %s: %s" % (label, error))
    if len(values) != 1:
        raise CheckFailure("provenance", "%s lacks one canonical %s value" % (label, key))
    try:
        chain = json.loads(values[0], object_pairs_hook=strict_pairs)
    except (json.JSONDecodeError, ValueError) as error:
        raise CheckFailure("provenance", "%s has malformed %s value: %s" %
                           (label, key, error))
    canonical = canonical_stack_chain(chain, "%s %s" % (label, key))
    if values[0] != canonical:
        raise CheckFailure("provenance", "%s has non-canonical %s value" % (label, key))
    return values[0]


def load_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=strict_pairs)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
        raise CheckFailure("fixture_schema", "%s is not strict JSON: %s" % (label, error))
    if not isinstance(value, dict):
        raise CheckFailure("fixture_schema", "%s must contain a JSON object" % label)
    return value


def exact_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    missing = sorted(expected - set(value))
    extra = sorted(set(value) - expected)
    if missing or extra:
        detail: list[str] = []
        if missing:
            detail.append("missing=" + ",".join(missing))
        if extra:
            detail.append("unknown=" + ",".join(extra))
        raise CheckFailure("fixture_schema", "%s has %s" % (label, " ".join(detail)))


def require_object(value: Any, keys: set[str], label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise CheckFailure("fixture_schema", "%s must be an object" % label)
    exact_keys(value, keys, label)
    return value


def require_int(value: Any, label: str, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise CheckFailure("fixture_schema", "%s must be an integer >= %d" % (label, minimum))
    return value


def require_sha(value: Any, label: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
        raise CheckFailure("fixture_schema", "%s must be a lowercase SHA-256" % label)
    return value


def validate_resource_document(document: dict[str, Any], label: str) -> dict[str, Any]:
    exact_keys(document, RESOURCE_KEYS, label)
    if document["schema"] != SCHEMA:
        raise CheckFailure("fixture_schema", "%s has schema %r, expected %s" %
                           (label, document["schema"], SCHEMA))
    if not isinstance(document["name"], str) or not document["name"]:
        raise CheckFailure("fixture_schema", "%s.name must be nonempty" % label)
    if document["target"] != "tavrn_routed_node":
        raise CheckFailure("fixture_schema", "%s.target must be tavrn_routed_node" % label)
    if document["feature"] != "FULL_TAVRN":
        raise CheckFailure("fixture_schema", "%s.feature must be FULL_TAVRN" % label)
    if document["timer"] not in ("FAST_TEST", "BALANCED"):
        raise CheckFailure("fixture_schema", "%s.timer must be FAST_TEST or BALANCED" % label)
    ram = require_object(document["ram"], {"data_bytes", "bss_bytes", "unallocated_bytes"},
                         label + ".ram")
    fixed = require_object(document["fixed_state"],
                           {"before_bytes", "after_bytes", "declared_delta_bytes",
                            "unexplained_delta_bytes"}, label + ".fixed_state")
    stack = require_object(document["stack"],
                           {"root", "chain", "total_bytes", "headroom_bytes"},
                           label + ".stack")
    heap = require_object(document["heap"], {"uses_heap"}, label + ".heap")
    binding = require_object(document["binding"],
                             {"full_symbol_count", "full_call_count", "aodv_symbol_count",
                              "aodv_reference_count"}, label + ".binding")
    # Historical resource-only fixtures intentionally have no capture.  Keep
    # their no-capture representation readable while requiring every supplied
    # capture to carry the complete current telemetry contract.
    raw_capture = document["capture"]
    legacy_capture_keys = {"supplied", "duration_seconds", "complete_passes",
                            "hard_selected_demand_deferred_passes",
                            "max_scheduler_gap_ms", "unavailable_count", "faults",
                            "targeted_controls", "rreq_controls", "tc_expiry_controls"}
    if isinstance(raw_capture, dict) and set(raw_capture) == legacy_capture_keys and \
            raw_capture.get("supplied") is False:
        raw_capture = {**raw_capture, "telemetry_dropped": 0}
        document["capture"] = raw_capture
    elif isinstance(raw_capture, dict) and set(raw_capture) == {
            "supplied", "duration_seconds", "complete_passes",
            "max_scheduler_gap_ms", "faults", "targeted_controls",
            "rreq_controls", "tc_expiry_controls"} and raw_capture.get("supplied") is False:
        raw_capture = {**raw_capture, "hard_selected_demand_deferred_passes": 0,
                       "unavailable_count": 0, "telemetry_dropped": 0}
        document["capture"] = raw_capture
    capture = require_object(raw_capture,
                              {"supplied", "duration_seconds", "complete_passes",
                                "hard_selected_demand_deferred_passes",
                                "max_scheduler_gap_ms", "unavailable_count", "faults",
                                "targeted_controls", "rreq_controls", "tc_expiry_controls",
                                "telemetry_dropped"},
                             label + ".capture")
    provenance = require_object(document["provenance"],
                                {"source_inventory_sha256", "map_sha256", "elf_sha256",
                                 "compile_commands_sha256", "checker_sha256",
                                 "build_script_sha256"}, label + ".provenance")
    main = require_object(document["preprocessed_main"], {"source"},
                          label + ".preprocessed_main")
    for field, value in ram.items():
        require_int(value, label + ".ram." + field)
    for field, value in fixed.items():
        require_int(value, label + ".fixed_state." + field)
    if not isinstance(stack["root"], str) or not stack["root"]:
        raise CheckFailure("fixture_schema", "%s.stack.root must be nonempty" % label)
    if not isinstance(stack["chain"], list):
        raise CheckFailure("fixture_schema", "%s.stack.chain must be a list" % label)
    for index, frame in enumerate(stack["chain"]):
        frame_object = require_object(frame, {"function", "frame_bytes"},
                                      "%s.stack.chain[%d]" % (label, index))
        if not isinstance(frame_object["function"], str) or not frame_object["function"]:
            raise CheckFailure("fixture_schema", "%s.stack.chain[%d].function is invalid" %
                               (label, index))
        require_int(frame_object["frame_bytes"], "%s.stack.chain[%d].frame_bytes" %
                    (label, index))
    require_int(stack["total_bytes"], label + ".stack.total_bytes")
    require_int(stack["headroom_bytes"], label + ".stack.headroom_bytes")
    if not isinstance(heap["uses_heap"], bool):
        raise CheckFailure("fixture_schema", "%s.heap.uses_heap must be boolean" % label)
    for field, value in binding.items():
        require_int(value, label + ".binding." + field)
    if not isinstance(capture["supplied"], bool):
        raise CheckFailure("fixture_schema", "%s.capture.supplied must be boolean" % label)
    for field in ("duration_seconds", "complete_passes",
                   "hard_selected_demand_deferred_passes", "max_scheduler_gap_ms",
                   "unavailable_count", "faults", "targeted_controls", "rreq_controls",
                   "tc_expiry_controls", "telemetry_dropped"):
        require_int(capture[field], label + ".capture." + field)
    for field, value in provenance.items():
        require_sha(value, label + ".provenance." + field)
    if not isinstance(main["source"], str):
        raise CheckFailure("fixture_schema", "%s.preprocessed_main.source must be a string" % label)
    expected = document["expected_failure"]
    if expected is not None and expected not in {
        "stack_total", "ram_unallocated", "fixed_state_delta",
        "hardware_capture", "heap", "binding", "preprocessed_inactive",
        "preprocessed_guard",
    }:
        raise CheckFailure("fixture_schema", "%s.expected_failure is unknown" % label)
    return document


def parse_kv_manifest(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise CheckFailure("provenance", "cannot read manifest %s: %s" % (path, error))
    for line in lines:
        if not line:
            continue
        if "=" not in line:
            raise CheckFailure("provenance", "malformed manifest line in %s" % path)
        key, value = line.split("=", 1)
        if not key or key in result:
            raise CheckFailure("provenance", "duplicate or empty manifest key %r" % key)
        result[key] = value
    return result


def require_manifest_decimal(manifest: dict[str, str], key: str, expected: int) -> int:
    value = manifest.get(key)
    if value is None:
        raise CheckFailure("provenance", "build manifest lacks required declaration: " + key)
    if not re.fullmatch(r"0|[1-9][0-9]*", value):
        raise CheckFailure("provenance", "build manifest declaration is malformed: " + key)
    parsed = int(value, 10)
    if parsed != expected:
        raise CheckFailure("provenance", "build manifest declaration differs: %s=%d, expected %d" %
                           (key, parsed, expected))
    return parsed


def config_uint_macro(config_header: Path, macro: str, label: str,
                      failure_code: str = "provenance") -> int:
    try:
        source = config_header.read_text(encoding="utf-8")
    except OSError as error:
        raise CheckFailure(failure_code, "cannot read generated config header: %s" % error)
    matches = re.findall(r"^#define\s+%s\s+([0-9]+)u?\s*$" % re.escape(macro),
                         source, flags=re.MULTILINE)
    if len(matches) != 1:
        raise CheckFailure(failure_code, "generated config lacks one %s macro" % label)
    return int(matches[0], 10)


def validate_routed_runtime_declarations(
        manifest: dict[str, str], config_header: Path) -> RoutedRuntimeDeclaration:
    """Cross-check the generated routed stack/reserve declaration surface."""
    if manifest.get("build.kind") != "ROUTED" or \
            manifest.get("build.phase1_target") != "ROUTED":
        raise CheckFailure("provenance", "resource evidence is not a routed build manifest")
    if manifest.get("capacity.routed_initial_task_stack_bytes.state") != "IMPLEMENTED":
        raise CheckFailure("provenance", "routed initial-task capacity state is not IMPLEMENTED")
    if manifest.get("capacity.routed_mesh_task_stack_bytes.state") != "IMPLEMENTED":
        raise CheckFailure("provenance", "routed mesh-task capacity state is not IMPLEMENTED")
    if manifest.get("capacity.routed_logger_task_stack_bytes.state") != "IMPLEMENTED":
        raise CheckFailure("provenance", "routed logger-task capacity state is not IMPLEMENTED")
    initial_stack = require_manifest_decimal(
        manifest, "build.initial_task_stack_bytes",
        REQUIRED_ROUTED_INITIAL_TASK_STACK_BYTES)
    mesh_stack = require_manifest_decimal(
        manifest, "build.routed_mesh_task_stack_bytes",
        REQUIRED_ROUTED_MESH_TASK_STACK_BYTES)
    mesh_capacity_stack = require_manifest_decimal(
        manifest, "capacity.routed_mesh_task_stack_bytes",
        REQUIRED_ROUTED_MESH_TASK_STACK_BYTES)
    capacity_stack = require_manifest_decimal(
        manifest, "capacity.routed_initial_task_stack_bytes",
        REQUIRED_ROUTED_INITIAL_TASK_STACK_BYTES)
    reserve = require_manifest_decimal(
        manifest, "resource.runtime_ram_reserve_bytes",
        REQUIRED_ROUTED_RUNTIME_RAM_RESERVE_BYTES)
    logger_stack = require_manifest_decimal(
        manifest, "build.routed_logger_task_stack_bytes",
        REQUIRED_ROUTED_LOGGER_TASK_STACK_BYTES)
    logger_capacity_stack = require_manifest_decimal(
        manifest, "capacity.routed_logger_task_stack_bytes",
        REQUIRED_ROUTED_LOGGER_TASK_STACK_BYTES)
    config_initial_stack = config_uint_macro(
        config_header, "TRON_BUILD_INITIAL_TASK_STACK_BYTES", "initial-task-stack")
    config_mesh_stack = config_uint_macro(
        config_header, "TRON_BUILD_ROUTED_MESH_TASK_STACK_BYTES", "mesh-task-stack")
    config_reserve = config_uint_macro(
        config_header, "TRON_BUILD_RUNTIME_RAM_RESERVE_BYTES", "runtime-ram-reserve")
    config_logger_stack = config_uint_macro(
        config_header, "TRON_BUILD_ROUTED_LOGGER_TASK_STACK_BYTES", "logger-task-stack")
    if config_initial_stack != initial_stack or config_initial_stack != capacity_stack:
        raise CheckFailure("provenance", "generated initial-task stack declarations disagree")
    if config_mesh_stack != mesh_stack or config_mesh_stack != mesh_capacity_stack:
        raise CheckFailure("provenance", "generated mesh-task stack declarations disagree")
    if config_reserve != reserve:
        raise CheckFailure("provenance", "generated runtime RAM reserve declarations disagree")
    if config_logger_stack != logger_stack or config_logger_stack != logger_capacity_stack:
        raise CheckFailure("provenance", "generated logger-task stack declarations disagree")
    return RoutedRuntimeDeclaration(initial_stack, reserve, logger_stack)


def manifest_application_ingress_enabled(manifest: dict[str, str]) -> bool:
    requested = manifest.get("application.wearable_ingress.requested")
    effective = manifest.get("application.wearable_ingress.effective")
    if requested not in ("ON", "OFF") or effective not in ("ON", "OFF"):
        raise CheckFailure("provenance", "build manifest lacks valid wearable-ingress state")
    if requested != effective:
        raise CheckFailure("provenance", "wearable-ingress requested/effective states differ")
    return effective == "ON"


def selected_source_paths(path: Path) -> list[str]:
    try:
        paths = [line.strip() for line in path.read_text(encoding="utf-8").splitlines()
                 if line.strip()]
    except OSError as error:
        raise CheckFailure("provenance", "cannot read selected sources: %s" % error)
    if not paths or len(paths) != len(set(paths)):
        raise CheckFailure("provenance", "selected sources must be nonempty and unique")
    return paths


def c_function_bodies(source: str) -> dict[str, str]:
    """Return simple C function bodies for the checked production sources."""
    header = re.compile(
        r"(?:^|\n)\s*(?:static\s+)?[A-Za-z_][A-Za-z0-9_\s\*]*?\s+"
        r"([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*\{", re.MULTILINE)
    bodies: dict[str, str] = {}
    for match in header.finditer(source):
        depth = 1
        index = match.end()
        while index < len(source) and depth:
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
            index += 1
        if depth:
            raise CheckFailure("provenance", "production C function has unbalanced braces")
        name = match.group(1)
        # Host-test fallbacks intentionally repeat a task starter under #else;
        # callers that inspect source select the first production definition.
        bodies.setdefault(name, source[match.end():index - 1])
    if not bodies:
        raise CheckFailure("provenance", "production C source has no parseable functions")
    return bodies


def require_static_mind_task_sources() -> None:
    """Prove each newly started application task supplies its own TA_USERBUF."""
    repo_root = Path(__file__).resolve().parents[1]
    for symbol, (relative, starter, user_stack_macro) in MIND_APPLICATION_TASK_SOURCES.items():
        source_path = repo_root / relative
        try:
            source = source_path.read_text(encoding="utf-8")
        except OSError as error:
            raise CheckFailure("provenance", "cannot read application task source %s: %s" %
                               (relative, error))
        body = c_function_bodies(source).get(starter)
        if body is None or source.count("tk_cre_tsk(") != 1 or "knl_Imalloc" in source:
            raise CheckFailure("heap", "application task source has an untracked task allocator: " + relative)
        static_macro = MIND_APPLICATION_STATIC_STACK_COMPONENTS[symbol]
        declaration = re.compile(
            r"static\s+(?:uint8_t|UB)\s+%s\s*\[\s*%s\s*\]" %
            (re.escape(symbol), re.escape(static_macro)))
        field_prefix = r"(?:\b(?:task|ctsk)\.|\.)"
        if "TA_USERBUF" not in body or \
                not re.search(field_prefix + r"bufptr\s*=\s*%s\s*[;,]" %
                              re.escape(symbol), body) or \
                not re.search(field_prefix + r"stksz\s*=\s*%s\s*[;,]" %
                              re.escape(user_stack_macro), body) or \
                declaration.search(source) is None:
            raise CheckFailure("heap", "application task lacks a generated static TA_USERBUF: " + relative)


def require_static_task_compile_definitions(commands_path: Path) -> None:
    repo_root = Path(__file__).resolve().parents[1]
    display_source = repo_root / "app/drivers/display.c"
    arguments, _ = parse_compile_command(commands_path, display_source, "tavrn_routed_node")
    definitions = [argument for argument in arguments
                   if argument.startswith("-DTRON_MIND_STATIC_TASK_BUFFERS")]
    if definitions != ["-DTRON_MIND_STATIC_TASK_BUFFERS=1"]:
        raise CheckFailure("heap", "production display task did not compile its static TA_USERBUF path")


def source_indirect_callback_calls(source_path: Path,
                                   expression: str) -> list[tuple[str, str]]:
    """Return field calls in source order, retaining their invocation function."""
    try:
        source = source_path.read_text(encoding="utf-8")
    except OSError as error:
        raise CheckFailure("provenance", "cannot read indirect callback source %s: %s" %
                           (source_path, error))
    calls: list[tuple[str, str]] = []
    for function, body in c_function_bodies(source).items():
        for field in re.findall(expression + r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", body):
            calls.append((function, field))
    return calls


def production_assignment_targets(preprocessed: str, instance: str) -> dict[str, str]:
    assignments: dict[str, list[str]] = {}
    for field, target in re.findall(
            r"\b%s\s*\.\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
            r"([A-Za-z_][A-Za-z0-9_]*)\s*;" % re.escape(instance), preprocessed):
        assignments.setdefault(field, []).append(target)
    targets: dict[str, str] = {}
    for field, values in assignments.items():
        if field == "context":
            continue
        if len(values) != 1 or values[0] == "NULL":
            raise CheckFailure("stack", "production callback assignment is unresolved or ambiguous: " +
                               instance + "." + field)
        targets[field] = values[0]
    return targets


def required_assignment_target(assignments: dict[str, str], label: str, field: str) -> str:
    target = assignments.get(field)
    if target is None:
        raise CheckFailure("stack", "production callback assignment is missing: " + label + "." + field)
    return target


def nested_callback_bindings(coordinator_source: Path,
                             assignments: dict[str, str]) -> list[IndirectCallBinding]:
    """Follow the two coordinator callbacks that are invoked by forwarder code.

    The root coordinator passes a stored operation to the forwarder, but the
    forwarder, not the coordinator, executes the indirect instruction.  Keep
    that nesting in the graph rather than assigning the callback to its owner.
    """
    try:
        source = coordinator_source.read_text(encoding="utf-8")
    except OSError as error:
        raise CheckFailure("provenance", "cannot read nested callback source %s: %s" %
                           (coordinator_source, error))
    body = c_function_bodies(source).get("mind_root_coordinator_prepare")
    if body is None:
        raise CheckFailure("stack", "production coordinator has no prepare invocation body")
    transfers = (
        ("mind_event_forwarder_consume_ingress", "ingress_take", "ingress_take", 2),
        ("mind_event_forwarder_service_local", "final_publish_local", "publish_local", 1),
    )
    bindings: list[IndirectCallBinding] = []
    for callee, field, parameter, expected_calls in transfers:
        pattern = (r"\b%s\s*\(\s*&\s*coordinator\s*->\s*events\s*,"
                   r"\s*coordinator\s*->\s*operations\s*\.\s*context\s*,"
                   r"\s*coordinator\s*->\s*operations\s*\.\s*%s\s*," %
                   (re.escape(callee), re.escape(field)))
        if len(re.findall(pattern, body)) != 1:
            raise CheckFailure("stack", "production nested callback transfer is missing or ambiguous: " +
                               callee + "." + field)
        forwarder_source = coordinator_source.parent / "mind_event_forwarder.c"
        forwarder_body = c_function_bodies(forwarder_source.read_text(encoding="utf-8")).get(callee)
        if forwarder_body is None:
            raise CheckFailure("stack", "production nested callback callee is missing: " + callee)
        invocation_count = len(re.findall(r"\b%s\s*\(" % re.escape(parameter), forwarder_body))
        if invocation_count != expected_calls:
            raise CheckFailure("stack", "production nested callback invocation count differs: " +
                               callee + "." + field)
        bindings.extend(IndirectCallBinding(callee,
                                            required_assignment_target(
                                                assignments,
                                                "mind_root_coordinator_operations", field))
                        for _ in range(invocation_count))
    return bindings


def production_indirect_callback_bindings(preprocessed_main: Path,
                                          include_mind: bool = True) -> tuple[
        dict[str, str], tuple[IndirectCallBinding, ...]]:
    """Derive actual routed and MIND callback invocation edges from production.

    This intentionally binds only call expressions, never mere field reads.
    Nested forwarder callbacks receive their edges at the forwarder function.
    Other inherited callback seams require versioned edge contracts.
    """
    try:
        source = preprocessed_main.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise CheckFailure("stack", "cannot read preprocessed production main: %s" % error)
    repo_root = Path(__file__).resolve().parents[1]
    groups: tuple[tuple[str, str, Path, str], ...] = (
        ("routed_cycle_operations", "routed_cycle_operations",
         repo_root / "app/tavrn_routed_node/src/routed_cycle.c", r"operations\s*->\s*"),
    )
    if include_mind:
        groups += (("mind_root_coordinator_operations", "operations",
                    repo_root / "app/mind_application/mind_root_coordinator.c",
                    r"coordinator\s*->\s*operations\s*\.\s*"),)
    bindings: dict[str, str] = {}
    call_bindings: list[IndirectCallBinding] = []
    assignment_sets: dict[str, dict[str, str]] = {}
    for label, instance, source_path, invocation_expression in groups:
        calls = source_indirect_callback_calls(source_path, invocation_expression)
        if not calls:
            raise CheckFailure("stack", "production callback source has no indirect calls: " + label)
        assignments = production_assignment_targets(source, instance)
        assignment_sets[label] = assignments
        for caller, field in calls:
            if field == "context":
                raise CheckFailure("stack", "production context pointer is used as a callback: " + label)
            target = assignments.get(field)
            key = label + "." + field
            previous = bindings.get(key)
            if target is not None and previous is not None and previous != target:
                raise CheckFailure("stack", "production indirect callback assignment differs: " + key)
            if target is not None:
                bindings[key] = target
            call_bindings.append(IndirectCallBinding(caller, target))
    if include_mind:
        coordinator_source = repo_root / "app/mind_application/mind_root_coordinator.c"
        call_bindings.extend(nested_callback_bindings(
            coordinator_source, assignment_sets["mind_root_coordinator_operations"]))
    return bindings, tuple(call_bindings)


def bind_indirect_callback_graph(graph: dict[str, set[str]], bindings: dict[str, str],
                                 caller_origins: dict[str, tuple[str, ...]]) -> dict[str, set[str]]:
    active_graph = {node: set(children) for node, children in graph.items()}
    if set(bindings) != set(caller_origins):
        raise CheckFailure("stack", "production indirect callback bindings lack invocation origins")
    for key, target in bindings.items():
        callers = caller_origins[key]
        if not callers:
            raise CheckFailure("stack", "production indirect callback is unreachable: " + key)
        for caller in callers:
            active_graph.setdefault(caller, set()).add(target)
    return active_graph


def production_indirect_stack_graph(graph: dict[str, set[str]], preprocessed_main: Path) -> tuple[
        dict[str, set[str]], dict[str, str]]:
    bindings, _ = production_indirect_callback_bindings(preprocessed_main)
    # Kept only for fixture compatibility.  Production stack analysis resolves
    # every BLX callsite through apply_indirect_callsite_mappings() below.
    return {node: set(children) for node, children in graph.items()}, bindings


def validate_mind_application_profile(manifest: dict[str, str], config_header: Path,
                                      args: argparse.Namespace) -> dict[str, Any] | None:
    if not manifest_application_ingress_enabled(manifest):
        if args.application_size_report:
            raise CheckFailure("provenance", "non-application build supplied application sizeof evidence")
        return None
    required_manifest_capacities = {
        "application.root_registry_capacity": 16,
        "application.root_campaign_capacity": 16,
        "application.root_ack_capacity": 16,
        "application.event_capacity": 16,
        "application.ingress_seen_capacity": 16,
        "application.ingress_queue_capacity": 8,
        "application.final_inbox_capacity": 8,
        "application.logger_capacity": 8,
        "application.uart_rx_ring_capacity": 32,
        "application.command_mailbox_capacity": 8,
        "application.uart_task_stack_bytes": MIND_AUXILIARY_TASK_STACK_BYTES,
        "application.ui_task_stack_bytes": MIND_AUXILIARY_TASK_STACK_BYTES,
        "application.display_task_stack_bytes": MIND_AUXILIARY_TASK_STACK_BYTES,
        "application.task_system_stack_bytes": MIND_TASK_SYSTEM_STACK_BYTES,
        "application.task_stack_alignment_bytes": MIND_TASK_STACK_ALIGNMENT_BYTES,
        "application.uart_task_static_buffer_bytes":
            MIND_AUXILIARY_TASK_STACK_BYTES + MIND_TASK_SYSTEM_STACK_BYTES,
        "application.ui_task_static_buffer_bytes":
            MIND_AUXILIARY_TASK_STACK_BYTES + MIND_TASK_SYSTEM_STACK_BYTES,
        "application.display_task_static_buffer_bytes":
            MIND_AUXILIARY_TASK_STACK_BYTES + MIND_TASK_SYSTEM_STACK_BYTES,
    }
    for key, expected in required_manifest_capacities.items():
        require_manifest_decimal(manifest, key, expected)
    required_config_capacities = {
        "TRON_BUILD_MIND_ROOT_CAPACITY": 16,
        "TRON_BUILD_MIND_CAMPAIGN_CAPACITY": 16,
        "TRON_BUILD_MIND_ROOT_ACK_CAPACITY": 16,
        "TRON_BUILD_MIND_EVENT_CAPACITY": 16,
        "TRON_BUILD_MIND_INGRESS_SEEN_CAPACITY": 16,
        "TRON_BUILD_MIND_INGRESS_QUEUE_CAPACITY": 8,
        "TRON_BUILD_MIND_FINAL_INBOX_CAPACITY": 8,
        "TRON_BUILD_MIND_LOG_CAPACITY": 8,
        "TRON_BUILD_MIND_UART_RX_RING_CAPACITY": 32,
        "TRON_BUILD_MIND_COMMAND_MAILBOX_CAPACITY": 8,
        "TRON_BUILD_MIND_UART_TASK_STACK_BYTES": MIND_AUXILIARY_TASK_STACK_BYTES,
        "TRON_BUILD_MIND_UI_TASK_STACK_BYTES": MIND_AUXILIARY_TASK_STACK_BYTES,
        "TRON_BUILD_MIND_DISPLAY_TASK_STACK_BYTES": MIND_AUXILIARY_TASK_STACK_BYTES,
        "TRON_BUILD_MIND_TASK_SYSTEM_STACK_BYTES": MIND_TASK_SYSTEM_STACK_BYTES,
        "TRON_BUILD_MIND_TASK_STACK_ALIGNMENT_BYTES": MIND_TASK_STACK_ALIGNMENT_BYTES,
        "TRON_BUILD_MIND_UART_TASK_STATIC_BUFFER_BYTES":
            MIND_AUXILIARY_TASK_STACK_BYTES + MIND_TASK_SYSTEM_STACK_BYTES,
        "TRON_BUILD_MIND_UI_TASK_STATIC_BUFFER_BYTES":
            MIND_AUXILIARY_TASK_STACK_BYTES + MIND_TASK_SYSTEM_STACK_BYTES,
        "TRON_BUILD_MIND_DISPLAY_TASK_STATIC_BUFFER_BYTES":
            MIND_AUXILIARY_TASK_STACK_BYTES + MIND_TASK_SYSTEM_STACK_BYTES,
    }
    for macro, expected in required_config_capacities.items():
        if config_uint_macro(config_header, macro, macro, "provenance") != expected:
            raise CheckFailure("provenance", "generated application capacity declarations disagree")
    require_static_mind_task_sources()
    require_static_task_compile_definitions(Path(args.compile_commands))
    if not args.compile_commands or not args.full_elf or not args.full_map:
        raise CheckFailure("provenance", "production wearable ingress lacks fresh application artifact evidence")
    if not args.application_size_report:
        raise CheckFailure("provenance", "production wearable ingress lacks application sizeof evidence")
    report_path = Path(args.application_size_report)
    report = validate_application_size_report(
        load_json(report_path, "application sizeof report"), "application sizeof report")
    provenance = report["provenance"]
    expected_provenance = {
        "compile_commands_sha256": Path(args.compile_commands),
        "elf_sha256": Path(args.full_elf),
        "map_sha256": Path(args.full_map),
        "config_header_sha256": config_header,
    }
    for key, path in expected_provenance.items():
        if provenance[key] != sha256_file(path):
            raise CheckFailure("provenance", "application sizeof report has stale %s" % key)
    if args.application_size_source and provenance["probe_source_sha256"] != \
            sha256_file(Path(args.application_size_source)):
        raise CheckFailure("provenance", "application sizeof report probe source differs")
    if not args.selected_sources:
        raise CheckFailure("provenance", "application sizeof report requires selected-source evidence")
    paths = selected_source_paths(Path(args.selected_sources))
    application_sources = {path for path in paths if path.startswith("app/mind_application/")}
    if application_sources != MIND_APPLICATION_SOURCES:
        raise CheckFailure("provenance", "production Layer-7 selected-source closure differs")
    repo_root = Path(__file__).resolve().parents[1]
    heap_call = re.compile(r"\b(?:malloc|calloc|realloc|free|_sbrk|sbrk)\s*\(")
    for relative in sorted(application_sources):
        source = repo_root / relative
        try:
            text = source.read_text(encoding="utf-8")
        except OSError as error:
            raise CheckFailure("provenance", "cannot read application source %s: %s" %
                               (relative, error))
        if heap_call.search(text):
            raise CheckFailure("heap", "application source references heap allocation: " + relative)
    return report


def post_reserve_ram_bytes(map_unallocated_ram_bytes: int,
                           runtime_ram_reserve_bytes: int) -> int:
    if runtime_ram_reserve_bytes > map_unallocated_ram_bytes:
        raise CheckFailure("ram_unallocated", "runtime RAM reserve exceeds map unallocated RAM")
    post_reserve = map_unallocated_ram_bytes - runtime_ram_reserve_bytes
    if post_reserve < RAM_MINIMUM:
        raise CheckFailure("ram_unallocated", "post-reserve RAM is below 8192 bytes")
    return post_reserve


def parse_map(path: Path) -> MapUsage:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        raise CheckFailure("map", "cannot read map %s: %s" % (path, error))
    ram_origin: int | None = None
    ram_length: int | None = None
    data: int | None = None
    bss: int | None = None
    memory_re = re.compile(r"^\s*(?:RAM|ram)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)(?:\s|$)")
    section_re = re.compile(r"^\.(data|bss)\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)(?:\s|$)")
    allocated_re = re.compile(r"^([A-Za-z_.][A-Za-z0-9_.]*)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)(?:\s|$)")
    allocations: list[tuple[int, int]] = []
    for line in lines:
        memory_match = memory_re.match(line)
        if memory_match:
            ram_origin = int(memory_match.group(1), 16)
            ram_length = int(memory_match.group(2), 16)
            continue
        section_match = section_re.match(line)
        if section_match:
            kind, size = section_match.groups()
            if kind == "data" and data is None:
                data = int(size, 16)
            if kind == "bss" and bss is None:
                bss = int(size, 16)
        allocation_match = allocated_re.match(line)
        if allocation_match:
            _, address, size = allocation_match.groups()
            allocations.append((int(address, 16), int(size, 16)))
    if ram_origin is None or ram_length is None or data is None or bss is None:
        raise CheckFailure("map", "GNU map %s lacks RAM/.data/.bss metadata" % path)
    ram_end = ram_origin + ram_length
    intervals = sorted(
        (max(ram_origin, address), min(ram_end, address + size))
        for address, size in allocations if size != 0 and address < ram_end and address + size > ram_origin)
    allocated = 0
    interval_end: int | None = None
    for start, end in intervals:
        if interval_end is None or start >= interval_end:
            allocated += end - start
            interval_end = end
        elif end > interval_end:
            allocated += end - interval_end
            interval_end = end
    if allocated > ram_length:
        raise CheckFailure("map", "GNU map RAM accounting exceeds declared RAM")
    return MapUsage(data, bss, allocated, ram_length - allocated)


def parse_su(paths: Iterable[Path]) -> StackFrames:
    frames = StackFrames()
    seen_files = 0
    for path in paths:
        seen_files += 1
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as error:
            raise CheckFailure("stack", "cannot read stack usage %s: %s" % (path, error))
        for line in lines:
            columns = line.split("\t")
            if len(columns) < 2:
                continue
            try:
                size = int(columns[1], 10)
            except ValueError:
                raise CheckFailure("stack", "invalid stack frame in %s" % path)
            location = columns[0].strip()
            match = re.match(r"^(.*):[0-9]+:[0-9]+:([^:]+)$", location)
            if match is None:
                raise CheckFailure("stack", "invalid .su identity in %s" % path)
            source, function = match.groups()
            if not source or not function:
                raise CheckFailure("stack", "empty stack function in %s" % path)
            frames.add(source, function, size)
    if not seen_files:
        raise CheckFailure("stack", "no .su files matched")
    frames.finish()
    return frames


def disassembly_text(path: Path | None, elf: Path | None) -> str:
    if path is not None:
        try:
            return path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            raise CheckFailure("binding", "cannot read disassembly %s: %s" % (path, error))
    if elf is None:
        raise CheckFailure("binding", "missing ELF/disassembly evidence")
    command = ["arm-none-eabi-objdump", "-d", str(elf)]
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise CheckFailure("binding", "objdump failed: " + completed.stderr.strip())
    return completed.stdout


def nm_symbols(elf: Path) -> list[str]:
    completed = subprocess.run(["arm-none-eabi-nm", "--defined-only", str(elf)],
                               capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise CheckFailure("binding", "nm failed: " + completed.stderr.strip())
    return [line.rsplit(None, 1)[-1] for line in completed.stdout.splitlines()
            if line.split()]


def call_graph(disassembly: str) -> dict[str, set[str]]:
    graph: dict[str, set[str]] = {}
    current: str | None = None
    header = re.compile(r"^\s*[0-9a-fA-F]+\s+<([^>]+)>:")
    target = re.compile(r"^\s*[0-9a-fA-F]+:\s+[0-9a-fA-F ]+\s+(?:bl(?:x|\.w)?|b(?:\.w|\.n)?)\s+[^<\n]*<([^>+]+)(?:\+0x[0-9a-fA-F]+)?>\s*$")
    for line in disassembly.splitlines():
        match = header.match(line)
        if match:
            current = match.group(1)
            graph.setdefault(current, set())
            continue
        if current is not None:
            call = target.search(line)
            if call:
                graph[current].add(call.group(1))
    return graph


def indirect_call_sites(disassembly: str) -> tuple[IndirectCallSite, ...]:
    """List every register-indirect BLX site, retaining stable caller order."""
    sites: list[IndirectCallSite] = []
    ordinals: dict[str, int] = {}
    current: str | None = None
    header = re.compile(r"^\s*[0-9a-fA-F]+\s+<([^>]+)>:")
    indirect = re.compile(
        r"^\s*([0-9a-fA-F]+):\s+[0-9a-fA-F ]+\s+blx\s+"
        r"(r(?:[0-9]|1[0-5]))\s*$")
    for line in disassembly.splitlines():
        match = header.match(line)
        if match:
            current = match.group(1)
            continue
        if current is None:
            continue
        match = indirect.match(line)
        if match is None:
            continue
        ordinal = ordinals.get(current, 0)
        sites.append(IndirectCallSite(current, ordinal, int(match.group(1), 16),
                                      match.group(2)))
        ordinals[current] = ordinal + 1
    return tuple(sites)


def load_stack_contract(path: Path, root: str,
                        require_callsite_complete: bool = False) -> dict[str, Any]:
    document = load_json(path, "required stack edge manifest")
    if document.get("schema") == EDGE_SCHEMA:
        exact_keys(document, {"schema", "root", "edges", "declared_leaves"},
                   "required stack edge manifest")
        document["indirect_calls"] = []
    elif document.get("schema") == INDIRECT_EDGE_SCHEMA:
        exact_keys(document, {"schema", "root", "edges", "indirect_calls", "declared_leaves"},
                   "required stack edge manifest")
    else:
        raise CheckFailure("stack", "invalid required stack edge manifest")
    if require_callsite_complete and document.get("schema") != INDIRECT_EDGE_SCHEMA:
        raise CheckFailure("stack", "production FULL mesh stack evidence requires a v3 callsite contract")
    if document["root"] != root or \
       not isinstance(document["edges"], list) or not document["edges"] or \
        not isinstance(document["indirect_calls"], list) or \
        not isinstance(document["declared_leaves"], list):
        raise CheckFailure("stack", "invalid required stack edge manifest")
    names: set[str] = set()
    for index, edge in enumerate(document["edges"]):
        item = require_object(edge, {"from", "to", "kind", "when"},
                              "required stack edge %d" % index)
        if not isinstance(item["from"], str) or not isinstance(item["to"], str) or \
            item["kind"] not in ("call", "indirect") or item["when"] not in ("always", "binding"):
            raise CheckFailure("stack", "invalid required stack edge")
    indirect_names: set[tuple[str, int]] = set()
    for index, raw_call in enumerate(document["indirect_calls"]):
        call = require_object(raw_call, {"from", "site", "to"},
                              "required indirect stack call %d" % index)
        caller = call["from"]
        target = call["to"]
        site = require_int(call["site"], "required indirect stack call site")
        if not isinstance(caller, str) or not isinstance(target, str) or \
                not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.]*", caller) or \
                not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.]*", target) or \
                (caller, site) in indirect_names:
            raise CheckFailure("stack", "invalid or duplicate required indirect stack call")
        indirect_names.add((caller, site))
    for index, raw_leaf in enumerate(document["declared_leaves"]):
        leaf = require_object(raw_leaf, {"symbol", "frame_bytes", "evidence"},
                              "declared stack leaf %d" % index)
        symbol = leaf["symbol"]
        if not isinstance(symbol, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.]*", symbol) or \
           symbol in names:
            raise CheckFailure("stack", "declared stack leaf symbol is invalid")
        names.add(symbol)
        require_int(leaf["frame_bytes"], "declared stack leaf frame bytes")
        if not isinstance(leaf["evidence"], dict):
            raise CheckFailure("stack", "declared stack leaf evidence must be an object")
        evidence = leaf["evidence"]
        kind = evidence.get("kind")
        if kind == "library_archive":
            exact_keys(evidence, {"kind", "path", "sha256", "compiler_manifest_key",
                                  "compiler_sha256"}, "declared library leaf evidence")
            if not isinstance(evidence["path"], str) or not Path(evidence["path"]).is_absolute() or \
               evidence["compiler_manifest_key"] != "build.compiler.sha256":
                raise CheckFailure("stack", "declared library leaf evidence is invalid")
            require_sha(evidence["sha256"], "declared library leaf archive hash")
            require_sha(evidence["compiler_sha256"], "declared library leaf compiler hash")
        elif kind == "kernel_source":
            exact_keys(evidence, {"kind", "path", "sha256", "commands_manifest_name_key",
                                  "commands_manifest_sha256_key"},
                       "declared kernel leaf evidence")
            source = Path(evidence["path"])
            if not isinstance(evidence["path"], str) or source.is_absolute() or ".." in source.parts or \
               evidence["commands_manifest_name_key"] != "evidence.ninja_commands.name" or \
               evidence["commands_manifest_sha256_key"] != "evidence.ninja_commands.sha256":
                raise CheckFailure("stack", "declared kernel leaf evidence is invalid")
            require_sha(evidence["sha256"], "declared kernel leaf source hash")
        else:
            raise CheckFailure("stack", "declared stack leaf evidence kind is invalid")
    return document


def reachable_nodes(graph: dict[str, set[str]], root: str) -> set[str]:
    reachable: set[str] = set()
    pending = [root]
    while pending:
        node = pending.pop()
        if node in reachable:
            continue
        reachable.add(node)
        pending.extend(graph.get(node, set()))
    return reachable


def callsite_mapping_from_contract(contract: dict[str, Any]) -> dict[tuple[str, int], str]:
    return {(item["from"], item["site"]): item["to"]
            for item in contract.get("indirect_calls", [])}


def bind_derived_indirect_call_sites(
        sites: tuple[IndirectCallSite, ...],
        bindings: Iterable[IndirectCallBinding]) -> tuple[dict[tuple[str, int], str],
                                                           set[tuple[str, int]]]:
    """Bind source-derived calls to the same caller's compiled BLX sites."""
    expected: dict[str, list[str | None]] = {}
    for binding in bindings:
        expected.setdefault(binding.caller, []).append(binding.target)
    actual: dict[str, list[IndirectCallSite]] = {}
    for site in sites:
        actual.setdefault(site.caller, []).append(site)
    result: dict[tuple[str, int], str] = {}
    inactive: set[tuple[str, int]] = set()
    for caller, targets in expected.items():
        caller_sites = actual.get(caller, [])
        if len(caller_sites) != len(targets):
            raise CheckFailure(
                "stack", "production derived callback callsite count differs for %s: "
                "source=%d disassembly=%d" % (caller, len(targets), len(caller_sites)))
        for site, target in zip(caller_sites, targets):
            key = (site.caller, site.ordinal)
            if target is None:
                inactive.add(key)
            else:
                result[key] = target
    return result, inactive


def apply_indirect_callsite_mappings(
        graph: dict[str, set[str]], root: str,
        sites: tuple[IndirectCallSite, ...],
        derived: Iterable[IndirectCallBinding],
        contract: dict[str, Any]) -> dict[str, set[str]]:
    """Fail closed over every reachable register-indirect call site.

    Contracts name the caller plus its ordinal BLX instruction.  This makes a
    second callback in one caller independently accountable; a broad function
    edge can never hide it.  Every contract mapping must resolve one live site.
    """
    active = {node: set(children) for node, children in graph.items()}
    derived_mappings, inactive_derived_sites = bind_derived_indirect_call_sites(sites, derived)
    contract_mappings = callsite_mapping_from_contract(contract)
    duplicate = sorted(set(derived_mappings) & set(contract_mappings))
    if duplicate:
        raise CheckFailure("stack", "indirect callback has both derived and contract mappings: " +
                            ",".join("%s#%d" % item for item in duplicate))
    inactive_contracts = sorted(inactive_derived_sites & set(contract_mappings))
    if inactive_contracts:
        raise CheckFailure("stack", "inactive indirect callback has a contract mapping: " +
                           ",".join("%s#%d" % item for item in inactive_contracts))
    inactive_contract_sites = {key for key, target in contract_mappings.items()
                               if target == INACTIVE_INDIRECT_TARGET}
    all_mappings = {**derived_mappings,
                    **{key: target for key, target in contract_mappings.items()
                       if key not in inactive_contract_sites}}
    site_keys = {(site.caller, site.ordinal) for site in sites}
    stale = sorted(set(contract_mappings) - site_keys)
    if stale:
        raise CheckFailure("stack", "indirect contract maps no production BLX site: " +
                           ",".join("%s#%d" % item for item in stale))
    while True:
        reachable = reachable_nodes(active, root)
        reachable_sites = [site for site in sites if site.caller in reachable]
        missing = sorted((site.caller, site.ordinal) for site in reachable_sites
                         if (site.caller, site.ordinal) not in all_mappings and
                         (site.caller, site.ordinal) not in inactive_derived_sites and
                         (site.caller, site.ordinal) not in inactive_contract_sites)
        if missing:
            raise CheckFailure("stack", "reachable register-indirect BLX is unresolved: " +
                               ",".join("%s#%d" % item for item in missing))
        before = sum(len(children) for children in active.values())
        for site in reachable_sites:
            key = (site.caller, site.ordinal)
            if key in inactive_derived_sites or key in inactive_contract_sites:
                continue
            active.setdefault(site.caller, set()).add(all_mappings[key])
        after = sum(len(children) for children in active.values())
        if after == before:
            break
    reachable = reachable_nodes(active, root)
    unused = sorted(key for key in contract_mappings if key not in
                    {(site.caller, site.ordinal) for site in sites if site.caller in reachable})
    if unused:
        raise CheckFailure("stack", "indirect callback mapping is unused from %s: %s" %
                           (root, ",".join("%s#%d" % item for item in unused)))
    return active


def require_rooted_stack_edges(contract: dict[str, Any],
                               required: frozenset[tuple[str, str, str, str]] =
                               REQUIRED_ROOTED_STACK_EDGES) -> None:
    supplied = {(edge["from"], edge["to"], edge["kind"], edge["when"])
                for edge in contract["edges"]}
    missing = sorted(required - supplied)
    if missing:
        raise CheckFailure(
            "stack", "required rooted stack edges are missing: " +
            ",".join("%s->%s[%s,%s]" % edge for edge in missing))


def verify_declared_leaf_evidence(contract: dict[str, Any], manifest_path: Path | None,
                                  manifest: dict[str, str]) -> None:
    if not contract["declared_leaves"]:
        return
    if manifest_path is None:
        raise CheckFailure("provenance", "declared stack leaves require a published manifest")
    repo_root = Path(__file__).resolve().parents[1]
    checked_libraries: set[tuple[str, str]] = set()
    checked_kernel_sources: set[str] = set()
    for leaf in contract["declared_leaves"]:
        evidence = leaf["evidence"]
        if evidence["kind"] == "library_archive":
            if manifest.get(evidence["compiler_manifest_key"]) != evidence["compiler_sha256"]:
                raise CheckFailure("provenance", "declared library leaf compiler evidence differs")
            library_path = Path(evidence["path"])
            library_key = (str(library_path), evidence["sha256"])
            if library_key not in checked_libraries:
                if not library_path.is_file() or sha256_file(library_path) != evidence["sha256"]:
                    raise CheckFailure("provenance", "declared library leaf archive differs")
                checked_libraries.add(library_key)
            continue
        source_path = repo_root / evidence["path"]
        if not source_path.is_file() or sha256_file(source_path) != evidence["sha256"]:
            raise CheckFailure("provenance", "declared kernel leaf source differs")
        if evidence["path"] in checked_kernel_sources:
            continue
        command_name = manifest.get(evidence["commands_manifest_name_key"])
        if command_name is None:
            raise CheckFailure("provenance", "declared kernel leaf lacks command evidence")
        command_path = manifest_sidecar(manifest_path, manifest,
                                        evidence["commands_manifest_name_key"])
        if command_path is None:
            raise CheckFailure("provenance", "declared kernel leaf command evidence is absent")
        verify_manifest_hash(manifest, evidence["commands_manifest_sha256_key"], command_path)
        try:
            command_text = command_path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            raise CheckFailure("provenance", "cannot read kernel command evidence: %s" % error)
        if str(source_path.resolve()) not in command_text:
            raise CheckFailure("provenance", "declared kernel leaf source was not compiled")
        checked_kernel_sources.add(evidence["path"])


def validate_edges(contract_or_path: dict[str, Any] | Path, resolvers: dict[str, str],
                    graph: dict[str, set[str]], frames: StackFrames, root: str,
                   include_binding: bool = True,
                   indirect_sites: tuple[IndirectCallSite, ...] | None = None,
                   derived_indirect_bindings: Iterable[IndirectCallBinding] = ()) -> list[StackFrame]:
    contract = load_stack_contract(contract_or_path, root) \
        if isinstance(contract_or_path, Path) else contract_or_path
    active_edges = [item for item in contract["edges"]
                    if include_binding or item["when"] != "binding"]
    # A routed cycle invokes both operation callbacks.  Treat every resolved
    # callback as a real edge in an overlay graph rather than selecting the
    # syntactically last sibling from the required-edge manifest.
    active_graph = {node: set(children) for node, children in graph.items()}
    if indirect_sites is not None:
        active_graph = apply_indirect_callsite_mappings(
            active_graph, root, indirect_sites, derived_indirect_bindings, contract)
    required_targets: set[str] = set()
    for item in active_edges:
        source = item["from"]
        destination = item["to"]
        if item["kind"] == "indirect":
            resolved = resolvers.get(destination)
            if resolved is None:
                raise CheckFailure("stack", "unresolved indirect edge %s" % destination)
            destination = resolved
            active_graph.setdefault(source, set()).add(destination)
        elif destination not in graph.get(source, set()):
            raise CheckFailure("stack", "missing required call %s -> %s" %
                               (source, destination))
        required_targets.add(destination)

    reachable = reachable_nodes(active_graph, root)
    detached = sorted(required_targets - reachable)
    if detached:
        raise CheckFailure("stack", "required stack edge is detached from root: " +
                           ",".join(detached))

    for leaf in contract["declared_leaves"]:
        frames.declare(leaf["symbol"], leaf["frame_bytes"])

    for destination in required_targets:
        frames.resolve(destination)

    # Required edges select the budgeted chain, but every statically visible
    # active descendant of the task must still have .su or declared-leaf
    # evidence.  Otherwise an unselected branch could hide a missing frame.
    def resolve_active(node: str, visiting: set[str]) -> None:
        for child in sorted(active_graph.get(node, set())):
            if child in visiting:
                # The raw disassembly graph includes control-plane cycles that
                # are not a recursive task call.  Do not walk one twice; the
                # required routed-operation overlay remains acyclic.
                continue
            frames.resolve(child)
            if frames.is_declared(child):
                continue
            resolve_active(child, visiting | {child})

    frames.resolve(root)
    resolve_active(root, {root})
    frames.require_declared_coverage()

    # Evaluate every resolved branch and choose the true maximum-byte
    # root-to-leaf chain.  Equal-byte hardware/register leaves are equally safe;
    # select a stable lexical representative rather than dropping either branch.
    paths: list[list[str]] = []

    def visit(node: str, prefix: list[str], visiting: set[str]) -> None:
        children = sorted(child for child in active_graph.get(node, set())
                          if child not in visiting)
        for child in children:
            frames.resolve(child)
        if not children:
            paths.append(prefix)
            return
        for child in children:
            if frames.is_declared(child):
                paths.append(prefix + [child])
                continue
            visit(child, prefix + [child], visiting | {child})

    visit(root, [root], {root})
    if not paths:
        raise CheckFailure("stack", "required stack graph has no active leaf")
    scored = [(sum(frames.resolve(symbol).frame_bytes for symbol in item), item)
              for item in paths]
    maximum = max(score for score, _ in scored)
    deepest = [item for score, item in scored if score == maximum]
    return [frames.resolve(symbol) for symbol in sorted(deepest)[0]]


def parse_compile_command(commands_path: Path, source: Path,
                          target: str | None = None) -> tuple[list[str], Path]:
    try:
        entries = json.loads(commands_path.read_text(encoding="utf-8"), object_pairs_hook=strict_pairs)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        raise CheckFailure("preprocess", "cannot parse compile commands: %s" % error)
    if not isinstance(entries, list):
        raise CheckFailure("preprocess", "compile_commands must be an array")
    matches: list[dict[str, Any]] = []
    for entry in entries:
        if not isinstance(entry, dict) or "file" not in entry:
            continue
        if target is not None and (not isinstance(entry.get("output"), str) or
                                   "CMakeFiles/%s.dir/" % target not in entry["output"]):
            continue
        candidate = Path(entry["file"])
        if not candidate.is_absolute():
            candidate = Path(entry.get("directory", commands_path.parent)) / candidate
        try:
            if candidate.resolve() == source.resolve():
                matches.append(entry)
        except OSError:
            continue
    if len(matches) != 1:
        label = target if target is not None else "selected main"
        raise CheckFailure("preprocess", "%s has %d compile commands" % (label, len(matches)))
    entry = matches[0]
    if "arguments" in entry and isinstance(entry["arguments"], list):
        arguments = [str(value) for value in entry["arguments"]]
    elif "command" in entry and isinstance(entry["command"], str):
        arguments = shlex.split(entry["command"])
    else:
        raise CheckFailure("preprocess", "compile command lacks command/arguments")
    if not arguments:
        raise CheckFailure("preprocess", "empty compile command")
    return arguments, Path(entry.get("directory", commands_path.parent))


def nm_sized_symbols(path: Path) -> dict[str, int]:
    completed = subprocess.run(
        ["arm-none-eabi-nm", "--print-size", "--format=posix", str(path)],
        capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise CheckFailure("provenance", "nm size query failed: " + completed.stderr.strip())
    symbols: dict[str, int] = {}
    for line in completed.stdout.splitlines():
        columns = line.split()
        if len(columns) < 4:
            continue
        try:
            symbols[columns[0]] = int(columns[3], 16)
        except ValueError:
            continue
    return symbols


def nm_ram_symbols(path: Path) -> dict[str, int]:
    """Return exactly the named ELF .data/.bss symbols used by map accounting."""
    completed = subprocess.run(
        ["arm-none-eabi-nm", "--defined-only", "--print-size", "--format=posix", str(path)],
        capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise CheckFailure("provenance", "RAM symbol query failed: " + completed.stderr.strip())
    symbols: dict[str, int] = {}
    for line in completed.stdout.splitlines():
        columns = line.split()
        if len(columns) < 4 or columns[1] not in {"B", "b", "D", "d"}:
            continue
        try:
            size = int(columns[3], 16)
        except ValueError:
            continue
        if size > 0:
            symbols[columns[0]] = size
    return symbols


def verify_external_artifact_seal(artifact_manifest: Path, seal_path: Path | None) -> None:
    if seal_path is None:
        raise CheckFailure("provenance", "application baseline requires an external artifact-manifest seal")
    try:
        seal_resolved = seal_path.resolve(strict=True)
        artifact_resolved = artifact_manifest.resolve(strict=True)
        line = seal_resolved.read_text(encoding="utf-8").strip()
    except OSError as error:
        raise CheckFailure("provenance", "cannot read application baseline seal: %s" % error)
    try:
        seal_resolved.relative_to(artifact_resolved.parent)
    except ValueError:
        pass
    else:
        raise CheckFailure("provenance", "application baseline seal must be external to its artifact")
    matched = re.fullmatch(r"([0-9a-f]{64})\s+[* ]?([^\s]+)", line)
    if matched is None or matched.group(2) != artifact_resolved.name or \
            matched.group(1) != sha256_file(artifact_resolved):
        raise CheckFailure("provenance", "application baseline artifact-manifest seal is stale or malformed")


def compile_application_size_probe(commands_path: Path, source: Path) -> dict[str, int]:
    """Compile the dedicated sizeof probe with one published ARM command."""
    repo_root = Path(__file__).resolve().parents[1]
    reference = repo_root / "app/mind_application/mind_application_wire.c"
    arguments, directory = parse_compile_command(commands_path, reference)
    if not source.is_file() or source.resolve().parent != (repo_root / "tests/application").resolve():
        raise CheckFailure("provenance", "application sizeof probe is not the checked-in test source")
    if not directory.is_dir():
        raise CheckFailure("provenance", "published application compile directory is absent")
    source_resolved = source.resolve()
    filtered: list[str] = [arguments[0]]
    skip_next = False
    for argument in arguments[1:]:
        if skip_next:
            skip_next = False
            continue
        if argument in ("-c", "-S", "-MMD", "-MD", "-MM", "-M", "-MP"):
            continue
        if argument in ("-o", "-MF", "-MT", "-MQ"):
            skip_next = True
            continue
        candidate = Path(argument)
        if not candidate.is_absolute():
            candidate = directory / candidate
        try:
            if candidate.resolve() == reference.resolve():
                continue
        except OSError:
            pass
        filtered.append(argument)
    with tempfile.TemporaryDirectory(prefix="mind-application-size.") as temporary:
        object_path = Path(temporary) / "mind_application_sizes.o"
        filtered.extend(["-c", str(source_resolved), "-o", str(object_path)])
        completed = subprocess.run(filtered, cwd=directory, capture_output=True,
                                   text=True, check=False)
        if completed.returncode != 0 or not object_path.is_file():
            raise CheckFailure("provenance", "application sizeof probe compilation failed: " +
                               completed.stderr.strip())
        return nm_sized_symbols(object_path)


def build_application_size_report(args: argparse.Namespace) -> dict[str, Any]:
    if not args.application_size_source or not args.compile_commands or not args.full_elf or \
            not args.full_map or not args.config_header:
        raise CheckFailure("arguments", "application sizeof report requires source, compile commands, "
                           "ELF, map, and config header")
    source = Path(args.application_size_source)
    size_symbols = compile_application_size_probe(Path(args.compile_commands), source)
    prefix = "mind_application_sizeof_"
    sizes = {name.removeprefix(prefix): value for name, value in size_symbols.items()
             if name.startswith(prefix)}
    required_sizes = set(MIND_APPLICATION_PRODUCTION_COMPONENTS.values()) | {
        "ingress_seen", "ingress_event", "ingress_counters", "topology_entry",
        "topology_counters", "topology_adapter", "root_entry", "root_campaign_target",
        "root_ack_obligation", "root_plane_counters", "root_plane", "event_target",
        "event_item", "event_forwarder_counters", "event_forwarder", "root_inbox_entry",
        "log_reservation", "command_attempt", "command_parser", "command_mailbox",
        "audio", "root_coordinator_token", "root_coordinator_operations",
        "root_coordinator_counters",
    }
    if set(sizes) != required_sizes or any(value <= 0 for value in sizes.values()):
        raise CheckFailure("provenance", "application sizeof probe lacks one required nonzero symbol")
    elf_symbols = nm_sized_symbols(Path(args.full_elf))
    components: dict[str, int] = {}
    for symbol, size_name in MIND_APPLICATION_PRODUCTION_COMPONENTS.items():
        elf_symbol = "fb" if symbol == "display_framebuffer" else symbol
        expected = sizes[size_name]
        actual = elf_symbols.get(elf_symbol)
        if actual != expected:
            raise CheckFailure("provenance", "production ELF size differs for %s" % symbol)
        components[symbol] = actual
    config_header = Path(args.config_header)
    for symbol, macro in MIND_APPLICATION_STATIC_STACK_COMPONENTS.items():
        expected = config_uint_macro(config_header, macro, macro, "provenance")
        actual = elf_symbols.get(symbol)
        if actual != expected:
            raise CheckFailure("provenance", "production ELF static task buffer differs for " + symbol)
        components[symbol] = actual
    aggregate = sum(components.values())
    report = {
        "schema": APPLICATION_SIZE_SCHEMA,
        "capacities": dict(MIND_APPLICATION_CAPACITIES),
        "sizeof": dict(sorted(sizes.items())),
        "production": {
            "components": components,
            "aggregate_new_static_bytes": aggregate,
        },
        "provenance": {
            "probe_source_sha256": sha256_file(source),
            "compile_commands_sha256": sha256_file(Path(args.compile_commands)),
            "elf_sha256": sha256_file(Path(args.full_elf)),
            "map_sha256": sha256_file(Path(args.full_map)),
            "config_header_sha256": sha256_file(Path(args.config_header)),
        },
    }
    return validate_application_size_report(report, "generated application sizeof report")


def validate_application_size_report(document: dict[str, Any], label: str) -> dict[str, Any]:
    exact_keys(document, {"schema", "capacities", "sizeof", "production", "provenance"}, label)
    if document["schema"] != APPLICATION_SIZE_SCHEMA:
        raise CheckFailure("fixture_schema", label + " has an invalid application sizeof schema")
    capacities = require_object(document["capacities"], set(MIND_APPLICATION_CAPACITIES),
                                label + ".capacities")
    if capacities != MIND_APPLICATION_CAPACITIES:
        raise CheckFailure("provenance", label + " capacities differ from the fixed Layer-7 contract")
    sizes = document["sizeof"]
    if not isinstance(sizes, dict) or not sizes or any(
            not isinstance(name, str) or require_int(value, label + ".sizeof." + name) == 0
            for name, value in sizes.items()):
        raise CheckFailure("fixture_schema", label + ".sizeof is invalid")
    production = require_object(document["production"],
                                {"components", "aggregate_new_static_bytes"}, label + ".production")
    components = require_object(production["components"],
                                set(MIND_APPLICATION_PRODUCTION_COMPONENTS) |
                                set(MIND_APPLICATION_STATIC_STACK_COMPONENTS),
                                label + ".production.components")
    for name, value in components.items():
        require_int(value, label + ".production.components." + name)
    aggregate = require_int(production["aggregate_new_static_bytes"],
                            label + ".production.aggregate_new_static_bytes")
    if aggregate != sum(components.values()):
        raise CheckFailure("provenance", label + " aggregate does not equal its exclusive components")
    provenance = require_object(document["provenance"],
                                {"probe_source_sha256", "compile_commands_sha256", "elf_sha256",
                                 "map_sha256", "config_header_sha256"}, label + ".provenance")
    for name, value in provenance.items():
        require_sha(value, label + ".provenance." + name)
    return document


def require_initial_task_compile_definition(commands_path: Path, source: Path,
                                            initial_task_stack_bytes: int,
                                            target: str | None = None) -> None:
    arguments, _ = parse_compile_command(commands_path, source, target)
    definitions = [argument for argument in arguments
                   if argument.startswith("-DINITTASK_STKSZ")]
    expected = "-DINITTASK_STKSZ=%d" % initial_task_stack_bytes
    if definitions != [expected]:
        raise CheckFailure(
            "provenance", "compile command for %s must contain exactly one %s" %
            (source, expected))


def require_routed_initial_task_compile_definitions(
        commands_path: Path, initial_task_stack_bytes: int) -> None:
    repo_root = Path(__file__).resolve().parents[1]
    require_initial_task_compile_definition(
        commands_path, repo_root / "app/tavrn_routed_node/src/main.c",
        initial_task_stack_bytes)
    require_initial_task_compile_definition(
        commands_path, repo_root / "libs/mtkernel_3/kernel/inittask/inittask.c",
        initial_task_stack_bytes, "mtkernel3_microbit_kernel_tavrn_routed_node")


def require_routed_logger_task_compile_definition(
        commands_path: Path, logger_task_stack_bytes: int) -> None:
    repo_root = Path(__file__).resolve().parents[1]
    arguments, _ = parse_compile_command(
        commands_path, repo_root / "app/tavrn_routed_node/src/main.c")
    definitions = [argument for argument in arguments
                   if argument.startswith("-DTRON_ROUTED_LOGGER_TASK_STACK_BYTES")]
    expected = "-DTRON_ROUTED_LOGGER_TASK_STACK_BYTES=%d" % logger_task_stack_bytes
    if definitions != [expected]:
        raise CheckFailure(
            "provenance", "compile command for routed logger must contain exactly one %s" %
            expected)


def initial_task_stack_report(declaration: RoutedRuntimeDeclaration,
                              frames: StackFrames, main_source: Path) -> InitialTaskStackReport:
    # This checks the production initial-task function's own static frame only.
    # The independent routed_mesh_task chain remains the complete call-chain
    # check; this must not be read as a startup-chain claim.
    frame = frames.resolve_source_function(main_source, "usermain")
    if frame.frame_bytes > declaration.initial_task_stack_bytes:
        raise CheckFailure("stack", "production usermain static frame exceeds initial-task stack")
    headroom = declaration.initial_task_stack_bytes - frame.frame_bytes
    if headroom < STACK_HEADROOM:
        raise CheckFailure("stack", "production usermain static frame leaves less than 1024 bytes "
                           "of logical initial-task headroom")
    return InitialTaskStackReport(declaration.initial_task_stack_bytes,
                                  frame.frame_bytes, headroom)


def preprocess_main(commands: Path, source: Path, output: Path) -> str:
    arguments, directory = parse_compile_command(commands, source)
    published_generated_headers = commands.with_name(
        commands.name.removesuffix(".compile_commands.json") + ".generated-headers")
    if not directory.is_dir():
        directory = commands.parent
    filtered: list[str] = [arguments[0]]
    skip_next = False
    source_resolved = source.resolve()
    for index, argument in enumerate(arguments[1:]):
        if skip_next:
            skip_next = False
            continue
        if argument in ("-c", "-S", "-MMD", "-MD", "-MM", "-M", "-MP"):
            continue
        if argument in ("-o", "-MF", "-MT", "-MQ"):
            skip_next = True
            continue
        if argument.startswith("-I") and len(argument) > 2:
            include_path = Path(argument[2:])
            if not include_path.is_dir() and "generated" in include_path.parts and \
                    published_generated_headers.is_dir():
                filtered.append("-I" + str(published_generated_headers))
                continue
        candidate = Path(argument)
        if not candidate.is_absolute():
            candidate = directory / candidate
        try:
            if candidate.resolve() == source_resolved:
                continue
        except OSError:
            pass
        filtered.append(argument)
    filtered.extend(["-E", "-P", "-o", str(output), str(source)])
    output.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(filtered, cwd=directory, capture_output=True, text=True,
                               check=False)
    if completed.returncode != 0 or not output.is_file():
        raise CheckFailure("preprocess", "preprocessing selected main failed: " +
                           completed.stderr.strip())
    return output.read_text(encoding="utf-8", errors="replace")


def function_body(source: str, name: str) -> str:
    match = re.search(r"\b%s\s*\([^;{}]*\)\s*\{" % re.escape(name), source)
    if not match:
        return ""
    depth = 0
    for index in range(match.end() - 1, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.end():index]
    return ""


def block_end(source: str, opening_brace: int) -> int:
    depth = 0
    for index in range(opening_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    return -1


def guarded_block_containing(body: str, required_tokens: tuple[str, ...],
                             position: int) -> bool:
    for match in re.finditer(r"\bif\s*\(([^{}]*)\)\s*\{", body):
        condition = match.group(1)
        if not all(token in condition for token in required_tokens):
            continue
        end = block_end(body, match.end() - 1)
        if end >= position >= match.end():
            return True
    return False


def validate_preprocessed_main(source: str, mesh_task_stack_bytes: int | None = None) -> None:
    if source.count("routed_cycle_operations.router_scheduler_event") != 1 or not re.search(
            r"routed_cycle_operations\.router_scheduler_event\s*=\s*"
            r"routed_cycle_router_scheduler_event\s*;", source):
        raise CheckFailure("preprocessed_inactive",
                           "active main lacks one router_scheduler_event binding")
    if source.count("routed_cycle_operations.router_tick") != 1 or not re.search(
            r"routed_cycle_operations\.router_tick\s*=\s*routed_cycle_router_tick\s*;", source):
        raise CheckFailure("preprocessed_inactive", "active main lacks one router_tick binding")
    body = function_body(source, "routed_cycle_router_tick")
    if body.count("tavrn_full_maintenance_binding_tick(") != 1:
        raise CheckFailure("preprocessed_inactive", "active main lacks one FULL binding call")
    if not re.search(r"tavrn_full_maintenance_binding_tick\s*\([^;]*"
                     r"&\s*routed_binding_result_storage\s*\)", body):
        raise CheckFailure("preprocessed_guard",
                           "FULL binding must write the static caller-owned result")
    if re.search(r"routed_binding_result_storage\s*=\s*"
                 r"tavrn_full_maintenance_binding_tick", body):
        raise CheckFailure("preprocessed_guard",
                           "FULL binding result must not be returned by value")
    if re.search(r"tavrn_router_phase_trace_t\s+trace\b", body) or not re.search(
            r"tavrn_router_phase_trace_t\s*\*\s*trace\s*=\s*"
            r"&\s*routed_binding_result_storage\.router_trace\s*;", body) or not re.search(
                r"return\s+\*\s*trace\s*;", body):
        raise CheckFailure("preprocessed_guard",
                           "FULL tick must return only the static stored trace at its ABI boundary")
    if "tavrn_router_tick_ex(" in body:
        raise CheckFailure("preprocessed_inactive", "main directly ticks AODV beside FULL binding")
    if any(re.search(r"\b%s\b" % token, body) for token in ("for", "while", "do")) or \
       "routed_cycle_router_tick(" in body:
        raise CheckFailure("preprocessed_guard", "binding callback must not loop or recurse")
    take = body.find("tavrn_full_application_mailbox_owner_take(")
    tick = body.find("tavrn_full_maintenance_binding_tick(")
    publish = body.find("tavrn_full_application_mailbox_owner_publish(")
    if take < 0 or publish < 0 or not (take < tick < publish):
        raise CheckFailure("preprocessed_guard", "mailbox take/binding/publish order is not active")
    if body.count("tavrn_full_application_mailbox_owner_take(") != 1 or \
       body.count("tavrn_full_application_mailbox_owner_publish(") != 1 or \
       not guarded_block_containing(body, ("tavrn_full_application_mailbox_owner_take",), tick) or \
       not guarded_block_containing(body, ("application_present", "application_result"), publish):
        raise CheckFailure("preprocessed_guard", "mailbox take/result guards do not enclose binding/publish")
    if mesh_task_stack_bytes is not None and len(re.findall(
            r"\.stksz\s*=\s*%du\s*}" % mesh_task_stack_bytes, source)) != 1:
        raise CheckFailure("preprocessed_guard", "production usermain lacks one generated mesh task allocation")


def validate_capture(document: dict[str, Any]) -> None:
    capture = document["capture"]
    if not capture["supplied"]:
        return
    if capture["duration_seconds"] < 300 or capture["complete_passes"] < 10 or \
        capture["hard_selected_demand_deferred_passes"] < 1 or \
        capture["max_scheduler_gap_ms"] == 0 or capture["max_scheduler_gap_ms"] > 2 or \
        any(capture[key] != 0 for key in
        ("unavailable_count", "faults", "targeted_controls", "rreq_controls",
         "tc_expiry_controls", "telemetry_dropped")):
        raise CheckFailure("hardware_capture", "hardware capture misses expiry acceptance bounds")


def load_capture(path: Path, artifact_manifest_path: Path | None = None) -> dict[str, Any]:
    document = load_json(path, "hardware capture")
    exact_keys(document, {"schema", "duration_seconds", "complete_passes",
                            "hard_selected_demand_deferred_passes",
                            "max_scheduler_gap_ms", "unavailable_count", "faults",
                            "targeted_controls", "rreq_controls", "tc_expiry_controls",
                            "telemetry_dropped", "provenance"},
               "hardware capture")
    if document["schema"] != CAPTURE_SCHEMA:
        raise CheckFailure("hardware_capture", "hardware capture schema mismatch")
    provenance = require_object(document["provenance"], CAPTURE_PROVENANCE_KEYS,
                                "hardware capture.provenance")
    if not isinstance(provenance["artifact_manifest_name"], str) or \
       not provenance["artifact_manifest_name"] or \
       Path(provenance["artifact_manifest_name"]).name != \
       provenance["artifact_manifest_name"]:
        raise CheckFailure("hardware_capture", "capture artifact manifest name is invalid")
    require_sha(provenance["artifact_manifest_sha256"],
                "hardware capture.provenance.artifact_manifest_sha256")
    require_sha(provenance["artifact_elf_sha256"],
                "hardware capture.provenance.artifact_elf_sha256")
    if provenance["feature"] != "FULL_TAVRN" or provenance["timer"] != "FAST_TEST" or \
       provenance["hooks"] != "ON" or provenance["expiry_full_table"] != "ON":
        raise CheckFailure("hardware_capture", "capture provenance is not the FULL FAST hook build")
    if artifact_manifest_path is not None:
        manifest = parse_kv_manifest(artifact_manifest_path)
        if sha256_file(artifact_manifest_path) != provenance["artifact_manifest_sha256"] or \
           artifact_manifest_path.name != provenance["artifact_manifest_name"] or \
           manifest.get("artifact.elf.sha256") != provenance["artifact_elf_sha256"] or \
           manifest.get("feature.level.effective") != provenance["feature"] or \
           manifest.get("build.timer_profile") != provenance["timer"] or \
           manifest.get("hook.enabled") != provenance["hooks"] or \
           manifest.get("hook.expiry_full_table") != provenance["expiry_full_table"]:
            raise CheckFailure("hardware_capture", "capture provenance differs from artifact manifest")
    capture = {"supplied": True}
    for key in ("duration_seconds", "complete_passes",
                 "hard_selected_demand_deferred_passes", "max_scheduler_gap_ms",
                 "unavailable_count", "faults", "targeted_controls", "rreq_controls",
                 "tc_expiry_controls", "telemetry_dropped"):
        capture[key] = require_int(document[key], "hardware capture." + key)
    return capture


def validate_thresholds(document: dict[str, Any], require_binding: bool,
                        require_aodv_absence: bool = False) -> None:
    if document["ram"]["unallocated_bytes"] < RAM_MINIMUM:
        raise CheckFailure("ram_unallocated", "unallocated RAM is below 8192 bytes")
    if document["stack"]["total_bytes"] > STACK_LIMIT or \
       document["stack"]["headroom_bytes"] < STACK_HEADROOM:
        raise CheckFailure("stack_total", "mesh stack total/headroom violates the 4096/1024 "
                           "bound: total=%d headroom=%d" %
                           (document["stack"]["total_bytes"],
                            document["stack"]["headroom_bytes"]))
    if document["fixed_state"]["unexplained_delta_bytes"] > FIXED_DELTA_MAXIMUM:
        raise CheckFailure("fixed_state_delta", "unexplained fixed-state delta exceeds 512 bytes")
    if document["heap"]["uses_heap"]:
        raise CheckFailure("heap", "heap allocation/reference is present")
    if require_binding and (document["binding"]["full_symbol_count"] != 1 or
                            document["binding"]["full_call_count"] != 1):
        raise CheckFailure("binding", "FULL binding symbol or call evidence is invalid")
    if require_aodv_absence and (document["binding"]["aodv_symbol_count"] != 0 or
                                 document["binding"]["aodv_reference_count"] != 0):
        raise CheckFailure("binding", "AODV_ONLY references the FULL binding")
    validate_capture(document)


def inventory_hash(path: Path) -> str:
    root = Path(__file__).resolve().parents[1]
    try:
        paths = [line.strip() for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    except OSError as error:
        raise CheckFailure("provenance", "cannot read selected sources: %s" % error)
    if not paths or len(paths) != len(set(paths)):
        raise CheckFailure("provenance", "selected sources must be nonempty and unique")
    records: list[str] = []
    for relative in paths:
        candidate = Path(relative)
        if candidate.is_absolute() or ".." in candidate.parts or not (root / candidate).is_file():
            raise CheckFailure("provenance", "invalid selected source %s" % relative)
        records.append("%s  %s\n" % (sha256_file(root / candidate), relative))
    return hashlib.sha256("".join(records).encode("utf-8")).hexdigest()


def require_initial_task_source_provenance(path: Path) -> None:
    try:
        sources = [line.strip() for line in path.read_text(encoding="utf-8").splitlines()
                   if line.strip()]
    except OSError as error:
        raise CheckFailure("provenance", "cannot read selected sources: %s" % error)
    required = "libs/mtkernel_3/include/sys/inittask.h"
    if sources.count(required) != 1:
        raise CheckFailure("provenance", "selected-source inventory lacks one inittask.h record")


def manifest_sidecar(manifest_path: Path, manifest: dict[str, str], key: str) -> Path | None:
    name = manifest.get(key)
    if name is None:
        return None
    relative = Path(name)
    if not name or relative.is_absolute() or ".." in relative.parts or relative == Path("."):
        raise CheckFailure("provenance", "manifest evidence path escapes artifact: " + key)
    candidate = manifest_path.parent / name
    try:
        artifact_root = manifest_path.parent.resolve(strict=True)
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(artifact_root)
    except (OSError, ValueError) as error:
        raise CheckFailure("provenance", "manifest evidence path escapes artifact: %s (%s)" %
                           (key, error))
    if not resolved.is_file():
        raise CheckFailure("provenance", "manifest evidence is missing: %s" % key)
    return resolved


def required_manifest_sidecar(manifest_path: Path, manifest: dict[str, str],
                              name_key: str, hash_key: str) -> Path:
    path = manifest_sidecar(manifest_path, manifest, name_key)
    if path is None:
        raise CheckFailure("provenance", "manifest lacks required evidence: " + name_key)
    expected = manifest.get(hash_key)
    if expected is None or not re.fullmatch(r"[0-9a-f]{64}", expected) or \
            sha256_file(path) != expected:
        raise CheckFailure("provenance", "required manifest evidence hash differs: " + hash_key)
    return path


def verify_published_resource_provenance(manifest_path: Path) -> None:
    """Verify final artifact sidecars after the gate report is immutable."""
    manifest = parse_kv_manifest(manifest_path)
    if manifest.get("resource.gate") != "PASSED":
        raise CheckFailure("provenance", "published artifact does not declare a passed resource gate")
    report = required_manifest_sidecar(manifest_path, manifest,
                                       "resource.gate_report.name", "resource.gate_report.sha256")
    try:
        report_text = report.read_text(encoding="utf-8")
    except OSError as error:
        raise CheckFailure("provenance", "cannot read resource gate report: %s" % error)
    if "\nPASS " not in "\n" + report_text:
        raise CheckFailure("provenance", "published resource gate report lacks PASS evidence")
    preprocessed = required_manifest_sidecar(
        manifest_path, manifest, "evidence.preprocessed_main.name",
        "evidence.preprocessed_main.sha256")
    validate_preprocessed_main(preprocessed.read_text(encoding="utf-8", errors="replace"))
    count_text = manifest.get("resource.contract.count")
    if count_text is None or not re.fullmatch(r"[1-9][0-9]*", count_text):
        raise CheckFailure("provenance", "published resource contracts lack a positive count")
    for index in range(int(count_text, 10)):
        contract = required_manifest_sidecar(
            manifest_path, manifest, "resource.contract.%d.name" % index,
            "resource.contract.%d.sha256" % index)
        document = load_json(contract, "published stack-edge contract")
        if document.get("schema") not in {EDGE_SCHEMA, INDIRECT_EDGE_SCHEMA}:
            raise CheckFailure("provenance", "published resource contract has an invalid schema")
        if index == 0 and manifest.get("feature.level.effective") == "FULL_TAVRN" and \
                document.get("schema") != INDIRECT_EDGE_SCHEMA:
            raise CheckFailure("provenance", "published FULL mesh contract is not callsite-complete v3")
    if manifest_application_ingress_enabled(manifest):
        probe = required_manifest_sidecar(
            manifest_path, manifest, "resource.application_size_probe.name",
            "resource.application_size_probe.sha256")
        report_path = required_manifest_sidecar(
            manifest_path, manifest, "resource.application_size.name",
            "resource.application_size.sha256")
        report_document = validate_application_size_report(
            load_json(report_path, "published application sizeof report"),
            "published application sizeof report")
        if report_document["provenance"]["probe_source_sha256"] != sha256_file(probe):
            raise CheckFailure("provenance", "published application sizeof probe differs from report")


def verify_capture_artifact_compatibility(normal_manifest_path: Path,
                                          capture_manifest_path: Path) -> None:
    """Verify that the capture hook build changed configuration, not source scope."""
    normal = parse_kv_manifest(normal_manifest_path)
    capture = parse_kv_manifest(capture_manifest_path)
    if normal.get("feature.level.effective") != "FULL_TAVRN" or \
            normal.get("build.timer_profile") != "FAST_TEST" or \
            normal.get("hook.enabled") != "OFF" or \
            normal.get("hook.expiry_full_table") != "OFF":
        raise CheckFailure("provenance", "normal resource artifact is not FULL FAST hooks-off")
    if capture.get("feature.level.effective") != "FULL_TAVRN" or \
            capture.get("build.timer_profile") != "FAST_TEST" or \
            capture.get("hook.enabled") != "ON" or \
            capture.get("hook.expiry_full_table") != "ON":
        raise CheckFailure("provenance", "capture artifact is not FULL FAST hooks-on")
    normal_inventory = required_manifest_sidecar(
        normal_manifest_path, normal, "source.inventory.name", "evidence.selected_sources.sha256")
    capture_inventory = required_manifest_sidecar(
        capture_manifest_path, capture, "source.inventory.name", "evidence.selected_sources.sha256")
    normal_hash = normal.get("source.inventory.sha256")
    capture_hash = capture.get("source.inventory.sha256")
    if not normal_hash or not capture_hash or normal_hash != capture_hash or \
            inventory_hash(normal_inventory) != normal_hash or \
            inventory_hash(capture_inventory) != capture_hash:
        raise CheckFailure("provenance", "hook and normal selected-source inventories differ")
    normal_config = parse_kv_manifest(required_manifest_sidecar(
        normal_manifest_path, normal, "evidence.target_config_manifest.name",
        "evidence.target_config_manifest.sha256"))
    capture_config = parse_kv_manifest(required_manifest_sidecar(
        capture_manifest_path, capture, "evidence.target_config_manifest.name",
        "evidence.target_config_manifest.sha256"))
    if set(normal_config) != set(capture_config):
        raise CheckFailure("provenance", "hook and normal build-config schemas differ")
    differences = {key for key in normal_config
                   if normal_config[key] != capture_config[key]}
    if not CAPTURE_REQUIRED_BUILD_CONFIG_DIFFERENCES.issubset(differences) or \
            not differences.issubset(CAPTURE_BUILD_CONFIG_DIFFERENCES):
        raise CheckFailure("provenance", "hook build differs outside permitted hook fields")


def verify_manifest_hash(manifest: dict[str, str], key: str, path: Path) -> None:
    expected = manifest.get(key)
    if expected is None:
        return
    if not re.fullmatch(r"[0-9a-f]{64}", expected) or sha256_file(path) != expected:
        raise CheckFailure("provenance", "%s hash differs from build manifest" % key)


def mesh_stack_bytes(config_header: Path) -> int:
    value = config_uint_macro(config_header, "TRON_BUILD_ROUTED_MESH_TASK_STACK_BYTES",
                               "mesh-stack-size", "stack")
    if value != REQUIRED_ROUTED_MESH_TASK_STACK_BYTES:
        raise CheckFailure("stack", "generated mesh stack size differs from the 4864-byte contract")
    return value


def logger_stack_bytes(config_header: Path) -> int:
    value = config_uint_macro(config_header,
                              "TRON_BUILD_ROUTED_LOGGER_TASK_STACK_BYTES",
                              "logger-stack-size", "stack")
    if value == 0:
        raise CheckFailure("stack", "generated logger stack size must be nonzero")
    return value


def stack_report(root: str, stack_bytes: int, chain: list[StackFrame]) -> StackReport:
    total = sum(frame.frame_bytes for frame in chain)
    return StackReport(root, stack_bytes, tuple(chain), total,
                       max(0, stack_bytes - total))


def validate_mesh_stack_threshold(report: StackReport) -> None:
    """Keep the measured-chain ceiling independent from task allocation."""
    if report.stack_bytes != REQUIRED_ROUTED_MESH_TASK_STACK_BYTES or \
            report.total_bytes > STACK_LIMIT or report.headroom_bytes < STACK_HEADROOM:
        raise CheckFailure(
            "stack_total", "mesh stack total/headroom violates the generated 4096/4864/1024 "
            "contract: total=%d allocation=%d headroom=%d" %
            (report.total_bytes, report.stack_bytes, report.headroom_bytes))


def require_benchmark_logger_evidence(manifest: dict[str, str],
                                      args: argparse.Namespace) -> bool:
    if manifest.get("bench.mode") != "ON":
        return False
    if args.logger_stack_root != "routed_logger_task":
        raise CheckFailure("stack", "benchmark stack gate requires --logger-stack-root routed_logger_task")
    if not args.logger_required_edge_manifest:
        raise CheckFailure("stack", "benchmark stack gate requires --logger-required-edge-manifest")
    return True


def validate_logger_stack_threshold(report: StackReport) -> None:
    if report.headroom_bytes < STACK_HEADROOM:
        raise CheckFailure(
            "stack_total", "logger stack total/headroom violates the generated %d/%d bound: "
            "total=%d headroom=%d" %
            (report.stack_bytes, STACK_HEADROOM,
             report.total_bytes, report.headroom_bytes))


def validate_mind_auxiliary_stack_threshold(report: StackReport) -> None:
    if report.stack_bytes != MIND_AUXILIARY_TASK_STACK_BYTES or \
            report.headroom_bytes < MIND_AUXILIARY_TASK_HEADROOM:
        raise CheckFailure(
            "stack_total", "application task stack total/headroom violates the generated %d/%d "
            "bound: root=%s total=%d headroom=%d" %
            (MIND_AUXILIARY_TASK_STACK_BYTES, MIND_AUXILIARY_TASK_HEADROOM,
             report.root, report.total_bytes, report.headroom_bytes))


def mind_application_stack_reports(args: argparse.Namespace,
                                   manifest_path: Path,
                                   manifest: dict[str, str]) -> tuple[StackReport, ...]:
    if not manifest_application_ingress_enabled(manifest):
        return ()
    required = (
        "su_glob", "disassembly", "mind_logger_required_edge_manifest",
        "mind_uart_required_edge_manifest", "mind_ui_required_edge_manifest",
        "mind_display_required_edge_manifest", "preprocessed_main_out",
    )
    missing = [name for name in required if not getattr(args, name)]
    if missing:
        raise CheckFailure("stack", "production wearable ingress lacks stack evidence: " +
                           ",".join(missing))
    paths = [Path(path) for path in sorted(glob.glob(args.su_glob, recursive=True))]
    disassembly = disassembly_text(Path(args.disassembly), None)
    graph = call_graph(disassembly)
    sites = indirect_call_sites(disassembly)
    _, derived_bindings = production_indirect_callback_bindings(
        Path(args.preprocessed_main_out))
    contracts = (
        ("routed_logger_task", REQUIRED_ROUTED_LOGGER_TASK_STACK_BYTES,
         Path(args.mind_logger_required_edge_manifest),
         {"mind_log_line_sink": "routed_mind_log_sink"}, validate_logger_stack_threshold),
        ("mind_uart_task", MIND_AUXILIARY_TASK_STACK_BYTES,
         Path(args.mind_uart_required_edge_manifest), {}, validate_mind_auxiliary_stack_threshold),
        ("mind_ui_task", MIND_AUXILIARY_TASK_STACK_BYTES,
         Path(args.mind_ui_required_edge_manifest), {}, validate_mind_auxiliary_stack_threshold),
        ("display_task", MIND_AUXILIARY_TASK_STACK_BYTES,
         Path(args.mind_display_required_edge_manifest), {}, validate_mind_auxiliary_stack_threshold),
    )
    reports: list[StackReport] = []
    for root, stack_bytes, contract_path, resolvers, threshold in contracts:
        contract = load_stack_contract(contract_path, root)
        verify_declared_leaf_evidence(contract, manifest_path, manifest)
        chain = validate_edges(contract, resolvers, graph, parse_su(paths), root,
                               include_binding=True, indirect_sites=sites,
                               derived_indirect_bindings=derived_bindings)
        report = stack_report(root, stack_bytes, chain)
        threshold(report)
        reports.append(report)
    return tuple(reports)


def stack_usage_hash(paths: list[Path], pattern: str) -> str:
    if not paths:
        raise CheckFailure("stack", "no .su files matched")
    root_text = pattern.split("**", 1)[0]
    root = Path(root_text).resolve()
    records: list[str] = []
    for path in paths:
        try:
            relative = path.resolve().relative_to(root)
        except ValueError:
            raise CheckFailure("stack", "stack evidence escapes its published root")
        records.append("%s  %s\n" % (sha256_file(path), relative.as_posix()))
    return hashlib.sha256("".join(records).encode("utf-8")).hexdigest()


def verify_baseline(args: argparse.Namespace, document: dict[str, Any]) -> BaselineEvidence | None:
    if args.baseline_manifest is None:
        return None
    if args.before_map is None or args.baseline_sha256 is None:
        raise CheckFailure("provenance", "baseline requires before map and baseline hash")
    baseline_path = Path(args.baseline_manifest)
    before_path = Path(args.before_map)
    if args.full_map is not None and before_path.resolve() == Path(args.full_map).resolve():
        raise CheckFailure("provenance", "baseline map must not be the current map")
    baseline = validate_resource_document(load_json(baseline_path, "baseline manifest"),
                                           "baseline manifest")
    try:
        checksum_line = Path(args.baseline_sha256).read_text(encoding="utf-8").strip()
    except OSError as error:
        raise CheckFailure("provenance", "cannot read baseline hash: %s" % error)
    expected = re.fullmatch(r"([0-9a-f]{64})\s+[* ]?([^\s]+)", checksum_line)
    if expected is None or expected.group(1) != sha256_file(baseline_path) or \
       Path(expected.group(2)).name != baseline_path.name:
        raise CheckFailure("provenance", "baseline manifest hash is stale or malformed")
    before_usage = parse_map(before_path)
    if baseline["ram"]["data_bytes"] != before_usage.data_bytes or \
       baseline["ram"]["bss_bytes"] != before_usage.bss_bytes or \
        baseline["provenance"]["map_sha256"] != sha256_file(before_path):
        raise CheckFailure("provenance", "baseline map data does not match its manifest")
    if baseline["schema"] != document["schema"] or baseline["target"] != document["target"] or \
       baseline["feature"] != document["feature"] or baseline["timer"] != document["timer"] or \
       not baseline["name"]:
        raise CheckFailure("provenance", "baseline-of-record schema/profile identity mismatch")
    actual_delta = document["fixed_state"]["after_bytes"] - before_usage.allocated_bytes
    unexplained = max(0, actual_delta - document["fixed_state"]["declared_delta_bytes"])
    document["fixed_state"]["before_bytes"] = before_usage.allocated_bytes
    document["fixed_state"]["unexplained_delta_bytes"] = unexplained
    before_inventory = baseline["provenance"]["source_inventory_sha256"]
    after_inventory = document["provenance"]["source_inventory_sha256"]
    return BaselineEvidence(
        baseline_of_record=baseline["name"],
        before_source_inventory_sha256=before_inventory,
        after_source_inventory_sha256=after_inventory,
        source_inventory_drift="yes" if before_inventory != after_inventory else "no",
    )


def verify_application_fixed_state(args: argparse.Namespace,
                                   document: dict[str, Any]) -> ApplicationFixedStateReport | None:
    if args.full_manifest is None:
        return None
    manifest = parse_kv_manifest(Path(args.full_manifest))
    enabled = manifest_application_ingress_enabled(manifest)
    application_arguments = (
        "application_before_map", "application_baseline_manifest",
        "application_baseline_artifact_manifest", "application_baseline_seal",
        "application_baseline_build_manifest",
        "application_baseline_selected_sources", "application_baseline_elf",
        "inherited_fixed_state_delta", "application_fixed_state_delta",
    )
    supplied = [name for name in application_arguments if getattr(args, name) is not None]
    if not enabled:
        if supplied:
            raise CheckFailure("provenance", "non-application build supplied application fixed-state baseline")
        return None
    missing = [name for name in application_arguments if getattr(args, name) is None]
    if missing:
        raise CheckFailure("provenance", "production wearable ingress lacks immutable application "
                           "baseline evidence: " + ",".join(missing))
    baseline_path = Path(args.application_baseline_manifest)
    baseline_map = Path(args.application_before_map)
    baseline = validate_resource_document(
        load_json(baseline_path, "application no-ingress baseline manifest"),
        "application no-ingress baseline manifest")
    artifact = parse_kv_manifest(Path(args.application_baseline_artifact_manifest))
    verify_external_artifact_seal(Path(args.application_baseline_artifact_manifest),
                                  Path(args.application_baseline_seal))
    if artifact.get("resource.manifest.name") != baseline_path.name or \
            artifact.get("resource.manifest.sha256") != sha256_file(baseline_path) or \
            artifact.get("artifact.elf.sha256") != sha256_file(Path(args.application_baseline_elf)) or \
            artifact.get("artifact.map.sha256") != sha256_file(baseline_map):
        raise CheckFailure("provenance", "application baseline artifact provenance differs")
    baseline_usage = parse_map(baseline_map)
    if baseline["ram"]["data_bytes"] != baseline_usage.data_bytes or \
            baseline["ram"]["bss_bytes"] != baseline_usage.bss_bytes or \
            baseline["provenance"]["map_sha256"] != sha256_file(baseline_map):
        raise CheckFailure("provenance", "application baseline map does not match its manifest")
    if baseline["feature"] != "FULL_TAVRN" or baseline["timer"] != document["timer"]:
        raise CheckFailure("provenance", "application baseline is not the matching FULL profile")
    baseline_build = parse_kv_manifest(Path(args.application_baseline_build_manifest))
    if artifact.get("evidence.target_config_manifest.sha256") != \
            sha256_file(Path(args.application_baseline_build_manifest)) or \
            artifact.get("evidence.selected_sources.sha256") != \
            sha256_file(Path(args.application_baseline_selected_sources)):
        raise CheckFailure("provenance", "application baseline sidecar provenance differs")
    if baseline_build.get("feature.level.effective") != "FULL_TAVRN" or \
            baseline_build.get("feature.repair.effective") != "ON" or \
            baseline_build.get("bench.mode") != "OFF" or \
            manifest_application_ingress_enabled(baseline_build):
        raise CheckFailure("provenance", "application baseline is not FULL+repair with ingress off")
    baseline_sources = selected_source_paths(Path(args.application_baseline_selected_sources))
    if any(source.startswith("app/mind_application/") for source in baseline_sources) or \
            "app/drivers/display.c" in baseline_sources or \
            any("routed_benchmark" in source for source in baseline_sources):
        raise CheckFailure("provenance", "application baseline source closure contains application state")
    if artifact.get("source.inventory.sha256") != \
            inventory_hash(Path(args.application_baseline_selected_sources)):
        raise CheckFailure("provenance", "application baseline selected-source hash differs")
    if baseline_usage.allocated_bytes >= document["fixed_state"]["after_bytes"]:
        raise CheckFailure("provenance", "application baseline does not precede production fixed state")
    measured = document["fixed_state"]["after_bytes"] - baseline_usage.allocated_bytes
    if args.application_fixed_state_delta != measured:
        raise CheckFailure("fixed_state_delta", "declared application fixed-state delta differs from "
                           "the immutable no-ingress map delta")
    inherited = args.inherited_fixed_state_delta
    declared = document["fixed_state"]["declared_delta_bytes"]
    if inherited < 0 or args.application_fixed_state_delta < 0 or \
            declared != inherited + args.application_fixed_state_delta:
        raise CheckFailure("fixed_state_delta", "declared fixed-state allowance does not separate "
                           "inherited TAVRN and measured application state")
    report = validate_application_size_report(
        load_json(Path(args.application_size_report), "application sizeof report"),
        "application sizeof report")
    baseline_symbols = nm_ram_symbols(Path(args.application_baseline_elf))
    current_symbols = nm_ram_symbols(Path(args.full_elf))
    added = {name: size for name, size in current_symbols.items()
             if name not in baseline_symbols}
    removed = {name: size for name, size in baseline_symbols.items()
               if name not in current_symbols}
    resized = {name: (baseline_symbols[name], current_symbols[name])
               for name in baseline_symbols.keys() & current_symbols.keys()
               if baseline_symbols[name] != current_symbols[name]}
    expected_added: dict[str, int] = {}
    for component, size in report["production"]["components"].items():
        symbol = "fb" if component == "display_framebuffer" else component
        if symbol in expected_added:
            raise CheckFailure("provenance", "application static symbol is accounted more than once: " +
                               symbol)
        expected_added[symbol] = size
    expected_removed = {"routed_next_submit_at": 4}
    if added != expected_added or removed != expected_removed or resized:
        raise CheckFailure("fixed_state_delta", "application ELF RAM symbol transition is not fully "
                           "accounted (added=%s removed=%s resized=%s)" %
                           (sorted(added), sorted(removed), sorted(resized)))
    replaced = expected_removed["routed_next_submit_at"]
    aggregate = report["production"]["aggregate_new_static_bytes"]
    accounted_delta = sum(added.values()) - sum(removed.values())
    if accounted_delta != aggregate - replaced:
        raise CheckFailure("fixed_state_delta", "application symbol accounting disagrees with sizeof aggregate")
    linker_placement = measured - accounted_delta
    if linker_placement < 0 or linker_placement > FIXED_DELTA_MAXIMUM or \
            linker_placement != APPLICATION_FIXED_STATE_LINKER_PLACEMENT_BYTES:
        raise CheckFailure("fixed_state_delta", "application map residual differs from the measured "
                           "%d-byte linker adjustment" %
                           APPLICATION_FIXED_STATE_LINKER_PLACEMENT_BYTES)
    return ApplicationFixedStateReport(
        baseline_usage.allocated_bytes, document["fixed_state"]["after_bytes"], measured,
        args.application_fixed_state_delta, aggregate, replaced, linker_placement)


def source_hash_inventory(path: Path) -> dict[str, str]:
    """Read the publisher's exact per-source SHA-256 inventory."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise CheckFailure("provenance", "cannot read selected-source hashes: %s" % error)
    result: dict[str, str] = {}
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64})  ([^\s]+)", line)
        if match is None:
            raise CheckFailure("provenance", "selected-source hash inventory is malformed")
        digest, source = match.groups()
        candidate = Path(source)
        if candidate.is_absolute() or ".." in candidate.parts or source in result:
            raise CheckFailure("provenance", "selected-source hash inventory is invalid")
        result[source] = digest
    if not result:
        raise CheckFailure("provenance", "selected-source hash inventory is empty")
    return result


def d2_required_sidecar(manifest_path: Path, manifest: dict[str, str], key: str) -> Path:
    """Resolve one D2 core file with mandatory name/hash/size provenance."""
    path = required_manifest_sidecar(
        manifest_path, manifest, key, key.removesuffix(".name") + ".sha256")
    size_key = key.removesuffix(".name") + ".size"
    size = manifest.get(size_key)
    if size is None or not re.fullmatch(r"0|[1-9][0-9]*", size) or \
            path.stat().st_size != int(size, 10):
        raise CheckFailure("provenance", "D2 artifact evidence size differs: " + size_key)
    return path


def d2_required_directory(manifest_path: Path, manifest: dict[str, str], key: str) -> Path:
    name = manifest.get(key)
    if name is None:
        raise CheckFailure("provenance", "D2 artifact lacks required evidence: " + key)
    relative = Path(name)
    if not name or relative.is_absolute() or ".." in relative.parts or relative == Path("."):
        raise CheckFailure("provenance", "D2 artifact evidence directory escapes artifact: " + key)
    try:
        root = manifest_path.parent.resolve(strict=True)
        resolved = (manifest_path.parent / relative).resolve(strict=True)
        resolved.relative_to(root)
    except (OSError, ValueError) as error:
        raise CheckFailure("provenance", "D2 artifact evidence directory escapes artifact: %s (%s)" %
                           (key, error))
    if not resolved.is_dir():
        raise CheckFailure("provenance", "D2 artifact evidence directory is missing: " + key)
    return resolved


def d2_relative_source_name(value: str, label: str) -> str:
    candidate = Path(value)
    if not value or candidate.is_absolute() or ".." in candidate.parts or candidate == Path(".") or \
            "\\" in value:
        raise CheckFailure("provenance", "%s has an invalid source name" % label)
    return value


def d2_inventory_paths(path: Path, label: str) -> list[str]:
    try:
        paths = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise CheckFailure("provenance", "cannot read %s: %s" % (label, error))
    if not paths or paths != sorted(paths) or len(paths) != len(set(paths)):
        raise CheckFailure("provenance", "%s must be a sorted, nonempty unique inventory" % label)
    return [d2_relative_source_name(value, label) for value in paths]


def d2_source_hash_inventory(path: Path, label: str) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise CheckFailure("provenance", "cannot read %s: %s" % (label, error))
    result: dict[str, str] = {}
    names: list[str] = []
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64})  ([^\s]+)", line)
        if match is None:
            raise CheckFailure("provenance", "%s is malformed" % label)
        digest, source = match.groups()
        source = d2_relative_source_name(source, label)
        if source in result:
            raise CheckFailure("provenance", "%s contains a duplicate source" % label)
        result[source] = digest
        names.append(source)
    if not result or names != sorted(names):
        raise CheckFailure("provenance", "%s must be sorted and nonempty" % label)
    return result


def d2_config_selected_sources(config: dict[str, str]) -> list[str]:
    indexed: dict[int, str] = {}
    for key, value in config.items():
        if key == "source.selected.sha256":
            continue
        match = re.fullmatch(r"source\.selected\.(0|[1-9][0-9]*)", key)
        if match is None:
            if key.startswith("source.selected."):
                raise CheckFailure("provenance", "D2 build-config selected-source key is malformed")
            continue
        index = int(match.group(1), 10)
        if index in indexed:
            raise CheckFailure("provenance", "D2 build-config selected-source index is duplicated")
        indexed[index] = d2_relative_source_name(value, "D2 build-config")
    if not indexed or set(indexed) != set(range(len(indexed))):
        raise CheckFailure("provenance", "D2 build-config selected-source indexes are incomplete")
    sources = [indexed[index] for index in range(len(indexed))]
    if len(sources) != len(set(sources)):
        raise CheckFailure("provenance", "D2 build-config selected-source names are duplicated")
    expected = hashlib.sha256("\n".join(sorted(sources)).encode("utf-8")).hexdigest()
    if config.get("source.selected.sha256") != expected:
        raise CheckFailure("provenance", "D2 build-config selected-source hash differs from its list")
    return sources


def d2_validate_compile_commands(path: Path) -> None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=strict_pairs)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
        raise CheckFailure("provenance", "D2 compile commands are malformed: %s" % error)
    if not isinstance(value, list) or not value or any(
            not isinstance(entry, dict) or not isinstance(entry.get("file"), str) or
            not entry.get("file") or
            (not isinstance(entry.get("command"), str) and not isinstance(entry.get("arguments"), list))
            for entry in value):
        raise CheckFailure("provenance", "D2 compile commands lack complete entries")


def d2_validate_generated_sources(directory: Path, generated_sources: set[str],
                                  hashes: dict[str, str]) -> None:
    actual: set[str] = set()
    for candidate in directory.rglob("*"):
        if not candidate.is_file():
            continue
        relative = candidate.relative_to(directory).as_posix()
        actual.add("generated/" + d2_relative_source_name(relative, "D2 generated source evidence"))
    if actual != generated_sources:
        raise CheckFailure("provenance", "D2 generated source evidence has unreferenced or missing files")
    for source in generated_sources:
        snapshot = directory / source.removeprefix("generated/")
        if sha256_file(snapshot) != hashes[source]:
            raise CheckFailure("provenance", "D2 generated source snapshot hash differs: " + source)


def d2_validate_su_evidence(manifest_path: Path, manifest: dict[str, str],
                            index: Path) -> tuple[Path, str]:
    directory = d2_required_directory(manifest_path, manifest, "resource.su.directory.name")
    if manifest.get("resource.su_glob") != manifest.get("resource.su.directory.name") + "/**/*.su":
        raise CheckFailure("provenance", "D2 stack evidence glob differs from its published directory")
    aggregate = manifest.get("resource.su.sha256")
    if aggregate is None or not re.fullmatch(r"[0-9a-f]{64}", aggregate) or \
            sha256_file(index) != aggregate:
        raise CheckFailure("provenance", "D2 stack evidence aggregate differs from its index")
    try:
        lines = index.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise CheckFailure("provenance", "cannot read D2 stack evidence index: %s" % error)
    indexed: set[str] = set()
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64})  ([^\s]+)", line)
        if match is None:
            raise CheckFailure("provenance", "D2 stack evidence index is malformed")
        expected, relative = match.groups()
        relative = d2_relative_source_name(relative, "D2 stack evidence index")
        if not relative.endswith(".su") or relative in indexed:
            raise CheckFailure("provenance", "D2 stack evidence index has an invalid duplicate entry")
        candidate = directory / relative
        try:
            resolved = candidate.resolve(strict=True)
            resolved.relative_to(directory.resolve(strict=True))
        except (OSError, ValueError) as error:
            raise CheckFailure("provenance", "D2 stack evidence index escapes its directory: %s" % error)
        if not resolved.is_file() or sha256_file(resolved) != expected:
            raise CheckFailure("provenance", "D2 stack evidence file is missing or stale: " + relative)
        indexed.add(relative)
    actual = {candidate.relative_to(directory).as_posix() for candidate in directory.rglob("*.su")
              if candidate.is_file()}
    if not indexed or actual != indexed:
        raise CheckFailure("provenance", "D2 stack evidence set is incomplete or has unreferenced files")
    return directory, aggregate


def d2_validate_resource_document(manifest: dict[str, str], evidence: dict[str, Path],
                                  checked_sources: list[str]) -> None:
    document = validate_resource_document(
        load_json(evidence["resource"], "D2 published resource manifest"),
        "D2 published resource manifest")
    if document["name"] != manifest.get("artifact.name") or document["target"] != "tavrn_routed_node" or \
            document["feature"] != "FULL_TAVRN" or document["timer"] != "BALANCED":
        raise CheckFailure("provenance", "D2 resource manifest profile differs from artifact")
    expected = {
        "source_inventory_sha256": manifest.get("source.inventory.sha256"),
        "map_sha256": sha256_file(evidence["map"]),
        "elf_sha256": sha256_file(evidence["elf"]),
        "compile_commands_sha256": sha256_file(evidence["compile_commands"]),
        "checker_sha256": sha256_file(Path(__file__).resolve()),
        "build_script_sha256": sha256_file(Path(__file__).resolve().parents[1] /
                                            "build-tavrn-ble.sh"),
    }
    if any(value is None for value in expected.values()) or \
            any(document["provenance"].get(key) != value for key, value in expected.items()):
        raise CheckFailure("provenance", "D2 resource manifest provenance differs from core evidence")
    if inventory_hash(evidence["sources"]) != manifest.get("source.inventory.sha256") or \
            checked_sources != d2_inventory_paths(evidence["sources"], "D2 selected-source inventory"):
        raise CheckFailure("provenance", "D2 selected-source inventory differs from its recorded digest")


def d2_validate_application_size_report(evidence: dict[str, Path]) -> None:
    if "application_size" not in evidence or "application_size_source" not in evidence:
        raise CheckFailure("provenance", "D2 ingress ON artifact lacks application sizeof source/report")
    report = validate_application_size_report(
        load_json(evidence["application_size"], "D2 application sizeof report"),
        "D2 application sizeof report")
    expected = {
        "probe_source_sha256": sha256_file(evidence["application_size_source"]),
        "compile_commands_sha256": sha256_file(evidence["compile_commands"]),
        "elf_sha256": sha256_file(evidence["elf"]),
        "map_sha256": sha256_file(evidence["map"]),
        "config_header_sha256": sha256_file(evidence["config_header"]),
    }
    if any(report["provenance"].get(key) != value for key, value in expected.items()):
        raise CheckFailure("provenance", "D2 application sizeof report provenance differs from core evidence")


def canonical_gate_values(path: Path, keys: set[str], label: str) -> dict[str, int]:
    """Read exactly one decimal value for every semantic resource-gate metric."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise CheckFailure("provenance", "cannot read %s: %s" % (label, error))
    values: dict[str, list[str]] = {key: [] for key in keys}
    pass_lines = 0
    for line in lines:
        if line.startswith("PASS "):
            pass_lines += 1
        key, separator, value = line.partition("=")
        if separator and key in values:
            values[key].append(value)
    if pass_lines != 1:
        raise CheckFailure("provenance", "%s must contain exactly one PASS line" % label)
    parsed: dict[str, int] = {}
    for key, raw_values in values.items():
        if len(raw_values) != 1 or not re.fullmatch(r"0|[1-9][0-9]*", raw_values[0]):
            raise CheckFailure("provenance", "%s lacks one canonical %s value" % (label, key))
        parsed[key] = int(raw_values[0], 10)
    return parsed


def finalize_resource_manifest_measurements(resource_path: Path, gate_path: Path) -> None:
    """Bind the published resource JSON to the just-completed gate's mesh result."""
    metrics = canonical_gate_values(
        gate_path, {"MESH_STACK_TOTAL_BYTES", "MESH_STACK_HEADROOM_BYTES"},
        "resource gate report")
    chain_value = canonical_gate_chain_value(
        gate_path, "STACK_CHAIN", "resource gate report")
    try:
        chain = json.loads(chain_value, object_pairs_hook=strict_pairs)
    except (json.JSONDecodeError, ValueError) as error:
        raise CheckFailure("provenance", "resource gate STACK_CHAIN is malformed: %s" % error)
    document = validate_resource_document(load_json(resource_path, "resource manifest"),
                                          "resource manifest")
    document["stack"] = {
        "root": "routed_mesh_task",
        "chain": chain,
        "total_bytes": metrics["MESH_STACK_TOTAL_BYTES"],
        "headroom_bytes": metrics["MESH_STACK_HEADROOM_BYTES"],
    }
    validate_resource_document(document, "finalized resource manifest")
    resource_path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def d2_contracts_by_root(manifest_path: Path, manifest: dict[str, str],
                         evidence: dict[str, Path]) -> dict[str, dict[str, Any]]:
    contracts: dict[str, dict[str, Any]] = {}
    for label, path in sorted(evidence.items()):
        if not label.startswith("contract."):
            continue
        raw = load_json(path, "D2 published stack-edge contract")
        root = raw.get("root")
        if not isinstance(root, str) or root in contracts:
            raise CheckFailure("provenance", "D2 stack contracts have an invalid or duplicate root")
        contract = load_stack_contract(path, root, True)
        verify_declared_leaf_evidence(contract, manifest_path, manifest)
        contracts[root] = contract
    expected = {"routed_mesh_task"}
    if manifest_application_ingress_enabled(manifest):
        expected |= {"routed_logger_task", "mind_uart_task", "mind_ui_task", "display_task"}
    if set(contracts) != expected:
        raise CheckFailure("provenance", "D2 stack contract roots are incomplete or unrecognized")
    require_rooted_stack_edges(contracts["routed_mesh_task"])
    return contracts


def d2_recompute_resource_result(manifest_path: Path, manifest: dict[str, str],
                                 evidence: dict[str, Path],
                                 off_manifest_path: Path | None = None,
                                 off_manifest: dict[str, str] | None = None,
                                 off_evidence: dict[str, Path] | None = None,
                                 off_seal: Path | None = None) -> dict[str, Any]:
    """Re-run D2's gate semantics from preserved raw artifact inputs.

    The gate log and resource JSON are publication outputs, not authorities.  A
    paired acceptance therefore reconstructs their values from the retained
    ELF/map/disassembly/.su/contracts/preprocessed assignment evidence.
    """
    resource = validate_resource_document(
        load_json(evidence["resource"], "D2 published resource manifest"),
        "D2 published resource manifest")
    config_header = evidence["config_header"]
    declarations = validate_routed_runtime_declarations(manifest, config_header)
    map_usage = parse_map(evidence["map"])
    if resource["ram"] != {
            "data_bytes": map_usage.data_bytes,
            "bss_bytes": map_usage.bss_bytes,
            "unallocated_bytes": map_usage.unallocated_bytes} or \
            resource["fixed_state"]["before_bytes"] != map_usage.allocated_bytes or \
            resource["fixed_state"]["after_bytes"] != map_usage.allocated_bytes:
        raise CheckFailure("provenance", "D2 resource RAM/fixed-state values differ from its map")
    runtime = RuntimeRamReport(
        map_usage.unallocated_bytes, declarations.runtime_ram_reserve_bytes,
        post_reserve_ram_bytes(map_usage.unallocated_bytes,
                               declarations.runtime_ram_reserve_bytes))
    contracts = d2_contracts_by_root(manifest_path, manifest, evidence)
    su_directory, _ = d2_validate_su_evidence(manifest_path, manifest, evidence["su_index"])
    su_paths = sorted(path for path in su_directory.rglob("*.su") if path.is_file())
    frames = parse_su(su_paths)
    preprocessed = evidence["preprocessed"].read_text(encoding="utf-8", errors="replace")
    validate_preprocessed_main(preprocessed, mesh_stack_bytes(config_header))
    bindings, derived = production_indirect_callback_bindings(
        evidence["preprocessed"], manifest_application_ingress_enabled(manifest))
    disassembly = disassembly_text(evidence["disassembly"], None)
    graph = call_graph(disassembly)
    sites = indirect_call_sites(disassembly)
    mesh_resolvers = {key: value for key, value in bindings.items()
                      if key.startswith("routed_cycle_operations.")}
    mesh_chain = validate_edges(
        contracts["routed_mesh_task"], mesh_resolvers, graph, frames, "routed_mesh_task",
        include_binding=True, indirect_sites=sites, derived_indirect_bindings=derived)
    mesh = stack_report("routed_mesh_task", mesh_stack_bytes(config_header), mesh_chain)
    validate_mesh_stack_threshold(mesh)
    symbols = nm_symbols(evidence["elf"])
    uses_heap = any(symbol.lstrip("_") in {"malloc", "calloc", "realloc", "free", "sbrk"}
                    or symbol in {"malloc", "calloc", "realloc", "free", "_sbrk", "sbrk"}
                    for symbol in symbols) or bool(re.search(
                        r"<(?:(?:_)?malloc|calloc|realloc|free|_sbrk|sbrk)>", disassembly))
    full_symbol_count = symbols.count("tavrn_full_maintenance_binding_tick")
    full_call_count = 1 if "tavrn_full_maintenance_binding_tick" in \
        graph.get("routed_cycle_router_tick", set()) else 0
    if uses_heap or full_symbol_count != 1 or full_call_count != 1:
        raise CheckFailure("provenance", "D2 recomputed heap/FULL binding result differs from gate policy")
    if resource["heap"]["uses_heap"] or resource["binding"] != {
            "full_symbol_count": full_symbol_count, "full_call_count": full_call_count,
            "aodv_symbol_count": 0, "aodv_reference_count": 0}:
        raise CheckFailure("provenance", "D2 resource heap/binding values differ from raw evidence")
    reports: dict[str, StackReport] = {"routed_mesh_task": mesh}
    if manifest_application_ingress_enabled(manifest):
        args = argparse.Namespace(
            application_size_report=str(evidence["application_size"]),
            application_size_source=str(evidence["application_size_source"]),
            compile_commands=str(evidence["compile_commands"]), full_elf=str(evidence["elf"]),
            full_map=str(evidence["map"]), selected_sources=str(evidence["sources"]),
        )
        validate_mind_application_profile(manifest, config_header, args)
        auxiliary = (
            ("routed_logger_task", REQUIRED_ROUTED_LOGGER_TASK_STACK_BYTES,
             {"mind_log_line_sink": "routed_mind_log_sink"}, validate_logger_stack_threshold),
            ("mind_uart_task", MIND_AUXILIARY_TASK_STACK_BYTES, {},
             validate_mind_auxiliary_stack_threshold),
            ("mind_ui_task", MIND_AUXILIARY_TASK_STACK_BYTES, {},
             validate_mind_auxiliary_stack_threshold),
            ("display_task", MIND_AUXILIARY_TASK_STACK_BYTES, {},
             validate_mind_auxiliary_stack_threshold),
        )
        for root, allocation, resolvers, threshold in auxiliary:
            chain = validate_edges(contracts[root], resolvers, graph, parse_su(su_paths), root,
                                   include_binding=True, indirect_sites=sites,
                                   derived_indirect_bindings=derived)
            report = stack_report(root, allocation, chain)
            threshold(report)
            reports[root] = report
    application_fixed: ApplicationFixedStateReport | None = None
    if manifest_application_ingress_enabled(manifest):
        if off_manifest_path is None or off_manifest is None or off_evidence is None or off_seal is None:
            raise CheckFailure("provenance", "D2 ON recomputation lacks sealed OFF application evidence")
        application_args = argparse.Namespace(
            full_manifest=str(manifest_path), full_elf=str(evidence["elf"]),
            application_before_map=str(off_evidence["map"]),
            application_baseline_manifest=str(off_evidence["resource"]),
            application_baseline_artifact_manifest=str(off_manifest_path),
            application_baseline_seal=str(off_seal),
            application_baseline_build_manifest=str(off_evidence["config_manifest"]),
            application_baseline_selected_sources=str(off_evidence["sources"]),
            application_baseline_elf=str(off_evidence["elf"]),
            inherited_fixed_state_delta=int(manifest["resource.fixed_state.inherited_delta_bytes"]),
            application_fixed_state_delta=int(manifest["resource.fixed_state.application_delta_bytes"]),
            application_size_report=str(evidence["application_size"]),
        )
        application_fixed = verify_application_fixed_state(application_args, resource)
    return {"resource": resource, "mesh": mesh, "runtime": runtime,
            "initial": initial_task_stack_report(
                declarations, frames, checker_source_root() / "app/tavrn_routed_node/src/main.c"),
            "reports": reports, "application_fixed": application_fixed}


def d2_reconcile_recomputed_result(manifest: dict[str, str], result: dict[str, Any],
                                   gate_path: Path) -> None:
    mesh: StackReport = result["mesh"]
    runtime: RuntimeRamReport = result["runtime"]
    initial: InitialTaskStackReport = result["initial"]
    expected = {
        "MESH_STACK_TOTAL_BYTES": mesh.total_bytes,
        "MESH_STACK_HEADROOM_BYTES": mesh.headroom_bytes,
        "MAP_UNALLOCATED_RAM_BYTES": runtime.map_unallocated_ram_bytes,
        "RUNTIME_RAM_RESERVE_BYTES": runtime.runtime_ram_reserve_bytes,
        "POST_RESERVE_RAM_BYTES": runtime.post_reserve_ram_bytes,
        "INITIAL_TASK_STACK_BYTES": initial.initial_task_stack_bytes,
        "INITIAL_TASK_STATIC_FRAME_BYTES": initial.static_frame_bytes,
        "INITIAL_TASK_LOGICAL_HEADROOM_BYTES": initial.logical_headroom_bytes,
    }
    if manifest_application_ingress_enabled(manifest):
        labels = {
            "routed_logger_task": "ROUTED_LOGGER_TASK",
            "mind_uart_task": "MIND_UART_TASK",
            "mind_ui_task": "MIND_UI_TASK",
            "display_task": "DISPLAY_TASK",
        }
        for root, label in labels.items():
            report: StackReport = result["reports"][root]
            expected[label + "_STACK_TOTAL_BYTES"] = report.total_bytes
            expected[label + "_STACK_HEADROOM_BYTES"] = report.headroom_bytes
        fixed: ApplicationFixedStateReport = result["application_fixed"]
        expected.update({
            "APPLICATION_FIXED_STATE_BASELINE_BYTES": fixed.baseline_allocated_bytes,
            "APPLICATION_FIXED_STATE_PRODUCTION_BYTES": fixed.production_allocated_bytes,
            "APPLICATION_FIXED_STATE_MEASURED_DELTA_BYTES": fixed.measured_delta_bytes,
            "APPLICATION_FIXED_STATE_DECLARED_DELTA_BYTES": fixed.declared_delta_bytes,
            "APPLICATION_FIXED_STATE_SIZEOF_AGGREGATE_BYTES": fixed.aggregate_new_static_bytes,
            "APPLICATION_FIXED_STATE_REPLACED_BYTES": fixed.replaced_static_bytes,
            "APPLICATION_FIXED_STATE_LINKER_PLACEMENT_BYTES": fixed.linker_placement_bytes,
        })
    actual = canonical_gate_values(gate_path, set(expected), "D2 resource gate report")
    if actual != expected:
        raise CheckFailure("provenance", "D2 resource gate metrics differ from recomputed raw evidence")
    expected_mesh_chain = canonical_stack_chain(mesh.chain, "recomputed mesh stack chain")
    if canonical_gate_chain_value(gate_path, "STACK_CHAIN", "D2 resource gate report") != \
            expected_mesh_chain:
        raise CheckFailure("provenance", "D2 resource gate STACK_CHAIN differs from recomputed raw evidence")
    if manifest_application_ingress_enabled(manifest):
        for root in ("routed_logger_task", "mind_uart_task", "mind_ui_task", "display_task"):
            key = root.upper() + "_STACK_CHAIN"
            expected_chain = canonical_stack_chain(result["reports"][root].chain,
                                                   "recomputed %s stack chain" % root)
            if canonical_gate_chain_value(gate_path, key, "D2 resource gate report") != expected_chain:
                raise CheckFailure("provenance", "D2 resource gate %s differs from recomputed raw evidence" %
                                   key)
    resource = result["resource"]
    expected_stack = {
        "root": mesh.root,
        "chain": json.loads(expected_mesh_chain),
        "total_bytes": mesh.total_bytes,
        "headroom_bytes": mesh.headroom_bytes,
    }
    if resource["stack"] != expected_stack:
        raise CheckFailure("provenance", "D2 resource stack values differ from recomputed raw evidence")


def d2_config_header_defines(path: Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise CheckFailure("provenance", "cannot read D2 generated config header: %s" % error)
    defines: dict[str, str] = {}
    for line in lines:
        match = re.fullmatch(r"#define\s+([A-Z0-9_]+)\s+(.+)", line)
        if match is not None:
            key, value = match.groups()
            if key in defines:
                raise CheckFailure("provenance", "D2 generated config header has duplicate define")
            defines[key] = value
    if not defines:
        raise CheckFailure("provenance", "D2 generated config header has no defines")
    return defines


def d2_artifact_evidence(manifest_path: Path) -> tuple[dict[str, str], dict[str, Path]]:
    manifest = parse_kv_manifest(manifest_path)
    verify_published_resource_provenance(manifest_path)
    required_values = {
        "feature.level.effective": "FULL_TAVRN",
        "feature.repair.effective": "ON",
        "build.timer_profile": "BALANCED",
        "application.node_number": "6",
        "hook.enabled": "OFF",
        "bench.mode": "OFF",
        "build.routed_mesh_task_stack_bytes": str(REQUIRED_ROUTED_MESH_TASK_STACK_BYTES),
        "resource.runtime_ram_reserve_bytes": str(REQUIRED_ROUTED_RUNTIME_RAM_RESERVE_BYTES),
        "resource.gate": "PASSED",
    }
    for key, expected in required_values.items():
        if manifest.get(key) != expected:
            raise CheckFailure("provenance", "D2 artifact semantic declaration differs: " + key)
    evidence = {
        "elf": d2_required_sidecar(manifest_path, manifest, "artifact.elf.name"),
        "map": d2_required_sidecar(manifest_path, manifest, "artifact.map.name"),
        "ninja_commands": d2_required_sidecar(manifest_path, manifest,
                                                "evidence.ninja_commands.name"),
        "compile_commands": d2_required_sidecar(manifest_path, manifest,
                                                   "evidence.compile_commands.name"),
        "disassembly": d2_required_sidecar(manifest_path, manifest, "evidence.disassembly.name"),
        "config_manifest": d2_required_sidecar(
            manifest_path, manifest, "evidence.target_config_manifest.name"),
        "config_header": d2_required_sidecar(
            manifest_path, manifest, "evidence.target_config_header.name"),
        "sources": d2_required_sidecar(manifest_path, manifest, "evidence.selected_sources.name"),
        "source_hashes": d2_required_sidecar(
            manifest_path, manifest, "evidence.selected_source_hashes.name"),
        "complete_sources": d2_required_sidecar(
            manifest_path, manifest, "evidence.complete_selected_sources.name"),
        "complete_source_hashes": d2_required_sidecar(
            manifest_path, manifest, "evidence.complete_selected_source_hashes.name"),
        "su_index": d2_required_sidecar(manifest_path, manifest, "resource.su.index.name"),
        "resource": d2_required_sidecar(manifest_path, manifest, "resource.manifest.name"),
        "gate": d2_required_sidecar(manifest_path, manifest, "resource.gate_report.name"),
        "preprocessed": d2_required_sidecar(manifest_path, manifest,
                                               "evidence.preprocessed_main.name"),
        "resource_checker": d2_required_sidecar(
            manifest_path, manifest, "evidence.resource_checker.name"),
    }
    seen: dict[Path, str] = {}
    for label, path in evidence.items():
        previous = seen.setdefault(path, label)
        if previous != label:
            raise CheckFailure("provenance", "D2 artifact reuses one sidecar for %s and %s" %
                               (previous, label))
    generated_directory = d2_required_directory(
        manifest_path, manifest, "evidence.generated_sources.directory.name")
    contract_count = manifest.get("resource.contract.count")
    if contract_count is None or not re.fullmatch(r"[1-9][0-9]*", contract_count):
        raise CheckFailure("provenance", "D2 artifact lacks published contracts")
    for index in range(int(contract_count, 10)):
        label = "contract.%d" % index
        evidence[label] = d2_required_sidecar(
            manifest_path, manifest, "resource.contract.%d.name" % index)
        previous = seen.setdefault(evidence[label], label)
        if previous != label:
            raise CheckFailure("provenance", "D2 artifact reuses one sidecar for %s and %s" %
                               (previous, label))
    if manifest_application_ingress_enabled(manifest):
        evidence["application_size"] = d2_required_sidecar(
            manifest_path, manifest, "resource.application_size.name")
        evidence["application_size_source"] = d2_required_sidecar(
            manifest_path, manifest, "resource.application_size_probe.name")
        for label in ("application_size", "application_size_source"):
            previous = seen.setdefault(evidence[label], label)
            if previous != label:
                raise CheckFailure("provenance", "D2 artifact reuses one sidecar for %s and %s" %
                                   (previous, label))
    if manifest.get("evidence.resource_checker.path") != "scripts/check_tavrn_expiry_resources.py" or \
            sha256_file(evidence["resource_checker"]) != sha256_file(Path(__file__).resolve()):
        raise CheckFailure("provenance", "D2 resource-checker evidence differs from the checked-in checker")
    if manifest.get("build.script.path") != "build-tavrn-ble.sh" or \
            manifest.get("build.script.sha256") != sha256_file(
                Path(__file__).resolve().parents[1] / "build-tavrn-ble.sh"):
        raise CheckFailure("provenance", "D2 build-script evidence differs from the checked-in publisher")
    d2_validate_compile_commands(evidence["compile_commands"])
    if not evidence["disassembly"].read_text(encoding="utf-8", errors="replace").strip():
        raise CheckFailure("provenance", "D2 disassembly evidence is empty")
    if evidence["elf"].read_bytes()[:4] != b"\x7fELF":
        raise CheckFailure("provenance", "D2 ELF evidence is malformed")
    parse_map(evidence["map"])
    validate_preprocessed_main(evidence["preprocessed"].read_text(encoding="utf-8", errors="replace"))
    checked_sources = d2_inventory_paths(evidence["sources"], "D2 selected-source inventory")
    checked_hashes = d2_source_hash_inventory(evidence["source_hashes"],
                                              "D2 selected-source hash inventory")
    if set(checked_hashes) != set(checked_sources) or any(
            sha256_file(Path(__file__).resolve().parents[1] / source) != checked_hashes[source]
            for source in checked_sources):
        raise CheckFailure("provenance", "D2 selected-source hashes differ from checked-in inputs")
    complete_sources = d2_inventory_paths(evidence["complete_sources"],
                                          "D2 complete selected-source inventory")
    complete_hashes = d2_source_hash_inventory(evidence["complete_source_hashes"],
                                               "D2 complete selected-source hash inventory")
    if set(complete_hashes) != set(complete_sources):
        raise CheckFailure("provenance", "D2 complete selected-source hashes differ from their inventory")
    config = parse_kv_manifest(evidence["config_manifest"])
    config_sources = d2_config_selected_sources(config)
    if set(config_sources) != set(complete_sources):
        raise CheckFailure("provenance", "D2 complete selected-source inventory differs from build-config")
    generated_sources = {source for source in complete_sources if source.startswith("generated/")}
    checked_complete_sources = set(complete_sources) - generated_sources
    if generated_sources != {source for source in config_sources if source.startswith("generated/")} or \
            checked_complete_sources != set(checked_sources) or any(
                complete_hashes[source] != checked_hashes[source] for source in checked_sources):
        raise CheckFailure("provenance", "D2 checked/generated source inventories are inconsistent")
    d2_validate_generated_sources(generated_directory, generated_sources, complete_hashes)
    d2_validate_su_evidence(manifest_path, manifest, evidence["su_index"])
    d2_validate_resource_document(manifest, evidence, checked_sources)
    if manifest_application_ingress_enabled(manifest):
        d2_validate_application_size_report(evidence)
    return manifest, evidence


def verify_d2_paired_acceptance(off_manifest_path: Path, off_seal: Path,
                                on_manifest_path: Path, report_path: Path) -> None:
    """Validate the explicit, non-historical D2 ingress-OFF/ON comparison."""
    off_manifest, off = d2_artifact_evidence(off_manifest_path)
    on_manifest, on = d2_artifact_evidence(on_manifest_path)
    verify_external_artifact_seal(off_manifest_path, off_seal)
    if manifest_application_ingress_enabled(off_manifest) or \
            not manifest_application_ingress_enabled(on_manifest):
        raise CheckFailure("provenance", "D2 pairing must compare ingress OFF to ingress ON")
    off_result = d2_recompute_resource_result(off_manifest_path, off_manifest, off)
    on_result = d2_recompute_resource_result(
        on_manifest_path, on_manifest, on, off_manifest_path, off_manifest, off, off_seal)
    d2_reconcile_recomputed_result(off_manifest, off_result, off["gate"])
    d2_reconcile_recomputed_result(on_manifest, on_result, on["gate"])
    off_sources = d2_inventory_paths(off["complete_sources"], "D2 OFF complete selected-source inventory")
    on_sources = d2_inventory_paths(on["complete_sources"], "D2 ON complete selected-source inventory")
    off_generated = {source for source in off_sources if source.startswith("generated/")}
    on_generated = {source for source in on_sources if source.startswith("generated/")}
    off_checked = set(off_sources) - off_generated
    on_checked = set(on_sources) - on_generated
    if len(D2_APPLICATION_SOURCE_ADDITIONS) != 15 or \
            on_checked - off_checked != D2_APPLICATION_SOURCE_ADDITIONS or off_checked - on_checked:
        raise CheckFailure("provenance", "D2 pairing has invalid source additions or removals")
    if off_generated != on_generated:
        raise CheckFailure("provenance", "D2 pairing added or removed a generated source")
    off_hashes = d2_source_hash_inventory(off["complete_source_hashes"],
                                          "D2 OFF complete selected-source hash inventory")
    on_hashes = d2_source_hash_inventory(on["complete_source_hashes"],
                                         "D2 ON complete selected-source hash inventory")
    if set(off_hashes) != set(off_sources) or set(on_hashes) != set(on_sources) or \
            any(off_hashes[source] != on_hashes.get(source) for source in off_checked):
        raise CheckFailure("provenance", "D2 pairing changed a common selected-source hash")
    if set(on_hashes) - set(off_hashes) != D2_APPLICATION_SOURCE_ADDITIONS:
        raise CheckFailure("provenance", "D2 pairing source-hash additions differ from source additions")
    off_config = parse_kv_manifest(off["config_manifest"])
    on_config = parse_kv_manifest(on["config_manifest"])
    if set(d2_config_selected_sources(off_config)) != set(off_sources) or \
            set(d2_config_selected_sources(on_config)) != set(on_sources):
        raise CheckFailure("provenance", "D2 build-config source list differs from complete inventory")
    off_semantic = {key: value for key, value in off_config.items()
                    if not key.startswith("source.selected.") and key != "source.selected.sha256"}
    on_semantic = {key: value for key, value in on_config.items()
                   if not key.startswith("source.selected.") and key != "source.selected.sha256"}
    expected_changes = {
        "build.implemented_capabilities": (
            off_semantic.get("build.implemented_capabilities", "") + ",mind-root-plane"),
        "application.wearable_ingress.requested": "ON",
        "application.wearable_ingress.effective": "ON",
        "application.root_plane.effective": "ON",
    }
    differences = {key for key in set(off_semantic) | set(on_semantic)
                   if off_semantic.get(key) != on_semantic.get(key)}
    if differences != set(expected_changes) or any(
            on_semantic.get(key) != value or off_semantic.get(key) in (None, "ON")
            for key, value in expected_changes.items()):
        raise CheckFailure("provenance", "D2 build-config semantic differences are not exact")
    off_header = d2_config_header_defines(off["config_header"])
    on_header = d2_config_header_defines(on["config_header"])
    header_differences = {key for key in set(off_header) | set(on_header)
                          if off_header.get(key) != on_header.get(key)}
    if header_differences != {"TRON_BUILD_ENABLE_WEARABLE_INGRESS"} or \
            off_header.get("TRON_BUILD_ENABLE_WEARABLE_INGRESS") != "0" or \
            on_header.get("TRON_BUILD_ENABLE_WEARABLE_INGRESS") != "1":
        raise CheckFailure("provenance", "D2 generated config header differs outside ingress 0->1")
    if any(off_hashes[source] != on_hashes[source] for source in off_generated):
        raise CheckFailure("provenance", "D2 generated source content differs outside the exact generated "
                           "header/build-manifest whitelist")
    for key in ("source.commit", "source.dirty", "source.tree", "source.submodule.sha256",
                 "source.worktree_content.sha256", "build.compiler.sha256", "build.cmake.sha256",
                 "build.ninja.sha256", "build.script.sha256", "evidence.resource_checker.sha256"):
        if off_manifest.get(key) != on_manifest.get(key):
            raise CheckFailure("provenance", "D2 artifact source/build state differs: " + key)
    if off_manifest.get("source.worktree_content.scope") != WORKTREE_CONTENT_DIGEST_SCOPE or \
            on_manifest.get("source.worktree_content.scope") != WORKTREE_CONTENT_DIGEST_SCOPE or \
            off_manifest.get("source.worktree_content.sha256") != \
            worktree_content_digest():
        raise CheckFailure("provenance", "D2 worktree content digest is stale or malformed")
    if "application_size" not in on:
        raise CheckFailure("provenance", "D2 ingress ON artifact lacks application sizeof evidence")
    report_values = {
        "schema": D2_PAIRED_ACCEPTANCE_SCHEMA,
        "acceptance.scope": "D2_APPLICATION_INCREMENTAL",
        "historical_expiry_fixed_state": "USER_WAIVED_NOT_REPRODUCED",
        "off.artifact_manifest.name": off_manifest_path.name,
        "off.artifact_manifest.sha256": sha256_file(off_manifest_path),
        "off.external_seal.name": off_seal.name,
        "off.external_seal.sha256": sha256_file(off_seal),
        "on.artifact_manifest.name": on_manifest_path.name,
        "on.artifact_manifest.sha256": sha256_file(on_manifest_path),
    }
    for prefix, evidence in (("off", off), ("on", on)):
        for name, path in sorted(evidence.items()):
            report_values["%s.%s.name" % (prefix, name)] = path.name
            report_values["%s.%s.sha256" % (prefix, name)] = sha256_file(path)
    for prefix, manifest in (("off", off_manifest), ("on", on_manifest)):
        report_values["%s.resource.su.sha256" % prefix] = manifest["resource.su.sha256"]
        report_values["%s.source.worktree_content.sha256" % prefix] = \
            manifest["source.worktree_content.sha256"]
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text("".join("%s=%s\n" % item for item in sorted(report_values.items())),
                           encoding="utf-8")


def build_evidence_document(args: argparse.Namespace) -> tuple[dict[str, Any],
                                                                StackReport | None]:
    if args.full_map is None or args.full_elf is None:
        raise CheckFailure("arguments", "resource evidence requires --full-map and --full-elf")
    map_path, elf_path = Path(args.full_map), Path(args.full_elf)
    map_usage = parse_map(map_path)
    selected_hash = "0" * 64
    if args.selected_sources is not None:
        selected_hash = inventory_hash(Path(args.selected_sources))
    manifest: dict[str, str] = {}
    manifest_path: Path | None = None
    benchmark_build = False
    logger_report: StackReport | None = None
    config_header_path = Path(args.config_header) if args.config_header else None
    if args.full_manifest is not None:
        manifest_path = Path(args.full_manifest)
        manifest = parse_kv_manifest(manifest_path)
        artifact_hash = manifest.get("artifact.elf.sha256")
        if artifact_hash is not None and artifact_hash != sha256_file(elf_path):
            raise CheckFailure("provenance", "ELF hash differs from artifact manifest")
        if "source.inventory.sha256" in manifest and manifest["source.inventory.sha256"] != selected_hash:
            raise CheckFailure("provenance", "selected-source inventory hash differs from artifact manifest")
        verify_manifest_hash(manifest, "artifact.map.sha256", map_path)
        if args.compile_commands:
            verify_manifest_hash(manifest, "build.compile_commands.sha256",
                                 Path(args.compile_commands))
            verify_manifest_hash(manifest, "evidence.compile_commands.sha256",
                                 Path(args.compile_commands))
        if args.disassembly:
            verify_manifest_hash(manifest, "evidence.disassembly.sha256", Path(args.disassembly))
        if config_header_path is None:
            config_header_path = manifest_sidecar(
                manifest_path, manifest, "evidence.target_config_header.name")
            if config_header_path is not None:
                verify_manifest_hash(manifest, "evidence.target_config_header.sha256",
                                     config_header_path)
        if config_header_path is None:
            raise CheckFailure("provenance", "routed build manifest lacks generated config header evidence")
        declaration = validate_routed_runtime_declarations(manifest, config_header_path)
        if manifest.get("resource.gate") == "PASSED":
            verify_published_resource_provenance(manifest_path)
        benchmark_build = manifest.get("bench.mode") == "ON"
        if benchmark_build and args.su_glob:
            require_benchmark_logger_evidence(manifest, args)
        if args.selected_sources is not None:
            require_initial_task_source_provenance(Path(args.selected_sources))
        if args.compile_commands:
            require_routed_initial_task_compile_definitions(
                Path(args.compile_commands), declaration.initial_task_stack_bytes)
            require_routed_logger_task_compile_definition(
                Path(args.compile_commands), declaration.logger_task_stack_bytes)
        validate_mind_application_profile(manifest, config_header_path, args)
    uses_heap = False
    symbols = nm_symbols(elf_path)
    heap_names = ("malloc", "calloc", "realloc", "free", "_sbrk", "sbrk")
    if any(symbol.lstrip("_") in heap_names or symbol in heap_names for symbol in symbols):
        uses_heap = True
    disassembly = disassembly_text(Path(args.disassembly) if args.disassembly else None, elf_path)
    if re.search(r"<(?:(?:_)?malloc|calloc|realloc|free|_sbrk|sbrk)>", disassembly):
        uses_heap = True
    full_symbol_count = symbols.count("tavrn_full_maintenance_binding_tick")
    graph = call_graph(disassembly)
    sites = indirect_call_sites(disassembly)
    production_indirect_bindings: dict[str, str] = {}
    production_derived_indirect_bindings: tuple[IndirectCallBinding, ...] = ()
    if manifest and manifest.get("feature.level.effective") == "FULL_TAVRN" and \
            args.su_glob and args.required_edge_manifest and args.stack_root:
        if not args.compile_commands or not args.main_source or not args.preprocessed_main_out:
            raise CheckFailure("stack", "production wearable ingress stack analysis lacks preprocessed "
                               "callback assignments")
        preprocessed = preprocess_main(Path(args.compile_commands), Path(args.main_source),
                                       Path(args.preprocessed_main_out))
        validate_preprocessed_main(preprocessed, mesh_stack_bytes(config_header_path))
        production_indirect_bindings, production_derived_indirect_bindings = \
            production_indirect_callback_bindings(
                Path(args.preprocessed_main_out), manifest_application_ingress_enabled(manifest))
    full_call_count = 1 if "tavrn_full_maintenance_binding_tick" in \
        graph.get("routed_cycle_router_tick", set()) else 0
    if args.require_binding_call and (full_symbol_count != 1 or full_call_count != 1):
        raise CheckFailure("binding", "FULL binding symbol/call is absent from fresh evidence")
    stack_bytes = 0
    if args.config_header:
        stack_bytes = mesh_stack_bytes(config_header_path)
    elif manifest_path is not None:
        if config_header_path is not None:
            stack_bytes = mesh_stack_bytes(config_header_path)
    stack = {"root": args.stack_root or "routed_mesh_task", "chain": [], "total_bytes": 0,
             "headroom_bytes": stack_bytes}
    if args.su_glob and args.required_edge_manifest and args.stack_root:
        contract = load_stack_contract(
            Path(args.required_edge_manifest), args.stack_root,
            bool(manifest and manifest.get("feature.level.effective") == "FULL_TAVRN" and
                 args.stack_root == "routed_mesh_task"))
        require_rooted_stack_edges(contract)
        verify_declared_leaf_evidence(contract, manifest_path, manifest)
        su_paths = [Path(path) for path in sorted(glob.glob(args.su_glob, recursive=True))]
        su_digest = stack_usage_hash(su_paths, args.su_glob)
        if manifest:
            expected_su = manifest.get("evidence.stack_usage.sha256") or \
                manifest.get("resource.su.sha256")
            if expected_su is not None and expected_su != su_digest:
                raise CheckFailure("provenance", "published .su evidence hash differs from build manifest")
        frames = parse_su(su_paths)
        resolvers: dict[str, str] = {}
        for key, target in production_indirect_bindings.items():
            if key.startswith("routed_cycle_operations."):
                resolvers[key] = target
        for value in args.resolve_operation_edge or []:
            if value.count("=") != 1:
                raise CheckFailure("stack", "invalid --resolve-operation-edge")
            key, target = value.split("=", 1)
            if not key or not target:
                raise CheckFailure("stack", "duplicate/invalid operation edge resolver")
            if key in resolvers:
                if resolvers[key] != target or key not in production_indirect_bindings:
                    raise CheckFailure("stack", "duplicate/invalid operation edge resolver")
                continue
            resolvers[key] = target
        if stack_bytes == 0:
            raise CheckFailure("stack", "stack analysis requires generated config stack evidence")
        chain = validate_edges(contract, resolvers, graph, frames, args.stack_root,
                               include_binding=not args.stack_baseline_only,
                               indirect_sites=sites if production_derived_indirect_bindings else None,
                               derived_indirect_bindings=production_derived_indirect_bindings)
        mesh_report = stack_report(args.stack_root, stack_bytes, chain)
        validate_mesh_stack_threshold(mesh_report)
        stack = {"root": mesh_report.root,
                 "chain": [{"function": frame.identity, "frame_bytes": frame.frame_bytes}
                           for frame in mesh_report.chain],
                 "total_bytes": mesh_report.total_bytes,
                 "headroom_bytes": mesh_report.headroom_bytes}
        if benchmark_build:
            logger_contract = load_stack_contract(
                Path(args.logger_required_edge_manifest), args.logger_stack_root)
            require_rooted_stack_edges(logger_contract, REQUIRED_LOGGER_STACK_EDGES)
            verify_declared_leaf_evidence(logger_contract, manifest_path, manifest)
            logger_frames = parse_su(su_paths)
            logger_chain = validate_edges(
                logger_contract, {}, graph, logger_frames, args.logger_stack_root,
                include_binding=True)
            logger_report = stack_report(
                args.logger_stack_root, logger_stack_bytes(config_header_path), logger_chain)
    aodv_symbols = 0
    aodv_refs = 0
    if args.aodv_elf:
        aodv_path = Path(args.aodv_elf)
        aodv_symbols = nm_symbols(aodv_path).count("tavrn_full_maintenance_binding_tick")
        aodv_dis = disassembly_text(Path(args.aodv_disassembly) if args.aodv_disassembly else None,
                                    aodv_path)
        aodv_refs = aodv_dis.count("<tavrn_full_maintenance_binding_tick>")
    compiler_hash = sha256_file(Path(args.compile_commands)) if args.compile_commands else "0" * 64
    checker_hash = sha256_file(Path(__file__).resolve())
    build_script = Path(__file__).resolve().parents[1] / "build-tavrn-ble.sh"
    build_hash = sha256_file(build_script) if build_script.is_file() else "0" * 64
    document = {
        "schema": SCHEMA, "name": args.name or "generated", "target": args.target,
        "feature": args.feature, "timer": args.timer,
        "ram": {"data_bytes": map_usage.data_bytes, "bss_bytes": map_usage.bss_bytes,
                "unallocated_bytes": map_usage.unallocated_bytes},
        "fixed_state": {"before_bytes": map_usage.allocated_bytes,
                        "after_bytes": map_usage.allocated_bytes,
                        "declared_delta_bytes": args.declared_fixed_state_delta,
                        "unexplained_delta_bytes": 0},
        "stack": stack, "heap": {"uses_heap": uses_heap},
        "binding": {"full_symbol_count": full_symbol_count, "full_call_count": full_call_count,
                    "aodv_symbol_count": aodv_symbols, "aodv_reference_count": aodv_refs},
        "capture": {"supplied": False, "duration_seconds": 0, "complete_passes": 0,
                     "hard_selected_demand_deferred_passes": 0,
                     "max_scheduler_gap_ms": 0, "unavailable_count": 0, "faults": 0,
                     "targeted_controls": 0, "rreq_controls": 0,
                     "tc_expiry_controls": 0, "telemetry_dropped": 0},
        "provenance": {"source_inventory_sha256": selected_hash,
                       "map_sha256": sha256_file(map_path), "elf_sha256": sha256_file(elf_path),
                       "compile_commands_sha256": compiler_hash, "checker_sha256": checker_hash,
                       "build_script_sha256": build_hash},
        "expected_failure": None, "preprocessed_main": {"source": ""},
    }
    return validate_resource_document(document, "generated resource manifest"), logger_report


def routed_runtime_reports(args: argparse.Namespace) -> tuple[RuntimeRamReport,
                                                               InitialTaskStackReport | None]:
    if args.full_manifest is None or args.full_map is None:
        raise CheckFailure("arguments", "routed runtime reports require map and build manifest")
    manifest_path = Path(args.full_manifest)
    manifest = parse_kv_manifest(manifest_path)
    config_header = Path(args.config_header) if args.config_header else manifest_sidecar(
        manifest_path, manifest, "evidence.target_config_header.name")
    if config_header is None:
        raise CheckFailure("provenance", "routed build manifest lacks generated config header evidence")
    declaration = validate_routed_runtime_declarations(manifest, config_header)
    map_usage = parse_map(Path(args.full_map))
    ram_report = RuntimeRamReport(
        map_usage.unallocated_bytes,
        declaration.runtime_ram_reserve_bytes,
        post_reserve_ram_bytes(map_usage.unallocated_bytes,
                               declaration.runtime_ram_reserve_bytes))
    initial_task_report: InitialTaskStackReport | None = None
    if args.require_binding_call:
        su_paths = [Path(path) for path in sorted(glob.glob(args.su_glob, recursive=True))]
        frames = parse_su(su_paths)
        initial_task_report = initial_task_stack_report(
            declaration, frames, Path(args.main_source))
    return ram_report, initial_task_report


def validate_live_evidence(args: argparse.Namespace,
                            document: dict[str, Any]) -> tuple[BaselineEvidence | None,
                                                                 RuntimeRamReport | None,
                                                                 InitialTaskStackReport | None,
                                                                 StackReport | None,
                                                                 tuple[StackReport, ...],
                                                                 ApplicationFixedStateReport | None]:
    logger_stack: StackReport | None = None
    if args.stack_baseline_only and args.require_binding_call:
        raise CheckFailure("arguments", "stack/baseline-only mode cannot require the binding")
    if args.stack_baseline_only:
        required = ("resource_manifest", "full_elf", "full_map", "full_manifest", "before_map",
                    "baseline_manifest", "baseline_sha256", "selected_sources", "su_glob",
                    "stack_root", "required_edge_manifest", "disassembly", "compile_commands",
                    "config_header", "main_source", "preprocessed_main_out")
        missing = [name for name in required if not getattr(args, name)]
        if missing:
            raise CheckFailure("arguments", "stack/baseline-only mode lacks " + ",".join(missing))
    if args.require_binding_call and (not args.compile_commands or not args.main_source or
                                       not args.preprocessed_main_out or not args.su_glob or
                                       not args.stack_root or not args.required_edge_manifest or
                                       not args.disassembly or not args.config_header or
                                       not args.full_manifest):
        raise CheckFailure("arguments", "binding gate requires main, stack, FULL and AODV evidence")
    if args.resource_manifest and args.full_map and args.full_elf:
        observed, logger_stack = build_evidence_document(args)
        declared = document["fixed_state"]["declared_delta_bytes"]
        for key in ("ram", "stack", "heap", "binding", "provenance"):
            document[key] = observed[key]
        document["fixed_state"]["after_bytes"] = observed["fixed_state"]["after_bytes"]
        document["fixed_state"]["before_bytes"] = observed["fixed_state"]["before_bytes"]
        document["fixed_state"]["declared_delta_bytes"] = declared
        if observed["provenance"]["map_sha256"] != \
           load_json(Path(args.resource_manifest), "resource manifest")["provenance"]["map_sha256"] or \
           observed["provenance"]["elf_sha256"] != \
           load_json(Path(args.resource_manifest), "resource manifest")["provenance"]["elf_sha256"]:
            raise CheckFailure("provenance", "resource manifest artifact hashes are stale")
    if args.hardware_capture:
        if not args.capture_artifact_manifest:
            raise CheckFailure("arguments", "hardware capture requires --capture-artifact-manifest")
        capture_manifest_path = Path(args.capture_artifact_manifest)
        document["capture"] = load_capture(Path(args.hardware_capture), capture_manifest_path)
        if args.full_manifest:
            verify_capture_artifact_compatibility(Path(args.full_manifest),
                                                  capture_manifest_path)
    elif args.capture_artifact_manifest:
        if not args.full_manifest:
            raise CheckFailure("arguments", "capture artifact validation requires --full-manifest")
        verify_capture_artifact_compatibility(Path(args.full_manifest),
                                              Path(args.capture_artifact_manifest))
    if not args.stack_baseline_only and (args.compile_commands or args.main_source):
        if not args.compile_commands or not args.main_source or not args.preprocessed_main_out:
            raise CheckFailure("arguments", "main validation requires compile commands, main source and output")
        source = preprocess_main(Path(args.compile_commands), Path(args.main_source),
                                 Path(args.preprocessed_main_out))
        validate_preprocessed_main(source)
    runtime_ram: RuntimeRamReport | None = None
    initial_task_stack: InitialTaskStackReport | None = None
    application_stacks: tuple[StackReport, ...] = ()
    application_fixed_state: ApplicationFixedStateReport | None = None
    if args.full_manifest is not None and args.full_map is not None:
        runtime_ram, initial_task_stack = routed_runtime_reports(args)
        manifest_path = Path(args.full_manifest)
        manifest = parse_kv_manifest(manifest_path)
        if not args.stack_baseline_only:
            application_stacks = mind_application_stack_reports(args, manifest_path, manifest)
            application_fixed_state = verify_application_fixed_state(args, document)
    return (verify_baseline(args, document), runtime_ram, initial_task_stack, logger_stack,
            application_stacks, application_fixed_state)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture")
    parser.add_argument("--expect-fail", action="store_true")
    parser.add_argument("--emit-resource-manifest")
    parser.add_argument("--finalize-resource-manifest", action="store_true")
    parser.add_argument("--resource-gate-report")
    parser.add_argument("--verify-published-provenance")
    parser.add_argument("--print-worktree-content-digest", action="store_true")
    parser.add_argument("--seal-passed-artifact-manifest")
    parser.add_argument("--seal-output")
    parser.add_argument("--current-resource-gate", action="store_true")
    parser.add_argument("--emit-application-size-report")
    parser.add_argument("--application-size-source")
    parser.add_argument("--application-size-report")
    parser.add_argument("--resource-manifest")
    parser.add_argument("--name")
    parser.add_argument("--target", default="tavrn_routed_node")
    parser.add_argument("--feature", default="FULL_TAVRN")
    parser.add_argument("--timer", default="FAST_TEST")
    parser.add_argument("--full-elf", "--elf", dest="full_elf")
    parser.add_argument("--full-map", "--after-map", dest="full_map")
    parser.add_argument("--full-manifest")
    parser.add_argument("--before-map")
    parser.add_argument("--baseline-manifest")
    parser.add_argument("--baseline-sha256")
    parser.add_argument("--declared-fixed-state-delta", type=int, default=0)
    parser.add_argument("--inherited-fixed-state-delta", type=int)
    parser.add_argument("--application-fixed-state-delta", type=int)
    parser.add_argument("--application-before-map")
    parser.add_argument("--application-baseline-manifest")
    parser.add_argument("--application-baseline-artifact-manifest")
    parser.add_argument("--application-baseline-seal")
    parser.add_argument("--application-baseline-build-manifest")
    parser.add_argument("--application-baseline-selected-sources")
    parser.add_argument("--application-baseline-elf")
    parser.add_argument("--selected-sources")
    parser.add_argument("--su-glob")
    parser.add_argument("--stack-root")
    parser.add_argument("--required-edge-manifest")
    parser.add_argument("--logger-stack-root")
    parser.add_argument("--logger-required-edge-manifest")
    parser.add_argument("--mind-logger-required-edge-manifest")
    parser.add_argument("--mind-uart-required-edge-manifest")
    parser.add_argument("--mind-ui-required-edge-manifest")
    parser.add_argument("--mind-display-required-edge-manifest")
    parser.add_argument("--resolve-operation-edge", action="append")
    parser.add_argument("--disassembly")
    parser.add_argument("--compile-commands")
    parser.add_argument("--config-header")
    parser.add_argument("--main-source")
    parser.add_argument("--preprocessed-main-out")
    parser.add_argument("--aodv-elf")
    parser.add_argument("--aodv-map")
    parser.add_argument("--aodv-manifest")
    parser.add_argument("--aodv-compile-commands")
    parser.add_argument("--aodv-disassembly")
    parser.add_argument("--require-binding-call", action="store_true")
    parser.add_argument("--stack-baseline-only", action="store_true")
    parser.add_argument("--hardware-capture")
    parser.add_argument("--capture-artifact-manifest")
    parser.add_argument("--d2-paired-acceptance-report")
    parser.add_argument("--d2-off-artifact-manifest")
    parser.add_argument("--d2-off-seal")
    parser.add_argument("--d2-on-artifact-manifest")
    return parser.parse_args(argv)


def has_live_artifact_arguments(args: argparse.Namespace) -> bool:
    return any((
        args.full_elf, args.full_map, args.full_manifest, args.before_map,
        args.baseline_manifest, args.baseline_sha256, args.selected_sources,
        args.su_glob, args.stack_root, args.required_edge_manifest,
        args.logger_stack_root, args.logger_required_edge_manifest,
        args.mind_logger_required_edge_manifest, args.mind_uart_required_edge_manifest,
        args.mind_ui_required_edge_manifest, args.mind_display_required_edge_manifest,
        args.application_size_source, args.application_size_report,
        args.application_before_map, args.application_baseline_manifest,
        args.application_baseline_artifact_manifest, args.application_baseline_seal,
        args.application_baseline_build_manifest,
        args.application_baseline_selected_sources, args.application_baseline_elf,
        args.inherited_fixed_state_delta, args.application_fixed_state_delta,
        args.resolve_operation_edge, args.disassembly, args.compile_commands,
        args.config_header, args.main_source, args.preprocessed_main_out,
        args.aodv_elf, args.aodv_map, args.aodv_manifest,
        args.aodv_compile_commands, args.aodv_disassembly,
        args.require_binding_call, args.hardware_capture,
        args.capture_artifact_manifest,
    ))


def require_complete_live_evidence(args: argparse.Namespace) -> None:
    required = (
        "full_elf", "full_map", "full_manifest", "before_map",
        "baseline_manifest", "baseline_sha256", "selected_sources", "su_glob",
        "stack_root", "required_edge_manifest", "disassembly", "compile_commands",
        "config_header", "main_source", "preprocessed_main_out",
    )
    missing = [name for name in required if not getattr(args, name)]
    if not args.require_binding_call:
        missing.append("require_binding_call")
    if missing:
        raise CheckFailure("arguments", "incomplete fresh resource evidence: " +
                           ",".join(missing))


def require_current_resource_evidence(args: argparse.Namespace) -> None:
    """D2's explicit current gate intentionally has no historical baseline."""
    required = (
        "resource_manifest", "full_elf", "full_map", "full_manifest", "selected_sources",
        "su_glob", "stack_root", "required_edge_manifest", "disassembly", "compile_commands",
        "config_header", "main_source", "preprocessed_main_out",
    )
    missing = [name for name in required if not getattr(args, name)]
    if not args.require_binding_call:
        missing.append("require_binding_call")
    if missing:
        raise CheckFailure("arguments", "incomplete current resource evidence: " +
                           ",".join(missing))


def run(args: argparse.Namespace) -> RunResult:
    live_fixture: dict[str, Any] | None = None
    baseline: BaselineEvidence | None = None
    runtime_ram: RuntimeRamReport | None = None
    initial_task_stack: InitialTaskStackReport | None = None
    logger_stack: StackReport | None = None
    application_stacks: tuple[StackReport, ...] = ()
    application_fixed_state: ApplicationFixedStateReport | None = None
    if args.declared_fixed_state_delta < 0:
        raise CheckFailure("arguments", "declared fixed-state delta must be nonnegative")
    if args.inherited_fixed_state_delta is not None and args.inherited_fixed_state_delta < 0 or \
            args.application_fixed_state_delta is not None and \
            args.application_fixed_state_delta < 0:
        raise CheckFailure("arguments", "application fixed-state deltas must be nonnegative")
    if args.print_worktree_content_digest:
        if args.emit_application_size_report or args.emit_resource_manifest or args.fixture or \
                args.resource_manifest or args.verify_published_provenance or \
                args.seal_passed_artifact_manifest or args.seal_output or \
                args.d2_paired_acceptance_report or has_live_artifact_arguments(args):
            raise CheckFailure("arguments", "worktree digest mode cannot consume resource evidence")
        return RunResult(worktree_content_digest(), None, None,
                         None, None, None, (), None)
    if args.finalize_resource_manifest:
        if not args.resource_manifest or not args.resource_gate_report or args.fixture or \
                args.emit_resource_manifest or args.emit_application_size_report or \
                args.verify_published_provenance or args.seal_passed_artifact_manifest or \
                args.seal_output or args.d2_paired_acceptance_report or has_live_artifact_arguments(args):
            raise CheckFailure("arguments", "resource finalization requires exactly manifest and gate report")
        finalize_resource_manifest_measurements(
            Path(args.resource_manifest), Path(args.resource_gate_report))
        return RunResult("RESOURCE_MANIFEST_FINALIZED", None, None, None, None, None, (), None)
    if args.emit_application_size_report:
        if args.emit_resource_manifest or args.fixture or args.resource_manifest:
            raise CheckFailure("arguments", "application sizeof emit mode cannot consume another manifest mode")
        report = build_application_size_report(args)
        target = Path(args.emit_application_size_report)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return RunResult("APPLICATION_SIZE_EMITTED", None, None, None, None, None, (), None)
    if args.verify_published_provenance:
        if args.emit_resource_manifest or args.fixture or args.resource_manifest or \
                has_live_artifact_arguments(args):
            raise CheckFailure("arguments", "published provenance mode cannot consume resource evidence")
        verify_published_resource_provenance(Path(args.verify_published_provenance))
        return RunResult("PUBLISHED_PROVENANCE_PASS", None, None, None, None, None, (), None)
    if args.seal_passed_artifact_manifest or args.seal_output:
        if not args.seal_passed_artifact_manifest or not args.seal_output or \
                args.emit_resource_manifest or args.fixture or args.resource_manifest or \
                args.current_resource_gate or args.stack_baseline_only or has_live_artifact_arguments(args):
            raise CheckFailure("arguments", "artifact sealing requires exactly manifest and output modes")
        artifact_manifest = Path(args.seal_passed_artifact_manifest)
        manifest = parse_kv_manifest(artifact_manifest)
        if manifest.get("resource.gate") != "PASSED":
            raise CheckFailure("provenance", "only a passed resource artifact manifest may be sealed")
        verify_published_resource_provenance(artifact_manifest)
        seal = Path(args.seal_output)
        try:
            seal.resolve().parent.relative_to(artifact_manifest.resolve().parent)
        except ValueError:
            pass
        else:
            raise CheckFailure("provenance", "external artifact seal must be outside the artifact directory")
        seal.parent.mkdir(parents=True, exist_ok=True)
        seal.write_text(sha256_file(artifact_manifest) + "  " + artifact_manifest.name + "\n",
                        encoding="utf-8")
        return RunResult("ARTIFACT_SEALED", None, None, None, None, None, (), None)
    if args.d2_paired_acceptance_report:
        if args.fixture or args.resource_manifest or args.emit_resource_manifest or \
                args.current_resource_gate or args.stack_baseline_only or has_live_artifact_arguments(args):
            raise CheckFailure("arguments", "D2 paired acceptance cannot consume live resource arguments")
        missing = [name for name in ("d2_off_artifact_manifest", "d2_off_seal",
                                     "d2_on_artifact_manifest") if not getattr(args, name)]
        if missing:
            raise CheckFailure("arguments", "D2 paired acceptance lacks " + ",".join(missing))
        verify_d2_paired_acceptance(
            Path(args.d2_off_artifact_manifest), Path(args.d2_off_seal),
            Path(args.d2_on_artifact_manifest), Path(args.d2_paired_acceptance_report))
        return RunResult("D2_PAIRED_ACCEPTANCE_PASS", None, None, None, None, None, (), None)
    if args.stack_baseline_only and args.fixture:
        raise CheckFailure("arguments", "stack/baseline-only mode requires fresh resource evidence")
    live_artifacts = has_live_artifact_arguments(args)
    if args.current_resource_gate and (args.stack_baseline_only or args.before_map or
                                       args.baseline_manifest or args.baseline_sha256):
        raise CheckFailure("arguments", "current resource gate cannot consume historical baseline evidence")
    if args.current_resource_gate:
        require_current_resource_evidence(args)
    elif not args.emit_resource_manifest and not args.stack_baseline_only and live_artifacts:
        require_complete_live_evidence(args)
    if args.fixture:
        fixture = validate_resource_document(load_json(Path(args.fixture), "fixture"), "fixture")
        if args.full_map and args.full_elf:
            # Fixture use with fresh firmware evidence is intentionally two-part:
            # validate the real ELF/map/main/provenance first, then inject only
            # deterministic threshold values.  A failing fixture therefore
            # cannot conceal a missing binding or arbitrary CLI failure.
            document, logger_stack = build_evidence_document(args)
            live_fixture = fixture
        else:
            document = fixture
            if document["preprocessed_main"]["source"]:
                validate_preprocessed_main(document["preprocessed_main"]["source"])
    elif args.resource_manifest:
        document = validate_resource_document(load_json(Path(args.resource_manifest), "resource manifest"),
                                              "resource manifest")
    else:
        document, logger_stack = build_evidence_document(args)
    if args.emit_resource_manifest:
        if args.fixture or args.resource_manifest:
            raise CheckFailure("arguments", "emit mode cannot consume a fixture/manifest")
        target = Path(args.emit_resource_manifest)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return RunResult("EMITTED", None, None, None, None, logger_stack, (), None)
    live_invocation = args.current_resource_gate or args.stack_baseline_only or live_artifacts
    if live_invocation:
        (baseline, runtime_ram, initial_task_stack, logger_stack, application_stacks,
         application_fixed_state) = validate_live_evidence(args, document)
    if live_fixture is not None:
        # The real artifact must satisfy every normal gate before a synthetic
        # threshold-failing fixture is allowed to prove its one named boundary.
        validate_thresholds(document, args.require_binding_call, bool(args.aodv_elf))
        expected = live_fixture["expected_failure"]
        if expected is not None:
            if expected == "stack_total":
                document["stack"]["total_bytes"] = live_fixture["stack"]["total_bytes"]
            elif expected == "ram_unallocated":
                document["ram"]["unallocated_bytes"] = live_fixture["ram"]["unallocated_bytes"]
            elif expected == "fixed_state_delta":
                document["fixed_state"]["unexplained_delta_bytes"] = \
                    live_fixture["fixed_state"]["unexplained_delta_bytes"]
            elif expected == "hardware_capture":
                document["capture"] = live_fixture["capture"]
    binding_required = not args.stack_baseline_only and \
        (args.require_binding_call or document["expected_failure"] == "binding")
    validate_thresholds(document, binding_required, bool(args.aodv_elf))
    if logger_stack is not None:
        validate_logger_stack_threshold(logger_stack)
    stack_chain = canonical_stack_chain(document["stack"]["chain"], "resource stack chain") \
        if document["stack"]["chain"] else None
    if live_invocation:
        verdict = "PASS"
    elif args.fixture:
        verdict = "FIXTURE_PASS"
    elif args.resource_manifest:
        verdict = "MANIFEST_PASS"
    else:
        raise CheckFailure("arguments", "ordinary PASS requires fresh resource evidence")
    return RunResult(verdict, baseline, stack_chain, runtime_ram, initial_task_stack,
                     logger_stack, application_stacks, application_fixed_state)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        result = run(args)
    except CheckFailure as error:
        expected: str | None = None
        if args.fixture:
            try:
                fixture = validate_resource_document(load_json(Path(args.fixture), "fixture"), "fixture")
                expected = fixture["expected_failure"]
            except CheckFailure:
                expected = None
        if args.expect_fail:
            if expected is None:
                print("ERROR expect-fail requires a valid named fixture failure", file=sys.stderr)
                return 2
            if error.code != expected:
                print("ERROR fixture %s failed %s, expected %s: %s" %
                      (args.fixture, error.code, expected, error), file=sys.stderr)
                return 2
            print("EXPECTED-FAIL %s: %s" % (error.code, error), file=sys.stderr)
            return 1
        print("FAIL %s: %s" % (error.code, error), file=sys.stderr)
        return 1 if error.code not in {"arguments", "fixture_schema"} else 2
    if args.expect_fail:
        print("ERROR fixture %s passed; expected %s" % (args.fixture, "named failure"),
              file=sys.stderr)
        return 2
    if args.print_worktree_content_digest:
        print(result.verdict)
        return 0
    if result.baseline is not None:
        print("BASELINE_OF_RECORD=" + result.baseline.baseline_of_record)
        print("BEFORE_SOURCE_INVENTORY_SHA256=" + result.baseline.before_source_inventory_sha256)
        print("AFTER_SOURCE_INVENTORY_SHA256=" + result.baseline.after_source_inventory_sha256)
        print("SOURCE_INVENTORY_DRIFT=" + result.baseline.source_inventory_drift)
    if result.stack_chain is not None:
        print("STACK_CHAIN=" + result.stack_chain)
        if args.config_header:
            chain = json.loads(result.stack_chain)
            total = sum(item["frame_bytes"] for item in chain)
            print("MESH_STACK_ROOT=routed_mesh_task")
            print("MESH_STACK_TOTAL_BYTES=" + str(total))
            print("MESH_STACK_HEADROOM_BYTES=" + str(
                max(0, mesh_stack_bytes(Path(args.config_header)) - total)))
    if result.runtime_ram is not None:
        print("MAP_UNALLOCATED_RAM_BYTES=" + str(result.runtime_ram.map_unallocated_ram_bytes))
        print("RUNTIME_RAM_RESERVE_BYTES=" + str(result.runtime_ram.runtime_ram_reserve_bytes))
        print("POST_RESERVE_RAM_BYTES=" + str(result.runtime_ram.post_reserve_ram_bytes))
    if result.initial_task_stack is not None:
        print("INITIAL_TASK_STACK_BYTES=" + str(result.initial_task_stack.initial_task_stack_bytes))
        print("INITIAL_TASK_STATIC_FRAME_BYTES=" + str(result.initial_task_stack.static_frame_bytes))
        print("INITIAL_TASK_LOGICAL_HEADROOM_BYTES=" +
              str(result.initial_task_stack.logical_headroom_bytes))
    if result.logger_stack is not None:
        print("LOGGER_STACK_ROOT=" + result.logger_stack.root)
        print("LOGGER_STACK_CHAIN=" + json.dumps(
            [{"function": frame.identity, "frame_bytes": frame.frame_bytes}
             for frame in result.logger_stack.chain], sort_keys=True,
            separators=(",", ":")))
        print("LOGGER_STACK_TOTAL_BYTES=" + str(result.logger_stack.total_bytes))
        print("LOGGER_STACK_HEADROOM_BYTES=" + str(result.logger_stack.headroom_bytes))
    for report in result.application_stacks:
        label = report.root.upper()
        print(label + "_STACK_CHAIN=" + canonical_stack_chain(
            report.chain, label + " stack chain"))
        print(label + "_STACK_TOTAL_BYTES=" + str(report.total_bytes))
        print(label + "_STACK_HEADROOM_BYTES=" + str(report.headroom_bytes))
    if result.application_fixed_state is not None:
        report = result.application_fixed_state
        print("APPLICATION_FIXED_STATE_BASELINE_BYTES=" +
              str(report.baseline_allocated_bytes))
        print("APPLICATION_FIXED_STATE_PRODUCTION_BYTES=" +
              str(report.production_allocated_bytes))
        print("APPLICATION_FIXED_STATE_MEASURED_DELTA_BYTES=" +
              str(report.measured_delta_bytes))
        print("APPLICATION_FIXED_STATE_DECLARED_DELTA_BYTES=" +
              str(report.declared_delta_bytes))
        print("APPLICATION_FIXED_STATE_SIZEOF_AGGREGATE_BYTES=" +
              str(report.aggregate_new_static_bytes))
        print("APPLICATION_FIXED_STATE_REPLACED_BYTES=" +
              str(report.replaced_static_bytes))
        print("APPLICATION_FIXED_STATE_LINKER_PLACEMENT_BYTES=" +
              str(report.linker_placement_bytes))
    target = (args.d2_paired_acceptance_report or args.seal_output or
              args.emit_application_size_report or
              (args.resource_manifest if args.finalize_resource_manifest else None) or
              args.emit_resource_manifest or
              args.verify_published_provenance or args.fixture or args.resource_manifest or
              "resource evidence")
    print("%s %s" % (result.verdict, target))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
