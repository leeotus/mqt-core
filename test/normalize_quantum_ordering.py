#!/usr/bin/env python3

# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT

"""Convert per-strategy benchmark output into one row per circuit and strategy."""

from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path

STRATEGIES = {
    "identity": "original",
    "degree": "static_degree",
    "spectral": "static_spectral",
    "recursive_bisection": "static_recursive_bisection",
    "force": "static_force",
    "mince": "static_mince",
    "quantum_score": "static_quantum_score",
}


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments.

    Returns:
        Parsed arguments.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("raw", nargs="+", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--mode", choices=("functionality", "state"))
    parser.add_argument("--failures", type=Path)
    return parser.parse_args()


def repetition(path: Path) -> int:
    """Extract a repetition number from a raw CSV filename.

    Args:
        path: Raw CSV path.

    Returns:
        The repetition number, defaulting to one.
    """
    match = re.search(r"_rep(\d+)\.csv$", path.name)
    return int(match.group(1)) if match else 1


def read_manifest(path: Path) -> dict[str, dict[str, str]]:
    """Read the generated circuit manifest by filename.

    Args:
        path: Generated TSV manifest.

    Returns:
        Manifest records keyed by circuit filename.
    """
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle, delimiter="\t"))
    return {Path(row["path"]).name: row for row in rows}


def main() -> int:
    """Run the normalization.

    Returns:
        Process exit code.
    """
    args = parse_args()
    manifest = read_manifest(args.manifest)
    output_rows: list[dict[str, str]] = []
    seen: set[tuple[str, str, str]] = set()

    for raw_path in args.raw:
        rep = repetition(raw_path)
        with raw_path.open(newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                circuit = Path(row["circuit"]).name
                metadata = manifest.get(circuit, {})
                for strategy, prefix in STRATEGIES.items():
                    nodes = row.get(f"{prefix}_nodes", "")
                    if not nodes:
                        continue
                    build_ms = float(row[f"{prefix}_time_ms"])
                    order_ms = 0.0 if strategy == "identity" else float(row[f"{prefix}_order_ms"])
                    output_rows.append({
                        "rep": str(rep),
                        "circuit": circuit,
                        "benchmark": metadata.get("benchmark", ""),
                        "level": metadata.get("level", ""),
                        "qubits": row["qubits"],
                        "gates": row["gates"],
                        "strategy": strategy,
                        "final_nodes": nodes,
                        "peak_nodes": row[f"{prefix}_peak_nodes"],
                        "build_time_ms": f"{build_ms:.3f}",
                        "order_time_ms": f"{order_ms:.3f}",
                        "total_time_ms": f"{build_ms + order_ms:.3f}",
                        "peak_rss_mb": f"{float(row['peak_rss_mb']):.1f}",
                        "status": row["status"],
                    })
                    seen.add((str(rep), circuit, strategy))

    if args.failures and args.mode:
        with args.failures.open(newline="", encoding="utf-8") as handle:
            for failure in csv.DictReader(handle):
                if failure["mode"] != args.mode:
                    continue
                circuit = Path(failure["circuit"]).name
                key = (failure["rep"], circuit, failure["strategy"])
                if key in seen:
                    continue
                metadata = manifest.get(circuit, {})
                output_rows.append({
                    "rep": failure["rep"],
                    "circuit": circuit,
                    "benchmark": metadata.get("benchmark", ""),
                    "level": metadata.get("level", ""),
                    "qubits": failure.get("qubits") or "-",
                    "gates": failure.get("gates") or "-",
                    "strategy": failure["strategy"],
                    "final_nodes": "-",
                    "peak_nodes": "-",
                    "build_time_ms": "-",
                    "order_time_ms": "-",
                    "total_time_ms": "-",
                    "peak_rss_mb": "-",
                    "status": failure["status"],
                })
                seen.add(key)

    columns = [
        "rep",
        "circuit",
        "benchmark",
        "level",
        "qubits",
        "gates",
        "strategy",
        "final_nodes",
        "peak_nodes",
        "build_time_ms",
        "order_time_ms",
        "total_time_ms",
        "peak_rss_mb",
        "status",
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        writer.writerows(
            sorted(
                output_rows,
                key=lambda row: (
                    int(row["rep"]),
                    row["circuit"],
                    list(STRATEGIES).index(row["strategy"]),
                ),
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
