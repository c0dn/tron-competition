#!/usr/bin/env python3
"""KISS POSIX serial bridge for MIND UART records and the local dashboard."""

from __future__ import annotations

import argparse
import json
import mimetypes
import os
import queue
import re
import select
import signal
import socket
import stat
import sys
import termios
import threading
import time
from collections import deque
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from socketserver import ThreadingMixIn
from typing import Any, Callable, Optional
from urllib.parse import parse_qsl, unquote, urlsplit

try:
    from . import layout_store
except ImportError:  # pragma: no cover - supports `python3 host/bridge.py`
    import layout_store  # type: ignore[no-redef]


API_SCHEMA = "mind.api.v2"
HEALTH_SCHEMA = "mind.health.v2"
COMMAND_SCHEMA = "mind.command.v1"
ERROR_SCHEMA = "mind.error.v1"

RING_CAPACITY = 256
MAX_PAGE_SIZE = 100
SESSION_ID_BYTES = 16
MAX_UNIX_EPOCH_MS = 253_402_300_799_999
MAX_LINE_BYTES = 512
MAX_REQUEST_BYTES = 4096
MAX_LAYOUT_REQUEST_BYTES = 65536
MAX_FLOORPLAN_REMOVE_REQUEST_BYTES = 256
MAX_FLOORPLAN_UPLOAD_REQUEST_BYTES = 7340032
COMMAND_QUEUE_CAPACITY = 32
HTTP_WORKER_CAPACITY = 16
MAX_STATIC_FILE_BYTES = 16 * 1024 * 1024
INITIAL_RECONNECT_SECONDS = 0.05
MAX_RECONNECT_SECONDS = 1.0
FRAME_ANCESTORS_POLICY = "frame-ancestors 'none'"

KNOWN_PREFIXES = (
    b"mind_event_v1",
    b"mind_event_v2",
    b"mind_root_v1",
    b"mind_command_v1",
    b"mind_gtt_begin_v1",
    b"mind_gtt_entry_v1",
    b"mind_gtt_end_v1",
)
GTT_PREFIXES = (b"mind_gtt_begin_v1", b"mind_gtt_entry_v1", b"mind_gtt_end_v1")
GTT_KINDS = frozenset({"gtt_begin", "gtt_entry", "gtt_end"})
ROOT_ON = b"ROOT ON\r"
ROOT_OFF = b"ROOT OFF\r"
ROOT_STATUS = b"ROOT STATUS\r"
GTT = b"GTT\r"
FIXED_COMMANDS = frozenset({ROOT_ON, ROOT_OFF, ROOT_STATUS, GTT})
DECIMAL = r"(?:0|[1-9][0-9]*)"
HEX12 = r"[0-9a-f]{12}"
HEX6 = r"[0-9a-f]{6}"
HTTP_TOKEN = r"[!#$%&'*+\-.^_`|~0-9A-Za-z]+"
LOOPBACK_HOST_PATTERN = re.compile(
    r"[ \t]*(?P<host>127\.0\.0\.1|localhost):(?P<port>[0-9]{1,5})[ \t]*", re.IGNORECASE
)
JSON_CONTENT_TYPE_PATTERN = re.compile(
    rf"[ \t]*(?P<type>{HTTP_TOKEN})/(?P<subtype>{HTTP_TOKEN})"
    rf"(?:[ \t]*;[ \t]*(?P<parameter>{HTTP_TOKEN})[ \t]*=[ \t]*(?P<value>{HTTP_TOKEN}))?[ \t]*",
    re.IGNORECASE,
)

EVENT_PATTERN = re.compile(
    rf"^mind_event_v1 now=(?P<now>{DECIMAL}) root=(?P<root>{HEX12}) "
    rf"wearable=(?P<wearable>{DECIMAL}) packet=(?P<packet>{HEX6}) "
    rf"schema=1 event=(?P<event>{DECIMAL}) confidence=(?P<confidence>{DECIMAL}) "
    rf"svm=(?P<svm>{DECIMAL}) mic=(?P<mic>{DECIMAL}) seq=(?P<seq>{DECIMAL}) "
    rf"observer=(?P<observer>{HEX12}) path=(?P<path>local|tavrn)$"
)
EVENT_V2_PATTERN = re.compile(
    rf"^mind_event_v2 now=(?P<now>{DECIMAL}) root=(?P<root>{HEX12}) "
    rf"wearable=(?P<wearable>{DECIMAL}) packet=(?P<packet>{HEX6}) "
    rf"schema=1 event=(?P<event>{DECIMAL}) confidence=(?P<confidence>{DECIMAL}) "
    rf"svm=(?P<svm>{DECIMAL}) mic=(?P<mic>{DECIMAL}) seq=(?P<seq>{DECIMAL}) "
    rf"observer=(?P<observer>{HEX12}) observer_rssi_dbm=(?P<observer_rssi_dbm>-{DECIMAL}) "
    rf"path=(?P<path>local|tavrn)$"
)
ROOT_PATTERN = re.compile(
    rf"^mind_root_v1 now=(?P<now>{DECIMAL}) local=(?P<local>{HEX12}) "
    rf"node=(?P<node>{DECIMAL}) role=(?P<role>leaf|root) "
    rf"roots=(?P<roots>{DECIMAL}) announced=(?P<announced>{DECIMAL}) "
    rf"acked=(?P<acked>{DECIMAL}) rejected=(?P<rejected>{DECIMAL}) "
    rf"pending=(?P<pending>{DECIMAL}) rootless_drop=(?P<rootless_drop>{DECIMAL})$"
)
COMMAND_PATTERN = re.compile(
    rf"^mind_command_v1 now=(?P<now>{DECIMAL}) local=(?P<local>{HEX12}) "
    r"command=(?P<command>on|off|status|gtt|invalid) "
    r"status=(?P<status>accepted|duplicate|busy|malformed|overflow|rejected)$"
)
GTT_BEGIN_PATTERN = re.compile(
    rf"^mind_gtt_begin_v1 query=(?P<query>{DECIMAL}) local=(?P<local>{HEX12}) "
    rf"entries=(?P<entries>{DECIMAL}) nondeparted=(?P<nondeparted>{DECIMAL})$"
)
GTT_ENTRY_PATTERN = re.compile(
    rf"^mind_gtt_entry_v1 query=(?P<query>{DECIMAL}) index=(?P<index>{DECIMAL}) "
    rf"adva=(?P<adva>{HEX12}) last=(?P<last>{DECIMAL}) soft=(?P<soft>{DECIMAL}) "
    rf"hard=(?P<hard>{DECIMAL}) departed_deadline=(?P<departed_deadline>{DECIMAL}) "
    rf"serial=(?P<serial>{DECIMAL}) serial_state=(?P<serial_state>{DECIMAL}) "
    rf"hop=(?P<hop>{DECIMAL}) hop_state=(?P<hop_state>{DECIMAL}) "
    rf"freshness=(?P<freshness>{DECIMAL}) departed=(?P<departed>{DECIMAL})$"
)
GTT_END_PATTERN = re.compile(
    rf"^mind_gtt_end_v1 query=(?P<query>{DECIMAL}) local=(?P<local>{HEX12}) "
    rf"entries=(?P<entries>{DECIMAL}) nondeparted=(?P<nondeparted>{DECIMAL})$"
)

VALUE_STATES = ("not_applicable", "known", "unknown")
FRESHNESS_STATES = ("not_applicable", "active", "soft_stale", "hard_expired", "departed")
DEPARTED_STATES = ("not_applicable", "false", "true", "unknown")


@dataclass(frozen=True)
class Timing:
    """Finite waits used by serial and HTTP ownership paths.

    Tests inject shorter values; production defaults leave ordinary local UART
    and dashboard requests ample time without allowing an unbounded wait.
    """

    command_deadline: float = 1.0
    submit_wait_slack: float = 0.15
    serial_poll_interval: float = 0.02
    http_io_timeout: float = 1.0
    http_request_deadline: float = 1.0
    http_reject_timeout: float = 0.05
    worker_join_timeout: float = 2.0


DEFAULT_TIMING = Timing()


class ParseError(ValueError):
    """A line used a MIND record prefix but did not satisfy its grammar."""


def _number(fields: dict[str, str], name: str, maximum: int, minimum: int = 0) -> int:
    value = int(fields[name])
    if not minimum <= value <= maximum:
        raise ParseError(f"{name} outside firmware range")
    return value


def parse_firmware_line(raw_line: bytes) -> Optional[dict[str, Any]]:
    """Return one exact UART record, ignore diagnostics, or raise ParseError."""

    if raw_line.endswith(b"\r"):
        raw_line = raw_line[:-1]
    recognized = raw_line.startswith(KNOWN_PREFIXES)
    try:
        line = raw_line.decode("ascii")
    except UnicodeDecodeError as error:
        if recognized:
            raise ParseError("non-ascii MIND line") from error
        return None

    event_match = EVENT_PATTERN.fullmatch(line)
    if event_match is not None:
        fields = event_match.groupdict()
        packet = fields["packet"]
        record = {
            "kind": "event",
            "now": _number(fields, "now", 0xFFFFFFFF),
            "root": fields["root"],
            "wearable": _number(fields, "wearable", 254, 1),
            "packet": packet,
            "schema": 1,
            "event": _number(fields, "event", 5),
            "confidence": _number(fields, "confidence", 100),
            "svm": _number(fields, "svm", 8000),
            "mic": _number(fields, "mic", 255),
            "seq": _number(fields, "seq", 255),
            "observer": fields["observer"],
            "observer_rssi_dbm": None,
            "path": fields["path"],
        }
        if record["seq"] != int(packet[-2:], 16):
            raise ParseError("event sequence does not match packet ID")
        if record["event"] == 0 and (record["confidence"] != 0 or record["mic"] != 0):
            raise ParseError("heartbeat has nonzero confidence or microphone")
        return record

    event_v2_match = EVENT_V2_PATTERN.fullmatch(line)
    if event_v2_match is not None:
        fields = event_v2_match.groupdict()
        packet = fields["packet"]
        record = {
            "kind": "event",
            "now": _number(fields, "now", 0xFFFFFFFF),
            "root": fields["root"],
            "wearable": _number(fields, "wearable", 254, 1),
            "packet": packet,
            "schema": 1,
            "event": _number(fields, "event", 5),
            "confidence": _number(fields, "confidence", 100),
            "svm": _number(fields, "svm", 8000),
            "mic": _number(fields, "mic", 255),
            "seq": _number(fields, "seq", 255),
            "observer": fields["observer"],
            "observer_rssi_dbm": -_number(
                {"magnitude": fields["observer_rssi_dbm"][1:]}, "magnitude", 127, 1
            ),
            "path": fields["path"],
        }
        if record["seq"] != int(packet[-2:], 16):
            raise ParseError("event sequence does not match packet ID")
        if record["event"] == 0 and (record["confidence"] != 0 or record["mic"] != 0):
            raise ParseError("heartbeat has nonzero confidence or microphone")
        return record

    root_match = ROOT_PATTERN.fullmatch(line)
    if root_match is not None:
        fields = root_match.groupdict()
        return {
            "kind": "root",
            "now": _number(fields, "now", 0xFFFFFFFF),
            "local": fields["local"],
            "node": _number(fields, "node", 6, 1),
            "role": fields["role"],
            "roots": _number(fields, "roots", 255),
            "announced": _number(fields, "announced", 255),
            "acked": _number(fields, "acked", 255),
            "rejected": _number(fields, "rejected", 255),
            "pending": _number(fields, "pending", 255),
            "rootless_drop": _number(fields, "rootless_drop", 0xFFFFFFFF),
        }

    command_match = COMMAND_PATTERN.fullmatch(line)
    if command_match is not None:
        fields = command_match.groupdict()
        return {
            "kind": "command",
            "now": _number(fields, "now", 0xFFFFFFFF),
            "local": fields["local"],
            "command": fields["command"],
            "status": fields["status"],
        }

    begin_match = GTT_BEGIN_PATTERN.fullmatch(line)
    if begin_match is not None:
        fields = begin_match.groupdict()
        return {
            "kind": "gtt_begin",
            "query": _number(fields, "query", 0xFFFFFFFF),
            "local": fields["local"],
            "entries": _number(fields, "entries", 16),
            "nondeparted": _number(fields, "nondeparted", 16),
        }

    entry_match = GTT_ENTRY_PATTERN.fullmatch(line)
    if entry_match is not None:
        fields = entry_match.groupdict()
        return {
            "kind": "gtt_entry",
            "query": _number(fields, "query", 0xFFFFFFFF),
            "index": _number(fields, "index", 15),
            "adva": fields["adva"],
            "last": _number(fields, "last", 0xFFFFFFFF),
            "soft": _number(fields, "soft", 0xFFFFFFFF),
            "hard": _number(fields, "hard", 0xFFFFFFFF),
            "departed_deadline": _number(fields, "departed_deadline", 0xFFFFFFFF),
            "serial": _number(fields, "serial", 0xFFFF),
            "serial_state": _number(fields, "serial_state", 2),
            "hop": _number(fields, "hop", 0xFF),
            "hop_state": _number(fields, "hop_state", 2),
            "freshness": _number(fields, "freshness", 4),
            "departed": _number(fields, "departed", 3),
        }

    end_match = GTT_END_PATTERN.fullmatch(line)
    if end_match is not None:
        fields = end_match.groupdict()
        return {
            "kind": "gtt_end",
            "query": _number(fields, "query", 0xFFFFFFFF),
            "local": fields["local"],
            "entries": _number(fields, "entries", 16),
            "nondeparted": _number(fields, "nondeparted", 16),
        }

    if recognized:
        raise ParseError("MIND line has invalid grammar")
    return None


def _is_gtt_line(raw_line: bytes) -> bool:
    return raw_line.startswith(GTT_PREFIXES)


def _normalized_gtt_entry(entry: dict[str, Any]) -> dict[str, Any]:
    return {
        "index": entry["index"],
        "adva": entry["adva"],
        "last": entry["last"],
        "soft": entry["soft"],
        "hard": entry["hard"],
        "departed_deadline": entry["departed_deadline"],
        "serial": entry["serial"],
        "serial_state": VALUE_STATES[entry["serial_state"]],
        "hop": entry["hop"],
        "hop_state": VALUE_STATES[entry["hop_state"]],
        "freshness": FRESHNESS_STATES[entry["freshness"]],
        "departed": DEPARTED_STATES[entry["departed"]],
    }


class CursorRing:
    """A lock-protected, process-lifetime monotonic cursor ring."""

    def __init__(self) -> None:
        self._records: deque[dict[str, Any]] = deque(maxlen=RING_CAPACITY)
        self._next_cursor = 1
        self._lock = threading.RLock()

    def append(self, record: dict[str, Any]) -> dict[str, Any]:
        with self._lock:
            committed = {"cursor": self._next_cursor, **record}
            if committed["kind"] == "event":
                # Host/API metadata only; this does not alter UART data.
                committed["received_at_ms"] = _bounded_unix_epoch_ms()
            self._records.append(committed)
            self._next_cursor += 1
            return committed

    def page(self, after: int, limit: int) -> dict[str, Any]:
        with self._lock:
            current = self._next_cursor - 1
            oldest = self._records[0]["cursor"] if self._records else self._next_cursor
            gap = bool(self._records and after < oldest - 1)
            start_after = oldest - 1 if gap else after
            events = [record.copy() for record in self._records if record["cursor"] > start_after]
            return {
                "schema": API_SCHEMA,
                "gap": gap,
                "oldest_cursor": oldest,
                "current_cursor": current,
                "events": events[:limit],
            }

    def cursors(self) -> tuple[int, int]:
        with self._lock:
            oldest = self._records[0]["cursor"] if self._records else self._next_cursor
            return oldest, self._next_cursor - 1


def configure_serial_115200(fd: int) -> None:
    """Set a descriptor to noncanonical, nonblocking 115200 8N1 POSIX serial."""

    attributes = termios.tcgetattr(fd)
    iflag, oflag, cflag, lflag, ispeed, ospeed, control = attributes
    iflag &= ~(
        termios.IGNBRK
        | termios.BRKINT
        | termios.IGNPAR
        | termios.PARMRK
        | termios.INPCK
        | termios.ISTRIP
        | termios.INLCR
        | termios.IGNCR
        | termios.ICRNL
        | termios.IXON
        | termios.IXOFF
        | getattr(termios, "IXANY", 0)
    )
    oflag &= ~termios.OPOST
    cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | getattr(termios, "CRTSCTS", 0))
    cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
    lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
    control[termios.VMIN] = 0
    control[termios.VTIME] = 0
    termios.tcsetattr(
        fd,
        termios.TCSANOW,
        [iflag, oflag, cflag, lflag, termios.B115200, termios.B115200, control],
    )


@dataclass
class CommandRequest:
    payload: bytes
    connection_generation: int
    done: threading.Event
    deadline: float
    result: Optional[str] = None


@dataclass
class PartialGTT:
    query: int
    local: str
    entry_count: int
    nondeparted_count: int
    entries: list[dict[str, Any]]


class SerialTransport:
    """The one fd, framer, command queue, and worker for an alias group."""

    def __init__(
        self,
        bridge: "Bridge",
        owner_index: int,
        grouping_identity: str,
        reopen_paths: tuple[str, ...],
        timing: Timing,
        *,
        open_fn: Callable[[str, int], int] = os.open,
        configure_fn: Callable[[int], None] = configure_serial_115200,
        read_fn: Callable[[int, int], bytes] = os.read,
        write_fn: Callable[[int, bytes], int] = os.write,
    ) -> None:
        self.bridge = bridge
        self.owner_index = owner_index
        # This resolved value is only the startup grouping key. Reopen attempts
        # deliberately use configured aliases so /dev/serial/by-id can retarget.
        self.grouping_identity = grouping_identity
        self._reopen_paths = list(dict.fromkeys(reopen_paths))
        self.timing = timing
        self._open_fn = open_fn
        self._configure_fn = configure_fn
        self._read_fn = read_fn
        self.write_fn = write_fn
        self._commands: queue.Queue[CommandRequest] = queue.Queue(COMMAND_QUEUE_CAPACITY)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name=f"mind-serial-{owner_index}", daemon=True)
        self._lifecycle_lock = threading.Lock()
        self._started = False
        self._state_lock = threading.RLock()
        self._connected = False
        self._connection_generation = 0
        self._parse_errors = 0
        self._overlong_lines = 0
        self._reconnects = 0
        self._last_record_cursor = 0
        self._latest_root: Optional[dict[str, Any]] = None
        self._latest_gtt: Optional[dict[str, Any]] = None
        self._partial_gtt: Optional[PartialGTT] = None
        self._gtt_generation = 0
        self._line = bytearray()
        self._discarding_overlong = False
        self._fd: Optional[int] = None
        self._active: Optional[CommandRequest] = None
        self._active_bytes = b""
        self._active_offset = 0
        self._next_write_attempt = 0.0
        self._ever_connected = False
        self._backoff = INITIAL_RECONNECT_SECONDS
        self._next_connect = 0.0
        # Test-only synchronization point after initial connection observation.
        # Admission always revalidates the captured generation after this hook.
        self._before_command_admission: Optional[Callable[[], None]] = None

    def add_reopen_path(self, path: str) -> None:
        """Retain one original CLI spelling as an ordered reconnect candidate."""

        if path not in self._reopen_paths:
            self._reopen_paths.append(path)

    def start(self) -> None:
        with self._lifecycle_lock:
            if self._started:
                return
            self._started = True
            self._thread.start()

    def stop(self) -> None:
        """Request worker shutdown; the worker alone completes queue entries."""

        self._stop.set()
        with self._lifecycle_lock:
            started = self._started
        if started and self._thread.is_alive():
            self._thread.join(timeout=self.timing.worker_join_timeout)

    def submit(self, payload: bytes) -> str:
        if payload not in FIXED_COMMANDS:
            return "write_failed"
        with self._lifecycle_lock:
            if self._started and not self._thread.is_alive():
                return "write_failed"
        now = time.monotonic()
        with self._state_lock:
            if not self._connected or self._stop.is_set():
                return "disconnected"
            generation = self._connection_generation
        if self._before_command_admission is not None:
            self._before_command_admission()
        with self._state_lock:
            if (
                not self._connected
                or self._stop.is_set()
                or generation != self._connection_generation
            ):
                return "disconnected"
            request = CommandRequest(
                payload=payload,
                connection_generation=generation,
                done=threading.Event(),
                deadline=now + self.timing.command_deadline,
            )
            try:
                self._commands.put_nowait(request)
            except queue.Full:
                # The frozen HTTP vocabulary has no queue-full error. A request not
                # accepted by the bounded writer is reported as an unsuccessful write.
                return "write_failed"
        request.done.wait(self.timing.command_deadline + self.timing.submit_wait_slack)
        return request.result or "write_failed"

    def is_connected(self) -> bool:
        with self._state_lock:
            return self._connected

    def physical_snapshot(self) -> dict[str, Any]:
        """Return the transport-owned health fields under its state lock."""

        with self._state_lock:
            root = self._latest_root.copy() if self._latest_root is not None else None
            return {
                "connected": self._connected,
                "parse_errors": self._parse_errors,
                "overlong_lines": self._overlong_lines,
                "reconnects": self._reconnects,
                "last_record_cursor": self._last_record_cursor,
                "root": root,
                # A complete snapshot only becomes roster authority when its
                # self identity is internally consistent with current root
                # authority. A later ROOT mismatch merely gates an otherwise
                # valid snapshot; a newer failed terminal GTT clears it.
                "gtt": self._gtt_snapshot(root),
                "owner_device": self.owner_index,
            }

    def snapshot(self, index: int, path: str) -> dict[str, Any]:
        """Return one legacy alias projection for direct transport callers."""

        return _project_device_snapshot(index, path, self.physical_snapshot())

    def feed_bytes(self, data: bytes) -> None:
        """Consume a serial chunk. Exposed for deterministic parser tests."""

        for byte in data:
            if byte == ord("\n"):
                if self._discarding_overlong:
                    self._discarding_overlong = False
                    self._line.clear()
                    continue
                line = bytes(self._line)
                self._line.clear()
                try:
                    record = parse_firmware_line(line)
                except ParseError:
                    with self._state_lock:
                        self._parse_errors += 1
                    if _is_gtt_line(line):
                        self._invalidate_gtt()
                    continue
                if record is not None:
                    self.bridge.record_from_transport(self, record)
                continue
            if self._discarding_overlong:
                continue
            if len(self._line) >= MAX_LINE_BYTES:
                overlong_gtt = _is_gtt_line(bytes(self._line))
                self._line.clear()
                self._discarding_overlong = True
                with self._state_lock:
                    self._overlong_lines += 1
                if overlong_gtt:
                    self._invalidate_gtt()
                continue
            self._line.append(byte)

    def _note_record(self, committed: dict[str, Any]) -> None:
        with self._state_lock:
            self._last_record_cursor = committed["cursor"]
            if committed["kind"] == "root":
                self._latest_root = {
                    key: value
                    for key, value in committed.items()
                    if key != "device"
                }

    def _gtt_snapshot(self, root: Optional[dict[str, Any]] = None) -> Optional[dict[str, Any]]:
        if self._latest_gtt is None or not self._gateway_gtt_is_valid(self._latest_gtt, root):
            return None
        return {**self._latest_gtt, "entries": [entry.copy() for entry in self._latest_gtt["entries"]]}

    @staticmethod
    def _gateway_gtt_is_valid(gtt: dict[str, Any], root: Optional[dict[str, Any]]) -> bool:
        local = gtt["local"]
        if sum(entry["adva"] == local for entry in gtt["entries"]) != 1:
            return False
        return root is None or root["local"] == local

    def _invalidate_gtt(self) -> None:
        with self._state_lock:
            # A malformed record only revokes an exposed roster when it
            # interrupts a newer response being assembled. Stray records with
            # no active response cannot supersede existing authority.
            if self._partial_gtt is not None:
                self._latest_gtt = None
            self._partial_gtt = None

    def _record_gtt(self, record: dict[str, Any]) -> None:
        with self._state_lock:
            kind = record["kind"]
            if kind == "gtt_begin":
                # A nested begin proves the in-flight response cannot complete;
                # it also supersedes any roster that was valid before it began.
                if self._partial_gtt is not None:
                    self._latest_gtt = None
                self._partial_gtt = PartialGTT(
                    query=record["query"],
                    local=record["local"],
                    entry_count=record["entries"],
                    nondeparted_count=record["nondeparted"],
                    entries=[],
                )
                return

            partial = self._partial_gtt
            if partial is None:
                return
            if kind == "gtt_entry":
                if (
                    record["query"] != partial.query
                    or record["index"] != len(partial.entries)
                    or len(partial.entries) >= partial.entry_count
                ):
                    self._latest_gtt = None
                    self._partial_gtt = None
                    return
                partial.entries.append(record)
                return

            if kind != "gtt_end":
                self._latest_gtt = None
                self._partial_gtt = None
                return
            if (
                record["query"] != partial.query
                or record["local"] != partial.local
                or record["entries"] != partial.entry_count
                or record["nondeparted"] != partial.nondeparted_count
                or len(partial.entries) != partial.entry_count
                or not self._valid_gtt(partial)
                or (self._latest_root is not None and self._latest_root["local"] != partial.local)
            ):
                # A complete newer GTT that cannot establish one authoritative
                # local identity invalidates any previously exposed roster.
                self._latest_gtt = None
                self._partial_gtt = None
                return
            self._gtt_generation += 1
            self._latest_gtt = {
                "generation": self._gtt_generation,
                "completed_at_ms": int(time.time() * 1000),
                "query_at_ms": partial.query,
                "local": partial.local,
                "entry_count": partial.entry_count,
                "nondeparted_count": partial.nondeparted_count,
                "entries": [_normalized_gtt_entry(entry) for entry in partial.entries],
            }
            self._partial_gtt = None

    @staticmethod
    def _valid_gtt(partial: PartialGTT) -> bool:
        if not (0 <= partial.entry_count <= 16 and 0 <= partial.nondeparted_count <= partial.entry_count):
            return False
        if sum(entry["departed"] != 2 for entry in partial.entries) != partial.nondeparted_count:
            return False
        return (
            len({entry["adva"] for entry in partial.entries}) == len(partial.entries)
            and
            sum(entry["adva"] == partial.local for entry in partial.entries) == 1
            and all(
            0 <= entry["last"] <= 0xFFFFFFFF
            and 0 <= entry["soft"] <= 0xFFFFFFFF
            and 0 <= entry["hard"] <= 0xFFFFFFFF
            and 0 <= entry["departed_deadline"] <= 0xFFFFFFFF
            and 0 <= entry["serial"] <= 0xFFFF
            and 0 <= entry["serial_state"] <= 2
            and 0 <= entry["hop"] <= 15
            and 0 <= entry["hop_state"] <= 2
            and 0 <= entry["freshness"] <= 4
            and 0 <= entry["departed"] <= 3
            and ((entry["departed"] == 2) == (entry["freshness"] == 4))
            for entry in partial.entries
            )
        )

    def _attempt_connect(self) -> None:
        for reopen_path in self._reopen_paths:
            fd: Optional[int] = None
            try:
                fd = self._open_fn(reopen_path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                self._configure_fn(fd)
            except OSError:
                try:
                    if fd is not None:
                        os.close(fd)
                except OSError:
                    pass
                continue
            self._fd = fd
            with self._state_lock:
                if self._ever_connected:
                    self._reconnects += 1
                self._ever_connected = True
                self._connection_generation += 1
                generation = self._connection_generation
                status_request = CommandRequest(
                    payload=ROOT_STATUS,
                    connection_generation=generation,
                    done=threading.Event(),
                    deadline=time.monotonic() + self.timing.command_deadline,
                )
                try:
                    # This precedes exposing the generation as connected, so no
                    # external command can be admitted ahead of ROOT STATUS.
                    self._commands.put_nowait(status_request)
                except queue.Full:
                    self._connection_generation += 1
                    status_queue_full = True
                else:
                    self._connected = True
                    status_queue_full = False
            self._backoff = INITIAL_RECONNECT_SECONDS
            if status_queue_full:
                self._close_fd()
                self._next_connect = time.monotonic() + self._backoff
                self._backoff = min(self._backoff * 2, MAX_RECONNECT_SECONDS)
                return
            return
        self._next_connect = time.monotonic() + self._backoff
        self._backoff = min(self._backoff * 2, MAX_RECONNECT_SECONDS)

    def _close_fd(self) -> None:
        fd, self._fd = self._fd, None
        with self._state_lock:
            if fd is not None or self._connected:
                self._connection_generation += 1
            self._connected = False
            self._latest_root = None
            self._latest_gtt = None
            self._partial_gtt = None
        self._reset_framer()
        if fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass

    def _reset_framer(self) -> None:
        self._line.clear()
        self._discarding_overlong = False

    def _complete_active(self, result: str) -> None:
        request, self._active = self._active, None
        self._active_bytes = b""
        self._active_offset = 0
        self._next_write_attempt = 0.0
        if request is not None:
            request.result = result
            request.done.set()

    def _fail_queued(self, result: str) -> None:
        while True:
            try:
                request = self._commands.get_nowait()
            except queue.Empty:
                return
            request.result = result
            request.done.set()

    def _disconnect(self, active_result: str) -> None:
        self._close_fd()
        self._complete_active(active_result)
        self._fail_queued("disconnected")
        self._next_connect = time.monotonic() + self._backoff
        self._backoff = min(self._backoff * 2, MAX_RECONNECT_SECONDS)

    def _start_next_command(self) -> None:
        if self._active is not None:
            return
        try:
            request = self._commands.get_nowait()
        except queue.Empty:
            return
        with self._state_lock:
            current_generation = self._connection_generation
            connected = self._connected
        if request.connection_generation != current_generation or not connected:
            request.result = "disconnected"
            request.done.set()
            return
        if request.deadline <= time.monotonic():
            request.result = "write_failed"
            request.done.set()
            return
        self._active = request
        self._active_bytes = request.payload
        self._active_offset = 0
        self._next_write_attempt = 0.0

    def _write_active(self) -> None:
        if self._fd is None or self._active is None:
            return
        if time.monotonic() >= self._active.deadline:
            self._disconnect("write_failed")
            return
        try:
            written = self.write_fn(self._fd, self._active_bytes[self._active_offset :])
        except (BlockingIOError, InterruptedError):
            self._next_write_attempt = time.monotonic() + self.timing.serial_poll_interval
            return
        except OSError:
            self._disconnect("write_failed")
            return
        if written <= 0:
            self._disconnect("write_failed")
            return
        self._active_offset += written
        if self._active_offset == len(self._active_bytes):
            self._complete_active("accepted")

    def _read_ready(self) -> None:
        if self._fd is None:
            return
        try:
            data = self._read_fn(self._fd, 4096)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            self._disconnect("write_failed")
            return
        if not data:
            self._disconnect("write_failed")
            return
        self.feed_bytes(data)

    def _run(self) -> None:
        try:
            while not self._stop.is_set():
                if self._fd is None:
                    self._fail_queued("disconnected")
                    now = time.monotonic()
                    if now >= self._next_connect:
                        self._attempt_connect()
                    self._stop.wait(
                        min(self.timing.serial_poll_interval, max(0.0, self._next_connect - time.monotonic()))
                    )
                    continue
                if self._active is not None and time.monotonic() >= self._active.deadline:
                    # A partial command is ambiguous. Closing its descriptor is
                    # required before another command could reach the device.
                    self._disconnect("write_failed")
                    continue
                self._start_next_command()
                now = time.monotonic()
                writable_fds = [self._fd] if self._active is not None and now >= self._next_write_attempt else []
                timeout = self.timing.serial_poll_interval
                if self._active is not None:
                    timeout = min(timeout, max(0.0, self._active.deadline - now))
                try:
                    readable, writable, _ = select.select([self._fd], writable_fds, [], timeout)
                except (OSError, ValueError):
                    self._disconnect("write_failed")
                    continue
                if readable:
                    self._read_ready()
                if writable and self._fd is not None:
                    self._write_active()
        finally:
            self._disconnect("disconnected")


class SerialDevice:
    """A stable CLI alias view over one shared physical SerialTransport."""

    def __init__(self, index: int, path: str, transport: SerialTransport) -> None:
        self.index = index
        self.path = path
        self.transport = transport

    @property
    def write_fn(self) -> Callable[[int, bytes], int]:
        return self.transport.write_fn

    @write_fn.setter
    def write_fn(self, value: Callable[[int, bytes], int]) -> None:
        self.transport.write_fn = value

    def submit(self, payload: bytes) -> str:
        return self.transport.submit(payload)

    def is_connected(self) -> bool:
        return self.transport.is_connected()

    def snapshot(self) -> dict[str, Any]:
        return self.transport.snapshot(self.index, self.path)

    def feed_bytes(self, data: bytes) -> None:
        self.transport.feed_bytes(data)


def _project_device_snapshot(index: int, path: str, physical: dict[str, Any]) -> dict[str, Any]:
    """Copy one physical snapshot into a stable CLI alias row."""

    root = physical["root"]
    gtt = physical["gtt"]
    return {
        "device": index,
        "path": path,
        **physical,
        "root": root.copy() if root is not None else None,
        "gtt": ({**gtt, "entries": [entry.copy() for entry in gtt["entries"]]} if gtt is not None else None),
    }


class Bridge:
    """Shared state between the serial workers and the localhost HTTP server."""

    def __init__(
        self,
        serial_paths: list[str],
        *,
        timing: Timing = DEFAULT_TIMING,
        state_dir: Optional[Path] = None,
        transport_factory: Callable[["Bridge", int, str, tuple[str, ...], Timing], SerialTransport] = SerialTransport,
    ) -> None:
        self.timing = timing
        # A process-lifetime opaque epoch from operating-system random bytes.
        self.session_id = os.urandom(SESSION_ID_BYTES).hex()
        self.ring = CursorRing()
        self.layout = layout_store.LayoutStore(state_dir or layout_store.default_state_dir())
        # Lock order is bridge snapshot -> ring -> transport state.  Transport
        # lifecycle paths never acquire this lock while holding state, so health
        # snapshots cannot deadlock with reconnect or disconnect handling.
        self._snapshot_lock = threading.RLock()
        self.transports: list[SerialTransport] = []
        by_grouping_identity: dict[str, SerialTransport] = {}
        self.devices: list[SerialDevice] = []
        for index, path in enumerate(serial_paths):
            grouping_identity = serial_grouping_identity(path)
            transport = by_grouping_identity.get(grouping_identity)
            if transport is None:
                transport = transport_factory(self, index, grouping_identity, (path,), timing)
                by_grouping_identity[grouping_identity] = transport
                self.transports.append(transport)
            else:
                transport.add_reopen_path(path)
            self.devices.append(SerialDevice(index, path, transport))

    def start(self) -> None:
        for transport in self.transports:
            transport.start()

    def stop(self) -> None:
        for transport in self.transports:
            transport.stop()
        self.layout.close()

    def record_from_transport(self, transport: SerialTransport, record: dict[str, Any]) -> None:
        with self._snapshot_lock:
            if record["kind"] in GTT_KINDS:
                transport._record_gtt(record)
                return
            committed = self.ring.append({"device": transport.owner_index, **record})
            transport._note_record(committed)

    def events_page(self, after: int, limit: int) -> dict[str, Any]:
        """Return one session-qualified API page from the event ring."""

        return {**self.ring.page(after, limit), "session_id": self.session_id}

    def health(self) -> dict[str, Any]:
        with self._snapshot_lock:
            oldest, current = self.ring.cursors()
            physical_snapshots = {transport: transport.physical_snapshot() for transport in self.transports}
            devices = [
                _project_device_snapshot(device.index, device.path, physical_snapshots[device.transport])
                for device in self.devices
            ]
        return {
            "schema": HEALTH_SCHEMA,
            "session_id": self.session_id,
            "oldest_cursor": oldest,
            "current_cursor": current,
            "devices": devices,
        }

    def root_command(self, device_index: int, active: bool) -> tuple[bool, str]:
        if device_index < 0 or device_index >= len(self.devices):
            return False, "unknown_device"
        result = self.devices[device_index].submit(ROOT_ON if active else ROOT_OFF)
        return result == "accepted", result

    def gtt_command(self, device_index: int) -> tuple[bool, str]:
        if device_index < 0 or device_index >= len(self.devices):
            return False, "unknown_device"
        result = self.devices[device_index].submit(GTT)
        return result == "accepted", result


def serial_grouping_identity(path: str) -> str:
    """Return the startup-only grouping identity for one serial argument."""

    try:
        return os.path.realpath(path, strict=getattr(os.path, "ALLOW_MISSING", False))
    except (OSError, ValueError):
        return os.path.abspath(os.path.normpath(path))


class BoundedThreadingHTTPServer(ThreadingMixIn, HTTPServer):
    """Threaded HTTP with a fixed request-worker ceiling."""

    # ThreadingMixIn.server_close() joins these workers before Bridge.stop()
    # permanently closes the SQLite connection.
    daemon_threads = False
    block_on_close = True
    allow_reuse_address = True
    request_queue_size = HTTP_WORKER_CAPACITY

    def __init__(
        self,
        server_address: tuple[str, int],
        handler_class: type[BaseHTTPRequestHandler],
        timing: Timing,
    ) -> None:
        self.timing = timing
        self._slots = threading.BoundedSemaphore(HTTP_WORKER_CAPACITY)
        self._active_lock = threading.Lock()
        self._active_requests = 0
        super().__init__(server_address, handler_class)

    def process_request(self, request: Any, client_address: Any) -> None:
        if not self._slots.acquire(blocking=False):
            self._reject_over_capacity(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self._slots.release()
            raise

    def process_request_thread(self, request: Any, client_address: Any) -> None:
        with self._active_lock:
            self._active_requests += 1
        try:
            super().process_request_thread(request, client_address)
        finally:
            with self._active_lock:
                self._active_requests -= 1
            self._slots.release()

    def active_request_count(self) -> int:
        with self._active_lock:
            return self._active_requests

    def _reject_over_capacity(self, request: Any) -> None:
        try:
            request.settimeout(self.timing.http_reject_timeout)
            request.sendall(
                b"HTTP/1.1 503 Service Unavailable\r\n"
                b"Connection: close\r\n"
                b"Content-Security-Policy: frame-ancestors 'none'\r\n"
                b"X-Frame-Options: DENY\r\n"
                b"Content-Length: 0\r\n\r\n"
            )
        except OSError:
            pass
        finally:
            try:
                request.close()
            except OSError:
                pass


class BridgeHTTPServer(BoundedThreadingHTTPServer):
    def __init__(self, bridge: Bridge, assets: Path, port: int) -> None:
        self.bridge = bridge
        # Keep a descriptor for the root instead of resolving every request.
        # O_NOFOLLOW rejects a symlinked asset root and openat below prevents
        # nested replacement/symlink races from escaping this pinned directory.
        self.assets = Path(os.path.abspath(assets))
        self._assets_fd: Optional[int] = None
        flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
        try:
            self._assets_fd = os.open(self.assets, flags)
            details = os.fstat(self._assets_fd)
            if not stat.S_ISDIR(details.st_mode):
                raise OSError("assets root is not a directory")
            self._assets_identity = (details.st_dev, details.st_ino)
            super().__init__(("127.0.0.1", port), BridgeRequestHandler, bridge.timing)
        except BaseException:
            if self._assets_fd is not None:
                os.close(self._assets_fd)
                self._assets_fd = None
            raise

    def server_close(self) -> None:
        try:
            super().server_close()
        finally:
            if self._assets_fd is not None:
                try:
                    os.close(self._assets_fd)
                except OSError:
                    pass
                self._assets_fd = None

    def _verify_assets_root(self) -> int:
        if self._assets_fd is None:
            raise OSError("assets root is closed")
        details = os.lstat(self.assets)
        if not stat.S_ISDIR(details.st_mode) or (details.st_dev, details.st_ino) != self._assets_identity:
            raise OSError("assets root was replaced")
        pinned = os.fstat(self._assets_fd)
        if not stat.S_ISDIR(pinned.st_mode) or (pinned.st_dev, pinned.st_ino) != self._assets_identity:
            raise OSError("assets root descriptor changed")
        return os.dup(self._assets_fd)

    def open_static(self, relative: str) -> tuple[int, int]:
        """Open a bounded regular asset through component-wise nofollow paths."""

        parts = relative.split("/")
        if not parts or any(part in {"", ".", ".."} for part in parts):
            raise OSError("unsafe static path")
        directory_fd = self._verify_assets_root()
        try:
            for part in parts[:-1]:
                child_fd = os.open(
                    part,
                    os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0),
                    dir_fd=directory_fd,
                )
                try:
                    if not stat.S_ISDIR(os.fstat(child_fd).st_mode):
                        raise OSError("non-directory static component")
                except BaseException:
                    os.close(child_fd)
                    raise
                os.close(directory_fd)
                directory_fd = child_fd
            fd = os.open(
                parts[-1],
                os.O_RDONLY | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0),
                dir_fd=directory_fd,
            )
            try:
                details = os.fstat(fd)
                if not stat.S_ISREG(details.st_mode) or details.st_size > MAX_STATIC_FILE_BYTES:
                    raise OSError("unsafe static file")
                # Detect an asset-root replacement that raced the initial pin check
                # before returning a descriptor to the response writer.
                verification_fd = self._verify_assets_root()
                try:
                    result = fd, details.st_size
                    fd = None
                    return result
                finally:
                    os.close(verification_fd)
            finally:
                if fd is not None:
                    os.close(fd)
        finally:
            os.close(directory_fd)


class BridgeRequestHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "mind-bridge/1"
    sys_version = ""

    @property
    def bridge_server(self) -> BridgeHTTPServer:
        return self.server  # type: ignore[return-value]

    def log_message(self, _format: str, *_args: Any) -> None:
        return

    def setup(self) -> None:
        self.request.settimeout(self.bridge_server.timing.http_io_timeout)
        super().setup()
        self._close_after_response = False
        self._request_host: Optional[str] = None
        self._request_expired = threading.Event()
        self._request_deadline = time.monotonic() + self.bridge_server.timing.http_request_deadline
        self._mutation_cancellation = layout_store.MutationCancellation(
            lambda: time.monotonic() < self._request_deadline
        )
        # Socket timeouts restart when a peer drips bytes. This bounded timer
        # shuts the socket at one absolute deadline; there can be no more than
        # HTTP_WORKER_CAPACITY live handlers/timers.
        self._deadline_timer = threading.Timer(
            max(0.0, self._request_deadline - time.monotonic()), self._expire_request
        )
        self._deadline_timer.daemon = True
        self._deadline_timer.start()

    def _expire_request(self) -> None:
        # This shares the store's commit-admission boundary.  Expiry either
        # wins before SQLite is admitted or waits until an admitted commit is
        # durable; it cannot land between a final check and ``commit()``.
        self._mutation_cancellation.cancel()
        self._request_expired.set()
        try:
            self.request.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    def _mutation_is_active(self) -> bool:
        """Check mutation activity; durable admission is coordinated by the store."""

        if not self._mutation_cancellation.is_active():
            self._request_expired.set()
            return False
        return True

    def handle(self) -> None:
        try:
            # The bridge deliberately accepts only one request per TCP
            # connection so unread mutation bytes can never desynchronize a
            # subsequent request.
            self.handle_one_request()
        except (OSError, TimeoutError):
            pass
        finally:
            self.close_connection = True
            self._deadline_timer.cancel()

    def end_headers(self) -> None:
        self.send_header("Content-Security-Policy", FRAME_ANCESTORS_POLICY)
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Connection", "close")
        super().end_headers()
        self.close_connection = True

    def handle_expect_100(self) -> bool:
        # Never invite a body before the loopback Host gate has run. This also
        # prevents an Expect request from consuming a second parser turn.
        self._request_host = self._validated_host()
        self._close_after_response = True
        parsed = urlsplit(self.path)
        if self._request_host is None:
            if parsed.path == "/api/layout" or parsed.path.startswith("/api/floorplan/"):
                self._send_dashboard_error(403, "forbidden_request")
            else:
                self._send_error(400, "invalid_host")
        elif parsed.path == "/api/layout" or parsed.path.startswith("/api/floorplan/"):
            self._send_dashboard_error(400, "invalid_body")
        else:
            self._send_error(400, "invalid_body")
        return False

    def parse_request(self) -> bool:
        self._request_host = None
        if not super().parse_request():
            return False
        self._request_host = self._validated_host()
        if self._request_host is None:
            self._close_after_response = True
            parsed = urlsplit(self.path)
            if parsed.path == "/api/layout" or parsed.path.startswith("/api/floorplan/"):
                self._send_dashboard_error(403, "forbidden_request")
            else:
                self._send_error(400, "invalid_host")
            return False
        return True

    def _send(self, status: int, body: bytes, content_type: str, *, api: bool = False) -> None:
        try:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            if api:
                self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        except (OSError, TimeoutError):
            self.close_connection = True
        self.close_connection = True

    def _send_json(self, status: int, payload: dict[str, Any]) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self._send(status, body, "application/json; charset=utf-8", api=True)

    def _send_error(self, status: int, error: str) -> None:
        self._send_json(status, {"schema": ERROR_SCHEMA, "accepted": False, "error": error})

    def _send_dashboard_error(self, status: int, error: str) -> None:
        self._send_json(status, layout_store.dashboard_error(error))

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlsplit(self.path)
        if parsed.path == "/api/layout":
            if parsed.query:
                self._send_dashboard_error(400, "invalid_body")
                return
            try:
                self._send_json(200, self.bridge_server.bridge.layout.get())
            except layout_store.StorageUnavailable:
                self._send_dashboard_error(503, "storage_unavailable")
            return
        floorplan = re.fullmatch(r"/api/floorplan/([0-9a-f]{64})", parsed.path)
        if floorplan is not None:
            if parsed.query:
                self._send_dashboard_error(404, "not_found")
                return
            self._serve_floorplan(floorplan[1])
            return
        if parsed.path.startswith("/api/floorplan/"):
            self._send_dashboard_error(404, "not_found")
            return
        if parsed.path == "/api/events":
            query = self._event_query(parsed.query)
            if query is None:
                self._send_error(400, "invalid_query")
                return
            after, limit = query
            self._send_json(200, self.bridge_server.bridge.events_page(after, limit))
            return
        if parsed.path == "/api/health" and not parsed.query:
            self._send_json(200, self.bridge_server.bridge.health())
            return
        self._serve_static(parsed.path)

    def do_PUT(self) -> None:  # noqa: N802
        parsed = urlsplit(self.path)
        if parsed.path == "/api/layout" and parsed.query:
            self._send_dashboard_error(400, "invalid_body")
            return
        if parsed.path != "/api/layout":
            self._send(404, b"Not found\n", "text/plain; charset=utf-8")
            return
        request_error = self._layout_mutation_request_error()
        if request_error is not None:
            status, error = request_error
            self._close_after_response = True
            self._send_dashboard_error(status, error)
            return
        body, body_error = self._dashboard_json_body(MAX_LAYOUT_REQUEST_BYTES)
        if body_error is not None:
            self._send_dashboard_error(*body_error)
            return
        try:
            base_revision, positions = self._parse_layout_update(body)
        except ValueError:
            self._send_dashboard_error(400, "invalid_body")
            return
        try:
            if not self._mutation_is_active():
                self.close_connection = True
                return
            response = self.bridge_server.bridge.layout.replace_positions(
                base_revision, positions, cancel_check=self._mutation_cancellation
            )
        except layout_store.Conflict as error:
            self._send_json(
                409,
                {
                    "schema": layout_store.CONFLICT_SCHEMA,
                    "error": "revision_conflict",
                    "current": error.current,
                },
            )
        except layout_store.MutationCancelled:
            self.close_connection = True
        except layout_store.StorageUnavailable:
            self._send_dashboard_error(503, "storage_unavailable")
        else:
            self._send_json(200, response)

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlsplit(self.path)
        if parsed.path in {"/api/floorplan/upload", "/api/floorplan/remove"}:
            self._floorplan_mutation(parsed)
            return
        if parsed.query or parsed.path not in {"/api/root", "/api/gtt"}:
            self._send(404, b"Not found\n", "text/plain; charset=utf-8")
            return
        request_error = self._mutation_request_error()
        if request_error is not None:
            status, error = request_error
            self._close_after_response = True
            self._send_error(status, error)
            return
        gtt = parsed.path == "/api/gtt"
        body = self._json_body(gtt=gtt)
        if isinstance(body, str):
            self._send_error(400, body)
            return
        device = body["device"]
        if gtt:
            accepted, result = self.bridge_server.bridge.gtt_command(device)
        else:
            accepted, result = self.bridge_server.bridge.root_command(device, body["active"])
        if accepted:
            self._send_json(
                202,
                {
                    "schema": COMMAND_SCHEMA,
                    "accepted": True,
                    "device": device,
                    "command": "gtt" if gtt else ("on" if body["active"] else "off"),
                },
            )
            return
        if result == "unknown_device":
            self._send_error(404, result)
        elif result in {"disconnected", "write_failed"}:
            self._send_error(503, result)
        else:
            self._send_error(503, "write_failed")

    def _floorplan_mutation(self, parsed: Any) -> None:
        if parsed.query:
            self._send_dashboard_error(400, "invalid_body")
            return
        request_error = self._layout_mutation_request_error()
        if request_error is not None:
            status, error = request_error
            self._close_after_response = True
            self._send_dashboard_error(status, error)
            return
        uploading = parsed.path == "/api/floorplan/upload"
        admitted = False
        if uploading:
            admitted = self.bridge_server.bridge.layout.try_admit_upload()
            if not admitted:
                self._close_after_response = True
                self._send_dashboard_error(503, "upload_busy")
                return
        try:
            body, body_error = self._dashboard_json_body(
                MAX_FLOORPLAN_UPLOAD_REQUEST_BYTES if uploading else MAX_FLOORPLAN_REMOVE_REQUEST_BYTES
            )
            if body_error is not None:
                self._send_dashboard_error(*body_error)
                return
            if uploading:
                self._floorplan_upload(body)
            else:
                self._floorplan_remove(body)
        finally:
            if admitted:
                self.bridge_server.bridge.layout.release_upload_admission()

    def _floorplan_upload(self, body: Any) -> None:
        try:
            base_revision, mime, encoded = self._parse_floorplan_upload(body)
        except ValueError:
            self._send_dashboard_error(400, "invalid_body")
            return
        try:
            decoded = layout_store.decode_canonical_base64(encoded)
        except ValueError:
            self._send_dashboard_error(400, "invalid_base64")
            return
        if len(decoded) > layout_store.MAX_DECODED_IMAGE_BYTES:
            self._send_dashboard_error(413, "image_too_large")
            return
        try:
            image = layout_store.inspect_image(mime, decoded)
        except layout_store.InvalidImage as error:
            status = 413 if error.error == "image_too_large" else 415 if error.error in {
                "unsupported_media_type",
                "image_type_mismatch",
            } else 422
            self._send_dashboard_error(status, error.error)
            return
        try:
            # Decode and validate before the transaction, then recheck the
            # absolute deadline immediately before acquiring the store lock.
            if not self._mutation_is_active():
                self.close_connection = True
                return
            response = self.bridge_server.bridge.layout.upload(
                base_revision, image, decoded, cancel_check=self._mutation_cancellation
            )
        except layout_store.Conflict as error:
            self._send_json(
                409,
                {
                    "schema": layout_store.CONFLICT_SCHEMA,
                    "error": "revision_conflict",
                    "current": error.current,
                },
            )
        except layout_store.MutationCancelled:
            self.close_connection = True
        except layout_store.StorageUnavailable:
            self._send_dashboard_error(503, "storage_unavailable")
        else:
            self._send_json(200, response)

    def _floorplan_remove(self, body: Any) -> None:
        try:
            base_revision = self._parse_floorplan_remove(body)
        except ValueError:
            self._send_dashboard_error(400, "invalid_body")
            return
        try:
            if not self._mutation_is_active():
                self.close_connection = True
                return
            response = self.bridge_server.bridge.layout.remove(
                base_revision, cancel_check=self._mutation_cancellation
            )
        except layout_store.Conflict as error:
            self._send_json(
                409,
                {
                    "schema": layout_store.CONFLICT_SCHEMA,
                    "error": "revision_conflict",
                    "current": error.current,
                },
            )
        except layout_store.MutationCancelled:
            self.close_connection = True
        except layout_store.StorageUnavailable:
            self._send_dashboard_error(503, "storage_unavailable")
        else:
            self._send_json(200, response)

    def _validated_host(self) -> Optional[str]:
        values = self.headers.get_all("Host") or []
        if len(values) != 1:
            return None
        match = LOOPBACK_HOST_PATTERN.fullmatch(values[0])
        if match is None:
            return None
        port = int(match["port"])
        if port != self.bridge_server.server_address[1]:
            return None
        return f"{match['host'].lower()}:{port}"

    def _mutation_request_error(self) -> Optional[tuple[int, str]]:
        if not self._has_json_content_type():
            return 415, "unsupported_media_type"

        origins = self.headers.get_all("Origin") or []
        if origins and (len(origins) != 1 or origins[0] != f"http://{self._request_host}"):
            return 403, "forbidden_origin"

        fetch_sites = self.headers.get_all("Sec-Fetch-Site") or []
        if fetch_sites and (len(fetch_sites) != 1 or fetch_sites[0] not in {"same-origin", "none"}):
            return 403, "forbidden_fetch_site"
        return None

    def _layout_mutation_request_error(self) -> Optional[tuple[int, str]]:
        """Apply the existing loopback CSRF gate with dashboard vocabulary."""

        request_error = self._mutation_request_error()
        if request_error is None:
            return None
        status, error = request_error
        if status == 403:
            return 403, "forbidden_request"
        return status, error

    def _has_json_content_type(self) -> bool:
        values = self.headers.get_all("Content-Type") or []
        if len(values) != 1:
            return False
        match = JSON_CONTENT_TYPE_PATTERN.fullmatch(values[0])
        if match is None or (match["type"].lower(), match["subtype"].lower()) != ("application", "json"):
            return False
        parameter = match["parameter"]
        return parameter is None or (parameter.lower(), match["value"].lower()) == ("charset", "utf-8")

    def _event_query(self, query: str) -> Optional[tuple[int, int]]:
        try:
            fields = parse_qsl(query, keep_blank_values=True, strict_parsing=True)
        except ValueError:
            return None
        values: dict[str, str] = {}
        for key, value in fields:
            if key not in {"after", "limit"} or key in values:
                return None
            values[key] = value
        after_raw = values.get("after", "0")
        limit_raw = values.get("limit", "100")
        if not re.fullmatch(DECIMAL, after_raw) or not re.fullmatch(DECIMAL, limit_raw):
            return None
        after = int(after_raw)
        limit = int(limit_raw)
        if limit < 1 or limit > MAX_PAGE_SIZE:
            return None
        return after, limit

    def _json_body(self, *, gtt: bool) -> dict[str, Any] | str:
        lengths = self.headers.get_all("Content-Length") or []
        if len(lengths) != 1 or not re.fullmatch(DECIMAL, lengths[0]):
            return self._invalid_body()
        size, too_large = self._bounded_content_length(MAX_REQUEST_BYTES)
        if size is None:
            self._close_after_response = True
            return "invalid_body"
        try:
            raw = self.rfile.read(size)
        except (OSError, TimeoutError):
            return self._invalid_body()
        if len(raw) != size:
            return self._invalid_body()
        try:
            parsed = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object)
        except _DuplicateJSONKey:
            return "invalid_body"
        except (UnicodeDecodeError, json.JSONDecodeError):
            return "invalid_json"
        required = {"device"} if gtt else {"device", "active"}
        if (
            type(parsed) is not dict
            or set(parsed) != required
            or type(parsed["device"]) is not int
            or parsed["device"] < 0
            or (not gtt and type(parsed["active"]) is not bool)
        ):
            return "invalid_body"
        return parsed

    def _invalid_body(self) -> str:
        self._close_after_response = True
        return "invalid_body"

    def _dashboard_json_body(self, limit: int) -> tuple[Any, Optional[tuple[int, str]]]:
        """Read one bounded JSON request body without accepting transfer coding."""

        if self.headers.get_all("Transfer-Encoding"):
            self._close_after_response = True
            return None, (400, "invalid_body")
        lengths = self.headers.get_all("Content-Length") or []
        if len(lengths) != 1 or not re.fullmatch(DECIMAL, lengths[0]):
            self._close_after_response = True
            return None, (400, "invalid_body")
        size, too_large = self._bounded_content_length(limit)
        if size is None:
            self._close_after_response = True
            return None, ((413, "request_too_large") if too_large else (400, "invalid_body"))
        try:
            raw = self.rfile.read(size)
        except (OSError, TimeoutError):
            self._close_after_response = True
            return None, (400, "invalid_body")
        if len(raw) != size:
            self._close_after_response = True
            return None, (400, "invalid_body")
        try:
            return (
                json.loads(
                    raw.decode("utf-8"),
                    object_pairs_hook=_unique_object,
                    parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)),
                ),
                None,
            )
        except _DuplicateJSONKey:
            return None, (400, "invalid_body")
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError):
            return None, (400, "invalid_json")

    def _bounded_content_length(self, limit: int) -> tuple[Optional[int], bool]:
        """Parse a decimal Content-Length without risking Python digit limits."""

        lengths = self.headers.get_all("Content-Length") or []
        if len(lengths) != 1 or re.fullmatch(DECIMAL, lengths[0]) is None:
            return None, False
        raw = lengths[0]
        maximum = str(limit)
        if len(raw) > len(maximum) or (len(raw) == len(maximum) and raw > maximum):
            return None, True
        return int(raw), False

    @staticmethod
    def _base_revision(value: Any) -> int:
        if type(value) is not int or value < 0:
            raise ValueError("invalid base revision")
        return value

    def _parse_layout_update(self, body: Any) -> tuple[int, list[dict[str, Any]]]:
        if type(body) is not dict or set(body) != {"schema", "base_revision", "positions"}:
            raise ValueError("invalid layout update")
        if body["schema"] != layout_store.LAYOUT_UPDATE_SCHEMA:
            raise ValueError("invalid layout update schema")
        return self._base_revision(body["base_revision"]), layout_store.normalize_positions(body["positions"])

    def _parse_floorplan_upload(self, body: Any) -> tuple[int, str, str]:
        if type(body) is not dict or set(body) != {"schema", "base_revision", "mime", "data_base64"}:
            raise ValueError("invalid floorplan upload")
        if body["schema"] != layout_store.FLOORPLAN_UPLOAD_SCHEMA:
            raise ValueError("invalid floorplan upload schema")
        if type(body["mime"]) is not str or type(body["data_base64"]) is not str:
            raise ValueError("invalid floorplan upload fields")
        return self._base_revision(body["base_revision"]), body["mime"], body["data_base64"]

    def _parse_floorplan_remove(self, body: Any) -> int:
        if type(body) is not dict or set(body) != {"schema", "base_revision"}:
            raise ValueError("invalid floorplan remove")
        if body["schema"] != layout_store.FLOORPLAN_REMOVE_SCHEMA:
            raise ValueError("invalid floorplan remove schema")
        return self._base_revision(body["base_revision"])

    def _serve_floorplan(self, sha256: str) -> None:
        try:
            image = self.bridge_server.bridge.layout.open_image(sha256)
        except (layout_store.StorageUnavailable, layout_store.StateCorrupt):
            self._send_dashboard_error(503, "storage_unavailable")
            return
        if image is None:
            self._send_dashboard_error(404, "not_found")
            return
        etag = f'"{image.sha256}"'
        try:
            if self._if_none_match_matches(etag):
                self._send_floorplan_headers(304, image, None)
                return
            self._send_floorplan_headers(200, image, image.length)
            data = memoryview(image.data)
            for offset in range(0, image.length, layout_store.IMAGE_CHUNK_BYTES):
                self.wfile.write(data[offset : offset + layout_store.IMAGE_CHUNK_BYTES])
        except (OSError, TimeoutError):
            self.close_connection = True

    def _send_floorplan_headers(self, status: int, image: layout_store.OpenImage, length: Optional[int]) -> None:
        self.send_response(status)
        self.send_header("Content-Type", image.mime)
        if length is not None:
            self.send_header("Content-Length", str(length))
        self.send_header("Cache-Control", "private, max-age=31536000, immutable")
        self.send_header("ETag", f'"{image.sha256}"')
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()

    def _if_none_match_matches(self, etag: str) -> bool:
        values = self.headers.get_all("If-None-Match") or []
        for value in values:
            for token in value.split(","):
                candidate = token.strip()
                if candidate == "*":
                    return True
                if candidate.startswith("W/"):
                    candidate = candidate[2:].strip()
                if candidate == etag:
                    return True
        return False

    def _serve_static(self, raw_path: str) -> None:
        decoded = unquote(raw_path)
        relative = "index.html" if decoded == "/" else decoded.lstrip("/")
        if not relative or "\x00" in relative or any(part in {"", ".", ".."} for part in relative.split("/")):
            self._send(404, b"Not found\n", "text/plain; charset=utf-8")
            return
        try:
            fd, length = self.bridge_server.open_static(relative)
        except (OSError, ValueError):
            self._send(404, b"Not found\n", "text/plain; charset=utf-8")
            return
        try:
            self.send_response(200)
            self.send_header("Content-Type", _mime_type(Path(relative)))
            self.send_header("Content-Length", str(length))
            self.end_headers()
            remaining = length
            while remaining:
                chunk = os.read(fd, min(layout_store.IMAGE_CHUNK_BYTES, remaining))
                if not chunk:
                    raise OSError("truncated static file")
                self.wfile.write(chunk)
                remaining -= len(chunk)
        except (OSError, TimeoutError):
            self.close_connection = True
        finally:
            try:
                os.close(fd)
            except OSError:
                pass


class _DuplicateJSONKey(ValueError):
    pass


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise _DuplicateJSONKey(key)
        result[key] = value
    return result


def _mime_type(path: Path) -> str:
    fixed = {
        ".css": "text/css; charset=utf-8",
        ".html": "text/html; charset=utf-8",
        ".js": "application/javascript; charset=utf-8",
        ".json": "application/json; charset=utf-8",
        ".mjs": "application/javascript; charset=utf-8",
        ".svg": "image/svg+xml",
        ".woff2": "font/woff2",
    }
    return fixed.get(path.suffix.lower(), mimetypes.guess_type(path.name)[0] or "application/octet-stream")


def _bounded_unix_epoch_ms() -> int:
    """Return a JSON-safe Unix epoch timestamp within the API contract range."""

    return min(MAX_UNIX_EPOCH_MS, max(0, int(time.time() * 1000)))


def _port(value: str) -> int:
    if not re.fullmatch(DECIMAL, value):
        raise argparse.ArgumentTypeError("port must be a decimal number from 0 through 65535")
    port = int(value)
    if port > 65535:
        raise argparse.ArgumentTypeError("port must be a decimal number from 0 through 65535")
    return port


def _state_dir(value: str) -> Path:
    try:
        return layout_store.state_dir_argument(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", action="append", default=[], metavar="PATH", help="the sole POSIX serial path")
    parser.add_argument("--port", type=_port, default=8787, help="localhost TCP port (default: 8787)")
    parser.add_argument(
        "--assets",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "dashboard" / "dist",
        help="built dashboard asset directory (default: dashboard/dist)",
    )
    parser.add_argument(
        "--state-dir",
        type=_state_dir,
        metavar="ABSOLUTE_PATH",
        help="absolute durable dashboard state directory (default: XDG data directory)",
    )
    args = parser.parse_args(argv)
    if len(args.serial) != 1:
        parser.error("exactly one --serial PATH is required")
    return args


def serve(bridge: Bridge, assets: Path, port: int) -> BridgeHTTPServer:
    return BridgeHTTPServer(bridge, assets, port)


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    bridge = Bridge(args.serial, state_dir=args.state_dir)
    httpd = serve(bridge, args.assets, args.port)
    bridge.start()
    def _interrupt(_signal: int, _frame: Any) -> None:
        raise KeyboardInterrupt

    previous = signal.signal(signal.SIGTERM, _interrupt)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        signal.signal(signal.SIGTERM, previous)
        httpd.server_close()
        bridge.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
