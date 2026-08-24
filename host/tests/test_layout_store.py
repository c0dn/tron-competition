from __future__ import annotations

import base64
import hashlib
import io
import json
import os
import socket
import sqlite3
import sys
import tempfile
import threading
import time
import unittest
import zlib
from email.message import Message
from pathlib import Path
from types import SimpleNamespace

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from host import bridge, layout_store  # noqa: E402


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return (
        len(payload).to_bytes(4, "big")
        + kind
        + payload
        + (zlib.crc32(kind + payload) & 0xFFFFFFFF).to_bytes(4, "big")
    )


def pillow_image(format: str, *, progressive: bool = False, size: tuple[int, int] = (3, 2)) -> bytes:
    output = io.BytesIO()
    with Image.new("RGB", size, (12, 34, 56)) as image:
        image.save(output, format=format, progressive=progressive)
    return output.getvalue()


def animated_png() -> bytes:
    output = io.BytesIO()
    with Image.new("RGBA", (2, 2), (1, 2, 3, 255)) as first, Image.new(
        "RGBA", (2, 2), (4, 5, 6, 255)
    ) as second:
        first.save(output, format="PNG", save_all=True, append_images=[second], duration=10, loop=0)
    return output.getvalue()


def png_with_dimensions(width: int, height: int) -> bytes:
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", width.to_bytes(4, "big") + height.to_bytes(4, "big") + b"\x08\x06\0\0\0")
        + png_chunk(b"IDAT", zlib.compress(b"\0\xff\xff\xff\xff"))
        + png_chunk(b"IEND", b"")
    )


PNG = pillow_image("PNG")
JPEG = pillow_image("JPEG")
PROGRESSIVE_JPEG = pillow_image("JPEG", progressive=True)
WEBP = pillow_image("WEBP")


def eventually(predicate: object, timeout: float = 1.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if callable(predicate) and predicate():
            return True
        time.sleep(0.01)
    return bool(callable(predicate) and predicate())


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


class LayoutStoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = (Path(self.temp.name) / "state").resolve()
        self.store = layout_store.LayoutStore(self.root)
        self.extra_stores: list[layout_store.LayoutStore] = []

    def tearDown(self) -> None:
        for store in self.extra_stores:
            store.close()
        self.store.close()
        self.temp.cleanup()

    def test_sqlite_first_boot_restart_noops_revisions_and_modes(self) -> None:
        self.assertEqual(
            self.store.get(),
            {
                "schema": "mind.dashboard.layout.v1",
                "status": "ready",
                "error": None,
                "revision": 0,
                "floorplan": None,
                "positions": [],
            },
        )
        self.assertEqual(self.store.database_path.name, "dashboard.sqlite3")
        self.assertEqual(os.stat(self.root).st_mode & 0o777, 0o700)
        self.assertEqual(os.stat(self.store.database_path).st_mode & 0o777, 0o600)
        with sqlite3.connect(self.store.database_path) as connection:
            self.assertEqual(connection.execute("PRAGMA user_version").fetchone(), (1,))
            columns = [row[1] for row in connection.execute("PRAGMA table_info(layout_state)")]
            self.assertEqual(
                columns,
                [
                    "id",
                    "revision",
                    "positions_json",
                    "floorplan_sha256",
                    "floorplan_mime",
                    "floorplan_width",
                    "floorplan_height",
                    "floorplan_blob",
                ],
            )

        positions = layout_store.normalize_positions(
            [
                {"adva": "0102545678c1", "x": 1, "y": 0},
                {"adva": "0102545678c0", "x": 0.25, "y": 0.5},
            ]
        )
        placed = self.store.replace_positions(0, positions)
        self.assertEqual((placed["revision"], [item["adva"] for item in placed["positions"]]), (1, ["0102545678c0", "0102545678c1"]))
        database_before = self.store.database_path.read_bytes()
        self.assertEqual(self.store.replace_positions(1, list(reversed(positions)))["revision"], 1)
        self.assertEqual(self.store.database_path.read_bytes(), database_before)
        self.assertEqual(self.store.remove(1)["revision"], 1)

        image = layout_store.inspect_image("image/png", PNG)
        self.assertEqual(self.store.upload(1, image, PNG)["revision"], 2)
        self.assertEqual(self.store.upload(2, image, PNG)["revision"], 2)
        self.assertEqual(self.store.remove(2)["revision"], 3)

        self.store.close()
        restarted = layout_store.LayoutStore(self.root)
        self.extra_stores.append(restarted)
        restored = restarted.get()
        self.assertEqual((restored["revision"], restored["floorplan"], restored["positions"]), (3, None, placed["positions"]))

    def test_cas_conflict_and_race_have_one_winner(self) -> None:
        first = self.store.replace_positions(
            0, [{"adva": "0102545678c0", "x": 0.1, "y": 0.2}]
        )
        with self.assertRaises(layout_store.Conflict) as stale:
            self.store.replace_positions(0, [])
        self.assertEqual((first["revision"], stale.exception.current["revision"]), (1, 1))

        barrier = threading.Barrier(2)
        results: list[str] = []
        result_lock = threading.Lock()

        def write(store: layout_store.LayoutStore, adva: str) -> None:
            barrier.wait(timeout=1)
            try:
                store.replace_positions(1, [{"adva": adva, "x": 0.5, "y": 0.5}])
            except layout_store.Conflict:
                result = "conflict"
            else:
                result = "success"
            with result_lock:
                results.append(result)

        writers = [
            threading.Thread(target=write, args=(self.store, "0102545678c0")),
            threading.Thread(target=write, args=(self.store, "0102545678c1")),
        ]
        for writer in writers:
            writer.start()
        for writer in writers:
            writer.join(timeout=2)
            self.assertFalse(writer.is_alive())
        self.assertEqual(sorted(results), ["conflict", "success"])

    def test_busy_corrupt_and_closed_store_are_unavailable_without_recovery(self) -> None:
        self.store.get()
        blocker = sqlite3.connect(self.store.database_path, isolation_level=None, timeout=0)
        try:
            blocker.execute("BEGIN EXCLUSIVE")
            with self.assertRaises(layout_store.StorageUnavailable):
                self.store.replace_positions(0, [])
        finally:
            blocker.rollback()
            blocker.close()

        self.store.close()
        self.store.database_path.write_bytes(b"not a sqlite database")
        corrupt_bytes = self.store.database_path.read_bytes()
        corrupt = layout_store.LayoutStore(self.root)
        self.extra_stores.append(corrupt)
        with self.assertRaises(layout_store.StorageUnavailable):
            corrupt.get()
        self.assertEqual(self.store.database_path.read_bytes(), corrupt_bytes)

        untouched_mtime = self.store.database_path.stat().st_mtime_ns
        corrupt.close()
        with self.assertRaises(layout_store.StorageUnavailable):
            corrupt.get()
        self.assertEqual(self.store.database_path.stat().st_mtime_ns, untouched_mtime)

    def test_active_database_inode_is_pinned_until_close(self) -> None:
        unlinked_root = (Path(self.temp.name) / "unlinked").resolve()
        unlinked = layout_store.LayoutStore(unlinked_root)
        self.extra_stores.append(unlinked)
        unlinked.get()
        os.unlink(unlinked.database_path)
        with self.assertRaises(layout_store.StorageUnavailable):
            unlinked.get()
        with self.assertRaises(layout_store.StorageUnavailable):
            unlinked.replace_positions(0, [])
        self.assertFalse(unlinked.database_path.exists())
        unlinked.close()
        restarted = layout_store.LayoutStore(unlinked_root)
        self.extra_stores.append(restarted)
        self.assertEqual(restarted.get()["revision"], 0)

        active_root = (Path(self.temp.name) / "replaced").resolve()
        active = layout_store.LayoutStore(active_root)
        self.extra_stores.append(active)
        active.get()
        source_root = (Path(self.temp.name) / "replacement-source").resolve()
        source = layout_store.LayoutStore(source_root)
        self.extra_stores.append(source)
        source.replace_positions(0, [{"adva": "0102545678c0", "x": 0.1, "y": 0.2}])
        source.close()
        os.replace(source.database_path, active.database_path)
        replacement_mode = os.stat(active.database_path).st_mode & 0o777
        with self.assertRaises(layout_store.StorageUnavailable):
            active.get()
        with self.assertRaises(layout_store.StorageUnavailable):
            active.replace_positions(0, [])
        self.assertEqual(os.stat(active.database_path).st_mode & 0o777, replacement_mode)
        active.close()
        restarted = layout_store.LayoutStore(active_root)
        self.extra_stores.append(restarted)
        self.assertEqual(restarted.get()["positions"], [{"adva": "0102545678c0", "x": 0.1, "y": 0.2}])

    def test_canonical_base64_xdg_and_position_boundaries(self) -> None:
        encoded = base64.b64encode(PNG).decode("ascii")
        self.assertEqual(layout_store.decode_canonical_base64(encoded), PNG)
        for value in (encoded + "\n", encoded.rstrip("="), "data:image/png;base64," + encoded, "A==="):
            with self.subTest(value=value), self.assertRaises(ValueError):
                layout_store.decode_canonical_base64(value)
        self.assertEqual(
            layout_store.default_state_dir({"XDG_DATA_HOME": "/var/data", "HOME": "/home/example"}),
            Path("/var/data/tron-dashboard"),
        )
        for xdg in ("", "relative"):
            with self.subTest(xdg=xdg):
                self.assertEqual(
                    layout_store.default_state_dir({"XDG_DATA_HOME": xdg, "HOME": "/home/example"}),
                    Path("/home/example/.local/share/tron-dashboard"),
                )
        with self.assertRaises(ValueError):
            layout_store.state_dir_argument("relative")
        for positions in (
            [{"adva": "0102545678c0", "x": float("nan"), "y": 0}],
            [{"adva": "0102545678c0", "x": 1.1, "y": 0}],
            [{"adva": "0102545678C0", "x": 0, "y": 0}],
            [{"adva": f"{number:012x}", "x": 0, "y": 0} for number in range(17)],
        ):
            with self.subTest(positions=positions), self.assertRaises(ValueError):
                layout_store.normalize_positions(positions)

    def test_pillow_validation_and_terminal_boundaries(self) -> None:
        valid = (
            ("image/png", PNG),
            ("image/jpeg", JPEG),
            ("image/jpeg", PROGRESSIVE_JPEG),
            ("image/webp", WEBP),
        )
        for mime, data in valid:
            with self.subTest(mime=mime):
                image = layout_store.inspect_image(mime, data)
                self.assertEqual((image.mime, image.sha256), (mime, hashlib.sha256(data).hexdigest()))
        for mime, data in (
            ("image/png", PNG + b"trailing"),
            ("image/png", PNG + png_chunk(b"IEND", b"")),
            ("image/jpeg", JPEG + b"trailing"),
            ("image/jpeg", JPEG + b"\xff\xd9"),
            ("image/webp", WEBP + b"trailing"),
            ("image/png", PNG[:-1]),
            ("image/jpeg", JPEG[:-2]),
            ("image/webp", WEBP[:-1]),
        ):
            with self.subTest(mime=mime, data_length=len(data)), self.assertRaises(layout_store.InvalidImage):
                layout_store.inspect_image(mime, data)
        with self.assertRaisesRegex(layout_store.InvalidImage, "image_type_mismatch"):
            layout_store.inspect_image("image/jpeg", PNG)
        with self.assertRaisesRegex(layout_store.InvalidImage, "animated_image"):
            layout_store.inspect_image("image/png", animated_png())
        with self.assertRaisesRegex(layout_store.InvalidImage, "image_dimensions"):
            layout_store.inspect_image("image/png", png_with_dimensions(8000, 8001))

    def test_tampered_sqlite_floorplan_is_unavailable_before_response_conflict_noop_or_open(self) -> None:
        cases: tuple[tuple[str, str, object], ...] = (
            ("sha", "floorplan_sha256", "0" * 64),
            ("mime", "floorplan_mime", "image/jpeg"),
            ("dimensions", "floorplan_width", 4),
            ("blob", "floorplan_blob", PNG + b"trailing"),
        )
        image = layout_store.inspect_image("image/png", PNG)
        for name, column, value in cases:
            with self.subTest(name=name):
                root = (Path(self.temp.name) / f"tampered-{name}").resolve()
                store = layout_store.LayoutStore(root)
                self.extra_stores.append(store)
                store.upload(0, image, PNG)
                store.close()
                with sqlite3.connect(store.database_path) as database:
                    database.execute(f"UPDATE layout_state SET {column} = ? WHERE id = 1", (value,))
                tampered = layout_store.LayoutStore(root)
                self.extra_stores.append(tampered)
                with self.assertRaises(layout_store.StorageUnavailable):
                    tampered.get()
                with self.assertRaises(layout_store.StorageUnavailable):
                    tampered.replace_positions(0, [])
                with self.assertRaises(layout_store.StorageUnavailable):
                    tampered.replace_positions(1, [])
                with self.assertRaises(layout_store.StorageUnavailable):
                    tampered.open_image(image.sha256)

    def test_open_image_is_an_immutable_verified_sqlite_snapshot(self) -> None:
        first = layout_store.inspect_image("image/png", PNG)
        self.store.upload(0, first, PNG)
        snapshot = self.store.open_image(first.sha256)
        self.assertIsNotNone(snapshot)
        assert snapshot is not None
        second_data = pillow_image("PNG", size=(4, 3))
        second = layout_store.inspect_image("image/png", second_data)
        self.store.upload(1, second, second_data)
        self.assertEqual((snapshot.data, snapshot.length, snapshot.mime), (PNG, len(PNG), "image/png"))
        self.assertIsNone(self.store.open_image(first.sha256))

    def test_sqlite_blob_response_streams_immutable_bytes_in_bounded_chunks(self) -> None:
        pixels = os.urandom(300 * 300)
        output = io.BytesIO()
        with Image.frombytes("L", (300, 300), pixels) as source:
            source.save(output, format="PNG")
        data = output.getvalue()
        self.assertGreater(len(data), layout_store.IMAGE_CHUNK_BYTES)
        image = layout_store.inspect_image("image/png", data)
        self.store.upload(0, image, data)

        class Writes:
            def __init__(self) -> None:
                self.chunks: list[bytes] = []

            def write(self, chunk: bytes) -> int:
                self.chunks.append(bytes(chunk))
                return len(chunk)

        writes = Writes()
        handler = object.__new__(bridge.BridgeRequestHandler)
        handler.server = SimpleNamespace(bridge=SimpleNamespace(layout=self.store))
        handler.headers = Message()
        handler.wfile = writes
        handler.close_connection = False
        handler.send_response = lambda _status: None
        handler.send_header = lambda _name, _value: None
        handler.end_headers = lambda: None
        handler._serve_floorplan(image.sha256)
        self.assertEqual(b"".join(writes.chunks), data)
        self.assertGreaterEqual(len(writes.chunks), 2)
        self.assertTrue(all(0 < len(chunk) <= layout_store.IMAGE_CHUNK_BYTES for chunk in writes.chunks))


class LayoutHTTPTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.assets = root / "dist"
        self.assets.mkdir()
        (self.assets / "index.html").write_text("ok", encoding="utf-8")
        self.state = bridge.Bridge(["test"], state_dir=(root / "state").resolve())
        self.server = ServerHarness(self.state, self.assets)
        self.png_base64 = base64.b64encode(PNG).decode("ascii")

    def tearDown(self) -> None:
        self.server.close()
        self.temp.cleanup()

    def request_json(self, method: str, path: str, payload: dict[str, object], headers: dict[str, str] | None = None) -> tuple[int, dict[str, str], dict[str, object]]:
        import http.client

        request_headers = {"Content-Type": "application/json"}
        if headers:
            request_headers.update(headers)
        connection = http.client.HTTPConnection("127.0.0.1", self.server.port, timeout=3)
        connection.request(method, path, json.dumps(payload, separators=(",", ":")).encode(), request_headers)
        response = connection.getresponse()
        result = response.status, dict(response.getheaders()), json.loads(response.read())
        connection.close()
        return result

    def test_api_security_boundaries_etag_and_multichunk_snapshot_stream(self) -> None:
        status, _, initial = self.request_json(
            "PUT",
            "/api/layout",
            {"schema": "mind.dashboard.layout.update.v1", "base_revision": 0, "positions": []},
        )
        self.assertEqual((status, initial["revision"]), (200, 0))
        status, _, uploaded = self.request_json(
            "POST",
            "/api/floorplan/upload",
            {
                "schema": "mind.dashboard.floorplan.upload.v1",
                "base_revision": 0,
                "mime": "image/png",
                "data_base64": self.png_base64,
            },
        )
        self.assertEqual((status, uploaded["revision"]), (200, 1))
        sha256 = uploaded["floorplan"]["sha256"]
        import http.client

        connection = http.client.HTTPConnection("127.0.0.1", self.server.port, timeout=3)
        connection.request("GET", f"/api/floorplan/{sha256}")
        response = connection.getresponse()
        self.assertEqual((response.status, response.getheader("ETag"), response.getheader("X-Content-Type-Options")), (200, f'"{sha256}"', "nosniff"))
        self.assertEqual(response.read(), PNG)
        connection.close()

        connection = http.client.HTTPConnection("127.0.0.1", self.server.port, timeout=3)
        connection.request("GET", f"/api/floorplan/{sha256}", headers={"If-None-Match": f'W/"{sha256}"'})
        response = connection.getresponse()
        self.assertEqual((response.status, response.read(), response.getheader("Content-Length")), (304, b"", None))
        connection.close()

        status, _, forbidden = self.request_json(
            "POST",
            "/api/floorplan/remove",
            {"schema": "mind.dashboard.floorplan.remove.v1", "base_revision": 1},
            {"Origin": f"http://attacker.invalid:{self.server.port}"},
        )
        self.assertEqual((status, forbidden["error"]), (403, "forbidden_request"))

    def test_decimal_content_length_overflow_is_413_before_body_read(self) -> None:
        client = socket.create_connection(("127.0.0.1", self.server.port), timeout=2)
        try:
            request = (
                f"POST /api/floorplan/upload HTTP/1.1\r\nHost: 127.0.0.1:{self.server.port}\r\n"
                "Content-Type: application/json\r\nContent-Length: "
                + ("9" * 5000)
                + "\r\n\r\n"
            ).encode()
            client.sendall(request)
            client.settimeout(2)
            response_parts: list[bytes] = []
            while True:
                chunk = client.recv(4096)
                if not chunk:
                    break
                response_parts.append(chunk)
            response = b"".join(response_parts)
        finally:
            client.close()
        self.assertIn(b"HTTP/1.1 413", response)
        self.assertIn(b'"error":"request_too_large"', response)

    def test_upload_admission_and_deadline_cannot_mutate_after_expiry(self) -> None:
        client = socket.create_connection(("127.0.0.1", self.server.port), timeout=2)
        try:
            request = (
                f"POST /api/floorplan/upload HTTP/1.1\r\nHost: 127.0.0.1:{self.server.port}\r\n"
                "Content-Type: application/json\r\nContent-Length: 100\r\n\r\n"
            ).encode()
            client.sendall(request)
            self.assertTrue(eventually(lambda: self.server.server.active_request_count() == 1))
            status, _, body = self.request_json(
                "POST",
                "/api/floorplan/upload",
                {
                    "schema": "mind.dashboard.floorplan.upload.v1",
                    "base_revision": 0,
                    "mime": "image/png",
                    "data_base64": self.png_base64,
                },
            )
            self.assertEqual((status, body["error"]), (503, "upload_busy"))
        finally:
            client.close()

        self.server.close()
        timed_state = bridge.Bridge(
            ["test"],
            timing=bridge.Timing(http_request_deadline=0.05, http_io_timeout=0.5),
            state_dir=(Path(self.temp.name) / "timed-state").resolve(),
        )
        self.state = timed_state
        self.server = ServerHarness(timed_state, self.assets)
        locked = threading.Event()
        release = threading.Event()

        def hold_store_lock() -> None:
            with timed_state.layout._lock:  # test-only scheduling seam
                locked.set()
                release.wait(timeout=1)

        holder = threading.Thread(target=hold_store_lock)
        holder.start()
        self.assertTrue(locked.wait(timeout=1))
        payload = json.dumps(
            {
                "schema": "mind.dashboard.layout.update.v1",
                "base_revision": 0,
                "positions": [{"adva": "0102545678c0", "x": 0.5, "y": 0.5}],
            },
            separators=(",", ":"),
        ).encode()
        client = socket.create_connection(("127.0.0.1", self.server.port), timeout=2)
        try:
            request = (
                f"PUT /api/layout HTTP/1.1\r\nHost: 127.0.0.1:{self.server.port}\r\n"
                f"Content-Type: application/json\r\nContent-Length: {len(payload)}\r\n\r\n"
            ).encode() + payload
            client.sendall(request)
            time.sleep(0.1)
        finally:
            release.set()
            client.close()
        holder.join(timeout=1)
        self.assertFalse(holder.is_alive())
        self.assertTrue(eventually(lambda: self.server.server.active_request_count() == 0))
        self.assertEqual(timed_state.layout.get()["revision"], 0)

    def test_server_close_joins_workers_before_the_store_is_closed(self) -> None:
        self.assertFalse(self.server.server.daemon_threads)
        self.server.close()
        with self.assertRaises(layout_store.StorageUnavailable):
            self.state.layout.get()


if __name__ == "__main__":
    unittest.main()
