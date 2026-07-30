#!/usr/bin/env python3
"""Render concise, screenshot-friendly BLE flood-mesh logs.

The BLE mesh has no routes to print - it is a controlled flood, so the only
on-air evidence of a multi-hop path is the hop count. A packet still at full
TTL came straight from its originator; anything below that was rebroadcast by
someone. This monitor tracks, per peer, which TTLs a node has actually heard,
so "never heard directly" is visible as an absence rather than inferred.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys

FIELD_RE = re.compile(r"([a-z_]+)=([^\s]+)")
RX_RE = re.compile(r"mesh rx dummy src=(0x[0-9a-f]+) seq=(\d+) ttl=(\d+)")

TTL_MAX = 3

COLORS = {
    "endpoint": "\033[1;35m",
    "relay": "\033[1;33m",
}
RESET = "\033[0m"
DIM = "\033[2m"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--role", choices=("endpoint", "relay"), required=True)
    parser.add_argument("--label", required=True, help="short pane label, e.g. NODE-A")
    parser.add_argument("--peer", help="far peer id this node should only reach via the relay")
    parser.add_argument("--device", required=True)
    parser.add_argument("--baud", default="115200")
    return parser.parse_args()


def number(fields: dict[str, str], key: str) -> int:
    try:
        return int(fields.get(key, "0"), 0)
    except ValueError:
        return 0


def delta_text(value: int, previous: int | None) -> str:
    if previous is None or value <= previous:
        return ""
    return f" (+{value - previous})"


def hop_verdict(seen: dict[int, int], peer: str | None) -> str:
    """Describe how this node is reaching `peer`, based on observed hop counts."""
    if peer is None:
        return ""
    if not seen:
        return f"from {peer}: nothing heard yet"

    direct = seen.get(TTL_MAX, 0)
    relayed = sum(count for ttl, count in seen.items() if ttl < TTL_MAX)
    spread = " ".join(f"ttl={t}x{seen[t]}" for t in sorted(seen, reverse=True))

    if direct == 0 and relayed > 0:
        return f"from {peer}: RELAYED ONLY [{spread}] direct=0"
    if direct > 0:
        return f"from {peer}: HEARD DIRECT [{spread}] <- not a 2-hop path"
    return f"from {peer}: [{spread}]"


def render(args: argparse.Namespace,
           fields: dict[str, str],
           seen: dict[int, int],
           previous: dict[str, int]) -> str:
    node_id = fields.get("id", "?")
    relayed = number(fields, "relay_scheduled")
    blocked = number(fields, "blocked_direct")
    dupes = number(fields, "duplicate_drop")
    change = delta_text(relayed, previous.get("relay_scheduled"))
    previous["relay_scheduled"] = relayed

    if args.role == "relay":
        detail = (
            f"hears BOTH endpoints direct (ttl={TTL_MAX}) | "
            f"FORWARDED={relayed}{change} | dup_suppressed={dupes}"
        )
    else:
        detail = (
            f"{hop_verdict(seen, args.peer)} | "
            f"direct_blocked={blocked} | FORWARDED={relayed}{change}"
        )

    colour = COLORS[args.role]
    return f"{colour}{args.label:<7} {node_id}{RESET} | {detail}"


def print_header(args: argparse.Namespace) -> None:
    colour = COLORS[args.role]
    if args.role == "relay":
        blurb = ("Normal node, blocks nobody. Hears both endpoints at full TTL, "
                 "so it is the only path between them.")
    else:
        blurb = (f"Normal node. Direct (full-TTL) frames from {args.peer} are dropped, "
                 f"so anything it hears from {args.peer} arrived through the relay.")
    print(f"{colour}=== {args.label} - BLE FLOOD MESH ==={RESET}")
    print(blurb)
    print(f"{DIM}TTL {TTL_MAX} = straight from originator. Lower = someone rebroadcast it.{RESET}")
    print(f"Serial: {args.device}")
    print("-" * 96, flush=True)


def main() -> int:
    args = parse_args()
    grabserial = shutil.which("grabserial")
    if grabserial is None:
        print("grabserial was not found in PATH", file=sys.stderr)
        return 1

    print_header(args)
    seen: dict[int, int] = {}
    previous: dict[str, int] = {}
    process = subprocess.Popen(
        [grabserial, "-d", args.device, "-b", args.baud, "-t"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    try:
        assert process.stdout is not None
        for line in process.stdout:
            rx = RX_RE.search(line)
            if rx is not None and args.peer is not None:
                src, _seq, ttl = rx.groups()
                if src == args.peer:
                    seen[int(ttl)] = seen.get(int(ttl), 0) + 1
                continue
            if "mesh counters" in line:
                fields = dict(FIELD_RE.findall(line))
                print(render(args, fields, seen, previous), flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
