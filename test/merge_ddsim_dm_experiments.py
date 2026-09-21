# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
"""Merge the per-run DDSIM density matrix results into one table."""

from __future__ import annotations

import csv
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

ORDERS = ["none", "gate_dependency", "degree", "spectral", "recursive_bisection"]
CANDIDATES = ORDERS[1:]
FAILURES = {"timeout", "out-of-memory", "error"}

COLUMNS = [
    "circuit",
    "rep",
    "order",
    "status",
    "qubits",
    "gates",
    "nodes",
    "simulation_time_ms",
]


def readRun(path: Path) -> dict[str, str]:
    text = path.read_text(encoding="utf-8").strip()
    if not text:
        return {}
    if not text.startswith("{"):
        return {"status": text if text in FAILURES else "error"}
    stats = json.loads(text)["statistics"]
    return {
        "status": "ok",
        "qubits": str(stats.get("n_qubits", "")),
        "gates": str(stats.get("applied_gates", "")),
        "nodes": str(stats.get("active_nodes", "")),
        "simulation_time_ms": f"{float(stats.get('simulation_time', 0.0)) * 1000.0:.3f}",
    }


def collect(work: Path) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for path in (work / "json").glob("*.json"):
        parts = path.stem.split("__")
        if len(parts) != 3:
            continue
        circuit, order, rep = parts[0], parts[1], parts[2].removeprefix("rep")
        record = readRun(path)
        if not record:
            continue
        record.update({"circuit": circuit, "order": order, "rep": rep})
        rows.append(record)
    return rows


def number(record: dict[str, str], key: str) -> float | None:
    try:
        return float(record.get(key, ""))
    except (TypeError, ValueError):
        return None


def orderKey(record: dict[str, str]) -> tuple:
    name = record["order"]
    return (
        record["circuit"],
        ORDERS.index(name) if name in ORDERS else len(ORDERS),
        int(record["rep"] or 0),
    )


def medianOf(records: list[dict[str, str]], key: str) -> float | None:
    values = [v for r in records if (v := number(r, key)) is not None]
    return statistics.median(values) if values else None


def fmt(value: float | None, digits: int = 3) -> str:
    return "-" if value is None else f"{value:.{digits}f}"


def main() -> int:
    work = Path(sys.argv[1])
    results = Path(sys.argv[2])
    results.mkdir(parents=True, exist_ok=True)

    rows = sorted(collect(work), key=orderKey)
    detail = results / "ddsim_density_matrix.csv"
    with detail.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=COLUMNS, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow({c: row.get(c, "") for c in COLUMNS})

    grouped: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        grouped[row["circuit"]].append(row)

    table: list[dict[str, str]] = []
    for circuit, records in sorted(grouped.items()):
        entry: dict[str, str] = {"circuit": circuit}
        entry["qubits"] = next(
            (r["qubits"] for r in records if r.get("qubits")), ""
        )
        entry["gates"] = next((r["gates"] for r in records if r.get("gates")), "")
        base = [r for r in records if r["order"] == "none" and r["status"] == "ok"]
        baseTime = medianOf(base, "simulation_time_ms")
        entry["none_ms"] = fmt(baseTime, 1)
        entry["none_nodes"] = fmt(medianOf(base, "nodes"), 0)
        bestOrder = ""
        bestTime = None
        worstRatio = None
        for order in CANDIDATES:
            good = [
                r for r in records if r["order"] == order and r["status"] == "ok"
            ]
            value = medianOf(good, "simulation_time_ms")
            entry[f"{order}_ms"] = fmt(value, 1)
            entry[f"{order}_nodes"] = fmt(medianOf(good, "nodes"), 0)
            ratio = value / baseTime if value and baseTime else None
            entry[f"{order}_ratio"] = fmt(ratio, 4)
            if ratio is not None:
                if bestTime is None or value < bestTime:
                    bestTime = value
                    bestOrder = order
                if worstRatio is None or ratio > worstRatio:
                    worstRatio = ratio
        entry["best_order"] = bestOrder
        entry["best_ms"] = fmt(bestTime, 1)
        entry["best_speedup"] = fmt(
            baseTime / bestTime if bestTime and baseTime else None, 3
        )
        table.append(entry)

    columns = ["circuit", "qubits", "gates", "none_ms", "none_nodes"]
    for order in CANDIDATES:
        columns += [f"{order}_ms", f"{order}_nodes", f"{order}_ratio"]
    columns += ["best_order", "best_ms", "best_speedup"]
    wide = results / "ddsim_density_matrix_by_order.csv"
    with wide.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        for entry in table:
            writer.writerow({c: entry.get(c, "") for c in columns})

    lines = ["# Static variable order in the DDSIM density matrix simulator", ""]
    lines.append(f"Runs: {len(rows)}")
    lines.append("")
    lines.append("## Speed-up of the simulation time against the natural order")
    lines.append("")
    lines.append("| order | improved circuits | median ratio | median speed-up |")
    lines.append("| --- | --- | --- | --- |")
    for order in CANDIDATES:
        ratios = [
            number(entry, f"{order}_ratio")
            for entry in table
            if number(entry, f"{order}_ratio") is not None
        ]
        if not ratios:
            continue
        median = statistics.median(ratios)
        lines.append(
            f"| {order} | {sum(1 for r in ratios if r < 1)}/{len(ratios)} | "
            f"{median:.4f} | {1 / median:.3f} |"
        )
    lines.append("")
    lines.append("## Per circuit")
    lines.append("")
    lines.append(
        "| circuit | qubits | natural (ms) | natural nodes | "
        + " | ".join(f"{o} (ms)" for o in CANDIDATES)
        + " | best |"
    )
    lines.append("|" + " --- |" * (len(CANDIDATES) + 5))
    for entry in sorted(
        table, key=lambda e: -(number(e, "none_ms") or 0.0)
    ):
        cells = " | ".join(entry.get(f"{o}_ms", "-") for o in CANDIDATES)
        lines.append(
            f"| {entry['circuit']} | {entry.get('qubits', '')} | "
            f"{entry.get('none_ms', '-')} | {entry.get('none_nodes', '-')} | "
            f"{cells} | {entry.get('best_order', '')} |"
        )
    lines.append("")
    (results / "ddsim_density_matrix_summary.md").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )
    print(f"merged {len(rows)} runs into {detail} and {wide}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
