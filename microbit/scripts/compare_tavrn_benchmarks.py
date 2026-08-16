#!/usr/bin/env python3
"""Best-effort comparison of two finalized TAVRN benchmark JSON bundles."""

from __future__ import annotations

import argparse
import collections
import csv
import json
from pathlib import Path
from typing import Any


def ratio(numerator: int | float, denominator: int | float) -> float | None:
    return numerator / denominator if denominator else None


def fmt(value: Any) -> str:
    if value is None:
        return "N/A"
    if isinstance(value, float):
        return f"{value:.3f}"
    return str(value)


def packet_metrics(data: dict[str, Any], workload: str | None = None) -> dict[str, Any]:
    rows = [row for row in data.get("app_packets", []) if row.get("offer") is not None]
    if workload is not None:
        rows = [row for row in rows if row.get("workload") == workload]
    offered = len(rows)
    attempted = sum(int(row.get("attempted") or 0) for row in rows)
    accepted = sum(int(row.get("accepted") or 0) for row in rows)
    delivered = sum(int(row.get("delivered") or 0) for row in rows)
    return {
        "offered": offered, "attempted": attempted, "accepted": accepted,
        "delivered": delivered, "offered_pdr": ratio(delivered, offered),
        "attempted_pdr": ratio(delivered, attempted),
        "accepted_pdr": ratio(delivered, accepted),
        "admission_ratio": ratio(accepted, attempted),
        "statuses": dict(collections.Counter(str(row.get("status")) for row in rows)),
    }


def control_totals(path: Path) -> dict[str, int]:
    totals: collections.Counter[str] = collections.Counter()
    if not path.is_file():
        return {}
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            try:
                totals[row["counter"]] += int(row["delta"])
            except (KeyError, TypeError, ValueError):
                continue
    totals["received_control_packets"] = sum(
        value for key, value in totals.items()
        if key.startswith("rx_control_proxy_") and
        not key.startswith("rx_control_proxy_bytes_"))
    totals["received_control_bytes"] = sum(
        value for key, value in totals.items()
        if key.startswith("rx_control_proxy_bytes_"))
    totals["aodv_dispatched_control_packets"] = sum(
        value for key, value in totals.items()
        if key.startswith("aodv_dispatch_enqueue_proxy_") and
        not key.startswith("aodv_dispatch_enqueue_proxy_bytes_"))
    totals["aodv_dispatched_control_bytes"] = sum(
        value for key, value in totals.items()
        if key.startswith("aodv_dispatch_enqueue_proxy_bytes_"))
    return dict(totals)


def summarize(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    invalid = collections.Counter(
        str(row.get("reason")) for row in data.get("invalid_intervals", []))
    fleet = [row for row in data.get("gtt_fleet", []) if row.get("status") == "OK"]
    gtt = {
        "windows": len(fleet),
        "converged_windows": sum(int(row.get("converged") or 0) for row in fleet),
        "mean_recall": (sum(float(row.get("recall") or 0) for row in fleet) / len(fleet)
                        if fleet else None),
        "mean_precision": (sum(float(row.get("precision") or 0) for row in fleet) / len(fleet)
                           if fleet else None),
        "mean_pairwise_agreement": (
            sum(float(row.get("pairwise_agreement") or 0) for row in fleet) / len(fleet)
            if fleet else None),
        "last": fleet[-1] if fleet else None,
    }
    return {
        "source": str(path), "profile": data.get("metadata", {}).get("profile"),
        "proving_status": data.get("proving_status"),
        "record_count": data.get("record_count"),
        "telemetry_corruption": data.get("telemetry_corruption"),
        "incomplete_reasons": data.get("incomplete_reasons", []),
        "invalid_reasons": dict(invalid),
        "application": packet_metrics(data),
        "heartbeat": packet_metrics(data, "heartbeat"),
        "throughput": packet_metrics(data, "throughput"),
        "bursts": data.get("throughput_bursts", []),
        "gtt": gtt,
        "control": control_totals(path.parent / "control_deltas.csv"),
    }


def markdown(left: dict[str, Any], right: dict[str, Any], left_name: str,
             right_name: str) -> str:
    lines = ["# TAVRN benchmark comparison", "",
             f"- {left_name}: `{left['proving_status']}`; corruption={left['telemetry_corruption']}",
             f"- {right_name}: `{right['proving_status']}`; corruption={right['telemetry_corruption']}", ""]
    for section in ("application", "heartbeat", "throughput"):
        lines += [f"## {section.title()}", "",
                  f"| Metric | {left_name} | {right_name} | {right_name}/{left_name} |",
                  "|---|---:|---:|---:|"]
        for key in ("offered", "attempted", "accepted", "delivered", "offered_pdr",
                    "attempted_pdr", "accepted_pdr", "admission_ratio"):
            lv, rv = left[section].get(key), right[section].get(key)
            lines.append(f"| {key} | {fmt(lv)} | {fmt(rv)} | {fmt(ratio(rv, lv) if lv is not None and rv is not None else None)} |")
        lines.append("")
    lines += ["## Throughput bursts", "",
              f"| Burst | {left_name} delivered/goodput/p50/p95/p99 | {right_name} delivered/goodput/p50/p95/p99 |",
              "|---|---|---|"]
    left_bursts = {str(row.get("label")): row for row in left["bursts"]}
    right_bursts = {str(row.get("label")): row for row in right["bursts"]}
    for label in sorted(set(left_bursts) | set(right_bursts)):
        values = []
        for row in (left_bursts.get(label, {}), right_bursts.get(label, {})):
            values.append(" / ".join(fmt(row.get(key)) for key in
                                     ("delivered", "packet_goodput_packets_per_second",
                                      "p50_ms", "p95_ms", "p99_ms")))
        lines.append(f"| {label} | {values[0]} | {values[1]} |")
    lines += ["", "## Control/transmission proxies", "",
              f"| Proxy | {left_name} | {right_name} | {right_name}/{left_name} |",
              "|---|---:|---:|---:|"]
    proxies = ("scheduler_tx_done_proxy", "link_tx_done_proxy", "received_control_packets",
               "received_control_bytes", "aodv_dispatched_control_packets",
               "aodv_dispatched_control_bytes", "retry_due", "retry_exhausted",
               "aodv_action_backpressure")
    for key in proxies:
        lv, rv = left["control"].get(key), right["control"].get(key)
        lines.append(f"| {key} | {fmt(lv)} | {fmt(rv)} | {fmt(ratio(rv, lv) if lv is not None and rv is not None else None)} |")
    lines += ["", "## FULL GTT", "",
              f"- Windows: {fmt(right['gtt']['windows'])}",
              f"- Converged windows: {fmt(right['gtt']['converged_windows'])}",
              f"- Mean recall: {fmt(right['gtt']['mean_recall'])}",
              f"- Mean precision: {fmt(right['gtt']['mean_precision'])}",
              f"- Mean pairwise agreement: {fmt(right['gtt']['mean_pairwise_agreement'])}", "",
              "## Evidence caveats", "",
              f"- {left_name} invalid reasons: `{left['invalid_reasons']}`",
              f"- {right_name} invalid reasons: `{right['invalid_reasons']}`",
              f"- {left_name} incomplete reasons: `{left['incomplete_reasons']}`",
              f"- {right_name} incomplete reasons: `{right['incomplete_reasons']}`", ""]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--left", required=True, type=Path)
    parser.add_argument("--right", required=True, type=Path)
    parser.add_argument("--left-name", default="AODV")
    parser.add_argument("--right-name", default="FULL_TAVRN")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    left, right = summarize(args.left), summarize(args.right)
    output = {"left_name": args.left_name, "right_name": args.right_name,
              "left": left, "right": right}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "comparison.json").write_text(
        json.dumps(output, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (args.output / "comparison.md").write_text(
        markdown(left, right, args.left_name, args.right_name), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
