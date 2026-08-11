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
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


SCHEMA = "tron.tavrn.expiry.resources.v1"
EDGE_SCHEMA = "tron.tavrn.expiry.required-stack-edges.v2"
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
STACK_LIMIT = 3072
STACK_HEADROOM = 1024
RAM_MINIMUM = 8192
FIXED_DELTA_MAXIMUM = 512
REQUIRED_ROUTED_INITIAL_TASK_STACK_BYTES = 4096
REQUIRED_ROUTED_RUNTIME_RAM_RESERVE_BYTES = 12288

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
class RoutedRuntimeDeclaration:
    initial_task_stack_bytes: int
    runtime_ram_reserve_bytes: int


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


def strict_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key %r" % key)
        result[key] = value
    return result


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
                           "max_scheduler_gap_ms", "faults", "targeted_controls",
                           "rreq_controls", "tc_expiry_controls"}
    if isinstance(raw_capture, dict) and set(raw_capture) == legacy_capture_keys and \
            raw_capture.get("supplied") is False:
        raw_capture = {**raw_capture, "hard_selected_demand_deferred_passes": 0,
                       "unavailable_count": 0}
        document["capture"] = raw_capture
    capture = require_object(raw_capture,
                              {"supplied", "duration_seconds", "complete_passes",
                               "hard_selected_demand_deferred_passes",
                               "max_scheduler_gap_ms", "unavailable_count", "faults",
                               "targeted_controls", "rreq_controls", "tc_expiry_controls"},
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
                  "tc_expiry_controls"):
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
    initial_stack = require_manifest_decimal(
        manifest, "build.initial_task_stack_bytes",
        REQUIRED_ROUTED_INITIAL_TASK_STACK_BYTES)
    capacity_stack = require_manifest_decimal(
        manifest, "capacity.routed_initial_task_stack_bytes",
        REQUIRED_ROUTED_INITIAL_TASK_STACK_BYTES)
    reserve = require_manifest_decimal(
        manifest, "resource.runtime_ram_reserve_bytes",
        REQUIRED_ROUTED_RUNTIME_RAM_RESERVE_BYTES)
    config_initial_stack = config_uint_macro(
        config_header, "TRON_BUILD_INITIAL_TASK_STACK_BYTES", "initial-task-stack")
    config_reserve = config_uint_macro(
        config_header, "TRON_BUILD_RUNTIME_RAM_RESERVE_BYTES", "runtime-ram-reserve")
    if config_initial_stack != initial_stack or config_initial_stack != capacity_stack:
        raise CheckFailure("provenance", "generated initial-task stack declarations disagree")
    if config_reserve != reserve:
        raise CheckFailure("provenance", "generated runtime RAM reserve declarations disagree")
    return RoutedRuntimeDeclaration(initial_stack, reserve)


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


def load_stack_contract(path: Path, root: str) -> dict[str, Any]:
    document = load_json(path, "required stack edge manifest")
    exact_keys(document, {"schema", "root", "edges", "declared_leaves"},
               "required stack edge manifest")
    if document["schema"] != EDGE_SCHEMA or document["root"] != root or \
       not isinstance(document["edges"], list) or not document["edges"] or \
       not isinstance(document["declared_leaves"], list):
        raise CheckFailure("stack", "invalid required stack edge manifest")
    names: set[str] = set()
    for index, edge in enumerate(document["edges"]):
        item = require_object(edge, {"from", "to", "kind", "when"},
                              "required stack edge %d" % index)
        if not isinstance(item["from"], str) or not isinstance(item["to"], str) or \
           item["kind"] not in ("call", "indirect") or item["when"] not in ("always", "binding"):
            raise CheckFailure("stack", "invalid required stack edge")
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


def require_rooted_stack_edges(contract: dict[str, Any]) -> None:
    supplied = {(edge["from"], edge["to"], edge["kind"], edge["when"])
                for edge in contract["edges"]}
    missing = sorted(REQUIRED_ROOTED_STACK_EDGES - supplied)
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
                   include_binding: bool = True) -> list[StackFrame]:
    contract = load_stack_contract(contract_or_path, root) \
        if isinstance(contract_or_path, Path) else contract_or_path
    active_edges = [item for item in contract["edges"]
                    if include_binding or item["when"] != "binding"]
    # A routed cycle invokes both operation callbacks.  Treat every resolved
    # callback as a real edge in an overlay graph rather than selecting the
    # syntactically last sibling from the required-edge manifest.
    active_graph = {node: set(children) for node, children in graph.items()}
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

    reachable: set[str] = set()
    pending = [root]
    while pending:
        node = pending.pop()
        if node in reachable:
            continue
        reachable.add(node)
        pending.extend(active_graph.get(node, set()))
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

    # Evaluate every resolved routed-cycle operation branch and choose the true
    # maximum-byte root-to-leaf chain.  Equal totals ending at different leaves
    # remain an explicit manifest ambiguity.
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
    terminal_symbols = {item[-1] for item in deepest}
    if len(deepest) != 1 and len(terminal_symbols) != 1:
        raise CheckFailure("stack", "ambiguous maximum-byte active stack path: " +
                           " | ".join(" -> ".join(item) for item in deepest))
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


def validate_preprocessed_main(source: str) -> None:
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


def validate_capture(document: dict[str, Any]) -> None:
    capture = document["capture"]
    if not capture["supplied"]:
        return
    if capture["duration_seconds"] < 300 or capture["complete_passes"] < 10 or \
        capture["hard_selected_demand_deferred_passes"] < 1 or \
        capture["max_scheduler_gap_ms"] == 0 or capture["max_scheduler_gap_ms"] > 2 or \
        any(capture[key] != 0 for key in
        ("unavailable_count", "faults", "targeted_controls", "rreq_controls",
         "tc_expiry_controls")):
        raise CheckFailure("hardware_capture", "hardware capture misses expiry acceptance bounds")


def load_capture(path: Path, artifact_manifest_path: Path | None = None) -> dict[str, Any]:
    document = load_json(path, "hardware capture")
    exact_keys(document, {"schema", "duration_seconds", "complete_passes",
                           "hard_selected_demand_deferred_passes",
                           "max_scheduler_gap_ms", "unavailable_count", "faults",
                           "targeted_controls", "rreq_controls", "tc_expiry_controls",
                           "provenance"},
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
                "tc_expiry_controls"):
        capture[key] = require_int(document[key], "hardware capture." + key)
    return capture


def validate_thresholds(document: dict[str, Any], require_binding: bool,
                        require_aodv_absence: bool = False) -> None:
    if document["ram"]["unallocated_bytes"] < RAM_MINIMUM:
        raise CheckFailure("ram_unallocated", "unallocated RAM is below 8192 bytes")
    if document["stack"]["total_bytes"] > STACK_LIMIT or \
       document["stack"]["headroom_bytes"] < STACK_HEADROOM:
        raise CheckFailure("stack_total", "mesh stack total/headroom violates the 3072/1024 "
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
    candidate = manifest_path.parent / name
    if not candidate.is_file():
        raise CheckFailure("provenance", "manifest evidence is missing: %s" % key)
    return candidate


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
    if value == 0:
        raise CheckFailure("stack", "generated mesh stack size must be nonzero")
    return value


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


def build_evidence_document(args: argparse.Namespace) -> dict[str, Any]:
    if args.full_map is None or args.full_elf is None:
        raise CheckFailure("arguments", "resource evidence requires --full-map and --full-elf")
    map_path, elf_path = Path(args.full_map), Path(args.full_elf)
    map_usage = parse_map(map_path)
    selected_hash = "0" * 64
    if args.selected_sources is not None:
        selected_hash = inventory_hash(Path(args.selected_sources))
    manifest: dict[str, str] = {}
    manifest_path: Path | None = None
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
        if args.selected_sources is not None:
            require_initial_task_source_provenance(Path(args.selected_sources))
        if args.compile_commands:
            require_routed_initial_task_compile_definitions(
                Path(args.compile_commands), declaration.initial_task_stack_bytes)
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
        contract = load_stack_contract(Path(args.required_edge_manifest), args.stack_root)
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
        for value in args.resolve_operation_edge or []:
            if value.count("=") != 1:
                raise CheckFailure("stack", "invalid --resolve-operation-edge")
            key, target = value.split("=", 1)
            if not key or not target or key in resolvers:
                raise CheckFailure("stack", "duplicate/invalid operation edge resolver")
            resolvers[key] = target
        if stack_bytes == 0:
            raise CheckFailure("stack", "stack analysis requires generated config stack evidence")
        chain = validate_edges(contract, resolvers, graph, frames, args.stack_root,
                               include_binding=not args.stack_baseline_only)
        total = sum(frame.frame_bytes for frame in chain)
        stack = {"root": args.stack_root,
                 "chain": [{"function": frame.identity, "frame_bytes": frame.frame_bytes}
                           for frame in chain],
                 "total_bytes": total, "headroom_bytes": max(0, stack_bytes - total)}
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
                    "tc_expiry_controls": 0},
        "provenance": {"source_inventory_sha256": selected_hash,
                       "map_sha256": sha256_file(map_path), "elf_sha256": sha256_file(elf_path),
                       "compile_commands_sha256": compiler_hash, "checker_sha256": checker_hash,
                       "build_script_sha256": build_hash},
        "expected_failure": None, "preprocessed_main": {"source": ""},
    }
    return validate_resource_document(document, "generated resource manifest")


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
                                                               InitialTaskStackReport | None]:
    if args.stack_baseline_only and args.require_binding_call:
        raise CheckFailure("arguments", "stack/baseline-only mode cannot require the binding")
    if args.stack_baseline_only:
        required = ("resource_manifest", "full_elf", "full_map", "full_manifest", "before_map",
                    "baseline_manifest", "baseline_sha256", "selected_sources", "su_glob",
                    "stack_root", "required_edge_manifest", "disassembly", "compile_commands",
                    "config_header")
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
        observed = build_evidence_document(args)
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
    if args.full_manifest is not None and args.full_map is not None:
        runtime_ram, initial_task_stack = routed_runtime_reports(args)
    return verify_baseline(args, document), runtime_ram, initial_task_stack


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture")
    parser.add_argument("--expect-fail", action="store_true")
    parser.add_argument("--emit-resource-manifest")
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
    parser.add_argument("--selected-sources")
    parser.add_argument("--su-glob")
    parser.add_argument("--stack-root")
    parser.add_argument("--required-edge-manifest")
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
    return parser.parse_args(argv)


def has_live_artifact_arguments(args: argparse.Namespace) -> bool:
    return any((
        args.full_elf, args.full_map, args.full_manifest, args.before_map,
        args.baseline_manifest, args.baseline_sha256, args.selected_sources,
        args.su_glob, args.stack_root, args.required_edge_manifest,
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


def run(args: argparse.Namespace) -> RunResult:
    live_fixture: dict[str, Any] | None = None
    baseline: BaselineEvidence | None = None
    runtime_ram: RuntimeRamReport | None = None
    initial_task_stack: InitialTaskStackReport | None = None
    if args.declared_fixed_state_delta < 0:
        raise CheckFailure("arguments", "declared fixed-state delta must be nonnegative")
    if args.stack_baseline_only and args.fixture:
        raise CheckFailure("arguments", "stack/baseline-only mode requires fresh resource evidence")
    live_artifacts = has_live_artifact_arguments(args)
    if not args.emit_resource_manifest and not args.stack_baseline_only and live_artifacts:
        require_complete_live_evidence(args)
    if args.fixture:
        fixture = validate_resource_document(load_json(Path(args.fixture), "fixture"), "fixture")
        if args.full_map and args.full_elf:
            # Fixture use with fresh firmware evidence is intentionally two-part:
            # validate the real ELF/map/main/provenance first, then inject only
            # deterministic threshold values.  A failing fixture therefore
            # cannot conceal a missing binding or arbitrary CLI failure.
            document = build_evidence_document(args)
            live_fixture = fixture
        else:
            document = fixture
            if document["preprocessed_main"]["source"]:
                validate_preprocessed_main(document["preprocessed_main"]["source"])
    elif args.resource_manifest:
        document = validate_resource_document(load_json(Path(args.resource_manifest), "resource manifest"),
                                              "resource manifest")
    else:
        document = build_evidence_document(args)
    if args.emit_resource_manifest:
        if args.fixture or args.resource_manifest:
            raise CheckFailure("arguments", "emit mode cannot consume a fixture/manifest")
        target = Path(args.emit_resource_manifest)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return RunResult("EMITTED", None, None, None, None)
    live_invocation = args.stack_baseline_only or live_artifacts
    if live_invocation:
        baseline, runtime_ram, initial_task_stack = validate_live_evidence(args, document)
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
    stack_chain = json.dumps(document["stack"]["chain"], sort_keys=True,
                              separators=(",", ":")) if document["stack"]["chain"] else None
    if live_invocation:
        verdict = "PASS"
    elif args.fixture:
        verdict = "FIXTURE_PASS"
    elif args.resource_manifest:
        verdict = "MANIFEST_PASS"
    else:
        raise CheckFailure("arguments", "ordinary PASS requires fresh resource evidence")
    return RunResult(verdict, baseline, stack_chain, runtime_ram, initial_task_stack)


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
    if result.baseline is not None:
        print("BASELINE_OF_RECORD=" + result.baseline.baseline_of_record)
        print("BEFORE_SOURCE_INVENTORY_SHA256=" + result.baseline.before_source_inventory_sha256)
        print("AFTER_SOURCE_INVENTORY_SHA256=" + result.baseline.after_source_inventory_sha256)
        print("SOURCE_INVENTORY_DRIFT=" + result.baseline.source_inventory_drift)
    if result.stack_chain is not None:
        print("STACK_CHAIN=" + result.stack_chain)
    if result.runtime_ram is not None:
        print("MAP_UNALLOCATED_RAM_BYTES=" + str(result.runtime_ram.map_unallocated_ram_bytes))
        print("RUNTIME_RAM_RESERVE_BYTES=" + str(result.runtime_ram.runtime_ram_reserve_bytes))
        print("POST_RESERVE_RAM_BYTES=" + str(result.runtime_ram.post_reserve_ram_bytes))
    if result.initial_task_stack is not None:
        print("INITIAL_TASK_STACK_BYTES=" + str(result.initial_task_stack.initial_task_stack_bytes))
        print("INITIAL_TASK_STATIC_FRAME_BYTES=" + str(result.initial_task_stack.static_frame_bytes))
        print("INITIAL_TASK_LOGICAL_HEADROOM_BYTES=" +
              str(result.initial_task_stack.logical_headroom_bytes))
    target = args.emit_resource_manifest or args.fixture or args.resource_manifest or "resource evidence"
    print("%s %s" % (result.verdict, target))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
