from __future__ import annotations

import http.client
import json
import os
import pty
import select
import socket
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from host import bridge  # noqa: E402


EVENT = (
    b"mind_event_v1 now=55 root=8081545678c0 wearable=7 packet=a1b2c3 "
    b"schema=1 event=5 confidence=73 svm=6687 mic=125 seq=195 "
    b"observer=0102545678c0 path=tavrn\n"
)
EVENT_V2 = (
    b"mind_event_v2 now=55 root=8081545678c0 wearable=7 packet=a1b2c3 "
    b"schema=1 event=5 confidence=73 svm=6687 mic=125 seq=195 "
    b"observer=0102545678c0 observer_rssi_dbm=-37 path=tavrn\n"
)
ROOT = (
    b"mind_root_v1 now=99 local=8081545678c0 node=6 role=root roots=16 "
    b"announced=2 acked=1 rejected=3 pending=4 rootless_drop=7\n"
)
COMMAND = b"mind_command_v1 now=12 local=8081545678c0 command=on status=accepted\n"


def gtt_begin(query: int = 100, local: str = "8081545678c0", entries: int = 1, nondeparted: int = 1) -> bytes:
    return f"mind_gtt_begin_v1 query={query} local={local} entries={entries} nondeparted={nondeparted}\n".encode()


def gtt_entry(
    index: int = 0,
    *,
    query: int = 100,
    adva: str = "8081545678c0",
    last: int = 90,
    soft: int = 110,
    hard: int = 120,
    departed_deadline: int = 130,
    serial: int = 4,
    serial_state: int = 1,
    hop: int = 2,
    hop_state: int = 1,
    freshness: int = 1,
    departed: int = 1,
) -> bytes:
    return (
        f"mind_gtt_entry_v1 query={query} index={index} adva={adva} last={last} soft={soft} hard={hard} "
        f"departed_deadline={departed_deadline} serial={serial} serial_state={serial_state} hop={hop} "
        f"hop_state={hop_state} freshness={freshness} departed={departed}\n"
    ).encode()


def gtt_end(query: int = 100, local: str = "8081545678c0", entries: int = 1, nondeparted: int = 1) -> bytes:
    return f"mind_gtt_end_v1 query={query} local={local} entries={entries} nondeparted={nondeparted}\n".encode()


def eventually(predicate, timeout: float = 2.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.01)
    return bool(predicate())


def read_exact(fd: int, expected: int, timeout: float = 2.0) -> bytes:
    result = bytearray()
    deadline = time.monotonic() + timeout
    while len(result) < expected and time.monotonic() < deadline:
        readable, _, _ = select.select([fd], [], [], max(0.0, deadline - time.monotonic()))
        if readable:
            try:
                result.extend(os.read(fd, expected - len(result)))
            except OSError:
                break
    return bytes(result)


def receive_until_eof(connection: socket.socket, timeout: float = 1.0) -> bytes:
    result = bytearray()
    connection.settimeout(timeout)
    while True:
        try:
            chunk = connection.recv(4096)
        except TimeoutError:
            break
        if not chunk:
            break
        result.extend(chunk)
    return bytes(result)


class ServerHarness:
    def __init__(self, state: bridge.Bridge, assets: Path) -> None:
        self.state = state
        self.server = bridge.serve(state, assets, 0)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def port(self) -> int:
        return self.server.server_address[1]

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.state.stop()

    def request(
        self,
        method: str,
        target: str,
        body: bytes | None = None,
        headers: dict[str, str] | None = None,
    ) -> tuple[int, dict[str, str], bytes]:
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        connection.request(method, target, body=body, headers=headers or {})
        response = connection.getresponse()
        data = response.read()
        result = response.status, dict(response.getheaders()), data
        connection.close()
        return result

    def raw_request(
        self,
        method: str,
        target: str,
        headers: list[tuple[str, str]],
        body: bytes = b"",
    ) -> tuple[int, dict[str, str], bytes]:
        request = (
            f"{method} {target} HTTP/1.1\r\n".encode("ascii")
            + b"".join(f"{name}: {value}\r\n".encode("ascii") for name, value in headers)
            + b"Connection: close\r\n\r\n"
            + body
        )
        connection = socket.create_connection(("127.0.0.1", self.port), timeout=3)
        try:
            connection.sendall(request)
            response = receive_until_eof(connection, timeout=3)
        finally:
            connection.close()
        response_headers, separator, response_body = response.partition(b"\r\n\r\n")
        if not separator:
            raise AssertionError(f"malformed HTTP response: {response!r}")
        lines = response_headers.split(b"\r\n")
        status = int(lines[0].split()[1])
        parsed_headers = {
            name.decode("ascii"): value.lstrip().decode("ascii")
            for line in lines[1:]
            for name, value in [line.split(b":", 1)]
        }
        return status, parsed_headers, response_body


class ParserTests(unittest.TestCase):
    def test_exact_event_root_and_command_records(self) -> None:
        event = bridge.parse_firmware_line(EVENT.rstrip())
        self.assertEqual(
            event,
            {
                "kind": "event",
                "now": 55,
                "root": "8081545678c0",
                "wearable": 7,
                "packet": "a1b2c3",
                "schema": 1,
                "event": 5,
                "confidence": 73,
                "svm": 6687,
                "mic": 125,
                "seq": 195,
                "observer": "0102545678c0",
                "observer_rssi_dbm": None,
                "path": "tavrn",
            },
        )
        self.assertEqual(bridge.parse_firmware_line(ROOT.rstrip())["kind"], "root")
        self.assertEqual(bridge.parse_firmware_line(COMMAND.rstrip())["command"], "on")
        self.assertEqual(
            bridge.parse_firmware_line(COMMAND.replace(b"command=on", b"command=gtt").rstrip())["command"],
            "gtt",
        )
        self.assertEqual(bridge.parse_firmware_line(EVENT.rstrip() + b"\r")["packet"], "a1b2c3")

    def test_exact_gtt_records_and_ranges(self) -> None:
        self.assertEqual(
            bridge.parse_firmware_line(gtt_begin(query=0, entries=16, nondeparted=16).rstrip()),
            {"kind": "gtt_begin", "query": 0, "local": "8081545678c0", "entries": 16, "nondeparted": 16},
        )
        entry = bridge.parse_firmware_line(
            gtt_entry(
                15,
                query=0xFFFFFFFF,
                last=0xFFFFFFFF,
                soft=0xFFFFFFFF,
                hard=0xFFFFFFFF,
                departed_deadline=0xFFFFFFFF,
                serial=0xFFFF,
                serial_state=2,
                hop=0xFF,
                hop_state=2,
                freshness=4,
                departed=2,
            ).rstrip()
        )
        self.assertEqual(entry["kind"], "gtt_entry")
        self.assertEqual(entry["hop"], 0xFF)
        self.assertEqual(bridge.parse_firmware_line(gtt_end(query=0, entries=0, nondeparted=0).rstrip())["kind"], "gtt_end")
        invalid = [
            gtt_begin(entries=17).rstrip(),
            gtt_entry(index=16).rstrip(),
            gtt_entry(serial=65536).rstrip(),
            gtt_entry(serial_state=3).rstrip(),
            gtt_entry(hop=256).rstrip(),
            gtt_entry(hop_state=3).rstrip(),
            gtt_entry(freshness=5).rstrip(),
            gtt_entry(departed=4).rstrip(),
            gtt_entry().replace(b"adva=8081545678c0", b"adva=8081545678C0").rstrip(),
            gtt_end(nondeparted=17).rstrip(),
        ]
        for line in invalid:
            with self.subTest(line=line), self.assertRaises(bridge.ParseError):
                bridge.parse_firmware_line(line)

    def test_invalid_recognized_lines_and_unrelated_diagnostics(self) -> None:
        invalid = [
            EVENT.replace(b"packet=a1b2c3", b"packet=A1B2C3").rstrip(),
            EVENT.replace(b"wearable=7", b"wearable=0").rstrip(),
            EVENT.replace(b"event=5", b"event=6").rstrip(),
            EVENT.replace(b"confidence=73", b"confidence=101").rstrip(),
            EVENT.replace(b"svm=6687", b"svm=8001").rstrip(),
            EVENT.replace(b"seq=195", b"seq=1").rstrip(),
            EVENT.replace(b"schema=1", b"schema=01").rstrip(),
            ROOT.replace(b"node=6", b"node=7").rstrip(),
            COMMAND.replace(b"status=accepted", b"status=ok").rstrip(),
            gtt_entry().replace(b"hop=2", b"hop=02").rstrip(),
            b"mind_event_v1 completely unrelated",
        ]
        for line in invalid:
            with self.subTest(line=line), self.assertRaises(bridge.ParseError):
                bridge.parse_firmware_line(line)
        heartbeat = EVENT.replace(b"event=5 confidence=73", b"event=0 confidence=1")
        with self.assertRaises(bridge.ParseError):
            bridge.parse_firmware_line(heartbeat.rstrip())
        self.assertIsNone(bridge.parse_firmware_line(b"UART ready"))

    def test_exact_v2_event_normalizes_signed_observer_rssi_and_rejects_invalid_ranges(self) -> None:
        record = bridge.parse_firmware_line(EVENT_V2.rstrip())
        self.assertEqual(record["observer_rssi_dbm"], -37)
        ring = bridge.CursorRing()
        with mock.patch.object(bridge.time, "time", return_value=1_700_000_000.123):
            self.assertEqual(ring.append(record)["observer_rssi_dbm"], -37)
        self.assertEqual((ring.page(0, 1)["schema"], ring.page(0, 1)["events"][0]["observer_rssi_dbm"]), ("mind.api.v2", -37))
        self.assertEqual(ring.page(0, 1)["events"][0]["received_at_ms"], 1_700_000_000_123)
        for invalid in (
            EVENT_V2.replace(b"observer_rssi_dbm=-37", b"observer_rssi_dbm=37"),
            EVENT_V2.replace(b"observer_rssi_dbm=-37", b"observer_rssi_dbm=-0"),
            EVENT_V2.replace(b"observer_rssi_dbm=-37", b"observer_rssi_dbm=-128"),
            EVENT_V2.replace(b"observer_rssi_dbm=-37", b"observer_rssi_dbm=-037"),
            EVENT_V2.replace(b"path=tavrn", b"observer_rssi_dbm=-37 path=tavrn"),
        ):
            with self.subTest(invalid=invalid), self.assertRaises(bridge.ParseError):
                bridge.parse_firmware_line(invalid.rstrip())

    def test_partial_overlong_and_recovery_do_not_mutate_early_state(self) -> None:
        state = bridge.Bridge(["test"])
        device = state.devices[0]
        device.feed_bytes(EVENT[:20])
        self.assertEqual(state.ring.cursors(), (1, 0))
        device.feed_bytes(EVENT[20:])
        self.assertEqual(state.ring.cursors(), (1, 1))
        device.feed_bytes(b"mind_event_v1 " + b"x" * (bridge.MAX_LINE_BYTES + 1) + b"\n")
        device.feed_bytes(b"diagnostic line\n" + ROOT)
        health = state.health()["devices"][0]
        self.assertEqual(health["overlong_lines"], 1)
        self.assertEqual(health["parse_errors"], 0)
        self.assertEqual(state.ring.cursors(), (1, 2))
        device.feed_bytes(b"mind_root_v1 bad\n")
        self.assertEqual(state.health()["devices"][0]["parse_errors"], 1)


class RingTests(unittest.TestCase):
    def test_bridge_sessions_are_opaque_per_process_and_only_events_receive_host_timestamps(self) -> None:
        state = bridge.Bridge(["test"])
        other = bridge.Bridge(["other"])
        self.addCleanup(state.stop)
        self.addCleanup(other.stop)
        self.assertRegex(state.session_id, r"^[0-9a-f]{32}$")
        self.assertNotEqual(state.session_id, other.session_id)

        with mock.patch.object(bridge.time, "time", return_value=1_700_000_000.789):
            state.devices[0].feed_bytes(EVENT + ROOT + COMMAND)
        page = state.events_page(0, 100)
        self.assertEqual(page["session_id"], state.session_id)
        self.assertEqual(page["events"][0]["received_at_ms"], 1_700_000_000_789)
        self.assertNotIn("received_at_ms", page["events"][1])
        self.assertNotIn("received_at_ms", page["events"][2])

    def test_256_boundary_cursor_and_gap_semantics(self) -> None:
        ring = bridge.CursorRing()
        for value in range(257):
            ring.append({"kind": "command", "device": 0, "now": value, "local": "000000000000", "command": "on", "status": "accepted"})
        stale = ring.page(0, 100)
        self.assertTrue(stale["gap"])
        self.assertEqual(stale["oldest_cursor"], 2)
        self.assertEqual(stale["current_cursor"], 257)
        self.assertEqual(stale["events"][0]["cursor"], 2)
        self.assertEqual(len(stale["events"]), 100)
        exact = ring.page(1, 100)
        self.assertFalse(exact["gap"])
        self.assertEqual(exact["events"][0]["cursor"], 2)
        self.assertEqual(ring.page(257, 100)["events"], [])

    def test_latest_root_health_shape_keeps_cursor_and_removes_device(self) -> None:
        state = bridge.Bridge(["test"])
        state.devices[0].feed_bytes(ROOT)
        health = state.health()
        root = health["devices"][0]["root"]
        self.assertEqual(
            root,
            {
                "cursor": 1,
                "kind": "root",
                "now": 99,
                "local": "8081545678c0",
                "node": 6,
                "role": "root",
                "roots": 16,
                "announced": 2,
                "acked": 1,
                "rejected": 3,
                "pending": 4,
                "rootless_drop": 7,
            },
        )
        self.assertNotIn("device", root)
        self.assertEqual(health["devices"][0]["last_record_cursor"], 1)

    def test_root_cursor_tracks_only_root_authority_not_aggregate_events(self) -> None:
        state = bridge.Bridge(["test"])
        device = state.devices[0]
        device.feed_bytes(ROOT)
        first = state.health()
        self.assertEqual((first["current_cursor"], first["devices"][0]["root"]["cursor"]), (1, 1))

        device.feed_bytes(EVENT)
        unrelated = state.health()
        self.assertEqual((unrelated["current_cursor"], unrelated["devices"][0]["root"]["cursor"]), (2, 1))

        device.feed_bytes(ROOT.replace(b"now=99", b"now=100"))
        updated = state.health()
        self.assertEqual((updated["current_cursor"], updated["devices"][0]["root"]["cursor"]), (3, 3))

    def test_stable_repeated_serial_indices_at_capacity_and_duplicates(self) -> None:
        self.assertEqual([device.index for device in bridge.Bridge(["one"]).devices], [0])
        self.assertEqual([device.index for device in bridge.Bridge(["one", "two"]).devices], [0, 1])
        paths = ["same", "same"] + [f"serial-{index}" for index in range(14)]
        state = bridge.Bridge(paths)
        self.assertEqual(len(state.devices), 16)
        self.assertEqual(len(state.transports), 15)
        self.assertEqual([(device.index, device.path) for device in state.devices[:3]], [(0, "same"), (1, "same"), (2, "serial-0")])
        self.assertEqual(state.health()["devices"][-1]["device"], 15)


class HealthSnapshotTests(unittest.TestCase):
    def test_health_never_exposes_a_device_cursor_after_its_ring_cursor(self) -> None:
        state = bridge.Bridge(["test"])
        transport = state.transports[0]
        original_cursors = state.ring.cursors
        cursors_read = threading.Event()
        release_health = threading.Event()
        published = threading.Event()
        results: list[dict[str, object]] = []

        def gated_cursors() -> tuple[int, int]:
            cursors = original_cursors()
            cursors_read.set()
            self.assertTrue(release_health.wait(timeout=1))
            return cursors

        def publish() -> None:
            record = bridge.parse_firmware_line(EVENT.rstrip())
            self.assertIsNotNone(record)
            state.record_from_transport(transport, record)
            published.set()

        state.ring.cursors = gated_cursors  # type: ignore[method-assign]
        reader = threading.Thread(target=lambda: results.append(state.health()))
        writer = threading.Thread(target=publish)
        reader.start()
        self.assertTrue(cursors_read.wait(timeout=1))
        writer.start()
        self.assertFalse(published.wait(timeout=0.1))
        release_health.set()
        reader.join(timeout=1)
        writer.join(timeout=1)
        self.assertFalse(reader.is_alive())
        self.assertFalse(writer.is_alive())
        self.assertEqual(len(results), 1)
        health = results[0]
        self.assertTrue(
            all(device["last_record_cursor"] <= health["current_cursor"] for device in health["devices"]),
        )

    def test_health_snapshot_boundary_includes_gtt_publication_atomically(self) -> None:
        state = bridge.Bridge(["test"])
        transport = state.transports[0]
        state.devices[0].feed_bytes(gtt_begin() + gtt_entry())
        end_record = bridge.parse_firmware_line(gtt_end().rstrip())
        self.assertIsNotNone(end_record)
        original_cursors = state.ring.cursors
        cursors_read = threading.Event()
        release_health = threading.Event()
        published = threading.Event()
        results: list[dict[str, object]] = []

        def gated_cursors() -> tuple[int, int]:
            cursors = original_cursors()
            cursors_read.set()
            self.assertTrue(release_health.wait(timeout=1))
            return cursors

        def publish() -> None:
            state.record_from_transport(transport, end_record)
            published.set()

        state.ring.cursors = gated_cursors  # type: ignore[method-assign]
        reader = threading.Thread(target=lambda: results.append(state.health()))
        writer = threading.Thread(target=publish)
        reader.start()
        self.assertTrue(cursors_read.wait(timeout=1))
        writer.start()
        self.assertFalse(published.wait(timeout=0.1))
        release_health.set()
        reader.join(timeout=1)
        writer.join(timeout=1)
        self.assertFalse(reader.is_alive())
        self.assertFalse(writer.is_alive())
        self.assertIsNone(results[0]["devices"][0]["gtt"])
        self.assertEqual(state.health()["devices"][0]["gtt"]["generation"], 1)

    def test_health_snapshots_each_physical_transport_once_then_projects_aliases(self) -> None:
        state = bridge.Bridge(["same", "same", "other"])
        state.devices[0].feed_bytes(ROOT + gtt_begin() + gtt_entry() + gtt_end())
        calls = {transport: 0 for transport in state.transports}
        for transport in state.transports:
            original_snapshot = transport.physical_snapshot

            def counted_snapshot(
                original_snapshot: object = original_snapshot,
                transport: bridge.SerialTransport = transport,
            ) -> dict[str, object]:
                calls[transport] += 1
                return original_snapshot()  # type: ignore[operator]

            transport.physical_snapshot = counted_snapshot  # type: ignore[method-assign]

        health = state.health()
        self.assertEqual(list(calls.values()), [1, 1])
        first, alias, other = health["devices"]
        self.assertEqual(
            [(first["device"], first["path"]), (alias["device"], alias["path"]), (other["device"], other["path"])],
            [(0, "same"), (1, "same"), (2, "other")],
        )
        self.assertEqual((first["owner_device"], alias["owner_device"], other["owner_device"]), (0, 0, 2))
        self.assertEqual(first["root"], alias["root"])
        self.assertEqual((first["root"]["cursor"], alias["root"]["cursor"]), (1, 1))
        self.assertEqual(first["gtt"], alias["gtt"])
        self.assertEqual(first["gtt"]["generation"], 1)
        self.assertIsNone(other["root"])
        self.assertIsNone(other["gtt"])


class GTTAssemblyTests(unittest.TestCase):
    def _state(self) -> bridge.Bridge:
        state = bridge.Bridge(["test"])
        self.addCleanup(state.stop)
        return state

    def test_complete_snapshot_is_atomic_normalized_and_excluded_from_event_ring(self) -> None:
        state = self._state()
        device = state.devices[0]
        device.feed_bytes(gtt_begin(entries=3, nondeparted=2))
        device.feed_bytes(
            gtt_entry(0, serial_state=0, hop_state=0, freshness=0, departed=0)
            + EVENT
            + ROOT
            + COMMAND.replace(b"command=on", b"command=gtt")
        )
        self.assertIsNone(state.health()["devices"][0]["gtt"])
        self.assertEqual([record["kind"] for record in state.ring.page(0, 100)["events"]], ["event", "root", "command"])
        device.feed_bytes(gtt_entry(1, adva="0102545678c1", serial_state=2, hop_state=2, freshness=4, departed=2))
        self.assertIsNone(state.health()["devices"][0]["gtt"])
        started = int(time.time() * 1000)
        device.feed_bytes(gtt_entry(2, adva="0102545678c2", serial_state=1, hop_state=1, freshness=3, departed=3) + gtt_end(entries=3, nondeparted=2))
        snapshot = state.health()["devices"][0]["gtt"]
        self.assertEqual(snapshot["generation"], 1)
        self.assertGreaterEqual(snapshot["completed_at_ms"], started)
        self.assertEqual(
            {key: snapshot[key] for key in snapshot if key != "completed_at_ms"},
            {
                "generation": 1,
                "query_at_ms": 100,
                "local": "8081545678c0",
                "entry_count": 3,
                "nondeparted_count": 2,
                "entries": [
                    {
                        "index": 0,
                        "adva": "8081545678c0",
                        "last": 90,
                        "soft": 110,
                        "hard": 120,
                        "departed_deadline": 130,
                        "serial": 4,
                        "serial_state": "not_applicable",
                        "hop": 2,
                        "hop_state": "not_applicable",
                        "freshness": "not_applicable",
                        "departed": "not_applicable",
                    },
                    {
                        "index": 1,
                        "adva": "0102545678c1",
                        "last": 90,
                        "soft": 110,
                        "hard": 120,
                        "departed_deadline": 130,
                        "serial": 4,
                        "serial_state": "unknown",
                        "hop": 2,
                        "hop_state": "unknown",
                        "freshness": "departed",
                        "departed": "true",
                    },
                    {
                        "index": 2,
                        "adva": "0102545678c2",
                        "last": 90,
                        "soft": 110,
                        "hard": 120,
                        "departed_deadline": 130,
                        "serial": 4,
                        "serial_state": "known",
                        "hop": 2,
                        "hop_state": "known",
                        "freshness": "hard_expired",
                        "departed": "unknown",
                    },
                ],
            },
        )

    def test_zero_entry_snapshot_fails_closed_and_sixteen_entry_snapshot_requires_self(self) -> None:
        state = self._state()
        device = state.devices[0]
        device.feed_bytes(gtt_begin(query=10, entries=0, nondeparted=0) + gtt_end(query=10, entries=0, nondeparted=0))
        self.assertIsNone(state.health()["devices"][0]["gtt"])
        device.feed_bytes(gtt_begin(query=11, entries=16, nondeparted=16))
        for index in range(16):
            adva = "8081545678c0" if index == 0 else f"{index + 1:012x}"
            device.feed_bytes(gtt_entry(index, query=11, adva=adva))
        self.assertIsNone(state.health()["devices"][0]["gtt"])
        device.feed_bytes(gtt_end(query=11, entries=16, nondeparted=16))
        snapshot = state.health()["devices"][0]["gtt"]
        self.assertEqual((snapshot["generation"], snapshot["entry_count"], snapshot["nondeparted_count"]), (1, 16, 16))
        self.assertEqual([entry["index"] for entry in snapshot["entries"]], list(range(16)))

    def test_replacement_and_invalid_records_discard_only_partial_assembly(self) -> None:
        def assert_invalid(lines: bytes) -> None:
            state = self._state()
            state.devices[0].feed_bytes(lines)
            self.assertIsNone(state.health()["devices"][0]["gtt"])

        state = self._state()
        device = state.devices[0]
        device.feed_bytes(gtt_begin(query=1) + gtt_entry(query=1) + gtt_begin(query=2, entries=0, nondeparted=0) + gtt_end(query=2, entries=0, nondeparted=0))
        self.assertIsNone(state.health()["devices"][0]["gtt"])

        assert_invalid(gtt_begin() + b"mind_gtt_entry_v1 malformed\n" + gtt_entry() + gtt_end())
        assert_invalid(gtt_begin() + gtt_entry() + gtt_entry() + gtt_end())
        assert_invalid(gtt_begin() + gtt_entry(1) + gtt_entry() + gtt_end())
        assert_invalid(gtt_begin() + gtt_entry(query=101) + gtt_end())
        assert_invalid(gtt_begin() + gtt_entry() + gtt_end(query=101))
        assert_invalid(gtt_begin() + b"mind_gtt_entry_v1 " + b"x" * (bridge.MAX_LINE_BYTES + 1) + b"\n" + gtt_entry() + gtt_end())
        assert_invalid(gtt_begin() + gtt_end())

    def test_nested_begin_revokes_a_prior_valid_roster(self) -> None:
        state = self._state()
        device = state.devices[0]
        device.feed_bytes(gtt_begin(query=1) + gtt_entry(query=1) + gtt_end(query=1))
        self.assertEqual(state.health()["devices"][0]["gtt"]["generation"], 1)

        device.feed_bytes(gtt_begin(query=2))
        self.assertEqual(state.health()["devices"][0]["gtt"]["generation"], 1)
        device.feed_bytes(gtt_begin(query=3))
        self.assertIsNone(state.health()["devices"][0]["gtt"])

        device.feed_bytes(gtt_entry(query=3) + gtt_end(query=3))
        self.assertEqual(state.health()["devices"][0]["gtt"]["generation"], 2)

    def test_terminal_validation_rejects_cross_field_constraints(self) -> None:
        cases = [
            gtt_begin(entries=0, nondeparted=1) + gtt_end(entries=0, nondeparted=1),
            gtt_begin() + gtt_entry(hop=16) + gtt_end(),
            gtt_begin(nondeparted=0) + gtt_entry(freshness=1, departed=2) + gtt_end(nondeparted=0),
            gtt_begin(nondeparted=0) + gtt_entry(freshness=1, departed=1) + gtt_end(nondeparted=0),
        ]
        for lines in cases:
            with self.subTest(lines=lines):
                state = self._state()
                state.devices[0].feed_bytes(lines)
                self.assertIsNone(state.health()["devices"][0]["gtt"])

    def test_roster_fails_closed_without_exactly_one_self_or_matching_root(self) -> None:
        state = self._state()
        device = state.devices[0]
        device.feed_bytes(gtt_begin() + gtt_entry(adva="0102545678c0") + gtt_end())
        self.assertIsNone(state.health()["devices"][0]["gtt"])

        device.feed_bytes(gtt_begin(query=101, entries=2, nondeparted=2))
        device.feed_bytes(gtt_entry(0, query=101) + gtt_entry(1, query=101, adva="8081545678c0") + gtt_end(query=101, entries=2, nondeparted=2))
        self.assertIsNone(state.health()["devices"][0]["gtt"])

        device.feed_bytes(gtt_begin(query=102) + gtt_entry(query=102) + gtt_end(query=102))
        self.assertIsNotNone(state.health()["devices"][0]["gtt"])
        device.feed_bytes(ROOT.replace(b"local=8081545678c0", b"local=0102545678c0"))
        self.assertIsNone(state.health()["devices"][0]["gtt"])

    def test_newer_terminal_identity_failures_clear_a_previously_valid_roster(self) -> None:
        state = self._state()
        device = state.devices[0]

        def valid(query: int) -> None:
            device.feed_bytes(gtt_begin(query=query) + gtt_entry(query=query) + gtt_end(query=query))
            self.assertIsNotNone(state.health()["devices"][0]["gtt"])

        valid(1)
        device.feed_bytes(
            gtt_begin(query=2, entries=1)
            + gtt_entry(query=2, adva="0102545678c0")
            + gtt_end(query=2, entries=1)
        )
        self.assertIsNone(state.health()["devices"][0]["gtt"])

        valid(3)
        device.feed_bytes(
            gtt_begin(query=4, entries=2, nondeparted=2)
            + gtt_entry(0, query=4)
            + gtt_entry(1, query=4)
            + gtt_end(query=4, entries=2, nondeparted=2)
        )
        self.assertIsNone(state.health()["devices"][0]["gtt"])

        valid(5)
        device.feed_bytes(gtt_begin(query=6) + gtt_entry(query=6) + gtt_end(query=6, local="0102545678c0"))
        self.assertIsNone(state.health()["devices"][0]["gtt"])

        valid(7)
        device.feed_bytes(b"mind_gtt_entry_v1 malformed\n")
        self.assertIsNotNone(state.health()["devices"][0]["gtt"])

    def test_invalid_active_refresh_revokes_prior_roster_until_a_later_complete_response(self) -> None:
        invalid_entries = (
            ("malformed", b"mind_gtt_entry_v1 malformed\n"),
            ("overlong", b"mind_gtt_entry_v1 " + b"x" * (bridge.MAX_LINE_BYTES + 1) + b"\n"),
            ("mismatched", gtt_entry(query=3)),
            ("out_of_order", gtt_entry(index=1, query=2)),
        )
        for name, invalid in invalid_entries:
            with self.subTest(name=name):
                state = self._state()
                device = state.devices[0]
                device.feed_bytes(gtt_begin(query=1) + gtt_entry(query=1) + gtt_end(query=1))
                self.assertEqual(state.health()["devices"][0]["gtt"]["generation"], 1)

                # These trailing records would complete the newer response if
                # the invalid record had not revoked its partial assembly.
                device.feed_bytes(
                    gtt_begin(query=2)
                    + invalid
                    + gtt_entry(query=2)
                    + gtt_end(query=2)
                )
                self.assertIsNone(state.health()["devices"][0]["gtt"])

                device.feed_bytes(gtt_begin(query=4) + gtt_entry(query=4) + gtt_end(query=4))
                restored = state.health()["devices"][0]["gtt"]
                self.assertEqual((restored["generation"], restored["query_at_ms"]), (2, 4))


class AliasTransportTests(unittest.TestCase):
    def test_duplicate_pty_aliases_share_one_transport_and_first_owner(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            master, slave = pty.openpty()
            path = os.ttyname(slave)
            alias = str(Path(temporary) / "serial-alias")
            os.symlink(path, alias)
            os.close(slave)
            opens: list[str] = []

            def factory(
                state: bridge.Bridge,
                owner_index: int,
                grouping_identity: str,
                reopen_paths: tuple[str, ...],
                timing: bridge.Timing,
            ) -> bridge.SerialTransport:
                def counted_open(open_path: str, flags: int) -> int:
                    opens.append(open_path)
                    return os.open(open_path, flags)

                return bridge.SerialTransport(
                    state,
                    owner_index,
                    grouping_identity,
                    reopen_paths,
                    timing,
                    open_fn=counted_open,
                )

            state = bridge.Bridge([path, alias, path], transport_factory=factory)
            state.start()
            try:
                self.assertEqual([device.index for device in state.devices], [0, 1, 2])
                self.assertEqual([device.path for device in state.devices], [path, alias, path])
                self.assertEqual(len(state.transports), 1)
                self.assertTrue(eventually(state.devices[0].is_connected))
                self.assertEqual(opens, [path])
                self.assertEqual(read_exact(master, len(bridge.ROOT_STATUS)), bridge.ROOT_STATUS)
                os.write(master, EVENT + ROOT)
                self.assertTrue(eventually(lambda: state.ring.cursors()[1] == 2))
                records = state.ring.page(0, 100)["events"]
                self.assertEqual([record["device"] for record in records], [0, 0])
                health = state.health()["devices"]
                self.assertEqual([entry["device"] for entry in health], [0, 1, 2])
                self.assertEqual([entry["connected"] for entry in health], [True, True, True])
                self.assertEqual([entry["reconnects"] for entry in health], [0, 0, 0])
                self.assertEqual([entry["last_record_cursor"] for entry in health], [2, 2, 2])
                self.assertEqual([entry["root"] for entry in health], [health[0]["root"]] * 3)
                self.assertEqual([entry["root"]["cursor"] for entry in health], [2, 2, 2])
                self.assertEqual([entry["owner_device"] for entry in health], [0, 0, 0])
                os.write(master, gtt_begin() + gtt_entry() + gtt_end())
                self.assertTrue(eventually(lambda: state.health()["devices"][0]["gtt"] is not None))
                self.assertEqual([entry["gtt"] for entry in state.health()["devices"]], [state.health()["devices"][0]["gtt"]] * 3)
                os.write(master, b"mind_root_v1 bad\n")
                self.assertTrue(eventually(lambda: state.health()["devices"][0]["parse_errors"] == 1))
                self.assertEqual([entry["parse_errors"] for entry in state.health()["devices"]], [1, 1, 1])

                def partial_write(fd: int, data: bytes) -> int:
                    return os.write(fd, data[:2])

                state.devices[1].write_fn = partial_write
                results: list[str] = []
                callers = [
                    threading.Thread(target=lambda: results.append(state.devices[0].submit(bridge.ROOT_ON))),
                    threading.Thread(target=lambda: results.append(state.devices[2].submit(bridge.ROOT_OFF))),
                ]
                for caller in callers:
                    caller.start()
                for caller in callers:
                    caller.join(timeout=2)
                self.assertEqual(sorted(results), ["accepted", "accepted"])
                output = read_exact(master, len(b"ROOT ON\r") + len(b"ROOT OFF\r"))
                self.assertIn(output, (b"ROOT ON\rROOT OFF\r", b"ROOT OFF\rROOT ON\r"))
                self.assertEqual(opens, [path])
            finally:
                state.stop()
                os.close(master)

    def test_reconnect_retries_configured_alias_after_direct_target_disappears(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            first_master, first_slave = pty.openpty()
            second_master, second_slave = pty.openpty()
            first_target = os.ttyname(first_slave)
            second_target = os.ttyname(second_slave)
            alias = str(Path(temporary) / "stable-by-id")
            os.symlink(first_target, alias)
            os.close(first_slave)
            os.close(second_slave)
            attempts: list[str] = []

            def factory(
                state: bridge.Bridge,
                owner_index: int,
                grouping_identity: str,
                reopen_paths: tuple[str, ...],
                timing: bridge.Timing,
            ) -> bridge.SerialTransport:
                def counted_open(open_path: str, flags: int) -> int:
                    attempts.append(open_path)
                    return os.open(open_path, flags)

                return bridge.SerialTransport(
                    state,
                    owner_index,
                    grouping_identity,
                    reopen_paths,
                    timing,
                    open_fn=counted_open,
                )

            state = bridge.Bridge(
                [first_target, alias],
                timing=bridge.Timing(serial_poll_interval=0.01),
                transport_factory=factory,
            )
            state.start()
            try:
                self.assertEqual(len(state.transports), 1)
                self.assertIs(state.devices[0].transport, state.devices[1].transport)
                self.assertTrue(eventually(state.devices[0].is_connected))
                self.assertEqual(attempts, [first_target])
                self.assertEqual(read_exact(first_master, len(bridge.ROOT_STATUS)), bridge.ROOT_STATUS)
                reconnect_attempt = len(attempts)
                os.close(first_master)
                self.assertTrue(eventually(lambda: not state.devices[0].is_connected()))

                replacement = Path(temporary) / "stable-by-id.retarget"
                os.symlink(second_target, replacement)
                os.replace(replacement, alias)

                self.assertTrue(eventually(lambda: state.devices[0].is_connected()))
                self.assertTrue(eventually(lambda: state.health()["devices"][0]["reconnects"] >= 1))
                self.assertEqual(read_exact(second_master, len(bridge.ROOT_STATUS)), bridge.ROOT_STATUS)
                retry_paths = attempts[reconnect_attempt:]
                self.assertGreaterEqual(len(retry_paths), 2)
                self.assertEqual(retry_paths[0], first_target)
                self.assertIn(alias, retry_paths)
                self.assertEqual(len(state.transports), 1)
                self.assertEqual([device.index for device in state.devices], [0, 1])
                self.assertEqual([device.path for device in state.devices], [first_target, alias])

                os.write(second_master, EVENT)
                self.assertTrue(eventually(lambda: state.ring.cursors()[1] == 1))
                self.assertEqual(state.ring.page(0, 1)["events"][0]["device"], 0)
                self.assertEqual(state.devices[1].submit(bridge.ROOT_ON), "accepted")
                self.assertEqual(read_exact(second_master, len(b"ROOT ON\r")), b"ROOT ON\r")
                health = state.health()["devices"]
                self.assertEqual([entry["reconnects"] for entry in health], [health[0]["reconnects"]] * 2)
                self.assertEqual([entry["last_record_cursor"] for entry in health], [1, 1])
            finally:
                state.stop()
                try:
                    os.close(first_master)
                except OSError:
                    pass
                os.close(second_master)


class ReconnectFramingTests(unittest.TestCase):
    def _transport_with_replacement_socket(self) -> tuple[bridge.Bridge, bridge.SerialTransport, socket.socket, socket.socket]:
        left_one, peer_one = socket.socketpair()
        left_two, peer_two = socket.socketpair()
        descriptors = [left_one.detach(), left_two.detach()]

        def fake_open(_path: str, _flags: int) -> int:
            if descriptors:
                return descriptors.pop(0)
            raise OSError("unavailable")

        state = bridge.Bridge([])
        transport = bridge.SerialTransport(
            state,
            0,
            "fake",
            ("fake",),
            bridge.Timing(command_deadline=0.2, serial_poll_interval=0.01),
            open_fn=fake_open,
            configure_fn=lambda _fd: None,
        )
        transport.start()
        self.assertTrue(eventually(transport.is_connected))
        return state, transport, peer_one, peer_two

    def test_partial_line_is_discarded_on_reconnect(self) -> None:
        state, transport, peer_one, peer_two = self._transport_with_replacement_socket()
        try:
            peer_one.sendall(EVENT[:30])
            self.assertTrue(eventually(lambda: len(transport._line) > 0))
            peer_one.close()
            self.assertTrue(eventually(lambda: transport.snapshot(0, "fake")["reconnects"] == 1))
            peer_two.sendall(EVENT)
            self.assertTrue(eventually(lambda: state.ring.cursors()[1] == 1))
            record = state.ring.page(0, 1)["events"][0]
            self.assertEqual(record["device"], 0)
            snapshot = transport.snapshot(0, "fake")
            self.assertEqual((snapshot["parse_errors"], snapshot["overlong_lines"]), (0, 0))
        finally:
            transport.stop()
            peer_two.close()

    def test_overlong_discard_mode_is_cleared_on_reconnect(self) -> None:
        state, transport, peer_one, peer_two = self._transport_with_replacement_socket()
        try:
            peer_one.sendall(b"x" * (bridge.MAX_LINE_BYTES + 1))
            self.assertTrue(eventually(lambda: transport.snapshot(0, "fake")["overlong_lines"] == 1))
            peer_one.close()
            self.assertTrue(eventually(lambda: transport.snapshot(0, "fake")["reconnects"] == 1))
            peer_two.sendall(EVENT)
            self.assertTrue(eventually(lambda: state.ring.cursors()[1] == 1))
            snapshot = transport.snapshot(0, "fake")
            self.assertEqual(snapshot["parse_errors"], 0)
            self.assertEqual(snapshot["overlong_lines"], 1)
        finally:
            transport.stop()
            peer_two.close()

    def test_reconnect_clears_authority_and_writes_status_before_fresh_commands(self) -> None:
        state, transport, peer_one, peer_two = self._transport_with_replacement_socket()
        try:
            self.assertEqual(read_exact(peer_one.fileno(), len(bridge.ROOT_STATUS)), bridge.ROOT_STATUS)
            peer_one.sendall(ROOT + gtt_begin() + gtt_entry() + gtt_end() + gtt_begin(query=101))
            self.assertTrue(
                eventually(
                    lambda: transport.snapshot(0, "fake")["root"] is not None
                    and transport.snapshot(0, "fake")["gtt"] is not None
                )
            )
            self.assertEqual(transport.snapshot(0, "fake")["root"]["cursor"], 1)
            peer_one.close()
            self.assertTrue(eventually(lambda: transport.snapshot(0, "fake")["reconnects"] == 1))
            restored = transport.snapshot(0, "fake")
            self.assertTrue(restored["connected"])
            self.assertIsNone(restored["root"])
            self.assertIsNone(restored["gtt"])
            peer_two.sendall(gtt_entry(query=101) + gtt_end(query=101) + EVENT)
            self.assertTrue(eventually(lambda: state.ring.cursors()[1] == 2))
            self.assertIsNone(transport.snapshot(0, "fake")["gtt"])
            self.assertEqual(transport.submit(bridge.GTT), "accepted")
            self.assertEqual(
                read_exact(peer_two.fileno(), len(bridge.ROOT_STATUS) + len(bridge.GTT)),
                bridge.ROOT_STATUS + bridge.GTT,
            )
        finally:
            transport.stop()
            peer_two.close()

    def test_submit_stale_across_reconnect_is_rejected_before_new_status_generation(self) -> None:
        _state, transport, peer_one, peer_two = self._transport_with_replacement_socket()
        admission_lock = threading.Lock()
        observed = threading.Event()
        release = threading.Event()
        observed_count = 0
        results: list[tuple[bytes, str]] = []

        def pause_admission() -> None:
            nonlocal observed_count
            with admission_lock:
                observed_count += 1
                if observed_count == 2:
                    observed.set()
            self.assertTrue(release.wait(timeout=1))

        try:
            self.assertEqual(read_exact(peer_one.fileno(), len(bridge.ROOT_STATUS)), bridge.ROOT_STATUS)
            transport._before_command_admission = pause_admission
            callers = [
                threading.Thread(target=lambda: results.append((bridge.ROOT_ON, transport.submit(bridge.ROOT_ON)))),
                threading.Thread(target=lambda: results.append((bridge.GTT, transport.submit(bridge.GTT)))),
            ]
            for caller in callers:
                caller.start()
            self.assertTrue(observed.wait(timeout=1))

            peer_one.close()
            self.assertTrue(eventually(lambda: transport.snapshot(0, "fake")["reconnects"] == 1))
            self.assertEqual(read_exact(peer_two.fileno(), len(bridge.ROOT_STATUS)), bridge.ROOT_STATUS)

            release.set()
            for caller in callers:
                caller.join(timeout=1)
                self.assertFalse(caller.is_alive())
            self.assertEqual(sorted(results), [(bridge.GTT, "disconnected"), (bridge.ROOT_ON, "disconnected")])
            self.assertEqual(read_exact(peer_two.fileno(), 1, timeout=0.1), b"")

            transport._before_command_admission = None
            self.assertEqual(transport.submit(bridge.GTT), "accepted")
            self.assertEqual(read_exact(peer_two.fileno(), len(bridge.GTT)), bridge.GTT)
        finally:
            release.set()
            transport._before_command_admission = None
            transport.stop()
            peer_two.close()


class HTTPTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.assets = Path(self.temp.name) / "dist"
        self.assets.mkdir()
        (self.assets / "index.html").write_text("<h1>MIND</h1>", encoding="utf-8")
        (self.assets / "app.js").write_text("export {};", encoding="utf-8")
        (self.assets / "style.css").write_text("body{}", encoding="utf-8")
        self.servers: list[ServerHarness] = []

    def tearDown(self) -> None:
        for server in reversed(self.servers):
            server.close()
        self.temp.cleanup()

    def test_events_defaults_query_rejection_and_no_store_json(self) -> None:
        state = bridge.Bridge(["test"])
        state.devices[0].feed_bytes(EVENT + ROOT + COMMAND)
        server = ServerHarness(state, self.assets)
        self.servers.append(server)
        status, headers, body = server.request("GET", "/api/events")
        response = json.loads(body)
        self.assertEqual(status, 200)
        self.assertEqual(headers["Cache-Control"], "no-store")
        self.assertEqual((response["schema"], response["events"][0]["observer_rssi_dbm"]), ("mind.api.v2", None))
        self.assertRegex(response["session_id"], r"^[0-9a-f]{32}$")
        self.assertIsInstance(response["events"][0]["received_at_ms"], int)
        self.assertGreaterEqual(response["events"][0]["received_at_ms"], 0)
        self.assertLessEqual(response["events"][0]["received_at_ms"], bridge.MAX_UNIX_EPOCH_MS)
        self.assertNotIn("received_at_ms", response["events"][1])
        self.assertNotIn("received_at_ms", response["events"][2])
        self.assertEqual([record["kind"] for record in response["events"]], ["event", "root", "command"])
        self.assertEqual(server.request("GET", "/api/events?after=1&limit=1")[0], 200)
        for query in ("after=-1", "after=1&after=2", "extra=1", "limit=0", "limit=101", "limit=one", "after=1.2"):
            with self.subTest(query=query):
                status, _, body = server.request("GET", "/api/events?" + query)
                self.assertEqual(status, 400)
                self.assertEqual(json.loads(body)["error"], "invalid_query")

    def test_every_response_type_has_exact_anti_framing_headers(self) -> None:
        state = bridge.Bridge([])
        sha256 = "0" * 64
        state.layout.open_image = lambda _sha256: bridge.layout_store.OpenImage(  # type: ignore[method-assign]
            b"floorplan", "image/png", sha256
        )
        server = ServerHarness(state, self.assets)
        self.servers.append(server)

        for name, path, expected_status in (
            ("api", "/api/health", 200),
            ("error", "/api/events?limit=0", 400),
            ("static", "/", 200),
            ("floorplan", f"/api/floorplan/{sha256}", 200),
        ):
            with self.subTest(name=name):
                status, headers, _ = server.request("GET", path)
                self.assertEqual(status, expected_status)
                self.assertEqual(
                    (
                        headers["Content-Security-Policy"],
                        headers["X-Frame-Options"],
                        headers["Connection"],
                    ),
                    ("frame-ancestors 'none'", "DENY", "close"),
                )

        status, headers, _ = server.request("GET", f"/api/floorplan/{sha256}")
        self.assertEqual(
            (status, headers["Cache-Control"], headers["X-Content-Type-Options"]),
            (200, "private, max-age=31536000, immutable", "nosniff"),
        )

    def test_event_receipt_timestamp_is_fixed_at_host_append_not_delayed_http_delivery(self) -> None:
        state = bridge.Bridge(["test"])
        with mock.patch.object(bridge.time, "time", return_value=1_700_000_000.250):
            state.devices[0].feed_bytes(EVENT)
        server = ServerHarness(state, self.assets)
        self.servers.append(server)

        with mock.patch.object(bridge.time, "time", return_value=1_700_000_005.750):
            status, _, body = server.request("GET", "/api/events?after=0&limit=100")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["events"][0]["received_at_ms"], 1_700_000_000_250)

    def test_empty_health_keeps_explicit_initial_cursor_pair(self) -> None:
        server = ServerHarness(bridge.Bridge([]), self.assets)
        self.servers.append(server)
        status, _, body = server.request("GET", "/api/health")
        self.assertEqual(status, 200)
        response = json.loads(body)
        self.assertEqual(
            {key: value for key, value in response.items() if key != "session_id"},
            {"schema": "mind.health.v2", "oldest_cursor": 1, "current_cursor": 0, "devices": []},
        )
        self.assertEqual(response["session_id"], server.state.session_id)

    def test_root_post_error_matrix(self) -> None:
        state = bridge.Bridge(["/does/not/exist"])
        server = ServerHarness(state, self.assets)
        self.servers.append(server)

        status, headers, body = server.request("POST", "/api/root", b"{", {"Content-Type": "application/json"})
        self.assertEqual((status, json.loads(body)["error"], headers["Cache-Control"]), (400, "invalid_json", "no-store"))
        for invalid in (b"{}", b'{"device":true,"active":true}', b'{"device":0,"active":true,"extra":1}', b'{"device":0,"device":0,"active":true}'):
            with self.subTest(invalid=invalid):
                status, _, body = server.request("POST", "/api/root", invalid, {"Content-Type": "application/json"})
                self.assertEqual((status, json.loads(body)["error"]), (400, "invalid_body"))
        status, _, body = server.request("POST", "/api/root", b'{"device":1,"active":true}', {"Content-Type": "application/json"})
        self.assertEqual((status, json.loads(body)["error"]), (404, "unknown_device"))
        status, _, body = server.request("POST", "/api/root", b'{"device":0,"active":true}', {"Content-Type": "application/json"})
        self.assertEqual((status, json.loads(body)["error"]), (503, "disconnected"))

    def test_gtt_post_error_matrix(self) -> None:
        state = bridge.Bridge(["/does/not/exist"])
        server = ServerHarness(state, self.assets)
        self.servers.append(server)

        status, headers, body = server.request("POST", "/api/gtt", b"{", {"Content-Type": "application/json"})
        self.assertEqual((status, json.loads(body)["error"], headers["Cache-Control"]), (400, "invalid_json", "no-store"))
        for invalid in (
            b"{}",
            b'{"device":true}',
            b'{"device":0,"active":true}',
            b'{"device":0,"device":0}',
        ):
            with self.subTest(invalid=invalid):
                status, _, body = server.request("POST", "/api/gtt", invalid, {"Content-Type": "application/json"})
                self.assertEqual((status, json.loads(body)["error"]), (400, "invalid_body"))
        status, _, body = server.request("POST", "/api/gtt", b'{"device":1}', {"Content-Type": "application/json"})
        self.assertEqual((status, json.loads(body)["error"]), (404, "unknown_device"))
        status, _, body = server.request("POST", "/api/gtt", b'{"device":0}', {"Content-Type": "application/json"})
        self.assertEqual((status, json.loads(body)["error"]), (503, "disconnected"))
        self.assertEqual(
            server.request("POST", "/api/gtt?unexpected=1", b'{"device":0}', {"Content-Type": "application/json"})[0],
            404,
        )

    def test_host_validation_rejects_rebinding_malformed_missing_and_wrong_port(self) -> None:
        server = ServerHarness(bridge.Bridge([]), self.assets)
        self.servers.append(server)
        wrong_port = 1 if server.port != 1 else 2
        requests = (
            ("missing", "GET", "/", [], b""),
            ("missing_port", "GET", "/api/health", [("Host", "localhost")], b""),
            ("malformed", "GET", "/api/health", [("Host", "localhost:not-a-port")], b""),
            ("rebound", "GET", "/api/events", [("Host", f"attacker.example:{server.port}")], b""),
            ("wrong_port", "GET", "/api/health", [("Host", f"localhost:{wrong_port}")], b""),
            (
                "duplicate",
                "POST",
                "/api/root",
                [
                    ("Host", f"localhost:{server.port}"),
                    ("Host", f"127.0.0.1:{server.port}"),
                    ("Content-Type", "application/json"),
                    ("Content-Length", "26"),
                ],
                b'{"device":0,"active":true}',
            ),
        )
        for name, method, target, headers, body in requests:
            with self.subTest(name=name):
                status, _, response = server.raw_request(method, target, headers, body)
                self.assertEqual((status, json.loads(response)["error"]), (400, "invalid_host"))

        status, _, body = server.raw_request("GET", "/", [("Host", f"localhost:{server.port}")])
        self.assertEqual((status, body), (200, b"<h1>MIND</h1>"))

    def test_mutations_require_one_utf8_json_content_type(self) -> None:
        server = ServerHarness(bridge.Bridge(["/does/not/exist"]), self.assets)
        self.servers.append(server)
        payload = b'{"device":0,"active":true}'

        def mutate(content_type_headers: list[tuple[str, str]]) -> tuple[int, dict[str, str], bytes]:
            return server.raw_request(
                "POST",
                "/api/root",
                [
                    ("Host", f"127.0.0.1:{server.port}"),
                    ("Content-Length", str(len(payload))),
                    *content_type_headers,
                ],
                payload,
            )

        for name, headers in (
            ("missing", []),
            ("plain_text", [("Content-Type", "text/plain")]),
            ("form", [("Content-Type", "application/x-www-form-urlencoded")]),
            ("non_utf8_charset", [("Content-Type", "application/json; charset=latin-1")]),
            (
                "duplicate",
                [("Content-Type", "application/json"), ("Content-Type", "application/json")],
            ),
        ):
            with self.subTest(name=name):
                status, _, response = mutate(headers)
                self.assertEqual((status, json.loads(response)["error"]), (415, "unsupported_media_type"))

        status, _, response = mutate([("Content-Type", "Application/JSON; Charset=UTF-8")])
        self.assertEqual((status, json.loads(response)["error"]), (503, "disconnected"))

    def test_mutations_reject_cross_origin_null_origin_and_cross_site_metadata(self) -> None:
        server = ServerHarness(bridge.Bridge(["/does/not/exist"]), self.assets)
        self.servers.append(server)
        payload = b'{"device":0}'

        def mutate(browser_headers: list[tuple[str, str]]) -> tuple[int, dict[str, str], bytes]:
            return server.raw_request(
                "POST",
                "/api/gtt",
                [
                    ("Host", f"localhost:{server.port}"),
                    ("Content-Type", "application/json"),
                    ("Content-Length", str(len(payload))),
                    *browser_headers,
                ],
                payload,
            )

        for name, headers, error in (
            ("cross_origin", [("Origin", f"http://127.0.0.1:{server.port}")], "forbidden_origin"),
            ("null_origin", [("Origin", "null")], "forbidden_origin"),
            ("duplicate_origin", [("Origin", f"http://localhost:{server.port}"), ("Origin", "null")], "forbidden_origin"),
            ("cross_site", [("Sec-Fetch-Site", "cross-site")], "forbidden_fetch_site"),
        ):
            with self.subTest(name=name):
                status, response_headers, response = mutate(headers)
                self.assertEqual((status, json.loads(response)["error"]), (403, error))
                self.assertNotIn("Access-Control-Allow-Origin", response_headers)

    def test_static_assets_types_missing_and_traversal_are_safe(self) -> None:
        outside = Path(self.temp.name) / "outside.txt"
        outside.write_text("secret", encoding="utf-8")
        os.symlink(outside, self.assets / "escape.txt")
        state = bridge.Bridge([])
        server = ServerHarness(state, self.assets)
        self.servers.append(server)
        self.assertEqual(server.server.server_address[0], "127.0.0.1")
        status, headers, body = server.request("GET", "/")
        self.assertEqual((status, headers["Content-Type"], body), (200, "text/html; charset=utf-8", b"<h1>MIND</h1>"))
        self.assertEqual(server.request("GET", "/app.js")[1]["Content-Type"], "application/javascript; charset=utf-8")
        self.assertEqual(server.request("GET", "/style.css")[1]["Content-Type"], "text/css; charset=utf-8")
        self.assertEqual(server.request("GET", "/missing.txt")[0], 404)
        self.assertEqual(server.request("GET", "/%2e%2e/outside.txt")[0], 404)
        self.assertEqual(server.request("GET", "/escape.txt")[0], 404)
        (self.assets / "directory").mkdir()
        self.assertEqual(server.request("GET", "/directory/")[0], 404)

    def test_static_assets_are_pinned_component_safe_bounded_and_streamed(self) -> None:
        nested = self.assets / "assets" / "nested"
        nested.mkdir(parents=True)
        payload = b"x" * (bridge.MAX_STATIC_FILE_BYTES // 32 + 1)
        asset = nested / "bundle.js"
        asset.write_bytes(payload)
        oversize = self.assets / "too-large.bin"
        oversize.write_bytes(b"x" * (bridge.MAX_STATIC_FILE_BYTES + 1))
        outside = Path(self.temp.name) / "outside.js"
        outside.write_bytes(b"outside")
        os.symlink(outside, nested / "escape.js")
        state = bridge.Bridge([])
        server = ServerHarness(state, self.assets)
        self.servers.append(server)
        original_read = os.read
        with mock.patch("host.bridge.os.read", wraps=original_read) as reads:
            status, headers, body = server.request("GET", "/assets/nested/bundle.js")
        sizes = [call.args[1] for call in reads.call_args_list]
        self.assertEqual((status, headers["Content-Type"], headers["Content-Length"], body), (200, "application/javascript; charset=utf-8", str(len(payload)), payload))
        self.assertGreaterEqual(len(sizes), 2)
        self.assertTrue(all(size <= bridge.layout_store.IMAGE_CHUNK_BYTES for size in sizes))
        self.assertEqual(server.request("GET", "/assets/nested/escape.js")[0], 404)
        self.assertEqual(server.request("GET", "/too-large.bin")[0], 404)

        class Writes:
            def __init__(self, fail_after: int | None = None) -> None:
                self.chunks: list[bytes] = []
                self.fail_after = fail_after

            def write(self, chunk: bytes) -> int:
                self.chunks.append(chunk)
                if self.fail_after is not None and len(self.chunks) > self.fail_after:
                    raise OSError("disconnected")
                return len(chunk)

        def stream_handler(fd: int, writes: Writes) -> bridge.BridgeRequestHandler:
            handler = object.__new__(bridge.BridgeRequestHandler)
            handler.server = SimpleNamespace(open_static=lambda _relative: (fd, len(payload)))
            handler.wfile = writes
            handler.close_connection = False
            handler.send_response = lambda _status: None
            handler.send_header = lambda _name, _value: None
            handler.end_headers = lambda: None
            return handler

        tracked_fd = os.open(asset, os.O_RDONLY)
        complete = Writes()
        stream_handler(tracked_fd, complete)._serve_static("/assets/nested/bundle.js")
        self.assertEqual(b"".join(complete.chunks), payload)
        self.assertTrue(all(0 < len(chunk) <= bridge.layout_store.IMAGE_CHUNK_BYTES for chunk in complete.chunks))
        with self.assertRaises(OSError):
            os.fstat(tracked_fd)

        failed_fd = os.open(asset, os.O_RDONLY)
        failed_handler = stream_handler(failed_fd, Writes(fail_after=1))
        failed_handler._serve_static("/assets/nested/bundle.js")
        self.assertTrue(failed_handler.close_connection)
        with self.assertRaises(OSError):
            os.fstat(failed_fd)

        # A renamed root must not make the pinned descriptor serve a replacement.
        moved = Path(self.temp.name) / "moved-dist"
        os.rename(self.assets, moved)
        self.assets.mkdir()
        (self.assets / "index.html").write_text("replacement", encoding="utf-8")
        self.assertEqual(server.request("GET", "/")[0], 404)

    def test_static_fifo_is_never_blockingly_opened_and_shutdown_completes(self) -> None:
        fifo = self.assets / "untrusted.pipe"
        os.mkfifo(fifo)
        server = ServerHarness(bridge.Bridge([]), self.assets)
        self.servers.append(server)
        started = time.monotonic()
        self.assertEqual(server.request("GET", "/untrusted.pipe")[0], 404)
        self.assertLess(time.monotonic() - started, 0.8)
        started = time.monotonic()
        server.close()
        self.servers.remove(server)
        self.assertLess(time.monotonic() - started, 0.8)

    def test_legacy_root_and_gtt_oversized_content_lengths_remain_invalid_body_400(self) -> None:
        server = ServerHarness(bridge.Bridge(["/does/not/exist"]), self.assets)
        self.servers.append(server)
        for endpoint, payload in (("/api/root", b'{"device":0,"active":true}'), ("/api/gtt", b'{"device":0}')):
            for declared in (str(bridge.MAX_REQUEST_BYTES + 1), "9" * 5000):
                with self.subTest(endpoint=endpoint, declared_length=len(declared)):
                    status, _, response = server.raw_request(
                        "POST",
                        endpoint,
                        [
                            ("Host", f"127.0.0.1:{server.port}"),
                            ("Content-Type", "application/json"),
                            ("Content-Length", declared),
                        ],
                        payload,
                    )
                    self.assertEqual((status, json.loads(response)["error"]), (400, "invalid_body"))

    def test_partial_http_body_times_out_with_invalid_body_and_releases_worker(self) -> None:
        timing = bridge.Timing(http_io_timeout=0.15, serial_poll_interval=0.01)
        server = ServerHarness(bridge.Bridge([], timing=timing), self.assets)
        self.servers.append(server)
        client = socket.create_connection(("127.0.0.1", server.port), timeout=1)
        try:
            started = time.monotonic()
            client.sendall(
                f"POST /api/root HTTP/1.1\r\nHost: localhost:{server.port}\r\n".encode()
                + b"Content-Length: 30\r\nContent-Type: application/json\r\n\r\n"
                b'{"device":0'
            )
            response = receive_until_eof(client)
            elapsed = time.monotonic() - started
            self.assertLess(elapsed, 0.8)
            self.assertIn(b"HTTP/1.1 400", response)
            self.assertIn(b"Connection: close", response)
            self.assertIn(b'"error":"invalid_body"', response)
        finally:
            client.close()
        started = time.monotonic()
        self.assertEqual(server.request("GET", "/api/health")[0], 200)
        self.assertLess(time.monotonic() - started, 0.8)

    def test_stalled_http_clients_are_rejected_then_release_capacity_and_shutdown(self) -> None:
        timing = bridge.Timing(http_io_timeout=0.15, serial_poll_interval=0.01)
        server = ServerHarness(bridge.Bridge([], timing=timing), self.assets)
        self.servers.append(server)
        clients: list[socket.socket] = []
        try:
            for _ in range(bridge.HTTP_WORKER_CAPACITY):
                client = socket.create_connection(("127.0.0.1", server.port), timeout=1)
                client.sendall(
                    f"POST /api/root HTTP/1.1\r\nHost: localhost:{server.port}\r\n".encode()
                    + b"Content-Length: 30\r\nContent-Type: application/json\r\n\r\n"
                )
                clients.append(client)
            self.assertTrue(eventually(lambda: server.server.active_request_count() == bridge.HTTP_WORKER_CAPACITY))
            started = time.monotonic()
            status, headers, _ = server.request("GET", "/api/health")
            self.assertEqual(status, 503)
            self.assertEqual(
                (headers["Content-Security-Policy"], headers["X-Frame-Options"], headers["Connection"]),
                ("frame-ancestors 'none'", "DENY", "close"),
            )
            self.assertLess(time.monotonic() - started, 0.8)
            self.assertTrue(eventually(lambda: server.server.active_request_count() == 0, timeout=1.0))
            self.assertEqual(server.request("GET", "/api/health")[0], 200)
            started = time.monotonic()
            server.close()
            self.assertLess(time.monotonic() - started, 1.2)
        finally:
            for client in clients:
                client.close()
            if server.thread.is_alive():
                server.close()

    def test_server_close_waits_for_live_mutation_worker_before_closing_store(self) -> None:
        state = bridge.Bridge(["test"], state_dir=(Path(self.temp.name) / "worker-state").resolve())
        server = ServerHarness(state, self.assets)
        entered = threading.Event()
        release = threading.Event()
        worker_exited = threading.Event()
        close_after_worker: list[bool] = []
        original_replace = state.layout.replace_positions
        original_close = state.layout.close

        def blocked_replace(*args, **kwargs):  # type: ignore[no-untyped-def]
            entered.set()
            release.wait(timeout=1)
            try:
                return original_replace(*args, **kwargs)
            finally:
                worker_exited.set()

        def observed_close() -> None:
            close_after_worker.append(worker_exited.is_set())
            original_close()

        state.layout.replace_positions = blocked_replace  # type: ignore[method-assign]
        state.layout.close = observed_close  # type: ignore[method-assign]
        payload = json.dumps(
            {
                "schema": "mind.dashboard.layout.update.v1",
                "base_revision": 0,
                "positions": [{"adva": "0102545678c0", "x": 0.5, "y": 0.5}],
            },
            separators=(",", ":"),
        ).encode()
        client = socket.create_connection(("127.0.0.1", server.port), timeout=2)
        closer: threading.Thread | None = None
        try:
            client.sendall(
                (
                    f"PUT /api/layout HTTP/1.1\r\nHost: 127.0.0.1:{server.port}\r\n"
                    f"Content-Type: application/json\r\nContent-Length: {len(payload)}\r\n\r\n"
                ).encode()
                + payload
            )
            self.assertTrue(entered.wait(timeout=1))
            closer = threading.Thread(target=server.close)
            closer.start()
            self.assertTrue(eventually(lambda: closer is not None and closer.is_alive()))
            self.assertEqual(server.server.active_request_count(), 1)
            release.set()
            closer.join(timeout=2)
            self.assertFalse(closer.is_alive())
            self.assertTrue(worker_exited.is_set())
            self.assertEqual(close_after_worker, [True])
            with self.assertRaises(bridge.layout_store.StorageUnavailable):
                state.layout.get()
        finally:
            release.set()
            client.close()
            if closer is not None and closer.is_alive():
                closer.join(timeout=2)


class SerialWorkerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.master, slave = pty.openpty()
        self.path = os.ttyname(slave)
        os.close(slave)
        self.state = bridge.Bridge([self.path])
        self.servers: list[ServerHarness] = []
        self.state.start()
        self.assertTrue(eventually(self.state.devices[0].is_connected), "PTY worker did not connect")
        self.assertEqual(read_exact(self.master, len(bridge.ROOT_STATUS)), bridge.ROOT_STATUS)

    def tearDown(self) -> None:
        for server in reversed(self.servers):
            server.close()
        self.state.stop()
        os.close(self.master)

    def test_post_accepts_only_after_full_partial_write_and_serializes_concurrent_bytes(self) -> None:
        device = self.state.devices[0]

        def partial_write(fd: int, data: bytes) -> int:
            return os.write(fd, data[:2])

        device.write_fn = partial_write
        server = ServerHarness(self.state, Path(tempfile.gettempdir()))
        self.servers.append(server)
        responses: list[tuple[int, dict[str, str], bytes]] = []
        response_lock = threading.Lock()

        def post(target: str, payload: dict[str, object]) -> None:
            result = server.request("POST", target, json.dumps(payload).encode(), {"Content-Type": "application/json"})
            with response_lock:
                responses.append(result)

        requests = [
            ("/api/root", {"device": 0, "active": True}),
            ("/api/gtt", {"device": 0}),
            ("/api/root", {"device": 0, "active": False}),
            ("/api/gtt", {"device": 0}),
            ("/api/root", {"device": 0, "active": True}),
            ("/api/gtt", {"device": 0}),
        ]
        callers = [threading.Thread(target=post, args=request) for request in requests]
        for caller in callers:
            caller.start()
        for caller in callers:
            caller.join(timeout=3)
        self.assertEqual([status for status, _, _ in responses], [202] * 6)
        output = read_exact(self.master, 2 * len(bridge.ROOT_ON) + len(bridge.ROOT_OFF) + 3 * len(bridge.GTT))
        self.assertEqual(len(output), 37)
        remaining = output
        commands = []
        while remaining:
            if remaining.startswith(b"ROOT ON\r"):
                commands.append(b"on")
                remaining = remaining[len(b"ROOT ON\r") :]
            elif remaining.startswith(b"ROOT OFF\r"):
                commands.append(b"off")
                remaining = remaining[len(b"ROOT OFF\r") :]
            elif remaining.startswith(bridge.GTT):
                commands.append(b"gtt")
                remaining = remaining[len(bridge.GTT) :]
            else:
                self.fail(f"interleaved serial bytes: {output!r}")
        self.assertEqual(sorted(commands), [b"gtt", b"gtt", b"gtt", b"off", b"on", b"on"])
        for _, _, body in responses:
            self.assertTrue(json.loads(body)["accepted"])
        self.assertEqual(sorted(json.loads(body)["command"] for _, _, body in responses), ["gtt", "gtt", "gtt", "off", "on", "on"])
        self.assertEqual(
            [json.loads(body) for _, _, body in responses if json.loads(body)["command"] == "gtt"],
            [{"schema": "mind.command.v1", "accepted": True, "device": 0, "command": "gtt"}] * 3,
        )

    def test_http_csrf_gate_allows_same_origin_and_originless_local_clients(self) -> None:
        server = ServerHarness(self.state, Path(tempfile.gettempdir()))
        self.servers.append(server)

        def rooted_request(host: str, active: bool, fetch_site: str = "same-origin") -> tuple[int, dict[str, str], bytes]:
            payload = json.dumps({"device": 0, "active": active}, separators=(",", ":")).encode()
            return server.raw_request(
                "POST",
                "/api/root",
                [
                    ("Host", f"{host}:{server.port}"),
                    ("Content-Type", "application/json"),
                    ("Content-Length", str(len(payload))),
                    ("Origin", f"http://{host}:{server.port}"),
                    ("Sec-Fetch-Site", fetch_site),
                ],
                payload,
            )

        localhost_status, _, localhost_body = rooted_request("localhost", True)
        loopback_status, _, loopback_body = rooted_request("127.0.0.1", False)
        none_status, _, none_body = rooted_request("localhost", False, "none")
        automation_status, _, automation_body = server.request(
            "POST",
            "/api/root",
            b'{"device":0,"active":true}',
            {"Content-Type": "application/json"},
        )

        self.assertEqual((localhost_status, json.loads(localhost_body)["accepted"]), (202, True))
        self.assertEqual((loopback_status, json.loads(loopback_body)["accepted"]), (202, True))
        self.assertEqual((none_status, json.loads(none_body)["accepted"]), (202, True))
        self.assertEqual((automation_status, json.loads(automation_body)["accepted"]), (202, True))
        self.assertEqual(
            read_exact(self.master, len(bridge.ROOT_ON) * 2 + len(bridge.ROOT_OFF) * 2),
            bridge.ROOT_ON + bridge.ROOT_OFF + bridge.ROOT_OFF + bridge.ROOT_ON,
        )

    def test_post_reports_write_failure(self) -> None:
        self.state.devices[0].write_fn = lambda _fd, _data: (_ for _ in ()).throw(OSError("broken"))
        server = ServerHarness(self.state, Path(tempfile.gettempdir()))
        self.servers.append(server)
        status, _, body = server.request("POST", "/api/root", b'{"device":0,"active":false}', {"Content-Type": "application/json"})
        self.assertEqual((status, json.loads(body)["error"]), (503, "write_failed"))

    def test_gtt_post_reports_write_failure(self) -> None:
        self.state.devices[0].write_fn = lambda _fd, _data: (_ for _ in ()).throw(OSError("broken"))
        server = ServerHarness(self.state, Path(tempfile.gettempdir()))
        self.servers.append(server)
        status, _, body = server.request("POST", "/api/gtt", b'{"device":0}', {"Content-Type": "application/json"})
        self.assertEqual((status, json.loads(body)["error"]), (503, "write_failed"))

    def test_partially_written_stalled_command_has_a_bounded_failure_and_closes_transport(self) -> None:
        self.state.stop()
        os.close(self.master)
        master, slave = pty.openpty()
        path = os.ttyname(slave)
        os.close(slave)
        self.master = master
        self.state = bridge.Bridge(
            [path],
            timing=bridge.Timing(command_deadline=0.15, submit_wait_slack=0.1, serial_poll_interval=0.01),
        )
        self.state.start()
        self.assertTrue(eventually(self.state.devices[0].is_connected))
        self.assertEqual(read_exact(self.master, len(bridge.ROOT_STATUS)), bridge.ROOT_STATUS)
        calls = 0

        def partial_then_block(fd: int, data: bytes) -> int:
            nonlocal calls
            calls += 1
            if calls == 1:
                return os.write(fd, data[:2])
            raise BlockingIOError()

        self.state.devices[0].write_fn = partial_then_block
        server = ServerHarness(self.state, Path(tempfile.gettempdir()))
        self.servers.append(server)
        responses: list[tuple[int, dict[str, str], bytes]] = []

        def post(active: bool) -> None:
            responses.append(
                server.request(
                    "POST",
                    "/api/root",
                    json.dumps({"device": 0, "active": active}).encode(),
                    {"Content-Type": "application/json"},
                )
            )

        started = time.monotonic()
        writers = [threading.Thread(target=post, args=(True,)), threading.Thread(target=post, args=(False,))]
        for writer in writers:
            writer.start()
        for writer in writers:
            writer.join(timeout=1)
        elapsed = time.monotonic() - started
        self.assertLess(elapsed, 0.8)
        self.assertEqual(len(responses), 2)
        self.assertEqual({json.loads(body)["error"] for status, _, body in responses if status == 503}, {"write_failed", "disconnected"})
        self.assertEqual(read_exact(self.master, 2, timeout=0.5), b"RO")
        self.assertTrue(eventually(lambda: not self.state.devices[0].is_connected(), timeout=0.5))
        self.assertEqual(read_exact(self.master, 1, timeout=0.1), b"")
        started = time.monotonic()
        self.state.stop()
        self.assertLess(time.monotonic() - started, 0.8)

    def test_serial_is_raw_115200_8n1(self) -> None:
        probe_master, probe_slave = pty.openpty()
        try:
            bridge.configure_serial_115200(probe_slave)
            attributes = __import__("termios").tcgetattr(probe_slave)
            termios = __import__("termios")
            self.assertEqual(attributes[4:6], [termios.B115200, termios.B115200])
            self.assertEqual(attributes[2] & termios.CSIZE, termios.CS8)
            self.assertFalse(attributes[2] & termios.PARENB)
            self.assertFalse(attributes[2] & termios.CSTOPB)
            self.assertFalse(attributes[3] & termios.ICANON)
        finally:
            os.close(probe_master)
            os.close(probe_slave)

    def test_reconnect_counter_increments_after_a_real_disconnect(self) -> None:
        left_one, peer_one = socket.socketpair()
        left_two, peer_two = socket.socketpair()
        descriptors = [left_one.detach(), left_two.detach()]

        def fake_open(_path: str, _flags: int) -> int:
            if descriptors:
                return descriptors.pop(0)
            raise OSError("unavailable")

        state = bridge.Bridge([])
        transport = bridge.SerialTransport(
            state,
            0,
            "fake",
            ("fake",),
            bridge.Timing(command_deadline=0.2, serial_poll_interval=0.01),
            open_fn=fake_open,
            configure_fn=lambda _fd: None,
        )
        transport.start()
        try:
            self.assertTrue(eventually(transport.is_connected))
            peer_one.close()
            self.assertTrue(eventually(lambda: transport.snapshot(0, "fake")["reconnects"] == 1))
            self.assertTrue(transport.is_connected())
        finally:
            transport.stop()
            peer_two.close()


class CLITests(unittest.TestCase):
    def test_grouping_identity_falls_back_when_allow_missing_is_unavailable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            missing = Path(temporary) / "missing-device"
            expected = os.path.abspath(str(missing))
            path_without_allow_missing = SimpleNamespace(
                realpath=os.path.realpath,
                abspath=os.path.abspath,
                normpath=os.path.normpath,
            )
            original_os = bridge.os
            try:
                bridge.os = SimpleNamespace(path=path_without_allow_missing)
                self.assertEqual(bridge.serial_grouping_identity(str(missing)), expected)
                self.assertEqual(bridge.serial_grouping_identity(str(missing.parent / "." / missing.name)), expected)
            finally:
                bridge.os = original_os

    def test_exactly_one_serial_cli_and_port_validation(self) -> None:
        args = bridge.parse_args(["--serial", "one", "--port", "0", "--assets", "/tmp/assets", "--state-dir", "/tmp/state"])
        self.assertEqual(args.serial, ["one"])
        self.assertEqual(args.port, 0)
        self.assertEqual(args.assets, Path("/tmp/assets"))
        self.assertEqual(args.state_dir, Path("/tmp/state"))
        self.assertEqual(
            bridge.serial_grouping_identity("missing-device"),
            bridge.serial_grouping_identity("./missing-device"),
        )
        with self.assertRaises(SystemExit):
            bridge.parse_args(["--port", "65536"])
        for argv in ([], ["--serial", "one", "--serial", "two"], ["--serial", "one", "--state-dir", "relative"]):
            with self.subTest(argv=argv), self.assertRaises(SystemExit):
                bridge.parse_args(argv)


if __name__ == "__main__":
    unittest.main()
