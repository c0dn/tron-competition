#!/usr/bin/env python3
"""Focused deterministic unit checks for expiry-resource stack selection."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile


def load_checker(root: pathlib.Path):
    path = root / "scripts" / "check_tavrn_expiry_resources.py"
    spec = importlib.util.spec_from_file_location("expiry_checker", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load resource checker")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    if len(sys.argv) != 2:
        return 2
    fixture_path = pathlib.Path(sys.argv[1])
    fixture = json.loads(fixture_path.read_text(encoding="utf-8"))
    root = fixture_path.parents[4]
    checker = load_checker(root)
    if "map_lines" in fixture:
        with tempfile.TemporaryDirectory() as directory:
            map_path = pathlib.Path(directory) / "firmware.map"
            map_path.write_text("\n".join(fixture["map_lines"]) + "\n", encoding="utf-8")
            usage = checker.parse_map(map_path)
        return 0 if {
            "data_bytes": usage.data_bytes,
            "bss_bytes": usage.bss_bytes,
            "allocated_bytes": usage.allocated_bytes,
            "unallocated_bytes": usage.unallocated_bytes,
        } == fixture["expected"] else 1
    frames = checker.StackFrames()
    for frame in fixture["frames"]:
        frames.add(frame["source"], frame["function"], frame["bytes"])
    frames.finish()
    if "expected_duplicate_static_bytes" in fixture and \
       frames.resolve("duplicate_static").frame_bytes != fixture["expected_duplicate_static_bytes"]:
        return 1
    edge_document = {"schema": checker.EDGE_SCHEMA, "root": fixture["root"],
                     "edges": fixture["edges"],
                     "declared_leaves": fixture.get("declared_leaves", [])}
    with tempfile.TemporaryDirectory() as directory:
        edge_path = pathlib.Path(directory) / "edges.json"
        edge_path.write_text(json.dumps(edge_document), encoding="utf-8")
        try:
            path = checker.validate_edges(
                edge_path, fixture.get("resolvers", {}),
                {key: set(value) for key, value in fixture["graph"].items()},
                frames, fixture["root"], include_binding=False)
        except checker.CheckFailure:
            return 0 if fixture.get("expected_failure") == "stack" else 1
    if fixture.get("expected_failure") == "stack":
        return 1
    if [frame.bare_name for frame in path] != fixture["expected_path"]:
        return 1
    if fixture.get("check_missing_clone"):
        try:
            frames.resolve("short_heavy.isra.0")
        except checker.CheckFailure:
            return 0
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
