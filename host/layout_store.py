"""Bounded SQLite-backed floorplan state for the localhost bridge."""

from __future__ import annotations

import base64
import binascii
import hashlib
import io
import json
import math
import os
import re
import sqlite3
import stat
import threading
import warnings
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Optional

from PIL import Image, UnidentifiedImageError


LAYOUT_SCHEMA = "mind.dashboard.layout.v1"
LAYOUT_UPDATE_SCHEMA = "mind.dashboard.layout.update.v1"
FLOORPLAN_UPLOAD_SCHEMA = "mind.dashboard.floorplan.upload.v1"
FLOORPLAN_REMOVE_SCHEMA = "mind.dashboard.floorplan.remove.v1"
ERROR_SCHEMA = "mind.dashboard.error.v1"
CONFLICT_SCHEMA = "mind.dashboard.layout.conflict.v1"

MAX_POSITIONS = 16
MAX_DECODED_IMAGE_BYTES = 5 * 1024 * 1024
MAX_IMAGE_DIMENSION = 8192
MAX_IMAGE_PIXELS = 64_000_000
IMAGE_CHUNK_BYTES = 65536
DATABASE_NAME = "dashboard.sqlite3"
SCHEMA_VERSION = 1

ADV_A_PATTERN = re.compile(r"[0-9a-f]{12}")
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
BASE64_PATTERN = re.compile(r"[A-Za-z0-9+/]*={0,2}")
MIMES = frozenset({"image/png", "image/jpeg", "image/webp"})
_FORMAT_MIMES = {"PNG": "image/png", "JPEG": "image/jpeg", "WEBP": "image/webp"}
_IMAGE_VALIDATION_LOCK = threading.Lock()


class StorageUnavailable(OSError):
    """SQLite state cannot safely be read or changed."""


class StateCorrupt(ValueError):
    """A row or image does not satisfy the persisted layout contract."""


class InvalidImage(ValueError):
    """An uploaded image is unsupported or structurally invalid."""

    def __init__(self, error: str) -> None:
        super().__init__(error)
        self.error = error


class Conflict(ValueError):
    """A CAS mutation supplied an obsolete base revision."""

    def __init__(self, current: dict[str, Any]) -> None:
        super().__init__("revision_conflict")
        self.current = current


class MutationCancelled(RuntimeError):
    """The HTTP deadline elapsed before a SQLite transaction started."""


@dataclass(frozen=True)
class ImageInfo:
    sha256: str
    mime: str
    width: int
    height: int


@dataclass(frozen=True)
class OpenImage:
    """An immutable SQLite BLOB snapshot suitable for bounded response writes."""

    data: bytes
    mime: str
    sha256: str

    @property
    def length(self) -> int:
        return len(self.data)


def default_state_dir(environ: Optional[dict[str, str]] = None) -> Path:
    values = os.environ if environ is None else environ
    xdg_data_home = values.get("XDG_DATA_HOME")
    if xdg_data_home and os.path.isabs(xdg_data_home):
        return Path(xdg_data_home) / "tron-dashboard"
    home = values.get("HOME") or str(Path.home())
    return Path(home) / ".local" / "share" / "tron-dashboard"


def state_dir_argument(value: str) -> Path:
    if not value or not os.path.isabs(value):
        raise ValueError("state directory must be an absolute path")
    return Path(value)


def dashboard_error(error: str) -> dict[str, Any]:
    return {"schema": ERROR_SCHEMA, "accepted": False, "error": error}


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise StateCorrupt("duplicate JSON key")
        result[key] = value
    return result


def _strict_json(raw: str) -> Any:
    try:
        return json.loads(
            raw,
            object_pairs_hook=_unique_object,
            parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)),
        )
    except (json.JSONDecodeError, ValueError) as error:
        raise StateCorrupt("invalid positions JSON") from error


def _finite_coordinate(value: Any) -> float:
    if type(value) not in {int, float}:
        raise StateCorrupt("coordinate is not a JSON number")
    try:
        normalized = float(value)
    except OverflowError as error:
        raise StateCorrupt("coordinate is too large") from error
    if not math.isfinite(normalized) or not 0.0 <= normalized <= 1.0:
        raise StateCorrupt("coordinate is outside normalized bounds")
    return 0.0 if normalized == 0.0 else normalized


def normalize_positions(value: Any) -> list[dict[str, Any]]:
    if type(value) is not list or len(value) > MAX_POSITIONS:
        raise ValueError("invalid positions")
    positions: list[dict[str, Any]] = []
    advas: set[str] = set()
    for entry in value:
        if type(entry) is not dict or set(entry) != {"adva", "x", "y"}:
            raise ValueError("invalid position")
        adva = entry["adva"]
        if type(adva) is not str or ADV_A_PATTERN.fullmatch(adva) is None or adva in advas:
            raise ValueError("invalid AdvA")
        try:
            x, y = _finite_coordinate(entry["x"]), _finite_coordinate(entry["y"])
        except StateCorrupt as error:
            raise ValueError("invalid coordinate") from error
        advas.add(adva)
        positions.append({"adva": adva, "x": x, "y": y})
    return sorted(positions, key=lambda entry: entry["adva"])


def _canonical_positions(positions: list[dict[str, Any]]) -> str:
    return json.dumps(positions, separators=(",", ":"), sort_keys=True, allow_nan=False)


def decode_canonical_base64(value: str) -> bytes:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise ValueError("invalid base64") from error
    if len(encoded) % 4 or BASE64_PATTERN.fullmatch(value) is None:
        raise ValueError("invalid base64")
    try:
        decoded = base64.b64decode(encoded, validate=True)
    except (binascii.Error, ValueError) as error:
        raise ValueError("invalid base64") from error
    if base64.b64encode(decoded) != encoded:
        raise ValueError("invalid base64")
    return decoded


def _dimensions(width: int, height: int) -> tuple[int, int]:
    if width <= 0 or height <= 0:
        raise InvalidImage("invalid_image")
    if width > MAX_IMAGE_DIMENSION or height > MAX_IMAGE_DIMENSION or width * height > MAX_IMAGE_PIXELS:
        raise InvalidImage("image_dimensions")
    return width, height


def _mime_from_magic(data: bytes) -> Optional[str]:
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    if data.startswith(b"\xff\xd8"):
        return "image/jpeg"
    if len(data) >= 12 and data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "image/webp"
    return None


def _validate_terminal_boundary(mime: str, data: bytes) -> None:
    if mime == "image/png":
        _validate_png_terminal_boundary(data)
        return
    if mime == "image/webp":
        if len(data) < 12 or int.from_bytes(data[4:8], "little") + 8 != len(data):
            raise InvalidImage("invalid_image")
        return
    if mime == "image/jpeg":
        _validate_jpeg_terminal_boundary(data)


def _validate_png_terminal_boundary(data: bytes) -> None:
    """Require the first syntactically bounded IEND chunk to be EOF."""

    position = 8
    while position < len(data):
        if len(data) - position < 12:
            raise InvalidImage("invalid_image")
        length = int.from_bytes(data[position : position + 4], "big")
        end = position + 12 + length
        if end > len(data):
            raise InvalidImage("invalid_image")
        kind = data[position + 4 : position + 8]
        if kind == b"IEND":
            if length != 0 or end != len(data):
                raise InvalidImage("invalid_image")
            return
        position = end
    raise InvalidImage("invalid_image")


def _validate_jpeg_terminal_boundary(data: bytes) -> None:
    """Walk JPEG markers/scans and require the first genuine EOI at EOF."""

    if len(data) < 4 or data[:2] != b"\xff\xd8":
        raise InvalidImage("invalid_image")
    position = 2
    in_entropy = False
    while position < len(data):
        if in_entropy:
            while position < len(data):
                if data[position] != 0xFF:
                    position += 1
                    continue
                marker_start = position
                position += 1
                while position < len(data) and data[position] == 0xFF:
                    position += 1
                if position == len(data):
                    raise InvalidImage("invalid_image")
                marker = data[position]
                if marker == 0x00 or 0xD0 <= marker <= 0xD7:
                    position += 1
                    continue
                if marker == 0xD9:
                    if position + 1 != len(data):
                        raise InvalidImage("invalid_image")
                    return
                position = marker_start
                in_entropy = False
                break
            else:
                raise InvalidImage("invalid_image")
            continue

        if data[position] != 0xFF:
            raise InvalidImage("invalid_image")
        position += 1
        while position < len(data) and data[position] == 0xFF:
            position += 1
        if position == len(data):
            raise InvalidImage("invalid_image")
        marker = data[position]
        position += 1
        if marker == 0x00 or marker == 0xD8 or 0xD0 <= marker <= 0xD7:
            raise InvalidImage("invalid_image")
        if marker == 0xD9:
            if position != len(data):
                raise InvalidImage("invalid_image")
            return
        if marker == 0x01:
            continue
        if len(data) - position < 2:
            raise InvalidImage("invalid_image")
        segment_length = int.from_bytes(data[position : position + 2], "big")
        if segment_length < 2 or segment_length > len(data) - position:
            raise InvalidImage("invalid_image")
        position += segment_length
        if marker == 0xDA:
            in_entropy = True
    raise InvalidImage("invalid_image")


def _image_properties(image: Image.Image, mime: str) -> tuple[int, int]:
    detected = _FORMAT_MIMES.get(image.format or "")
    if detected is None:
        raise InvalidImage("invalid_image")
    if detected != mime:
        raise InvalidImage("image_type_mismatch")
    width, height = _dimensions(*image.size)
    if getattr(image, "is_animated", False) or getattr(image, "n_frames", 1) != 1:
        raise InvalidImage("animated_image")
    return width, height


def inspect_image(mime: str, data: bytes) -> ImageInfo:
    """Validate raw image bytes without re-encoding or changing their hash."""

    if mime not in MIMES:
        raise InvalidImage("unsupported_media_type")
    if len(data) > MAX_DECODED_IMAGE_BYTES:
        raise InvalidImage("image_too_large")
    detected_magic = _mime_from_magic(data)
    if detected_magic is not None and detected_magic != mime:
        raise InvalidImage("image_type_mismatch")
    if detected_magic is None:
        raise InvalidImage("invalid_image")
    _validate_terminal_boundary(mime, data)

    bomb_error = Image.DecompressionBombError
    try:
        # MAX_IMAGE_PIXELS and warning filters are process globals. Keep this
        # small validation section serial so concurrent requests retain its cap.
        with _IMAGE_VALIDATION_LOCK:
            previous_limit = Image.MAX_IMAGE_PIXELS
            Image.MAX_IMAGE_PIXELS = MAX_IMAGE_PIXELS
            try:
                with warnings.catch_warnings():
                    warnings.simplefilter("error", Image.DecompressionBombWarning)
                    with Image.open(io.BytesIO(data)) as first:
                        width, height = _image_properties(first, mime)
                        first.verify()
                    # verify() deliberately invalidates the first object.
                    with Image.open(io.BytesIO(data)) as second:
                        second_width, second_height = _image_properties(second, mime)
                        if (second_width, second_height) != (width, height):
                            raise InvalidImage("invalid_image")
                        second.load()
            finally:
                Image.MAX_IMAGE_PIXELS = previous_limit
    except InvalidImage:
        raise
    except (Image.DecompressionBombWarning, bomb_error):
        raise InvalidImage("image_dimensions") from None
    except (UnidentifiedImageError, OSError, SyntaxError, EOFError, ValueError):
        raise InvalidImage("invalid_image") from None
    return ImageInfo(hashlib.sha256(data).hexdigest(), mime, width, height)


class LayoutStore:
    """One locked SQLite connection and one revisioned singleton layout row."""

    def __init__(self, state_dir: Path) -> None:
        if not state_dir.is_absolute():
            raise ValueError("state directory must be absolute")
        self.state_dir = state_dir
        self.database_path = state_dir / DATABASE_NAME
        # Kept as the old public test seam; it now always names the SQLite file.
        self.state_path = self.database_path
        self._lock = threading.RLock()
        self._upload_admission = threading.Lock()
        self._connection: Optional[sqlite3.Connection] = None
        self._database_identity: Optional[tuple[int, int]] = None
        self._closed = False

    def close(self) -> None:
        """Permanently close the store; subsequent calls never reopen SQLite."""

        with self._lock:
            if self._closed:
                return
            self._closed = True
            connection, self._connection = self._connection, None
            self._database_identity = None
            if connection is not None:
                try:
                    connection.close()
                except sqlite3.Error:
                    pass

    def try_admit_upload(self) -> bool:
        with self._lock:
            if self._closed:
                return False
        return self._upload_admission.acquire(blocking=False)

    def release_upload_admission(self) -> None:
        self._upload_admission.release()

    def get(self) -> dict[str, Any]:
        with self._lock:
            try:
                return self._response(self._read_state(self._connection_for_use()))
            except StorageUnavailable:
                raise
            except (sqlite3.Error, OSError, StateCorrupt, ValueError) as error:
                raise StorageUnavailable("cannot read SQLite layout state") from error

    def replace_positions(
        self,
        base_revision: int,
        positions: list[dict[str, Any]],
        *,
        cancel_check: Optional[Callable[[], bool]] = None,
    ) -> dict[str, Any]:
        canonical = _canonical_positions(normalize_positions(positions))

        def update(state: dict[str, Any]) -> Optional[tuple[str, tuple[Any, ...]]]:
            if state["positions_json"] == canonical:
                return None
            return "UPDATE layout_state SET revision = ?, positions_json = ? WHERE id = 1", (
                state["revision"] + 1,
                canonical,
            )

        return self._mutate(base_revision, update, cancel_check)

    def upload(
        self,
        base_revision: int,
        image: ImageInfo,
        data: bytes,
        *,
        cancel_check: Optional[Callable[[], bool]] = None,
    ) -> dict[str, Any]:
        if inspect_image(image.mime, data) != image:
            raise StorageUnavailable("unverified image candidate")

        def update(state: dict[str, Any]) -> Optional[tuple[str, tuple[Any, ...]]]:
            floorplan = state["floorplan"]
            if (
                floorplan is not None
                and floorplan["sha256"] == image.sha256
                and floorplan["mime"] == image.mime
                and floorplan["width"] == image.width
                and floorplan["height"] == image.height
                and floorplan["blob"] == data
            ):
                return None
            return (
                "UPDATE layout_state SET revision = ?, floorplan_sha256 = ?, floorplan_mime = ?, "
                "floorplan_width = ?, floorplan_height = ?, floorplan_blob = ? WHERE id = 1",
                (state["revision"] + 1, image.sha256, image.mime, image.width, image.height, data),
            )

        return self._mutate(base_revision, update, cancel_check)

    def remove(
        self,
        base_revision: int,
        *,
        cancel_check: Optional[Callable[[], bool]] = None,
    ) -> dict[str, Any]:
        def update(state: dict[str, Any]) -> Optional[tuple[str, tuple[Any, ...]]]:
            if state["floorplan"] is None:
                return None
            return (
                "UPDATE layout_state SET revision = ?, floorplan_sha256 = NULL, floorplan_mime = NULL, "
                "floorplan_width = NULL, floorplan_height = NULL, floorplan_blob = NULL WHERE id = 1",
                (state["revision"] + 1,),
            )

        return self._mutate(base_revision, update, cancel_check)

    def open_image(self, sha256: str) -> Optional[OpenImage]:
        """Return an immutable, verified BLOB snapshot while holding the DB lock."""

        with self._lock:
            try:
                state = self._read_state(self._connection_for_use())
                floorplan = state["floorplan"]
                if floorplan is None or floorplan["sha256"] != sha256:
                    return None
                return OpenImage(bytes(floorplan["blob"]), floorplan["mime"], floorplan["sha256"])
            except StorageUnavailable:
                raise
            except (sqlite3.Error, OSError, StateCorrupt, ValueError) as error:
                raise StorageUnavailable("cannot read SQLite floorplan blob") from error

    def _mutate(
        self,
        base_revision: int,
        update: Callable[[dict[str, Any]], Optional[tuple[str, tuple[Any, ...]]]],
        cancel_check: Optional[Callable[[], bool]],
    ) -> dict[str, Any]:
        if cancel_check is not None and not cancel_check():
            raise MutationCancelled()
        with self._lock:
            connection: Optional[sqlite3.Connection] = None
            transaction_started = False
            try:
                # A deadline can expire while waiting for the RLock. Do not
                # start a transaction after that point.
                if cancel_check is not None and not cancel_check():
                    raise MutationCancelled()
                connection = self._connection_for_use()
                if cancel_check is not None and not cancel_check():
                    raise MutationCancelled()
                connection.execute("BEGIN IMMEDIATE")
                transaction_started = True
                state = self._read_state(connection)
                current = self._response(state)
                if base_revision != state["revision"]:
                    connection.rollback()
                    transaction_started = False
                    raise Conflict(current)
                statement = update(state)
                if statement is None:
                    # No-op is read-only from the durable state perspective.
                    connection.rollback()
                    transaction_started = False
                    return current
                sql, parameters = statement
                connection.execute(sql, parameters)
                connection.commit()
                transaction_started = False
                return self._response(self._read_state(connection))
            except (Conflict, MutationCancelled):
                if transaction_started and connection is not None:
                    self._rollback(connection)
                raise
            except (sqlite3.Error, OSError, StateCorrupt, ValueError) as error:
                if transaction_started and connection is not None:
                    self._rollback(connection)
                raise StorageUnavailable("SQLite layout mutation failed") from error

    def _connection_for_use(self) -> sqlite3.Connection:
        if self._closed:
            raise StorageUnavailable("layout store is closed")
        if self._connection is not None:
            self._ensure_paths()
            return self._connection
        created_database = self._ensure_paths()
        if not created_database:
            try:
                with self.database_path.open("rb") as database:
                    if database.read(16) != b"SQLite format 3\x00":
                        raise StorageUnavailable("SQLite database header is invalid")
            except StorageUnavailable:
                raise
            except OSError as error:
                raise StorageUnavailable("cannot read SQLite layout database") from error
        try:
            connection = sqlite3.connect(
                self.database_path,
                timeout=0,
                isolation_level=None,
                check_same_thread=False,
            )
            connection.execute("PRAGMA busy_timeout = 0")
            self._initialize_schema(connection, preexisting_database=not created_database)
        except (sqlite3.Error, OSError) as error:
            try:
                connection.close()  # type: ignore[has-type]
            except (UnboundLocalError, sqlite3.Error):
                pass
            raise StorageUnavailable("cannot open SQLite layout database") from error
        self._connection = connection
        try:
            self._pin_active_database()
        except StorageUnavailable:
            self._connection = None
            connection.close()
            raise
        return connection

    def _initialize_schema(self, connection: sqlite3.Connection, *, preexisting_database: bool) -> None:
        version = connection.execute("PRAGMA user_version").fetchone()
        if version is None or type(version[0]) is not int:
            raise sqlite3.DatabaseError("invalid SQLite schema version")
        if version[0] == SCHEMA_VERSION:
            self._require_singleton_row(connection)
            return
        if version[0] != 0:
            raise sqlite3.DatabaseError("unsupported SQLite schema version")
        existing = connection.execute(
            "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'layout_state'"
        ).fetchone()
        if preexisting_database or existing is not None:
            raise sqlite3.DatabaseError("unrecognized SQLite layout database")
        connection.execute("BEGIN IMMEDIATE")
        try:
            connection.execute(
                "CREATE TABLE layout_state ("
                "id INTEGER PRIMARY KEY CHECK (id = 1), "
                "revision INTEGER NOT NULL CHECK (revision >= 0), "
                "positions_json TEXT NOT NULL CHECK (length(positions_json) <= 65536), "
                "floorplan_sha256 TEXT NULL, "
                "floorplan_mime TEXT NULL, "
                "floorplan_width INTEGER NULL, "
                "floorplan_height INTEGER NULL, "
                "floorplan_blob BLOB NULL CHECK (floorplan_blob IS NULL OR length(floorplan_blob) <= 5242880), "
                "CHECK ((floorplan_sha256 IS NULL AND floorplan_mime IS NULL AND floorplan_width IS NULL "
                "AND floorplan_height IS NULL AND floorplan_blob IS NULL) OR "
                "(floorplan_sha256 IS NOT NULL AND floorplan_mime IS NOT NULL AND floorplan_width IS NOT NULL "
                "AND floorplan_height IS NOT NULL AND floorplan_blob IS NOT NULL))"
                ")"
            )
            connection.execute(
                "INSERT INTO layout_state (id, revision, positions_json) VALUES (1, 0, ?)", ("[]",)
            )
            connection.execute(f"PRAGMA user_version = {SCHEMA_VERSION}")
            connection.commit()
        except BaseException:
            self._rollback(connection)
            raise

    @staticmethod
    def _rollback(connection: sqlite3.Connection) -> None:
        try:
            connection.rollback()
        except sqlite3.Error:
            pass

    @staticmethod
    def _require_singleton_row(connection: sqlite3.Connection) -> None:
        rows = connection.execute("SELECT id FROM layout_state").fetchall()
        if rows != [(1,)]:
            raise sqlite3.DatabaseError("SQLite layout singleton is invalid")

    def _ensure_paths(self) -> bool:
        """Secure the directory and return whether this call created the DB file."""

        if self._connection is not None:
            self._verify_active_paths()
            return False
        created_database = False
        try:
            try:
                details = os.lstat(self.state_dir)
            except FileNotFoundError:
                try:
                    self.state_dir.mkdir(mode=0o700, parents=True, exist_ok=False)
                except FileExistsError:
                    # Another thread/process may have won first-boot directory
                    # creation; validate its result below rather than failing.
                    pass
                details = os.lstat(self.state_dir)
            if not stat.S_ISDIR(details.st_mode) or stat.S_ISLNK(details.st_mode):
                raise OSError("state path is not a directory")
            os.chmod(self.state_dir, 0o700, follow_symlinks=False)

            try:
                database = os.lstat(self.database_path)
            except FileNotFoundError:
                flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
                descriptor = os.open(self.database_path, flags, 0o600)
                os.close(descriptor)
                created_database = True
            else:
                if not stat.S_ISREG(database.st_mode) or stat.S_ISLNK(database.st_mode):
                    raise OSError("database path is not a regular file")
            os.chmod(self.database_path, 0o600, follow_symlinks=False)
        except OSError as error:
            raise StorageUnavailable("cannot secure SQLite state path") from error
        return created_database

    def _pin_active_database(self) -> None:
        try:
            database = os.lstat(self.database_path)
        except OSError as error:
            raise StorageUnavailable("cannot pin SQLite database") from error
        if (
            not stat.S_ISREG(database.st_mode)
            or stat.S_ISLNK(database.st_mode)
            or stat.S_IMODE(database.st_mode) != 0o600
        ):
            raise StorageUnavailable("SQLite database is not securely pinned")
        self._database_identity = (database.st_dev, database.st_ino)

    def _verify_active_paths(self) -> None:
        """Never repair a path after SQLite has pinned the current inode."""

        try:
            directory = os.lstat(self.state_dir)
            database = os.lstat(self.database_path)
        except OSError as error:
            raise StorageUnavailable("pinned SQLite path is unavailable") from error
        if (
            not stat.S_ISDIR(directory.st_mode)
            or stat.S_ISLNK(directory.st_mode)
            or stat.S_IMODE(directory.st_mode) != 0o700
            or not stat.S_ISREG(database.st_mode)
            or stat.S_ISLNK(database.st_mode)
            or stat.S_IMODE(database.st_mode) != 0o600
            or self._database_identity != (database.st_dev, database.st_ino)
        ):
            raise StorageUnavailable("pinned SQLite path was replaced")

    def _read_state(self, connection: sqlite3.Connection) -> dict[str, Any]:
        row = connection.execute(
            "SELECT revision, positions_json, floorplan_sha256, floorplan_mime, floorplan_width, "
            "floorplan_height, floorplan_blob FROM layout_state WHERE id = 1"
        ).fetchone()
        if row is None:
            raise StateCorrupt("missing SQLite layout row")
        revision, positions_json, sha256, mime, width, height, blob = row
        if type(revision) is not int or revision < 0 or type(positions_json) is not str:
            raise StateCorrupt("invalid SQLite layout row")
        positions = normalize_positions(_strict_json(positions_json))
        if _canonical_positions(positions) != positions_json:
            raise StateCorrupt("noncanonical positions JSON")
        fields = (sha256, mime, width, height, blob)
        if all(field is None for field in fields):
            floorplan: Optional[dict[str, Any]] = None
        elif any(field is None for field in fields):
            raise StateCorrupt("partial floorplan metadata")
        else:
            if (
                type(sha256) is not str
                or SHA256_PATTERN.fullmatch(sha256) is None
                or type(mime) is not str
                or mime not in MIMES
                or type(width) is not int
                or type(height) is not int
                or not isinstance(blob, bytes)
            ):
                raise StateCorrupt("invalid floorplan metadata")
            _dimensions(width, height)
            if len(blob) > MAX_DECODED_IMAGE_BYTES:
                raise StateCorrupt("oversized floorplan blob")
            image = inspect_image(mime, blob)
            if image != ImageInfo(sha256, mime, width, height):
                raise StateCorrupt("floorplan metadata does not match blob")
            floorplan = {"sha256": sha256, "mime": mime, "width": width, "height": height, "blob": blob}
        return {"revision": revision, "positions_json": positions_json, "positions": positions, "floorplan": floorplan}

    @staticmethod
    def _response(state: dict[str, Any]) -> dict[str, Any]:
        floorplan = state["floorplan"]
        return {
            "schema": LAYOUT_SCHEMA,
            "status": "ready",
            "error": None,
            "revision": state["revision"],
            "floorplan": (
                None
                if floorplan is None
                else {
                    "sha256": floorplan["sha256"],
                    "mime": floorplan["mime"],
                    "width": floorplan["width"],
                    "height": floorplan["height"],
                    "url": f"/api/floorplan/{floorplan['sha256']}",
                }
            ),
            "positions": [position.copy() for position in state["positions"]],
        }
