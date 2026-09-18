#!/usr/bin/env python3
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
"""Merge the per-arm ordering tables into one table."""

from __future__ import annotations

import csv
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 4:
        print("usage: merge_ordering_arms.py <work-dir> <output> <arm>...")
        return 2
    work = Path(sys.argv[1])
    output = Path(sys.argv[2])
    arms = sys.argv[3:]

    tables: dict[str, dict[str, dict[str, str]]] = {}
    for arm in arms:
        start, threshold = arm.split(":")
        path = work / f"arm_{start}_{threshold}.csv"
        rows: dict[str, dict[str, str]] = {}
        with path.open() as handle:
            for row in csv.DictReader(handle):
                rows[row["circuit"]] = row
        tables[arm] = rows

    circuits = list(tables[arms[0]].keys())
    header = [
        "circuit",
        "qubits",
        "gates",
        "status",
        "original_nodes",
        "original_time_ms",
        "static_best_strategy",
        "static_best_nodes",
        "static_best_time_ms",
        "static_degree_nodes",
        "static_spectral_nodes",
        "static_recursive_bisection_nodes",
        "static_min_fill_nodes",
        "static_gate_dependency_nodes",
    ]
    for arm in arms:
        start, threshold = arm.split(":")
        prefix = f"{start}{threshold}"
        header += [
            f"{prefix}_nodes",
            f"{prefix}_peak",
            f"{prefix}_time_ms",
            f"{prefix}_reorder_time_ms",
            f"{prefix}_reorderings",
            f"{prefix}_exchanges",
        ]
    header += [
        "best_static_nodes",
        "best_dynamic_nodes",
        "best_dynamic_arm",
        "dynamic_gain_vs_static",
        "best_peak",
        "peak_shrink",
    ]

    with output.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(header)
        for circuit in circuits:
            first = tables[arms[0]][circuit]
            row = [
                Path(circuit).name,
                first.get("qubits", ""),
                first.get("gates", ""),
                first.get("status", ""),
                first.get("original_nodes", ""),
                first.get("original_time_ms", ""),
                first.get("static_best_strategy", ""),
                first.get("static_best_nodes", ""),
                first.get("static_best_time_ms", ""),
                first.get("static_degree_nodes", ""),
                first.get("static_spectral_nodes", ""),
                first.get("static_recursive_bisection_nodes", ""),
                first.get("static_min_fill_nodes", ""),
                first.get("static_gate_dependency_nodes", ""),
            ]
            candidates: list[tuple[int, str, int]] = []
            peaks: list[int] = []
            for arm in arms:
                entry = tables[arm].get(circuit, {})
                nodes = entry.get("dynamic_construction_nodes", "")
                peak = entry.get("dynamic_construction_peak_size", "")
                row += [
                    nodes,
                    peak,
                    entry.get("dynamic_construction_time_ms", ""),
                    entry.get("dynamic_construction_reorder_time_ms", ""),
                    entry.get("dynamic_construction_reorderings", ""),
                    entry.get("dynamic_construction_exchanges", ""),
                ]
                if nodes not in ("", None):
                    candidates.append((int(nodes), arm, int(peak or 0)))
                if peak not in ("", None):
                    peaks.append(int(peak))
            staticBest = first.get("static_best_nodes", "")
            original = first.get("original_nodes", "")
            row.append(staticBest)
            if candidates:
                best = min(candidates)
                row += [best[0], best[1]]
                row.append(
                    f"{int(staticBest) / best[0]:.3f}"
                    if staticBest not in ("", None)
                    else ""
                )
                row.append(min(peaks))
                row.append(
                    f"{int(original) / min(peaks):.1f}"
                    if original not in ("", None) and min(peaks) > 0
                    else ""
                )
            else:
                row += ["", "", "", "", ""]
            writer.writerow(row)
    print(f"wrote {output} with {len(circuits)} circuits")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
