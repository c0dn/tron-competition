#!/usr/bin/env python3
"""Keep production D2 declared-library evidence on one frozen toolchain."""

from __future__ import annotations

import json
import pathlib
import sys


COMPILER_SHA256 = "04d818b91fff08550e414c23704cc2344ae8568a896951e5772c722210c99395"
LIBRARIES = {
    "/usr/lib/gcc/arm-none-eabi/16.2.0/thumb/v7e-m+fp/hard/libgcc.a":
        "3885a8d6661a68ce1da5b6586a54b5beb796df13e6dfaa40c1708f9392620d87",
    "/usr/lib/gcc/arm-none-eabi/16.2.0/../../../../arm-none-eabi/lib/thumb/v7e-m+fp/hard/libg.a":
        "a09944e71b329a81e34948a47108f71fa185c3a225823284b1dd1976da852659",
}
EXPECTED_PRODUCTION_CONTRACTS = frozenset({
    "required-logger-stack-edges.json",
    "required-mind-display-stack-edges.json",
    "required-mind-logger-stack-edges.json",
    "required-mind-mesh-stack-edges.json",
    "required-mind-uart-stack-edges.json",
    "required-mind-ui-stack-edges.json",
    "required-repair-stack-edges.json",
    "required-stack-edges.json",
})


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1


def main() -> int:
    microbit_root = pathlib.Path(__file__).resolve().parents[2]
    fixtures = microbit_root / "tests/protocol/fixtures/tavrn_expiry_resources"
    # Only production required contracts participate. Synthetic unit fixtures,
    # including declared-leaf-pass.json with deliberate zero evidence, are not
    # D2 records and must remain outside this assertion.
    contracts = sorted((*fixtures.glob("required-*-stack-edges.json"),
                        fixtures / "required-stack-edges.json"))
    names = {path.name for path in contracts}
    if names != EXPECTED_PRODUCTION_CONTRACTS:
        return fail("production required-stack fixture set differs: %s" %
                    ",".join(sorted(names)))

    for path in contracts:
        try:
            contract = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            return fail("cannot read %s: %s" % (path, error))
        leaves = contract.get("declared_leaves")
        if not isinstance(leaves, list):
            return fail("%s lacks declared_leaves" % path.name)
        for index, leaf in enumerate(leaves):
            if not isinstance(leaf, dict) or not isinstance(leaf.get("evidence"), dict):
                return fail("%s declared leaf %d lacks evidence" % (path.name, index))
            evidence = leaf["evidence"]
            if evidence.get("kind") != "library_archive":
                return fail("%s declared leaf %d is not library evidence" % (path.name, index))
            archive_hash = LIBRARIES.get(evidence.get("path"))
            if archive_hash is None or evidence.get("sha256") != archive_hash or \
                    evidence.get("compiler_manifest_key") != "build.compiler.sha256" or \
                    evidence.get("compiler_sha256") != COMPILER_SHA256:
                return fail("%s declared leaf %d has unsupported D2 toolchain evidence" %
                            (path.name, index))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
