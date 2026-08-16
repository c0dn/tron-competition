#!/usr/bin/env python3
"""Focused deterministic unit checks for expiry-resource stack selection."""

from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import shutil
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
    # Production FULL ingress-OFF has no MIND-derived target, while ingress-ON
    # can source-derive its callback. Both shapes must fail closed at a BLX site.
    off_site = (checker.IndirectCallSite("off_indirect", 0, 0, "r3"),)
    on_site = (checker.IndirectCallSite("on_indirect", 0, 0, "r3"),)
    for sites_for_mode, bindings_for_mode, contract_for_mode in (
            (off_site, (), {"indirect_calls": []}),
            (on_site, (checker.IndirectCallBinding("on_indirect", "on_target"),),
             {"indirect_calls": [{"from": "on_indirect", "site": 0,
                                  "to": "wrong_contract_target"}]})):
        try:
            checker.apply_indirect_callsite_mappings(
                {"routed_mesh_task": {sites_for_mode[0].caller}}, "routed_mesh_task",
                sites_for_mode, bindings_for_mode, contract_for_mode)
        except checker.CheckFailure as error:
            if error.code != "stack":
                return 1
        else:
            return 1
    unused_site = (checker.IndirectCallSite("unreachable_indirect", 0, 0, "r3"),)
    try:
        checker.apply_indirect_callsite_mappings(
            {"routed_mesh_task": set()}, "routed_mesh_task", unused_site, (),
            {"indirect_calls": [{"from": "unreachable_indirect", "site": 0,
                                 "to": "unused_target"}]})
    except checker.CheckFailure as error:
        if error.code != "stack":
            return 1
    else:
        return 1
    with tempfile.TemporaryDirectory() as directory:
        contract_path = pathlib.Path(directory) / "v2-contract.json"
        contract_path.write_text(json.dumps({
            "schema": checker.EDGE_SCHEMA, "root": "routed_mesh_task",
            "edges": [{"from": "routed_mesh_task", "to": "leaf",
                       "kind": "call", "when": "always"}],
            "declared_leaves": []}), encoding="utf-8")
        try:
            checker.load_stack_contract(contract_path, "routed_mesh_task", True)
        except checker.CheckFailure as error:
            if error.code != "stack":
                return 1
        else:
            return 1
        contract_path.write_text(json.dumps({
            "schema": checker.INDIRECT_EDGE_SCHEMA, "root": "routed_mesh_task",
            "edges": [{"from": "routed_mesh_task", "to": "leaf",
                       "kind": "call", "when": "always"}],
            "indirect_calls": [
                {"from": "duplicate", "site": 0, "to": "a"},
                {"from": "duplicate", "site": 0, "to": "b"}],
            "declared_leaves": []}), encoding="utf-8")
        try:
            checker.load_stack_contract(contract_path, "routed_mesh_task", True)
        except checker.CheckFailure as error:
            if error.code != "stack":
                return 1
        else:
            return 1
    application_components = {
        symbol: 1 for symbol in checker.MIND_APPLICATION_PRODUCTION_COMPONENTS |
        checker.MIND_APPLICATION_STATIC_STACK_COMPONENTS
    }
    if checker.APPLICATION_FIXED_STATE_LINKER_PLACEMENT_BYTES != 14:
        return 1
    application_report = {
        "schema": checker.APPLICATION_SIZE_SCHEMA,
        "capacities": dict(checker.MIND_APPLICATION_CAPACITIES),
        "sizeof": {"ingress": 1},
        "production": {
            "components": application_components,
            "aggregate_new_static_bytes": sum(application_components.values()),
        },
        "provenance": {
            "probe_source_sha256": "0" * 64,
            "compile_commands_sha256": "0" * 64,
            "elf_sha256": "0" * 64,
            "map_sha256": "0" * 64,
            "config_header_sha256": "0" * 64,
        },
    }
    try:
        checker.validate_application_size_report(application_report,
                                                 "application report")
    except checker.CheckFailure:
        return 1
    application_report["production"]["aggregate_new_static_bytes"] -= 1
    try:
        checker.validate_application_size_report(application_report,
                                                 "application report")
    except checker.CheckFailure as error:
        if error.code != "provenance":
            return 1
    else:
        return 1
    try:
        checker.require_static_mind_task_sources()
    except checker.CheckFailure:
        return 1
    with tempfile.TemporaryDirectory() as directory:
        temporary = pathlib.Path(directory)
        artifact_dir = temporary / "artifact"
        artifact_dir.mkdir()
        artifact = artifact_dir / "baseline.manifest"
        seal = temporary / "baseline.manifest.sha256"
        artifact.write_text("immutable baseline\n", encoding="utf-8")
        seal.write_text(checker.sha256_file(artifact) + "  baseline.manifest\n",
                        encoding="utf-8")
        checker.verify_external_artifact_seal(artifact, seal)
        try:
            checker.verify_external_artifact_seal(artifact, None)
        except checker.CheckFailure as error:
            if error.code != "provenance":
                return 1
        else:
            return 1
        artifact.write_text("mutated baseline\n", encoding="utf-8")
        try:
            checker.verify_external_artifact_seal(artifact, seal)
        except checker.CheckFailure as error:
            if error.code != "provenance":
                return 1
        else:
            return 1
        seal.write_text("0" * 64 + "  baseline.manifest\n", encoding="utf-8")
        try:
            checker.verify_external_artifact_seal(artifact, seal)
        except checker.CheckFailure as error:
            if error.code != "provenance":
                return 1
        else:
            return 1
    with tempfile.TemporaryDirectory() as directory:
        temporary = pathlib.Path(directory)
        gate = temporary / "resource-gate.txt"
        preprocessed = temporary / "main.i"
        contract = temporary / "stack-contract.json"
        manifest_path = temporary / "artifact.manifest"
        gate.write_text("PASS synthetic resource gate\n", encoding="utf-8")
        preprocessed.write_text(
            "routed_cycle_operations.router_scheduler_event = "
            "routed_cycle_router_scheduler_event;\n"
            "routed_cycle_operations.router_tick = routed_cycle_router_tick;\n"
            "void routed_cycle_router_tick(void) {\n"
            "if (tavrn_full_application_mailbox_owner_take()) {\n"
            "tavrn_full_maintenance_binding_tick(&routed_binding_result_storage);\n"
            "if (application_present && application_result) { "
            "tavrn_full_application_mailbox_owner_publish(); }\n}\n"
            "tavrn_router_phase_trace_t *trace = "
            "&routed_binding_result_storage.router_trace;\nreturn *trace;\n}\n",
            encoding="utf-8")
        contract.write_text(json.dumps({"schema": checker.EDGE_SCHEMA}), encoding="utf-8")
        manifest_path.write_text("\n".join((
            "resource.gate=PASSED",
            "resource.gate_report.name=" + gate.name,
            "resource.gate_report.sha256=" + checker.sha256_file(gate),
            "evidence.preprocessed_main.name=" + preprocessed.name,
            "evidence.preprocessed_main.sha256=" + checker.sha256_file(preprocessed),
            "resource.contract.count=1",
            "resource.contract.0.name=" + contract.name,
            "resource.contract.0.sha256=" + checker.sha256_file(contract),
            "application.wearable_ingress.requested=OFF",
            "application.wearable_ingress.effective=OFF",
            "")), encoding="utf-8")
        checker.verify_published_resource_provenance(manifest_path)
        gate.write_text("mutated\n", encoding="utf-8")
        try:
            checker.verify_published_resource_provenance(manifest_path)
        except checker.CheckFailure as error:
            if error.code != "provenance":
                return 1
        else:
            return 1
        gate.write_text("PASS synthetic resource gate\n", encoding="utf-8")
        contract.unlink()
        try:
            checker.verify_published_resource_provenance(manifest_path)
        except checker.CheckFailure as error:
            if error.code != "provenance":
                return 1
        else:
            return 1
    indirect_fixture = json.loads((fixture_path.parent / "indirect-maximum-path.json").read_text(
        encoding="utf-8"))
    graph = {node: set(children) for node, children in indirect_fixture["graph"].items()}
    sites = tuple(checker.IndirectCallSite(item["caller"], item["ordinal"], 0, "r3")
                  for item in indirect_fixture["sites"])
    derived = tuple(checker.IndirectCallBinding(item["caller"], item["target"])
                    for item in indirect_fixture["derived"])
    graph = checker.apply_indirect_callsite_mappings(
        graph, indirect_fixture["root"], sites, derived, {"indirect_calls": []})
    if any(target not in graph.get(caller, set())
           for caller, target in (("mind_root_coordinator_submit", "generic_submit_callback"),
                                  ("mind_event_forwarder_consume_ingress",
                                   "nested_indirect_heavy_callback"))):
        return 1
    frames = checker.StackFrames()
    for name, size in indirect_fixture["frames"].items():
        frames.add("fixture.c", name, size)
    frames.finish()
    chain = checker.validate_edges({"edges": [], "declared_leaves": []}, {}, graph, frames,
                                   indirect_fixture["root"])
    if chain[-1].bare_name != indirect_fixture["expected_terminal"]:
        return 1
    if [frame.bare_name for frame in chain] != indirect_fixture["expected_paths"]["nested_callback"]:
        return 1
    generic_path = indirect_fixture["expected_paths"]["generic_submit"]
    if not all(generic_path[index + 1] in graph.get(generic_path[index], set())
               for index in range(len(generic_path) - 1)):
        return 1
    unresolved_site = (checker.IndirectCallSite("unresolved_indirect", 0, 0, "r3"),)
    try:
        checker.apply_indirect_callsite_mappings(
            {"routed_mesh_task": {"unresolved_indirect"}}, "routed_mesh_task",
            unresolved_site, (), {"indirect_calls": []})
    except checker.CheckFailure as error:
        if error.code != "stack":
            return 1
    else:
        return 1
    ambiguous_site = (checker.IndirectCallSite("ambiguous_indirect", 0, 0, "r3"),)
    try:
        checker.apply_indirect_callsite_mappings(
            {"routed_mesh_task": {"ambiguous_indirect"}}, "routed_mesh_task",
            ambiguous_site, (checker.IndirectCallBinding("ambiguous_indirect", "derived_target"),),
            {"indirect_calls": [{"from": "ambiguous_indirect", "site": 0,
                                 "to": "contract_target"}]})
    except checker.CheckFailure as error:
        if error.code != "stack":
            return 1
    else:
        return 1
    try:
        checker.apply_indirect_callsite_mappings(
            {"routed_mesh_task": {"unresolved_indirect"}}, "routed_mesh_task",
            unresolved_site,
            (checker.IndirectCallBinding("unresolved_indirect", "derived_target"),),
            {"indirect_calls": [{"from": "extra_indirect", "site": 0,
                                 "to": "extra_target"}]})
    except checker.CheckFailure as error:
        if error.code != "stack":
            return 1
    else:
        return 1
    production_bindings, production_calls = checker.production_indirect_callback_bindings(
        root / "app" / "tavrn_routed_node" / "src" / "main.c")
    if production_bindings.get("mind_root_coordinator_operations.generic_submit") != \
            "routed_mind_generic_submit":
        return 1
    if (checker.IndirectCallBinding("mind_root_coordinator_submit",
                                    "routed_mind_generic_submit") not in production_calls or
            checker.IndirectCallBinding("mind_root_coordinator_prepare",
                                        "routed_mind_generic_submit") in production_calls or
            sum(call == checker.IndirectCallBinding("mind_event_forwarder_consume_ingress",
                                                     "routed_mind_ingress_take")
                for call in production_calls) != 2 or
            checker.IndirectCallBinding("mind_event_forwarder_service_local",
                                        "routed_mind_final_publish_local") not in production_calls):
        return 1
    auxiliary = checker.StackReport("mind_uart_task", 512, (), 384, 128)
    try:
        checker.validate_mind_auxiliary_stack_threshold(auxiliary)
    except checker.CheckFailure:
        return 1
    try:
        checker.validate_mind_auxiliary_stack_threshold(
            checker.StackReport("mind_uart_task", 512, (), 385, 127))
    except checker.CheckFailure as error:
        if error.code != "stack_total":
            return 1
    else:
        return 1
    for total in (4097, 3841):
        try:
            checker.validate_mesh_stack_threshold(
                checker.StackReport("routed_mesh_task", 4864, (), total, 4864 - total))
        except checker.CheckFailure as error:
            if error.code != "stack_total":
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
            "#define TRON_BUILD_ROUTED_MESH_TASK_STACK_BYTES 4864u\n"
            "#define TRON_BUILD_RUNTIME_RAM_RESERVE_BYTES 13360u\n"
            "#define TRON_BUILD_ROUTED_LOGGER_TASK_STACK_BYTES 1840u\n",
            encoding="utf-8")
        manifest = {
            "build.kind": "ROUTED",
            "build.phase1_target": "ROUTED",
            "build.initial_task_stack_bytes": "4096",
            "capacity.routed_initial_task_stack_bytes": "4096",
            "capacity.routed_initial_task_stack_bytes.state": "IMPLEMENTED",
            "build.routed_mesh_task_stack_bytes": "4864",
            "capacity.routed_mesh_task_stack_bytes": "4864",
            "capacity.routed_mesh_task_stack_bytes.state": "IMPLEMENTED",
            "build.routed_logger_task_stack_bytes": "1840",
            "capacity.routed_logger_task_stack_bytes": "1840",
            "capacity.routed_logger_task_stack_bytes.state": "IMPLEMENTED",
            "resource.runtime_ram_reserve_bytes": "13360",
        }

        def expect_failure(candidate, code):
            try:
                checker.validate_routed_runtime_declarations(candidate, config)
            except checker.CheckFailure as error:
                return error.code == code
            return False

        declaration = checker.validate_routed_runtime_declarations(manifest, config)
        if declaration.initial_task_stack_bytes != 4096 or \
                declaration.runtime_ram_reserve_bytes != 13360 or \
                declaration.logger_task_stack_bytes != 1840:
            return 1
        mismatched_config = temporary / "mismatched_tron_build_config.h"
        mismatched_config.write_text(
            "#define TRON_BUILD_INITIAL_TASK_STACK_BYTES 4096u\n"
            "#define TRON_BUILD_ROUTED_MESH_TASK_STACK_BYTES 4864u\n"
            "#define TRON_BUILD_RUNTIME_RAM_RESERVE_BYTES 0u\n"
            "#define TRON_BUILD_ROUTED_LOGGER_TASK_STACK_BYTES 1840u\n",
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
                ("capacity.routed_initial_task_stack_bytes", "2048"),
                ("build.routed_logger_task_stack_bytes", "1024"),
                ("capacity.routed_logger_task_stack_bytes", "1024")):
            candidate = dict(manifest)
            if value is None:
                candidate.pop(key)
            else:
                candidate[key] = value
            if not expect_failure(candidate, "provenance"):
                return 1
        if checker.post_reserve_ram_bytes(21552, 13360) != 8192:
            return 1
        for unallocated, reserve in ((21551, 13360), (13359, 13360)):
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
        logger_mismatch = temporary / "mismatched_logger_tron_build_config.h"
        logger_mismatch.write_text(
            "#define TRON_BUILD_INITIAL_TASK_STACK_BYTES 4096u\n"
            "#define TRON_BUILD_ROUTED_MESH_TASK_STACK_BYTES 4864u\n"
            "#define TRON_BUILD_RUNTIME_RAM_RESERVE_BYTES 13360u\n"
            "#define TRON_BUILD_ROUTED_LOGGER_TASK_STACK_BYTES 1024u\n",
            encoding="utf-8")
        try:
            checker.validate_routed_runtime_declarations(manifest, logger_mismatch)
        except checker.CheckFailure as error:
            if error.code != "provenance":
                return 1
        else:
            return 1
        try:
            checker.require_benchmark_logger_evidence(
                {"bench.mode": "ON"},
                SimpleNamespace(logger_stack_root=None,
                                logger_required_edge_manifest=None))
        except checker.CheckFailure as error:
            if error.code != "stack":
                return 1
        else:
            return 1
        logger_frame = checker.StackFrame("fixture:routed_logger_task",
                                          "routed_logger_task", 1)
        logger_report = checker.StackReport("routed_logger_task", 1840,
                                            (logger_frame,), 513, 1023)
        try:
            checker.validate_logger_stack_threshold(logger_report)
        except checker.CheckFailure as error:
            if error.code != "stack_total":
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
    if fixture_path.name == "stack-max-bytes.json":
        # The checker may run from this monorepo or from the clean-publisher
        # fixture where microbit itself is the Git worktree root.
        if checker.git_toplevel() != root.parent.resolve():
            return 1
        with tempfile.TemporaryDirectory() as directory:
            standalone = pathlib.Path(directory) / "microbit"
            scripts = standalone / "scripts"
            scripts.mkdir(parents=True)
            shutil.copy2(root / "scripts" / "check_tavrn_expiry_resources.py",
                         scripts / "check_tavrn_expiry_resources.py")
            for command in (
                    ["git", "init", "--quiet", str(standalone)],
                    ["git", "-C", str(standalone), "add", "scripts"],
                    ["git", "-C", str(standalone), "-c", "user.name=fixture",
                     "-c", "user.email=fixture@example.invalid", "commit", "--quiet", "-m", "fixture"]):
                if subprocess.run(command, check=False).returncode != 0:
                    return 1
            standalone_spec = importlib.util.spec_from_file_location(
                "standalone_expiry_checker", scripts / "check_tavrn_expiry_resources.py")
            if standalone_spec is None or standalone_spec.loader is None:
                return 1
            standalone_checker = importlib.util.module_from_spec(standalone_spec)
            sys.modules[standalone_spec.name] = standalone_checker
            standalone_spec.loader.exec_module(standalone_checker)
            if standalone_checker.git_toplevel() != standalone.resolve():
                return 1
            clean_digest = standalone_checker.worktree_content_digest()
            (standalone / "relevant_untracked_test.py").write_text("x = 1\n", encoding="utf-8")
            if standalone_checker.worktree_content_digest() == clean_digest:
                return 1

    # D2 paired acceptance must prove every published measurement input, not
    # merely the reports which mention it.  Keep this synthetic fixture local so
    # each adversarial mutation is isolated and does not require an ARM build.
    if fixture_path.name == "stack-max-bytes.json" and \
            os.environ.get("TRON_RUN_LEGACY_D2_SYNTHETIC") == "1":
        def write_kv(path: pathlib.Path, values: dict[str, str]) -> None:
            path.write_text("".join("%s=%s\n" % item for item in sorted(values.items())),
                            encoding="utf-8")

        def expect_d2_failure(action) -> bool:
            with tempfile.TemporaryDirectory() as directory:
                pair = make_d2_pair(pathlib.Path(directory))
                action(pair)
                try:
                    checker.verify_d2_paired_acceptance(
                        pair["off"]["manifest"], pair["seal"], pair["on"]["manifest"],
                        pathlib.Path(directory) / "rejected.env")
                except checker.CheckFailure:
                    return True
                return False

        def make_d2_pair(temporary: pathlib.Path):
            common_sources = [
                "app/protocol/tavrn_full.c",
                "app/tavrn_routed_node/src/main.c",
                "libs/mtkernel_3/include/sys/inittask.h",
            ]
            additions = sorted(checker.D2_APPLICATION_SOURCE_ADDITIONS)
            generated_sources = ["generated/tron_build_info.c", "generated/tron_timer_config.c"]
            preprocessed_text = (
                "routed_cycle_operations.router_scheduler_event = "
                "routed_cycle_router_scheduler_event;\n"
                "routed_cycle_operations.router_tick = routed_cycle_router_tick;\n"
                "void routed_cycle_router_tick(void) {\n"
                "if (tavrn_full_application_mailbox_owner_take()) {\n"
                "tavrn_full_maintenance_binding_tick(&routed_binding_result_storage);\n"
                "if (application_present && application_result) { "
                "tavrn_full_application_mailbox_owner_publish(); }\n}\n"
                "tavrn_router_phase_trace_t *trace = "
                "&routed_binding_result_storage.router_trace;\nreturn *trace;\n}\n")

            def build_artifact(name: str, ingress: bool):
                directory = temporary / name
                directory.mkdir()
                stem = "synthetic-%s" % name
                files = {
                    "elf": directory / (stem + ".elf"),
                    "map": directory / (stem + ".map"),
                    "disassembly": directory / (stem + ".disassembly.txt"),
                    "compile_commands": directory / (stem + ".compile_commands.json"),
                    "ninja_commands": directory / (stem + ".ninja-commands.txt"),
                    "config_header": directory / (stem + ".build-config.h"),
                    "config_manifest": directory / (stem + ".build-config.manifest"),
                    "sources": directory / (stem + ".selected-sources.txt"),
                    "source_hashes": directory / (stem + ".selected-source-hashes.txt"),
                    "complete_sources": directory / (stem + ".complete-selected-sources.txt"),
                    "complete_source_hashes": directory / (stem + ".complete-selected-source-hashes.txt"),
                    "su_index": directory / (stem + ".stack-usage.index"),
                    "resource": directory / (stem + ".resources.json"),
                    "gate": directory / (stem + ".resource-gate.txt"),
                    "preprocessed": directory / (stem + ".main.i"),
                    "contract": directory / (stem + ".contract.json"),
                    "resource_checker": directory / (stem + ".check.py"),
                }
                checked_sources = sorted(common_sources + (additions if ingress else []))
                complete_sources = sorted(checked_sources + generated_sources)
                generated_directory = directory / (stem + ".generated-sources")
                generated_directory.mkdir()
                generated_contents = {
                    "generated/tron_build_info.c": b"const char generated_info[] = \"same\";\n",
                    "generated/tron_timer_config.c": b"const unsigned generated_timer = 1u;\n",
                }
                for source, contents in generated_contents.items():
                    snapshot = generated_directory / source.removeprefix("generated/")
                    snapshot.parent.mkdir(parents=True, exist_ok=True)
                    snapshot.write_bytes(contents)
                files["elf"].write_bytes(b"\x7fELFsynthetic\n")
                files["map"].write_text(
                    "RAM 0x20000000 0x00010000\n.data 0x20000000 0x00000010\n"
                    ".bss 0x20000010 0x00000010\n", encoding="utf-8")
                files["disassembly"].write_text("00000000 <synthetic>:\n", encoding="utf-8")
                files["compile_commands"].write_text(json.dumps([{
                    "directory": str(root), "file": str(root / common_sources[0]),
                    "arguments": ["cc", "-c", common_sources[0]],
                }]), encoding="utf-8")
                files["ninja_commands"].write_text("cc synthetic\n", encoding="utf-8")
                files["config_header"].write_text(
                    "#define TRON_BUILD_ENABLE_WEARABLE_INGRESS %d\n" % int(ingress),
                    encoding="utf-8")
                files["sources"].write_text("".join(source + "\n" for source in checked_sources),
                                            encoding="utf-8")
                checked_hashes = {
                    source: checker.sha256_file(root / source) for source in checked_sources
                }
                complete_hashes = dict(checked_hashes)
                complete_hashes.update({
                    source: checker.sha256_file(
                        generated_directory / source.removeprefix("generated/"))
                    for source in generated_sources
                })

                def write_hashes(path: pathlib.Path, hashes: dict[str, str]) -> None:
                    path.write_text("".join("%s  %s\n" % (hashes[source], source)
                                                for source in sorted(hashes)), encoding="utf-8")

                write_hashes(files["source_hashes"], checked_hashes)
                files["complete_sources"].write_text(
                    "".join(source + "\n" for source in complete_sources), encoding="utf-8")
                write_hashes(files["complete_source_hashes"], complete_hashes)
                su_directory = directory / (stem + ".stack-usage")
                su_directory.mkdir()
                su_file = su_directory / "synthetic.su"
                su_file.write_text("synthetic.c:1:1:synthetic\t1\tstatic\n", encoding="utf-8")
                su_index = "%s  synthetic.su\n" % checker.sha256_file(su_file)
                files["su_index"].write_text(su_index, encoding="utf-8")
                files["gate"].write_text("PASS synthetic resource gate\n", encoding="utf-8")
                files["preprocessed"].write_text(preprocessed_text, encoding="utf-8")
                files["contract"].write_text(json.dumps({
                    "schema": checker.INDIRECT_EDGE_SCHEMA, "root": "routed_mesh_task",
                    "edges": [{"from": "routed_mesh_task", "to": "synthetic",
                               "kind": "call", "when": "always"}],
                    "indirect_calls": [], "declared_leaves": [],
                }), encoding="utf-8")
                files["resource_checker"].write_bytes(
                    (root / "scripts" / "check_tavrn_expiry_resources.py").read_bytes())
                config = {
                    "build.implemented_capabilities": "base" +
                    (",mind-root-plane" if ingress else ""),
                    "application.wearable_ingress.requested": "ON" if ingress else "OFF",
                    "application.wearable_ingress.effective": "ON" if ingress else "OFF",
                    "application.root_plane.effective": "ON" if ingress else "OFF",
                }

                def set_config_sources() -> None:
                    for key in list(config):
                        if key.startswith("source.selected."):
                            config.pop(key)
                    for index, source in enumerate(complete_sources):
                        config["source.selected.%d" % index] = source
                    config["source.selected.sha256"] = checker.hashlib.sha256(
                        "\n".join(sorted(complete_sources)).encode("utf-8")).hexdigest()
                    write_kv(files["config_manifest"], config)

                set_config_sources()
                resource_document = {
                    "schema": checker.SCHEMA, "name": stem, "target": "tavrn_routed_node",
                    "feature": "FULL_TAVRN", "timer": "BALANCED",
                    "ram": {"data_bytes": 16, "bss_bytes": 16, "unallocated_bytes": 65536},
                    "fixed_state": {"before_bytes": 32, "after_bytes": 32,
                                    "declared_delta_bytes": 0, "unexplained_delta_bytes": 0},
                    "stack": {"root": "routed_mesh_task", "chain": [], "total_bytes": 0,
                              "headroom_bytes": 4864},
                    "heap": {"uses_heap": False},
                    "binding": {"full_symbol_count": 1, "full_call_count": 1,
                                "aodv_symbol_count": 0, "aodv_reference_count": 0},
                    "capture": {"supplied": False, "duration_seconds": 0, "complete_passes": 0,
                                "hard_selected_demand_deferred_passes": 0,
                                "max_scheduler_gap_ms": 0, "unavailable_count": 0, "faults": 0,
                                "targeted_controls": 0, "rreq_controls": 0,
                                "tc_expiry_controls": 0},
                    "provenance": {}, "expected_failure": None,
                    "preprocessed_main": {"source": ""},
                }
                resource_document["provenance"] = {
                    "source_inventory_sha256": checker.inventory_hash(files["sources"]),
                    "map_sha256": checker.sha256_file(files["map"]),
                    "elf_sha256": checker.sha256_file(files["elf"]),
                    "compile_commands_sha256": checker.sha256_file(files["compile_commands"]),
                    "checker_sha256": checker.sha256_file(root / "scripts" /
                                                            "check_tavrn_expiry_resources.py"),
                    "build_script_sha256": checker.sha256_file(root / "build-tavrn-ble.sh"),
                }
                files["resource"].write_text(json.dumps(resource_document, sort_keys=True),
                                               encoding="utf-8")
                if ingress:
                    files["application_size_source"] = directory / (stem + ".size-probe.c")
                    files["application_size"] = directory / (stem + ".sizes.json")
                    files["application_size_source"].write_text("int synthetic_probe;\n",
                                                                  encoding="utf-8")
                    components = {name: 1 for name in
                                  checker.MIND_APPLICATION_PRODUCTION_COMPONENTS |
                                  checker.MIND_APPLICATION_STATIC_STACK_COMPONENTS}
                    files["application_size"].write_text(json.dumps({
                        "schema": checker.APPLICATION_SIZE_SCHEMA,
                        "capacities": dict(checker.MIND_APPLICATION_CAPACITIES),
                        "sizeof": {"ingress": 1},
                        "production": {"components": components,
                                       "aggregate_new_static_bytes": sum(components.values())},
                        "provenance": {
                            "probe_source_sha256": checker.sha256_file(files["application_size_source"]),
                            "compile_commands_sha256": checker.sha256_file(files["compile_commands"]),
                            "elf_sha256": checker.sha256_file(files["elf"]),
                            "map_sha256": checker.sha256_file(files["map"]),
                            "config_header_sha256": checker.sha256_file(files["config_header"]),
                        },
                    }, sort_keys=True), encoding="utf-8")
                values = {
                    "artifact.name": stem,
                    "feature.level.effective": "FULL_TAVRN",
                    "feature.repair.effective": "ON",
                    "build.timer_profile": "BALANCED", "application.node_number": "6",
                    "application.wearable_ingress.requested": "ON" if ingress else "OFF",
                    "application.wearable_ingress.effective": "ON" if ingress else "OFF",
                    "hook.enabled": "OFF", "bench.mode": "OFF",
                    "build.routed_mesh_task_stack_bytes": "4864",
                    "resource.runtime_ram_reserve_bytes": "13360", "resource.gate": "PASSED",
                    "resource.stack_usage": "ON", "resource.su.directory.name": su_directory.name,
                    "resource.su_glob": su_directory.name + "/**/*.su",
                    "resource.su.sha256": checker.sha256_file(files["su_index"]),
                    "resource.contract.count": "1",
                    "source.inventory.sha256": checker.inventory_hash(files["sources"]),
                    "source.commit": "0" * 40, "source.tree": "1" * 40, "source.dirty": "yes",
                    "source.submodule.sha256": "2" * 64,
                    "source.worktree_content.scope": checker.WORKTREE_CONTENT_DIGEST_SCOPE,
                    "source.worktree_content.sha256": checker.worktree_content_digest(),
                    "build.compiler.sha256": "3" * 64, "build.cmake.sha256": "4" * 64,
                    "build.ninja.sha256": "5" * 64,
                    "build.script.path": "build-tavrn-ble.sh",
                    "build.script.sha256": checker.sha256_file(root / "build-tavrn-ble.sh"),
                    "evidence.generated_sources.directory.name": generated_directory.name,
                    "evidence.resource_checker.path": "scripts/check_tavrn_expiry_resources.py",
                }
                keys = {
                    "artifact.elf": "elf", "artifact.map": "map",
                    "evidence.ninja_commands": "ninja_commands",
                    "evidence.compile_commands": "compile_commands",
                    "evidence.disassembly": "disassembly",
                    "evidence.target_config_header": "config_header",
                    "evidence.target_config_manifest": "config_manifest",
                    "evidence.selected_sources": "sources",
                    "evidence.selected_source_hashes": "source_hashes",
                    "evidence.complete_selected_sources": "complete_sources",
                    "evidence.complete_selected_source_hashes": "complete_source_hashes",
                    "resource.su.index": "su_index", "resource.manifest": "resource",
                    "resource.gate_report": "gate", "evidence.preprocessed_main": "preprocessed",
                    "resource.contract.0": "contract", "evidence.resource_checker": "resource_checker",
                }
                if ingress:
                    keys["resource.application_size"] = "application_size"
                    keys["resource.application_size_probe"] = "application_size_source"

                def refresh_manifest() -> None:
                    for key, file_key in keys.items():
                        sidecar = files[file_key]
                        values[key + ".name"] = sidecar.name
                        values[key + ".sha256"] = checker.sha256_file(sidecar)
                        values[key + ".size"] = str(sidecar.stat().st_size)
                    write_kv(directory / (stem + ".manifest"), values)

                manifest = directory / (stem + ".manifest")
                refresh_manifest()
                return {"directory": directory, "files": files, "values": values,
                        "keys": keys, "manifest": manifest, "config": config,
                        "resource_document": resource_document, "checked_sources": checked_sources,
                        "complete_sources": complete_sources, "checked_hashes": checked_hashes,
                        "complete_hashes": complete_hashes, "generated_directory": generated_directory,
                        "su_file": su_file, "refresh": refresh_manifest,
                        "set_config_sources": set_config_sources, "write_hashes": write_hashes}

            off = build_artifact("off", False)
            on = build_artifact("on", True)
            seal = temporary / "off.manifest.sha256"
            seal.write_text(checker.sha256_file(off["manifest"]) + "  " + off["manifest"].name + "\n",
                            encoding="utf-8")
            return {"off": off, "on": on, "seal": seal}

        with tempfile.TemporaryDirectory() as directory:
            pair = make_d2_pair(pathlib.Path(directory))
            checker.verify_d2_paired_acceptance(pair["off"]["manifest"], pair["seal"],
                                                pair["on"]["manifest"],
                                                pathlib.Path(directory) / "passing.env")
        for file_key in ("elf", "map", "ninja_commands", "disassembly", "compile_commands",
                         "config_header", "config_manifest", "sources", "source_hashes",
                         "complete_sources", "complete_source_hashes", "su_index", "resource",
                         "gate", "preprocessed", "contract", "resource_checker",
                         "application_size", "application_size_source"):
            if not expect_d2_failure(lambda pair, key=file_key:
                                     pair["on"]["files"][key].write_bytes(b"tampered\n")):
                return 1
        for file_key in ("elf", "map", "ninja_commands", "disassembly", "compile_commands",
                         "config_header", "config_manifest", "sources", "source_hashes",
                         "complete_sources", "complete_source_hashes", "su_index", "resource",
                         "gate", "preprocessed", "contract", "resource_checker",
                         "application_size", "application_size_source"):
            if not expect_d2_failure(lambda pair, key=file_key: pair["on"]["files"][key].unlink()):
                return 1
        if not expect_d2_failure(lambda pair: pair["on"]["su_file"].write_text("tampered\n",
                                                                                   encoding="utf-8")):
            return 1
        if not expect_d2_failure(lambda pair: pair["on"]["su_file"].unlink()):
            return 1

        def add_hidden_generated(pair) -> None:
            on = pair["on"]
            hidden = "generated/hidden.c"
            (on["generated_directory"] / "hidden.c").write_text("int hidden;\n", encoding="utf-8")
            on["complete_sources"].append(hidden)
            on["complete_sources"].sort()
            on["complete_hashes"][hidden] = checker.sha256_file(on["generated_directory"] / "hidden.c")
            on["files"]["complete_sources"].write_text(
                "".join(source + "\n" for source in on["complete_sources"]), encoding="utf-8")
            on["write_hashes"](on["files"]["complete_source_hashes"], on["complete_hashes"])
            on["set_config_sources"]()
            on["refresh"]()

        if not expect_d2_failure(add_hidden_generated):
            return 1

        def remove_source(pair) -> None:
            on = pair["on"]
            removed = sorted(checker.D2_APPLICATION_SOURCE_ADDITIONS)[0]
            on["checked_sources"].remove(removed)
            on["complete_sources"].remove(removed)
            on["checked_hashes"].pop(removed)
            on["complete_hashes"].pop(removed)
            on["files"]["sources"].write_text(
                "".join(source + "\n" for source in on["checked_sources"]), encoding="utf-8")
            on["files"]["complete_sources"].write_text(
                "".join(source + "\n" for source in on["complete_sources"]), encoding="utf-8")
            on["write_hashes"](on["files"]["source_hashes"], on["checked_hashes"])
            on["write_hashes"](on["files"]["complete_source_hashes"], on["complete_hashes"])
            on["set_config_sources"]()
            on["resource_document"]["provenance"]["source_inventory_sha256"] = \
                checker.inventory_hash(on["files"]["sources"])
            on["files"]["resource"].write_text(json.dumps(on["resource_document"], sort_keys=True),
                                                   encoding="utf-8")
            on["values"]["source.inventory.sha256"] = checker.inventory_hash(on["files"]["sources"])
            on["refresh"]()

        if not expect_d2_failure(remove_source):
            return 1

        def mutate_common_hash(pair) -> None:
            on = pair["on"]
            source = "app/protocol/tavrn_full.c"
            on["checked_hashes"][source] = "f" * 64
            on["complete_hashes"][source] = "f" * 64
            on["write_hashes"](on["files"]["source_hashes"], on["checked_hashes"])
            on["write_hashes"](on["files"]["complete_source_hashes"], on["complete_hashes"])
            on["refresh"]()

        if not expect_d2_failure(mutate_common_hash):
            return 1

        def mutate_generated_source(pair) -> None:
            on = pair["on"]
            source = "generated/tron_build_info.c"
            snapshot = on["generated_directory"] / source.removeprefix("generated/")
            snapshot.write_text("int changed_generated;\n", encoding="utf-8")
            on["complete_hashes"][source] = checker.sha256_file(snapshot)
            on["write_hashes"](on["files"]["complete_source_hashes"], on["complete_hashes"])
            on["refresh"]()

        if not expect_d2_failure(mutate_generated_source):
            return 1
        if not expect_d2_failure(lambda pair: pair["on"]["values"].update(
            {"build.script.sha256": "0" * 64}) or write_kv(
                pair["on"]["manifest"], pair["on"]["values"])):
            return 1
        if not expect_d2_failure(lambda pair: pair["on"]["values"].update(
            {"evidence.resource_checker.sha256": "0" * 64}) or write_kv(
                pair["on"]["manifest"], pair["on"]["values"])):
            return 1
        if not expect_d2_failure(lambda pair: pair["on"]["values"].update(
            {"source.worktree_content.sha256": "0" * 64}) or write_kv(
                pair["on"]["manifest"], pair["on"]["values"])):
            return 1
        if not expect_d2_failure(lambda pair: pair["seal"].write_text(
            "0" * 64 + "  " + pair["off"]["manifest"].name + "\n", encoding="utf-8")):
            return 1
        if not expect_d2_failure(lambda pair: pair["on"]["values"].update(
            {"evidence.disassembly.name": "../escape"}) or write_kv(
                pair["on"]["manifest"], pair["on"]["values"])):
            return 1

        def duplicate_sidecar(pair) -> None:
            on = pair["on"]
            config = on["files"]["config_header"]
            on["values"]["evidence.disassembly.name"] = config.name
            on["values"]["evidence.disassembly.sha256"] = checker.sha256_file(config)
            on["values"]["evidence.disassembly.size"] = str(config.stat().st_size)
            write_kv(on["manifest"], on["values"])

        if not expect_d2_failure(duplicate_sidecar):
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
