# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
"""Merge the per-run QCEC experiment rows into the result tables.

The runner stores one file per run under ``rows/check`` and ``rows/dd``. Every
file holds either one CSV row of the benchmark tool or one status word. This
script merges those files, cleans the circuit names, and writes

* ``qcec_static_ordering_checks.csv``   one row per run
* ``qcec_static_ordering_dd.csv``       one row per decision diagram run
* ``qcec_static_ordering_by_policy.csv`` one row per pair and checker
* ``qcec_static_ordering_summary.md``   the aggregate report
"""

from __future__ import annotations

import csv
import statistics
import sys
from collections import defaultdict
from pathlib import Path

POLICIES = [
    "none",
    "gate_dependency",
    "degree",
    "spectral",
    "recursive_bisection",
]

CANDIDATE_POLICIES = POLICIES[1:]

DRIVER_CHECK_COLUMNS = [
    "pair",
    "ref_qubits",
    "ref_gates",
    "impl_qubits",
    "impl_gates",
    "policy",
    "checker",
    "equivalent",
    "preprocessing_time_ms",
    "check_time_ms",
    "total_time_ms",
    "peak_rss_mb",
    "started_simulations",
    "performed_simulations",
    "selection_time_ms",
]

DRIVER_DD_COLUMNS = [
    "pair",
    "policy",
    "ref_qubits",
    "ref_gates",
    "ref_dd_nodes",
    "ref_build_ms",
    "impl_dd_nodes",
    "impl_build_ms",
    "peak_rss_mb",
    "selection_time_ms",
]

CHECK_OUT = [
    "pair",
    "rep",
    "policy",
    "checker",
    "status",
    "equivalent",
    "ref_qubits",
    "ref_gates",
    "impl_qubits",
    "impl_gates",
    "preprocessing_time_ms",
    "check_time_ms",
    "total_time_ms",
    "peak_rss_mb",
    "started_simulations",
    "performed_simulations",
    "selection_time_ms",
]

DD_OUT = [
    "pair",
    "rep",
    "policy",
    "status",
    "ref_qubits",
    "ref_gates",
    "ref_dd_nodes",
    "ref_build_ms",
    "peak_rss_mb",
    "selection_time_ms",
]

FAILURE_STATUS = {"timeout", "out-of-memory", "error"}

SENSITIVE_THRESHOLD_MS = 100.0

# Columns of the order-sensitive table.
TABLE_POLICIES = ["none", "degree", "spectral", "recursive_bisection"]


def readRow(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8").strip()
    if not text:
        return []
    if "," not in text:
        return [text]
    return next(csv.reader([text]))


def collect(work: Path, kind: str) -> list[dict[str, str]]:
    """Read every stored run of one kind and return complete records."""
    driverColumns = DRIVER_CHECK_COLUMNS if kind == "check" else DRIVER_DD_COLUMNS
    directory = work / "rows" / kind
    records: list[dict[str, str]] = []
    for path in directory.glob("*.csv"):
        parts = path.stem.split("__")
        pair = parts[0]
        policy = parts[1]
        checker = parts[2] if kind == "check" else ""
        rep = parts[-1].removeprefix("rep")
        row = readRow(path)
        if not row:
            continue
        if len(row) == 1:
            status = row[0] if row[0] in FAILURE_STATUS else "error"
            record = dict.fromkeys(driverColumns, "")
            record["pair"] = pair
            record["policy"] = policy
            if kind == "check":
                record["checker"] = checker
        else:
            # Runs from an earlier revision may carry other columns. Pad short
            # rows and drop the surplus instead of discarding the measurement.
            row = (row + [""] * len(driverColumns))[: len(driverColumns)]
            status = "ok"
            record = dict(zip(driverColumns, row))
        record["rep"] = rep
        record["status"] = status
        records.append(record)
    return records


def sortKey(record: dict[str, str]) -> tuple:
    policy = record["policy"]
    index = POLICIES.index(policy) if policy in POLICIES else len(POLICIES)
    return (
        record["pair"],
        record.get("checker", ""),
        index,
        int(record["rep"] or 0),
    )


def writeCsv(path: Path, columns: list[str], records: list[dict[str, str]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        for record in records:
            writer.writerow({column: record.get(column, "") for column in columns})


def number(record: dict[str, str], column: str) -> float | None:
    try:
        return float(record.get(column, ""))
    except (TypeError, ValueError):
        return None


def medianOf(records: list[dict[str, str]], column: str) -> float | None:
    values = [value for record in records if (value := number(record, column)) is not None]
    return statistics.median(values) if values else None


def formatNumber(value: float | None, digits: int = 3) -> str:
    if value is None:
        return "-"
    return f"{value:.{digits}f}"


def main() -> int:
    work = Path(sys.argv[1])
    results = Path(sys.argv[2])
    results.mkdir(parents=True, exist_ok=True)

    checks = sorted(collect(work, "check"), key=sortKey)
    ddRows = sorted(collect(work, "dd"), key=sortKey)

    writeCsv(results / "qcec_static_ordering_checks.csv", CHECK_OUT, checks)
    writeCsv(results / "qcec_static_ordering_dd.csv", DD_OUT, ddRows)

    # One row per pair and checker with a column group per policy.
    grouped: dict[tuple[str, str], list[dict[str, str]]] = defaultdict(list)
    for record in checks:
        grouped[(record["pair"], record["checker"])].append(record)

    tableRows: list[dict[str, str]] = []
    for (pair, checker), records in sorted(grouped.items()):
        base = [r for r in records if r["policy"] == "none" and r["status"] == "ok"]
        baseTime = medianOf(base, "total_time_ms")
        row: dict[str, str] = {"pair": pair, "checker": checker}
        candidates = [r for r in records if r["status"] == "ok"]
        row["ref_qubits"] = next(
            (r["ref_qubits"] for r in records if r["ref_qubits"]), ""
        )
        row["impl_qubits"] = next(
            (r["impl_qubits"] for r in records if r["impl_qubits"]), ""
        )
        row["ref_gates"] = next(
            (r["ref_gates"] for r in records if r["ref_gates"]), ""
        )
        row["impl_gates"] = next(
            (r["impl_gates"] for r in records if r["impl_gates"]), ""
        )
        row["verdict"] = (
            "all-equivalent"
            if candidates and all(r["equivalent"] == "equivalent" for r in candidates)
            else "check"
        )
        bestPolicy = ""
        bestTime = None
        for policy in POLICIES:
            subset = [r for r in records if r["policy"] == policy]
            good = [r for r in subset if r["status"] == "ok"]
            value = medianOf(good, "total_time_ms")
            row[f"{policy}_time_ms"] = formatNumber(value)
            row[f"{policy}_rss_mb"] = formatNumber(medianOf(good, "peak_rss_mb"), 1)
            row[f"{policy}_check_ms"] = formatNumber(medianOf(good, "check_time_ms"))
            row[f"{policy}_selection_ms"] = formatNumber(
                medianOf(good, "selection_time_ms"), 1
            )
            ratio = value / baseTime if value and baseTime else None
            row[f"{policy}_ratio"] = formatNumber(ratio, 4)
            if value is not None and ratio is not None:
                if bestTime is None or value < bestTime:
                    bestTime = value
                    bestPolicy = policy
        row["best_policy"] = bestPolicy
        row["best_time_ms"] = formatNumber(bestTime)
        row["best_ratio"] = formatNumber(
            bestTime / baseTime if bestTime and baseTime else None, 4
        )
        tableRows.append(row)

    tableColumns = ["pair", "checker", "ref_qubits", "ref_gates", "impl_qubits",
                    "impl_gates", "verdict"]
    for policy in POLICIES:
        tableColumns += [f"{policy}_time_ms", f"{policy}_rss_mb", f"{policy}_ratio"]
    tableColumns += ["best_policy", "best_time_ms", "best_ratio"]
    writeCsv(results / "qcec_static_ordering_by_policy.csv", tableColumns, tableRows)

    # Aggregate report.
    lines = ["# Static variable ordering in MQT QCEC", ""]
    lines.append(f"Runs in the check table: {len(checks)}")
    lines.append(f"Runs in the decision diagram table: {len(ddRows)}")
    lines.append("")

    solved = [r for r in checks if r["status"] == "ok"]
    failed = [r for r in checks if r["status"] != "ok"]
    lines.append(f"Completed runs: {len(solved)}; failed runs: {len(failed)}")
    lines.append("")

    wrong = [r for r in solved if r["equivalent"] != "equivalent"]
    lines.append(f"Runs that report a difference: {len(wrong)}")
    for record in wrong[:20]:
        lines.append(
            f"* {record['pair']} {record['checker']} {record['policy']}: "
            f"{record['equivalent']}"
        )
    lines.append("")

    lines.append("## Speed-up of the total check time against the natural order")
    lines.append("")
    lines.append(
        "All cells include the circuits whose natural order is already good. "
        "Those cells only add measurement noise, so the second table keeps the "
        "cells where the natural order needs at least "
        f"{SENSITIVE_THRESHOLD_MS} ms."
    )
    lines.append("")
    lines.append("| policy | improved cells | median ratio | median speed-up | best speed-up |")
    lines.append("| --- | --- | --- | --- | --- |")
    for policy in CANDIDATE_POLICIES:
        ratios = []
        for row in tableRows:
            baseTime = number(row, "none_time_ms")
            value = number(row, f"{policy}_time_ms")
            if baseTime and value:
                ratios.append(value / baseTime)
        improved = sum(1 for ratio in ratios if ratio < 1.0)
        median = statistics.median(ratios) if ratios else None
        lines.append(
            f"| {policy} | {improved}/{len(ratios)} | {formatNumber(median, 4)} | "
            f"{formatNumber(1 / median if median else None, 3)} | "
            f"{formatNumber(1 / min(ratios) if ratios else None, 1)} |"
        )
    lines.append("")

    sensitive = [
        row
        for row in tableRows
        if (number(row, "none_time_ms") or 0.0) >= SENSITIVE_THRESHOLD_MS
    ]
    sensitive.sort(key=lambda row: -number(row, "none_time_ms"))
    lines.append(f"## Order-sensitive cells (natural order at least {SENSITIVE_THRESHOLD_MS} ms)")
    lines.append("")
    header = "| pair | checker | " + " | ".join(
        "natural" if policy == "none" else policy for policy in TABLE_POLICIES
    )
    header += " | best | speed-up |"
    lines.append(header)
    lines.append("|" + " --- |" * (len(TABLE_POLICIES) + 4))
    for row in sensitive:
        cells = []
        for policy in TABLE_POLICIES:
            value = number(row, f"{policy}_time_ms")
            cells.append(formatNumber(value, 1) if value is not None else "-")
        base = number(row, "none_time_ms")
        best = number(row, "best_time_ms")
        speedup = base / best if base and best else None
        lines.append(
            f"| {row['pair']} | {row['checker']} | " + " | ".join(cells)
            + f" | {row['best_policy']} | {formatNumber(speedup, 1)}x |"
        )
    lines.append("")

    lines.append("## Order-sensitive cells, counters over the subset")
    lines.append("")
    lines.append("| policy | improved | median ratio | best speed-up | worst ratio |")
    lines.append("| --- | --- | --- | --- | --- |")
    for policy in CANDIDATE_POLICIES:
        ratios = []
        for row in sensitive:
            baseTime = number(row, "none_time_ms")
            value = number(row, f"{policy}_time_ms")
            if baseTime and value:
                ratios.append(value / baseTime)
        if not ratios:
            continue
        lines.append(
            f"| {policy} | {sum(1 for ratio in ratios if ratio < 1)}/{len(ratios)} | "
            f"{formatNumber(statistics.median(ratios), 4)} | "
            f"{formatNumber(1 / min(ratios), 1)}x | "
            f"{formatNumber(max(ratios), 2)}x |"
        )
    lines.append("")

    lines.append("## Order-sensitive cells, check time against selection cost")
    lines.append("")
    lines.append(
        "`check` is the time QCEC reports for the checkers. `select` is the "
        "time the driver needs to compute the order. Every policy in this "
        "table is static, so `select` stays far below `check`."
    )
    lines.append("")
    lines.append(
        "| pair | checker | natural check | degree select | spectral select | "
        "recursive_bisection select |"
    )
    lines.append("| --- | --- | --- | --- | --- | --- |")
    for row in sensitive:
        lines.append(
            f"| {row['pair']} | {row['checker']} | {row['none_check_ms']} | "
            f"{row['degree_selection_ms']} | {row['spectral_selection_ms']} | "
            f"{row['recursive_bisection_selection_ms']} |"
        )
    lines.append("")

    lines.append("## Per checker")
    lines.append("")
    lines.append("| checker | pairs | best policy | median best speed-up |")
    lines.append("| --- | --- | --- | --- |")
    byChecker: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in tableRows:
        byChecker[row["checker"]].append(row)
    for checker, rows in sorted(byChecker.items()):
        ratios = [
            number(row, "none_time_ms") / number(row, "best_time_ms")
            for row in rows
            if number(row, "none_time_ms") and number(row, "best_time_ms")
        ]
        winners = defaultdict(int)
        for row in rows:
            if row["best_policy"]:
                winners[row["best_policy"]] += 1
        top = ", ".join(
            f"{name} {count}" for name, count in sorted(winners.items(), key=lambda i: -i[1])
        )
        lines.append(
            f"| {checker} | {len(rows)} | {top} | "
            f"{formatNumber(statistics.median(ratios) if ratios else None, 3)} |"
        )
    lines.append("")

    lines.append("## Decision diagram size")
    lines.append("")
    lines.append("| policy | solved | median ref nodes | median ref build ms |")
    lines.append("| --- | --- | --- | --- |")
    for policy in POLICIES:
        subset = [r for r in ddRows if r["policy"] == policy and r["status"] == "ok"]
        refNodes = [
            value
            for record in subset
            if (value := number(record, "ref_dd_nodes")) is not None and value > 0
        ]
        refMs = [
            value
            for record in subset
            if (value := number(record, "ref_build_ms")) is not None and value > 0
        ]
        lines.append(
            f"| {policy} | {len(refNodes)} | "
            f"{formatNumber(statistics.median(refNodes) if refNodes else None, 1)} | "
            f"{formatNumber(statistics.median(refMs) if refMs else None, 3)} |"
        )
    lines.append("")

    lines.append("## Reference diagram nodes per circuit")
    lines.append("")
    lines.append("| pair | qubits | gates | " + " | ".join(POLICIES) + " |")
    lines.append("|" + " --- |" * (len(POLICIES) + 3))
    byPair: dict[str, dict[str, list[float]]] = defaultdict(lambda: defaultdict(list))
    meta: dict[str, tuple[str, str]] = {}
    for record in ddRows:
        if record["status"] != "ok":
            continue
        value = number(record, "ref_dd_nodes")
        if value is None or value <= 0:
            continue
        byPair[record["pair"]][record["policy"]].append(value)
        meta[record["pair"]] = (record["ref_qubits"], record["ref_gates"])
    ordered = sorted(
        byPair,
        key=lambda pair: -statistics.median(byPair[pair].get("none", [0])),
    )
    for pair in ordered:
        qubits, gates = meta.get(pair, ("", ""))
        cells = []
        for policy in POLICIES:
            values = byPair[pair].get(policy, [])
            cells.append(formatNumber(statistics.median(values), 0) if values else "-")
        lines.append(
            f"| {pair} | {qubits} | {gates} | " + " | ".join(cells) + " |"
        )
    lines.append("")

    (results / "qcec_static_ordering_summary.md").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )

    print(f"merged {len(checks)} check rows and {len(ddRows)} dd rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
