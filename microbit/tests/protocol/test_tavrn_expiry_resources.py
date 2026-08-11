#!/usr/bin/env python3
"""Focused deterministic unit checks for expiry-resource stack selection."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
from types import SimpleNamespace


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
    passing_fixture = fixture_path.parent / "pass.json"

    def run_checker_cli(*arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(root / "scripts" / "check_tavrn_expiry_resources.py"),
             *arguments], capture_output=True, text=True, check=False)

    fixture_only = run_checker_cli("--fixture", str(passing_fixture))
    if fixture_only.returncode != 0 or "FIXTURE_PASS " not in fixture_only.stdout or \
            "\nPASS " in fixture_only.stdout:
        return 1
    manifest_only = run_checker_cli("--resource-manifest", str(passing_fixture))
    if manifest_only.returncode != 0 or "MANIFEST_PASS " not in manifest_only.stdout or \
            "\nPASS " in manifest_only.stdout:
        return 1
    for wrapper in ("--fixture", "--resource-manifest"):
        partial = run_checker_cli(wrapper, str(passing_fixture), "--full-map", "missing.map")
        if partial.returncode != 2 or "FAIL arguments: incomplete fresh resource evidence:" \
                not in partial.stderr:
            return 1
    try:
        checker.require_rooted_stack_edges({"edges": []})
    except checker.CheckFailure as error:
        if error.code != "stack":
            return 1
    else:
        return 1
    with tempfile.TemporaryDirectory() as directory:
        same_map = pathlib.Path(directory) / "same.map"
        same_map.write_text("", encoding="utf-8")
        try:
            checker.verify_baseline(
                SimpleNamespace(baseline_manifest="unused.json", before_map=str(same_map),
                                baseline_sha256="unused.sha256", full_map=str(same_map)),
                {})
        except checker.CheckFailure as error:
            if error.code != "provenance":
                return 1
        else:
            return 1
    with tempfile.TemporaryDirectory() as directory:
        temporary = pathlib.Path(directory)
        config = temporary / "tron_build_config.h"
        config.write_text(
            "#define TRON_BUILD_INITIAL_TASK_STACK_BYTES 4096u\n"
            "#define TRON_BUILD_RUNTIME_RAM_RESERVE_BYTES 12288u\n",
            encoding="utf-8")
        manifest = {
            "build.kind": "ROUTED",
            "build.phase1_target": "ROUTED",
            "build.initial_task_stack_bytes": "4096",
            "capacity.routed_initial_task_stack_bytes": "4096",
            "capacity.routed_initial_task_stack_bytes.state": "IMPLEMENTED",
            "resource.runtime_ram_reserve_bytes": "12288",
        }

        def expect_failure(candidate, code):
            try:
                checker.validate_routed_runtime_declarations(candidate, config)
            except checker.CheckFailure as error:
                return error.code == code
            return False

        declaration = checker.validate_routed_runtime_declarations(manifest, config)
        if declaration.initial_task_stack_bytes != 4096 or \
                declaration.runtime_ram_reserve_bytes != 12288:
            return 1
        mismatched_config = temporary / "mismatched_tron_build_config.h"
        mismatched_config.write_text(
            "#define TRON_BUILD_INITIAL_TASK_STACK_BYTES 4096u\n"
            "#define TRON_BUILD_RUNTIME_RAM_RESERVE_BYTES 0u\n",
            encoding="utf-8")
        try:
            checker.validate_routed_runtime_declarations(manifest, mismatched_config)
        except checker.CheckFailure as error:
            if error.code != "provenance":
                return 1
        else:
            return 1
        for key, value in (
                ("resource.runtime_ram_reserve_bytes", None),
                ("resource.runtime_ram_reserve_bytes", "12KiB"),
                ("resource.runtime_ram_reserve_bytes", "0"),
                ("build.initial_task_stack_bytes", None),
                ("capacity.routed_initial_task_stack_bytes", "2048")):
            candidate = dict(manifest)
            if value is None:
                candidate.pop(key)
            else:
                candidate[key] = value
            if not expect_failure(candidate, "provenance"):
                return 1
        if checker.post_reserve_ram_bytes(20480, 12288) != 8192:
            return 1
        for unallocated, reserve in ((20479, 12288), (12287, 12288)):
            try:
                checker.post_reserve_ram_bytes(unallocated, reserve)
            except checker.CheckFailure as error:
                if error.code != "ram_unallocated":
                    return 1
            else:
                return 1
        main_source = temporary / "app" / "tavrn_routed_node" / "src" / "main.c"
        main_source.parent.mkdir(parents=True)
        main_source.write_text("", encoding="utf-8")
        production = checker.StackFrames()
        production.add(str(main_source), "usermain", 440)
        production.add("/kernel/usermain/usermain.c", "usermain", 16)
        production.finish()
        initial_report = checker.initial_task_stack_report(declaration, production, main_source)
        if initial_report.static_frame_bytes != 440 or \
                initial_report.logical_headroom_bytes != 3656:
            return 1
        wrong = checker.StackFrames()
        wrong.add("/kernel/usermain/usermain.c", "usermain", 16)
        wrong.finish()
        try:
            checker.initial_task_stack_report(declaration, wrong, main_source)
        except checker.CheckFailure as error:
            if error.code != "stack":
                return 1
        else:
            return 1
        ambiguous = checker.StackFrames()
        ambiguous.add(str(main_source), "usermain", 440)
        ambiguous.add(str(main_source.parent / ".." / "src" / "main.c"), "usermain", 440)
        ambiguous.finish()
        try:
            checker.initial_task_stack_report(declaration, ambiguous, main_source)
        except checker.CheckFailure as error:
            if error.code != "stack":
                return 1
        else:
            return 1
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
