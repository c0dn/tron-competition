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

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from host import bridge  # noqa: E402


EVENT = (
    b"mind_event_v1 now=55 root=8081545678c0 wearable=7 packet=a1b2c3 "
    b"schema=1 event=5 confidence=73 svm=6687 mic=125 seq=195 "
    b"observer=0102545678c0 path=tavrn\n"
)
ROOT = (
    b"mind_root_v1 now=99 local=8081545678c0 node=6 role=root roots=16 "
    b"announced=2 acked=1 rejected=3 pending=4 rootless_drop=7\n"
)
COMMAND = b"mind_command_v1 now=12 local=8081545678c0 command=on status=accepted\n"


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
                "path": "tavrn",
            },
        )
        self.assertEqual(bridge.parse_firmware_line(ROOT.rstrip())["kind"], "root")
        self.assertEqual(bridge.parse_firmware_line(COMMAND.rstrip())["command"], "on")
        self.assertEqual(bridge.parse_firmware_line(EVENT.rstrip() + b"\r")["packet"], "a1b2c3")

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
            b"mind_event_v1 completely unrelated",
        ]
        for line in invalid:
            with self.subTest(line=line), self.assertRaises(bridge.ParseError):
                bridge.parse_firmware_line(line)
        heartbeat = EVENT.replace(b"event=5 confidence=73", b"event=0 confidence=1")
        with self.assertRaises(bridge.ParseError):
            bridge.parse_firmware_line(heartbeat.rstrip())
        self.assertIsNone(bridge.parse_firmware_line(b"UART ready"))

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

    def test_latest_root_health_shape_has_no_cursor_or_device(self) -> None:
        state = bridge.Bridge(["test"])
        state.devices[0].feed_bytes(ROOT)
        health = state.health()
        root = health["devices"][0]["root"]
        self.assertEqual(root["kind"], "root")
        self.assertNotIn("cursor", root)
        self.assertNotIn("device", root)
        self.assertEqual(root["local"], "8081545678c0")
        self.assertEqual(health["devices"][0]["last_record_cursor"], 1)

    def test_stable_repeated_serial_indices_at_capacity_and_duplicates(self) -> None:
        self.assertEqual([device.index for device in bridge.Bridge(["one"]).devices], [0])
        self.assertEqual([device.index for device in bridge.Bridge(["one", "two"]).devices], [0, 1])
        paths = ["same", "same"] + [f"serial-{index}" for index in range(14)]
        state = bridge.Bridge(paths)
        self.assertEqual(len(state.devices), 16)
        self.assertEqual(len(state.transports), 15)
        self.assertEqual([(device.index, device.path) for device in state.devices[:3]], [(0, "same"), (1, "same"), (2, "serial-0")])
        self.assertEqual(state.health()["devices"][-1]["device"], 15)


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
                os.write(master, b"mind_root_v1 bad\n")
                self.assertTrue(eventually(lambda: state.health()["devices"][0]["parse_errors"] == 1))
                self.assertEqual([entry["parse_errors"] for entry in state.health()["devices"]], [1, 1, 1])

                def partial_write(fd: int, data: bytes) -> int:
                    return os.write(fd, data[:2])

                state.devices[1].write_fn = partial_write
                results: list[str] = []
                callers = [
                    threading.Thread(target=lambda: results.append(state.devices[0].submit(True))),
                    threading.Thread(target=lambda: results.append(state.devices[2].submit(False))),
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
                reconnect_attempt = len(attempts)
                os.close(first_master)
                self.assertTrue(eventually(lambda: not state.devices[0].is_connected()))

                replacement = Path(temporary) / "stable-by-id.retarget"
                os.symlink(second_target, replacement)
                os.replace(replacement, alias)

                self.assertTrue(eventually(lambda: state.devices[0].is_connected()))
                self.assertTrue(eventually(lambda: state.health()["devices"][0]["reconnects"] >= 1))
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
                self.assertEqual(state.devices[1].submit(True), "accepted")
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


class HTTPTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.assets = Path(self.temp.name) / "dist"
        self.assets.mkdir()
        (self.assets / "index.html").write_text("<h1>MIND</h1>", encoding="utf-8")
        (self.assets / "app.js").write_text("export {};", encoding="utf-8")
        (self.assets / "style.css").write_text("body{}", encoding="utf-8")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_events_defaults_query_rejection_and_no_store_json(self) -> None:
        state = bridge.Bridge(["test"])
        state.devices[0].feed_bytes(EVENT + ROOT + COMMAND)
        server = ServerHarness(state, self.assets)
        self.addCleanup(server.close)
        status, headers, body = server.request("GET", "/api/events")
        response = json.loads(body)
        self.assertEqual(status, 200)
        self.assertEqual(headers["Cache-Control"], "no-store")
        self.assertEqual([record["kind"] for record in response["events"]], ["event", "root", "command"])
        self.assertEqual(server.request("GET", "/api/events?after=1&limit=1")[0], 200)
        for query in ("after=-1", "after=1&after=2", "extra=1", "limit=0", "limit=101", "limit=one", "after=1.2"):
            with self.subTest(query=query):
                status, _, body = server.request("GET", "/api/events?" + query)
                self.assertEqual(status, 400)
                self.assertEqual(json.loads(body)["error"], "invalid_query")

    def test_empty_health_keeps_explicit_initial_cursor_pair(self) -> None:
        server = ServerHarness(bridge.Bridge([]), self.assets)
        self.addCleanup(server.close)
        status, _, body = server.request("GET", "/api/health")
        self.assertEqual(status, 200)
        self.assertEqual(
            json.loads(body),
            {"schema": "mind.health.v1", "oldest_cursor": 1, "current_cursor": 0, "devices": []},
        )

    def test_root_post_error_matrix(self) -> None:
        state = bridge.Bridge(["/does/not/exist"])
        server = ServerHarness(state, self.assets)
        self.addCleanup(server.close)

        status, headers, body = server.request("POST", "/api/root", b"{", {"Content-Type": "application/json"})
        self.assertEqual((status, json.loads(body)["error"], headers["Cache-Control"]), (400, "invalid_json", "no-store"))
        for invalid in (b"{}", b'{"device":true,"active":true}', b'{"device":0,"active":true,"extra":1}', b'{"device":0,"device":0,"active":true}'):
            with self.subTest(invalid=invalid):
                status, _, body = server.request("POST", "/api/root", invalid)
                self.assertEqual((status, json.loads(body)["error"]), (400, "invalid_body"))
        status, _, body = server.request("POST", "/api/root", b'{"device":1,"active":true}')
        self.assertEqual((status, json.loads(body)["error"]), (404, "unknown_device"))
        status, _, body = server.request("POST", "/api/root", b'{"device":0,"active":true}')
        self.assertEqual((status, json.loads(body)["error"]), (503, "disconnected"))

    def test_static_assets_types_missing_and_traversal_are_safe(self) -> None:
        outside = Path(self.temp.name) / "outside.txt"
        outside.write_text("secret", encoding="utf-8")
        os.symlink(outside, self.assets / "escape.txt")
        state = bridge.Bridge([])
        server = ServerHarness(state, self.assets)
        self.addCleanup(server.close)
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

    def test_partial_http_body_times_out_with_invalid_body_and_releases_worker(self) -> None:
        timing = bridge.Timing(http_io_timeout=0.15, serial_poll_interval=0.01)
        server = ServerHarness(bridge.Bridge([], timing=timing), self.assets)
        self.addCleanup(server.close)
        client = socket.create_connection(("127.0.0.1", server.port), timeout=1)
        try:
            started = time.monotonic()
            client.sendall(
                b"POST /api/root HTTP/1.1\r\nHost: localhost\r\n"
                b"Content-Length: 30\r\nContent-Type: application/json\r\n\r\n"
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
        clients: list[socket.socket] = []
        try:
            for _ in range(bridge.HTTP_WORKER_CAPACITY):
                client = socket.create_connection(("127.0.0.1", server.port), timeout=1)
                client.sendall(
                    b"POST /api/root HTTP/1.1\r\nHost: localhost\r\n"
                    b"Content-Length: 30\r\nContent-Type: application/json\r\n\r\n"
                )
                clients.append(client)
            self.assertTrue(eventually(lambda: server.server.active_request_count() == bridge.HTTP_WORKER_CAPACITY))
            started = time.monotonic()
            self.assertEqual(server.request("GET", "/api/health")[0], 503)
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


class SerialWorkerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.master, slave = pty.openpty()
        self.path = os.ttyname(slave)
        os.close(slave)
        self.state = bridge.Bridge([self.path])
        self.state.start()
        self.assertTrue(eventually(self.state.devices[0].is_connected), "PTY worker did not connect")

    def tearDown(self) -> None:
        self.state.stop()
        os.close(self.master)

    def test_post_accepts_only_after_full_partial_write_and_serializes_concurrent_bytes(self) -> None:
        device = self.state.devices[0]

        def partial_write(fd: int, data: bytes) -> int:
            return os.write(fd, data[:2])

        device.write_fn = partial_write
        server = ServerHarness(self.state, Path(tempfile.gettempdir()))
        self.addCleanup(server.close)
        responses: list[tuple[int, dict[str, str], bytes]] = []
        response_lock = threading.Lock()

        def post(active: bool) -> None:
            result = server.request("POST", "/api/root", json.dumps({"device": 0, "active": active}).encode())
            with response_lock:
                responses.append(result)

        callers = [threading.Thread(target=post, args=(index % 2 == 0,)) for index in range(6)]
        for caller in callers:
            caller.start()
        for caller in callers:
            caller.join(timeout=3)
        self.assertEqual([status for status, _, _ in responses], [202] * 6)
        output = read_exact(self.master, 3 * len(b"ROOT ON\r") + 3 * len(b"ROOT OFF\r"))
        self.assertEqual(len(output), 51)
        remaining = output
        commands = []
        while remaining:
            if remaining.startswith(b"ROOT ON\r"):
                commands.append(b"on")
                remaining = remaining[len(b"ROOT ON\r") :]
            elif remaining.startswith(b"ROOT OFF\r"):
                commands.append(b"off")
                remaining = remaining[len(b"ROOT OFF\r") :]
            else:
                self.fail(f"interleaved serial bytes: {output!r}")
        self.assertEqual(sorted(commands), [b"off", b"off", b"off", b"on", b"on", b"on"])
        for _, _, body in responses:
            self.assertTrue(json.loads(body)["accepted"])

    def test_post_reports_write_failure(self) -> None:
        self.state.devices[0].write_fn = lambda _fd, _data: (_ for _ in ()).throw(OSError("broken"))
        server = ServerHarness(self.state, Path(tempfile.gettempdir()))
        self.addCleanup(server.close)
        status, _, body = server.request("POST", "/api/root", b'{"device":0,"active":false}')
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
        calls = 0

        def partial_then_block(fd: int, data: bytes) -> int:
            nonlocal calls
            calls += 1
            if calls == 1:
                return os.write(fd, data[:2])
            raise BlockingIOError()

        self.state.devices[0].write_fn = partial_then_block
        server = ServerHarness(self.state, Path(tempfile.gettempdir()))
        self.addCleanup(server.close)
        responses: list[tuple[int, dict[str, str], bytes]] = []

        def post(active: bool) -> None:
            responses.append(server.request("POST", "/api/root", json.dumps({"device": 0, "active": active}).encode()))

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

    def test_repeated_serial_cli_and_port_validation(self) -> None:
        args = bridge.parse_args(["--serial", "one", "--serial", "one", "--serial", "two", "--port", "0", "--assets", "/tmp/assets"])
        self.assertEqual(args.serial, ["one", "one", "two"])
        self.assertEqual(args.port, 0)
        self.assertEqual(args.assets, Path("/tmp/assets"))
        self.assertEqual(
            bridge.serial_grouping_identity("missing-device"),
            bridge.serial_grouping_identity("./missing-device"),
        )
        with self.assertRaises(SystemExit):
            bridge.parse_args(["--port", "65536"])


if __name__ == "__main__":
    unittest.main()
