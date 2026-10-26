#!/usr/bin/env python3

# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT

"""Summarize static ordering measurements on quantum benchmark circuits."""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from collections import Counter
from pathlib import Path

STRATEGIES = {
    "degree": "static_degree",
    "spectral": "static_spectral",
    "recursive_bisection": "static_recursive_bisection",
    "force": "static_force",
    "mince": "static_mince",
    "quantum_score": "static_quantum_score",
}


def read_rows(path: Path) -> list[dict[str, str]]:
    """Read successful measurement rows.

    Args:
        path: CSV file written by the ordering benchmark.

    Returns:
        Rows with status ``ok``.
    """
    with path.open(newline="", encoding="utf-8") as handle:
        return [row for row in csv.DictReader(handle) if row["status"] == "ok"]


def as_float(row: dict[str, str], key: str) -> float | None:
    """Parse a numeric field.

    Args:
        row: Benchmark row.
        key: Column name.

    Returns:
        The parsed value, or ``None`` when the field is empty or invalid.
    """
    try:
        return float(row[key])
    except (KeyError, TypeError, ValueError):
        return None


def median(values: list[float]) -> float:
    """Return the median of non-empty values.

    Args:
        values: Numeric values.

    Returns:
        The median, or infinity for an empty list.
    """
    return statistics.median(values) if values else math.inf


def summarize(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    """Compute per-strategy node and time ratios.

    Args:
        rows: Successful benchmark rows.

    Returns:
        One summary record per strategy.
    """
    records: list[dict[str, str]] = []
    for strategy, prefix in STRATEGIES.items():
        node_ratios: list[float] = []
        peak_ratios: list[float] = []
        time_ratios: list[float] = []
        wins = ties = losses = 0
        for row in rows:
            original_nodes = as_float(row, "original_nodes")
            strategy_nodes = as_float(row, f"{prefix}_nodes")
            if not original_nodes or strategy_nodes is None:
                continue
            ratio = strategy_nodes / original_nodes
            node_ratios.append(ratio)
            if strategy_nodes < original_nodes:
                wins += 1
            elif strategy_nodes == original_nodes:
                ties += 1
            else:
                losses += 1

            original_peak = as_float(row, "original_peak_nodes")
            strategy_peak = as_float(row, f"{prefix}_peak_nodes")
            if original_peak and strategy_peak is not None:
                peak_ratios.append(strategy_peak / original_peak)

            original_time = as_float(row, "original_time_ms")
            strategy_time = as_float(row, f"{prefix}_time_ms")
            if original_time and strategy_time is not None:
                time_ratios.append(strategy_time / original_time)

        records.append({
            "strategy": strategy,
            "circuits": str(len(node_ratios)),
            "wins": str(wins),
            "ties": str(ties),
            "losses": str(losses),
            "median_node_ratio": f"{median(node_ratios):.4f}",
            "median_peak_ratio": f"{median(peak_ratios):.4f}",
            "median_time_ratio": f"{median(time_ratios):.4f}",
            "best_node_ratio": f"{min(node_ratios, default=math.inf):.4f}",
            "worst_node_ratio": f"{max(node_ratios, default=math.inf):.4f}",
            "best_peak_ratio": f"{min(peak_ratios, default=math.inf):.4f}",
            "worst_peak_ratio": f"{max(peak_ratios, default=math.inf):.4f}",
        })
    return records


def circuit_family(row: dict[str, str]) -> str:
    """Return the benchmark family from a generated circuit name.

    Args:
        row: Benchmark row.

    Returns:
        The benchmark name without level, optimization, and size suffixes.
    """
    stem = Path(row["circuit"]).stem
    for marker in ("_indep_opt", "_alg_", "_nativegates_", "_mapped_"):
        if marker in stem:
            return stem.split(marker, maxsplit=1)[0]
    return stem


def best_strategy(row: dict[str, str]) -> str:
    """Return the static strategy with the fewest final nodes.

    Args:
        row: Benchmark row.

    Returns:
        Strategy name, or ``unknown`` when no strategy result is present.
    """
    candidates = []
    for strategy, prefix in STRATEGIES.items():
        nodes = as_float(row, f"{prefix}_nodes")
        if nodes is not None:
            candidates.append((nodes, strategy))
    return min(candidates, default=(math.inf, "unknown"))[1]


def summarize_families(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    """Summarize results by benchmark family.

    Args:
        rows: Successful benchmark rows.

    Returns:
        One record per benchmark family.
    """
    grouped: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        grouped.setdefault(circuit_family(row), []).append(row)

    records = []
    for family, family_rows in sorted(grouped.items()):
        ratios = []
        for row in family_rows:
            original = as_float(row, "original_nodes")
            quantum = as_float(row, "static_quantum_score_nodes")
            if original and quantum is not None:
                ratios.append(quantum / original)
        winners = Counter(best_strategy(row) for row in family_rows)
        records.append({
            "family": family,
            "circuits": str(len(family_rows)),
            "quantum_median": f"{median(ratios):.4f}",
            "most_frequent_best": winners.most_common(1)[0][0],
        })
    return records


def write_markdown(path: Path, rows: list[dict[str, str]], source: Path) -> None:
    """Write the pilot report.

    Args:
        path: Output Markdown file.
        rows: Successful benchmark rows.
        source: Input CSV file.
    """
    records = summarize(rows)
    lines = [
        "# Quantum static ordering pilot",
        "",
        f"Source: `{source}`",
        "",
        f"Successful circuits: {len(rows)}",
        "",
        "| strategy | circuits | wins | ties | losses | nodes | peak | time | best peak | worst peak |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    lines.extend(
        f"| {record['strategy']} | {record['circuits']} | {record['wins']} | "
        f"{record['ties']} | {record['losses']} | "
        f"{record['median_node_ratio']} | {record['median_peak_ratio']} | "
        f"{record['median_time_ratio']} | {record['best_peak_ratio']} | "
        f"{record['worst_peak_ratio']} |"
        for record in records
    )
    lines.extend([
        "",
        "## By benchmark family",
        "",
        "| family | circuits | QuantumScore median node ratio | most frequent best |",
        "| --- | ---: | ---: | --- |",
    ])
    lines.extend(
        f"| {record['family']} | {record['circuits']} | {record['quantum_median']} | {record['most_frequent_best']} |"
        for record in summarize_families(rows)
    )
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    """Run the summary.

    Returns:
        Process exit code.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_markdown(args.output, read_rows(args.input), args.input)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
