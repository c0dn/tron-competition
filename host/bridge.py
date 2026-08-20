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


API_SCHEMA = "mind.api.v1"
HEALTH_SCHEMA = "mind.health.v1"
COMMAND_SCHEMA = "mind.command.v1"
ERROR_SCHEMA = "mind.error.v1"

RING_CAPACITY = 256
MAX_PAGE_SIZE = 100
MAX_LINE_BYTES = 512
MAX_REQUEST_BYTES = 4096
COMMAND_QUEUE_CAPACITY = 32
HTTP_WORKER_CAPACITY = 16
INITIAL_RECONNECT_SECONDS = 0.05
MAX_RECONNECT_SECONDS = 1.0

KNOWN_PREFIXES = (b"mind_event_v1", b"mind_root_v1", b"mind_command_v1")
DECIMAL = r"(?:0|[1-9][0-9]*)"
HEX12 = r"[0-9a-f]{12}"
HEX6 = r"[0-9a-f]{6}"

EVENT_PATTERN = re.compile(
    rf"^mind_event_v1 now=(?P<now>{DECIMAL}) root=(?P<root>{HEX12}) "
    rf"wearable=(?P<wearable>{DECIMAL}) packet=(?P<packet>{HEX6}) "
    rf"schema=1 event=(?P<event>{DECIMAL}) confidence=(?P<confidence>{DECIMAL}) "
    rf"svm=(?P<svm>{DECIMAL}) mic=(?P<mic>{DECIMAL}) seq=(?P<seq>{DECIMAL}) "
    rf"observer=(?P<observer>{HEX12}) path=(?P<path>local|tavrn)$"
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
    r"command=(?P<command>on|off|status|invalid) "
    r"status=(?P<status>accepted|duplicate|busy|malformed|overflow|rejected)$"
)


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

    if recognized:
        raise ParseError("MIND line has invalid grammar")
    return None


class CursorRing:
    """A lock-protected, process-lifetime monotonic cursor ring."""

    def __init__(self) -> None:
        self._records: deque[dict[str, Any]] = deque(maxlen=RING_CAPACITY)
        self._next_cursor = 1
        self._lock = threading.RLock()

    def append(self, record: dict[str, Any]) -> dict[str, Any]:
        with self._lock:
            committed = {"cursor": self._next_cursor, **record}
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
    active: bool
    done: threading.Event
    deadline: float
    result: Optional[str] = None


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
        self._parse_errors = 0
        self._overlong_lines = 0
        self._reconnects = 0
        self._last_record_cursor = 0
        self._latest_root: Optional[dict[str, Any]] = None
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

    def submit(self, active: bool) -> str:
        with self._state_lock:
            if not self._connected or self._stop.is_set():
                return "disconnected"
        with self._lifecycle_lock:
            if self._started and not self._thread.is_alive():
                return "write_failed"
        now = time.monotonic()
        request = CommandRequest(
            active=active,
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

    def snapshot(self, index: int, path: str) -> dict[str, Any]:
        with self._state_lock:
            return {
                "device": index,
                "path": path,
                "connected": self._connected,
                "parse_errors": self._parse_errors,
                "overlong_lines": self._overlong_lines,
                "reconnects": self._reconnects,
                "last_record_cursor": self._last_record_cursor,
                "root": self._latest_root.copy() if self._latest_root is not None else None,
            }

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
                    continue
                if record is not None:
                    self.bridge.record_from_transport(self, record)
                continue
            if self._discarding_overlong:
                continue
            if len(self._line) >= MAX_LINE_BYTES:
                self._line.clear()
                self._discarding_overlong = True
                with self._state_lock:
                    self._overlong_lines += 1
                continue
            self._line.append(byte)

    def _note_record(self, committed: dict[str, Any]) -> None:
        with self._state_lock:
            self._last_record_cursor = committed["cursor"]
            if committed["kind"] == "root":
                self._latest_root = {
                    key: value
                    for key, value in committed.items()
                    if key not in {"cursor", "device"}
                }

    def _set_connected(self, value: bool) -> None:
        with self._state_lock:
            self._connected = value

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
            if self._ever_connected:
                with self._state_lock:
                    self._reconnects += 1
            self._ever_connected = True
            self._backoff = INITIAL_RECONNECT_SECONDS
            self._set_connected(True)
            return
        self._next_connect = time.monotonic() + self._backoff
        self._backoff = min(self._backoff * 2, MAX_RECONNECT_SECONDS)

    def _close_fd(self) -> None:
        fd, self._fd = self._fd, None
        if fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass
        self._reset_framer()
        self._set_connected(False)

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
        if request.deadline <= time.monotonic():
            request.result = "write_failed"
            request.done.set()
            return
        self._active = request
        self._active_bytes = b"ROOT ON\r" if request.active else b"ROOT OFF\r"
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

    def submit(self, active: bool) -> str:
        return self.transport.submit(active)

    def is_connected(self) -> bool:
        return self.transport.is_connected()

    def snapshot(self) -> dict[str, Any]:
        return self.transport.snapshot(self.index, self.path)

    def feed_bytes(self, data: bytes) -> None:
        self.transport.feed_bytes(data)


class Bridge:
    """Shared state between the serial workers and the localhost HTTP server."""

    def __init__(
        self,
        serial_paths: list[str],
        *,
        timing: Timing = DEFAULT_TIMING,
        transport_factory: Callable[["Bridge", int, str, tuple[str, ...], Timing], SerialTransport] = SerialTransport,
    ) -> None:
        self.timing = timing
        self.ring = CursorRing()
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

    def record_from_transport(self, transport: SerialTransport, record: dict[str, Any]) -> None:
        committed = self.ring.append({"device": transport.owner_index, **record})
        transport._note_record(committed)

    def health(self) -> dict[str, Any]:
        oldest, current = self.ring.cursors()
        return {
            "schema": HEALTH_SCHEMA,
            "oldest_cursor": oldest,
            "current_cursor": current,
            "devices": [device.snapshot() for device in self.devices],
        }

    def root_command(self, device_index: int, active: bool) -> tuple[bool, str]:
        if device_index >= len(self.devices):
            return False, "unknown_device"
        result = self.devices[device_index].submit(active)
        return result == "accepted", result


def serial_grouping_identity(path: str) -> str:
    """Return the startup-only grouping identity for one serial argument."""

    try:
        return os.path.realpath(path, strict=getattr(os.path, "ALLOW_MISSING", False))
    except (OSError, ValueError):
        return os.path.abspath(os.path.normpath(path))


class BoundedThreadingHTTPServer(ThreadingMixIn, HTTPServer):
    """Threaded HTTP with a fixed request-worker ceiling."""

    daemon_threads = True
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
        self.assets = assets.resolve()
        super().__init__(("127.0.0.1", port), BridgeRequestHandler, bridge.timing)


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

    def handle(self) -> None:
        try:
            super().handle()
        except (OSError, TimeoutError):
            self.close_connection = True

    def _send(self, status: int, body: bytes, content_type: str, *, api: bool = False) -> None:
        try:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            if api:
                self.send_header("Cache-Control", "no-store")
            if self._close_after_response:
                self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        except (OSError, TimeoutError):
            self.close_connection = True
        if self._close_after_response:
            self.close_connection = True

    def _send_json(self, status: int, payload: dict[str, Any]) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self._send(status, body, "application/json; charset=utf-8", api=True)

    def _send_error(self, status: int, error: str) -> None:
        self._send_json(status, {"schema": ERROR_SCHEMA, "accepted": False, "error": error})

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlsplit(self.path)
        if parsed.path == "/api/events":
            query = self._event_query(parsed.query)
            if query is None:
                self._send_error(400, "invalid_query")
                return
            after, limit = query
            self._send_json(200, self.bridge_server.bridge.ring.page(after, limit))
            return
        if parsed.path == "/api/health" and not parsed.query:
            self._send_json(200, self.bridge_server.bridge.health())
            return
        self._serve_static(parsed.path)

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlsplit(self.path)
        if parsed.path != "/api/root" or parsed.query:
            self._send(404, b"Not found\n", "text/plain; charset=utf-8")
            return
        body = self._json_body()
        if isinstance(body, str):
            self._send_error(400, body)
            return
        device, active = body
        accepted, result = self.bridge_server.bridge.root_command(device, active)
        if accepted:
            self._send_json(
                202,
                {
                    "schema": COMMAND_SCHEMA,
                    "accepted": True,
                    "device": device,
                    "command": "on" if active else "off",
                },
            )
            return
        if result == "unknown_device":
            self._send_error(404, result)
        elif result in {"disconnected", "write_failed"}:
            self._send_error(503, result)
        else:
            self._send_error(503, "write_failed")

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

    def _json_body(self) -> tuple[int, bool] | str:
        lengths = self.headers.get_all("Content-Length") or []
        if len(lengths) != 1 or not re.fullmatch(DECIMAL, lengths[0]):
            return self._invalid_body()
        size = int(lengths[0])
        if size > MAX_REQUEST_BYTES:
            return self._invalid_body()
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
        if (
            type(parsed) is not dict
            or set(parsed) != {"device", "active"}
            or type(parsed["device"]) is not int
            or parsed["device"] < 0
            or type(parsed["active"]) is not bool
        ):
            return "invalid_body"
        return parsed["device"], parsed["active"]

    def _invalid_body(self) -> str:
        self._close_after_response = True
        return "invalid_body"

    def _serve_static(self, raw_path: str) -> None:
        decoded = unquote(raw_path)
        relative = "index.html" if decoded == "/" else decoded.lstrip("/")
        if not relative or "\x00" in relative or any(part in {"", ".", ".."} for part in relative.split("/")):
            self._send(404, b"Not found\n", "text/plain; charset=utf-8")
            return
        try:
            candidate = (self.bridge_server.assets / relative).resolve()
            candidate.relative_to(self.bridge_server.assets)
        except (OSError, ValueError):
            self._send(404, b"Not found\n", "text/plain; charset=utf-8")
            return
        if not candidate.is_file():
            self._send(404, b"Not found\n", "text/plain; charset=utf-8")
            return
        try:
            body = candidate.read_bytes()
        except OSError:
            self._send(404, b"Not found\n", "text/plain; charset=utf-8")
            return
        self._send(200, body, _mime_type(candidate))


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


def _port(value: str) -> int:
    if not re.fullmatch(DECIMAL, value):
        raise argparse.ArgumentTypeError("port must be a decimal number from 0 through 65535")
    port = int(value)
    if port > 65535:
        raise argparse.ArgumentTypeError("port must be a decimal number from 0 through 65535")
    return port


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", action="append", default=[], metavar="PATH", help="POSIX serial path (repeatable)")
    parser.add_argument("--port", type=_port, default=8787, help="localhost TCP port (default: 8787)")
    parser.add_argument(
        "--assets",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "dashboard" / "dist",
        help="built dashboard asset directory (default: dashboard/dist)",
    )
    return parser.parse_args(argv)


def serve(bridge: Bridge, assets: Path, port: int) -> BridgeHTTPServer:
    return BridgeHTTPServer(bridge, assets, port)


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(argv)
    bridge = Bridge(args.serial)
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
