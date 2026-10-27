#!/usr/bin/env python3

# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT

"""Append one completed circuit's parallel ordering results to CSV files."""

from __future__ import annotations

import argparse
import csv
import os
import re
from pathlib import Path

STRATEGIES = {
    "identity": (4, 5, 6, None),
    "degree": (7, 8, 9, 10),
    "spectral": (11, 12, 13, 14),
    "recursive_bisection": (15, 16, 17, 18),
    "force": (19, 20, 21, 22),
    "mince": (23, 24, 25, 26),
    "quantum_score": (27, 28, 29, 30),
}
MEMORY_ERROR = re.compile(
    r"bad_alloc|bad array new length|cannot allocate memory|"
    r"memory exhausted|out of memory",
    re.IGNORECASE,
)
RESULT_COLUMNS = [
    "rep",
    "mode",
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
FAILURE_COLUMNS = ["rep", "mode", "circuit", "strategy", "status"]


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments.

    Returns:
        Parsed arguments.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--failures", type=Path, required=True)
    parser.add_argument("--work-dir", action="append", required=True)
    parser.add_argument("--rep", required=True)
    parser.add_argument("--circuit", required=True)
    return parser.parse_args()


def read_manifest(path: Path) -> dict[str, dict[str, str]]:
    """Read circuit metadata by basename.

    Args:
        path: Circuit manifest.

    Returns:
        Manifest rows keyed by circuit basename.
    """
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle, delimiter="\t"))
    return {Path(row["path"]).name: row for row in rows}


def classify_failure(return_code: int, error_path: Path) -> str:
    """Classify a failed worker process.

    Args:
        return_code: Worker return code.
        error_path: Captured stderr.

    Returns:
        Failure status.
    """
    if return_code == 124:
        return "timeout"
    error = error_path.read_text(errors="replace") if error_path.is_file() else ""
    if MEMORY_ERROR.search(error):
        return "memory-limit"
    if return_code in {134, 137}:
        return "memory-limit-or-killed"
    return f"runtime-error-{return_code}"


def result_row(
    mode: str,
    circuit: str,
    metadata: dict[str, str],
    strategy: str,
    work_dir: Path,
    rep: str,
) -> tuple[dict[str, str], bool]:
    """Convert one worker result into a normalized CSV row.

    Args:
        mode: DD construction mode.
        circuit: Circuit basename.
        metadata: Manifest metadata.
        strategy: Ordering strategy.
        work_dir: Temporary worker directory.
        rep: Repetition number.

    Returns:
        CSV row and whether it represents a failure.
    """
    row_path = work_dir / f"{mode}.{strategy}.row"
    return_code_path = work_dir / f"{mode}.{strategy}.rc"
    error_path = work_dir / f"{mode}.{strategy}.err"
    return_code = (
        int(return_code_path.read_text().strip())
        if return_code_path.is_file()
        else 1
    )

    values: dict[str, str] = {
        "rep": rep,
        "mode": mode,
        "circuit": circuit,
        "benchmark": metadata.get("benchmark", ""),
        "level": metadata.get("level", ""),
        "qubits": "-",
        "gates": "-",
        "strategy": strategy,
        "final_nodes": "-",
        "peak_nodes": "-",
        "build_time_ms": "-",
        "order_time_ms": "-",
        "total_time_ms": "-",
        "peak_rss_mb": "-",
        "status": "",
    }
    if return_code != 0:
        values["status"] = classify_failure(return_code, error_path)
        return values, True
    if not row_path.is_file():
        values["status"] = "missing-result"
        return values, True

    with row_path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.reader(handle))
    if not rows or len(rows[0]) < 31:
        values["status"] = "invalid-result"
        return values, True
    row = rows[0]
    values["qubits"] = row[1] or "-"
    values["gates"] = row[2] or "-"
    values["status"] = row[3]
    if row[3] != "ok":
        return values, True

    node_index, peak_index, build_index, order_index = STRATEGIES[strategy]
    nodes = row[node_index]
    if not nodes:
        values["status"] = "missing-strategy-result"
        return values, True
    build_ms = float(row[build_index])
    order_ms = float(row[order_index]) if order_index is not None else 0.0
    values.update({
        "final_nodes": nodes,
        "peak_nodes": row[peak_index],
        "build_time_ms": f"{build_ms:.3f}",
        "order_time_ms": f"{order_ms:.3f}",
        "total_time_ms": f"{build_ms + order_ms:.3f}",
        "peak_rss_mb": f"{float(row[-1]):.1f}",
    })
    return values, False


def append_rows(path: Path, columns: list[str], rows: list[dict[str, str]]) -> None:
    """Append rows and flush them to disk.

    Args:
        path: CSV destination.
        columns: CSV columns.
        rows: Rows to append.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    write_header = not path.is_file() or path.stat().st_size == 0
    with path.open("a", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        if write_header:
            writer.writeheader()
        writer.writerows(rows)
        handle.flush()
        os.fsync(handle.fileno())


def main() -> int:
    """Append one circuit's results.

    Returns:
        Process exit code.
    """
    args = parse_args()
    metadata = read_manifest(args.manifest).get(args.circuit, {})
    work_dirs = {}
    for item in args.work_dir:
        mode, directory = item.split("=", 1)
        work_dirs[mode] = Path(directory)

    rows = []
    failures = []
    for mode, work_dir in work_dirs.items():
        for strategy in STRATEGIES:
            row, failed = result_row(
                mode,
                args.circuit,
                metadata,
                strategy,
                work_dir,
                args.rep,
            )
            rows.append(row)
            if failed:
                failures.append({
                    "rep": args.rep,
                    "mode": mode,
                    "circuit": args.circuit,
                    "strategy": strategy,
                    "status": row["status"],
                })

    append_rows(args.results, RESULT_COLUMNS, rows)
    append_rows(args.failures, FAILURE_COLUMNS, failures)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
