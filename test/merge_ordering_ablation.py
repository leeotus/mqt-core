#!/usr/bin/env python3
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
"""Merge the per-threshold ablation tables into one table."""

from __future__ import annotations

import csv
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) < 4:
        print("usage: merge_ordering_ablation.py <work-dir> <output> <threshold>...")
        return 2
    work = Path(sys.argv[1])
    output = Path(sys.argv[2])
    thresholds = sys.argv[3:]

    tables: dict[str, dict[str, dict[str, str]]] = {}
    for threshold in thresholds:
        path = work / f"threshold_{threshold}.csv"
        rows: dict[str, dict[str, str]] = {}
        with path.open() as handle:
            for row in csv.DictReader(handle):
                rows[row["circuit"]] = row
        tables[threshold] = rows

    # Keep the order of the first table.
    circuits = list(tables[thresholds[0]].keys())

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
    for threshold in thresholds:
        header += [
            f"dynamic{threshold}_nodes",
            f"dynamic{threshold}_peak",
            f"dynamic{threshold}_time_ms",
            f"dynamic{threshold}_reorderings",
            f"dynamic{threshold}_exchanges",
        ]
    header += ["best_static_nodes", "best_dynamic_nodes", "dynamic_gain_vs_static"]

    with output.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(header)
        for circuit in circuits:
            first = tables[thresholds[0]][circuit]
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
            dynamicSizes = []
            for threshold in thresholds:
                entry = tables[threshold].get(circuit, {})
                nodes = entry.get("dynamic_construction_nodes", "")
                row += [
                    nodes,
                    entry.get("dynamic_construction_peak_size", ""),
                    entry.get("dynamic_construction_time_ms", ""),
                    entry.get("dynamic_construction_reorderings", ""),
                    entry.get("dynamic_construction_exchanges", ""),
                ]
                if nodes not in ("", None):
                    dynamicSizes.append(int(nodes))
            staticBest = first.get("static_best_nodes", "")
            row.append(staticBest)
            if dynamicSizes:
                best = min(dynamicSizes)
                row.append(best)
                row.append(
                    f"{int(staticBest) / best:.3f}" if staticBest not in ("", None) else ""
                )
            else:
                row += ["", ""]
            writer.writerow(row)
    print(f"wrote {output} with {len(circuits)} circuits")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
