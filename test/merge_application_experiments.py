#!/usr/bin/env python3
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
"""Merge the application experiments into one table per application.

Every variant (original order and the static strategies) was measured in its
own process. The merge step joins the variants by circuit and takes the median
of the repeated measurements.
"""

from __future__ import annotations

import csv
import statistics as st
import sys
from pathlib import Path


def number(row: dict[str, str] | None, key: str) -> float | None:
    if row is None:
        return None
    value = row.get(key, "")
    if value in ("", None, "-"):
        return None
    try:
        return float(value)
    except ValueError:
        return None


def main() -> int:
    if len(sys.argv) < 5:
        print(
            "usage: merge_application_experiments.py <work> <results> <repeats> "
            "<variant>..."
        )
        return 2
    work = Path(sys.argv[1])
    results = Path(sys.argv[2])
    repeats = int(sys.argv[3])
    variants = sys.argv[4:]
    strategies = [variant for variant in variants if variant != "original"]

    for name, title in (("e1", "statevector"), ("e3", "unitary")):
        tables: dict[str, list[dict[str, dict[str, str]]]] = {}
        for variant in variants:
            tables[variant] = []
            for rep in range(1, repeats + 1):
                path = work / f"{name}_{variant}_rep{rep}.csv"
                with path.open() as handle:
                    tables[variant].append(
                        {row["circuit"]: row for row in csv.DictReader(handle)}
                    )
        circuits = list(tables["original"][0].keys())

        output = results / f"application_{name}_{title}.csv"
        with output.open("w", newline="") as handle:
            writer = csv.writer(handle)
            header = ["circuit", "qubits", "gates", "status",
                      "original_nodes", "original_time_ms"]
            for strategy in strategies:
                header += [
                    f"{strategy}_nodes",
                    f"{strategy}_time_ms",
                    f"{strategy}_order_ms",
                    f"{strategy}_nodes_ratio",
                    f"{strategy}_time_ratio",
                ]
            header += ["best_strategy", "best_nodes", "peak_rss_mb"]
            writer.writerow(header)

            for circuit in circuits:
                def entry(variant: str) -> dict[str, str] | None:
                    for table in tables[variant]:
                        if circuit in table:
                            return table[circuit]
                    return None

                def median(variant: str, key: str) -> float:
                    values = [
                        number(table.get(circuit), key) for table in tables[variant]
                    ]
                    values = [value for value in values if value is not None]
                    return st.median(values) if values else 0.0

                first = entry(variants[0])
                if first is None or first.get("status") != "ok":
                    writer.writerow(
                        [
                            Path(circuit).name,
                            (first or {}).get("qubits", ""),
                            (first or {}).get("gates", ""),
                            (first or {}).get("status", "missing"),
                        ]
                        + [""] * (len(header) - 4)
                    )
                    continue

                originalNodes = median("original", "original_nodes")
                originalTime = median("original", "original_time_ms")
                row = [
                    Path(circuit).name,
                    first.get("qubits", ""),
                    first.get("gates", ""),
                    first["status"],
                    f"{originalNodes:.0f}",
                    f"{originalTime:.4f}",
                ]
                best = None
                for strategy in strategies:
                    nodes = median(strategy, f"static_{strategy}_nodes")
                    build = median(strategy, f"static_{strategy}_time_ms")
                    order = median(strategy, f"static_{strategy}_order_ms")
                    row += [
                        f"{nodes:.0f}",
                        f"{build:.4f}",
                        f"{order:.4f}",
                        f"{nodes / originalNodes:.4f}" if originalNodes else "",
                        f"{(build + order) / originalTime:.4f}" if originalTime else "",
                    ]
                    if best is None or nodes < best[0]:
                        best = (nodes, strategy)
                peaks = [
                    number(table.get(circuit), "peak_rss_mb")
                    for variant in variants
                    for table in tables[variant]
                ]
                peaks = [peak for peak in peaks if peak is not None]
                row += [
                    best[1],
                    f"{best[0]:.0f}",
                    f"{st.median(peaks):.1f}" if peaks else "",
                ]
                writer.writerow(row)
        print(f"wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
