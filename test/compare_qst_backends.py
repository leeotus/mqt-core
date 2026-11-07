#!/usr/bin/env python3

# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT

"""Run the dense and QMDD QST backends on the same circuit input."""

from __future__ import annotations

import argparse
import csv
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

COMPARED_FIELDS = [
    "fidelity",
    "purity",
    "time_ms",
    "converged",
    "dd_size",
    "max_iterations",
    "max_outcomes",
    "true_purity",
    "trace_distance",
    "trace",
    "trace_error",
    "log_likelihood",
    "observed_outcomes",
    "retained_outcomes",
    "initial_dd_size",
    "peak_dd_size",
    "peak_r_dd_size",
    "density_ms",
    "basis_ms",
    "sampling_ms",
    "reconstruct_init_ms",
    "reconstruct_iter_ms",
    "metrics_ms",
    "total_ms",
    "elapsed_s",
    "peak_rss_mb",
    "status",
]


def parse_args() -> argparse.Namespace:
    """Parse command-line comparison options.

    Returns:
        Parsed comparison options.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--circuits", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--probe",
        type=Path,
        default=ROOT / "build/release/bin/mqt-core-qst-probe",
    )
    parser.add_argument("--time-limit", type=int, default=0)
    parser.add_argument("--memory-limit", type=int, default=0)
    parser.add_argument("--shots", type=int, default=1000)
    parser.add_argument("--random-bases", type=int, default=50)
    parser.add_argument("--iterations", type=int, default=50)
    parser.add_argument("--max-outcomes", type=int, default=0)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--limit", type=int, default=0)
    return parser.parse_args()


def run_backend(args: argparse.Namespace, backend: str, output: Path) -> dict[str, dict[str, str]]:
    """Run one backend and return its rows keyed by circuit path.

    Returns:
        CSV rows keyed by circuit path.
    """
    command = [
        "bash",
        str(ROOT / "test/run_qst_batch.sh"),
        "--probe",
        str(args.probe),
        "--output",
        str(output),
        "--time-limit",
        str(args.time_limit),
        "--memory-limit",
        str(args.memory_limit),
        "--shots",
        str(args.shots),
        "--random-bases",
        str(args.random_bases),
        "--iterations",
        str(args.iterations),
        "--max-outcomes",
        str(args.max_outcomes),
        "--seed",
        str(args.seed),
        "--limit",
        str(args.limit),
        "--backend",
        backend,
        "--full-path",
        str(args.circuits),
    ]
    # The command is a fixed local script. All variable arguments are paths or
    # integers parsed by argparse, and shell expansion remains disabled.
    subprocess.run(command, check=True, cwd=ROOT, shell=False)  # ruff: ignore[subprocess-without-shell-equals-true]
    with output.open(encoding="utf-8", newline="") as stream:
        return {row["circuit"]: row for row in csv.DictReader(stream)}


def ratio(numerator: str, denominator: str) -> str:
    """Return the numerator divided by the denominator as CSV text."""
    try:
        top = float(numerator)
        bottom = float(denominator)
    except ValueError:
        return "-"
    if bottom <= 0.0:
        return "-"
    return f"{top / bottom:.3f}"


def difference(left: str, right: str) -> str:
    """Return the left value minus the right value as CSV text."""
    try:
        return f"{float(left) - float(right):.6f}"
    except ValueError:
        return "-"


def row_value(row: dict[str, str], field: str) -> str:
    """Return a row field or a missing-value marker."""
    return row.get(field, "-")


def main() -> None:
    """Run both backends and write the combined comparison CSV."""
    args = parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="mqt-core-qst-") as directory:
        temporary = Path(directory)
        dense = run_backend(args, "dense", temporary / "dense.csv")
        qmdd = run_backend(args, "qmdd", temporary / "qmdd.csv")

    columns = ["circuit", "n", "bases", "shots", "iterations"]
    columns.extend(f"dense_{field}" for field in COMPARED_FIELDS)
    columns.extend(f"qmdd_{field}" for field in COMPARED_FIELDS)
    columns.extend([
        "recon_ratio",
        "wall_ratio",
        "memory_ratio",
        "fidelity_delta",
        "trace_distance_delta",
        "purity_delta",
        "peak_dd_ratio",
        "retained_ratio",
    ])
    with args.output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        for circuit, dense_row in dense.items():
            qmdd_row = qmdd.get(circuit)
            if qmdd_row is None:
                continue
            combined = {
                "circuit": circuit,
                "n": dense_row["n"],
                "bases": dense_row["bases"],
                "shots": dense_row["shots"],
                "iterations": dense_row["iterations"],
                "recon_ratio": ratio(qmdd_row["time_ms"], dense_row["time_ms"]),
                "wall_ratio": ratio(qmdd_row["elapsed_s"], dense_row["elapsed_s"]),
                "memory_ratio": ratio(qmdd_row["peak_rss_mb"], dense_row["peak_rss_mb"]),
                "fidelity_delta": difference(qmdd_row["fidelity"], dense_row["fidelity"]),
                "trace_distance_delta": difference(qmdd_row["trace_distance"], dense_row["trace_distance"]),
                "purity_delta": difference(qmdd_row["purity"], dense_row["purity"]),
                "peak_dd_ratio": ratio(qmdd_row["peak_dd_size"], dense_row["peak_dd_size"]),
                "retained_ratio": ratio(qmdd_row["retained_outcomes"], dense_row["retained_outcomes"]),
            }
            for prefix, row in (("dense", dense_row), ("qmdd", qmdd_row)):
                for field in COMPARED_FIELDS:
                    combined[f"{prefix}_{field}"] = row_value(row, field)
            writer.writerow(combined)


if __name__ == "__main__":
    main()
