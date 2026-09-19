#!/usr/bin/env python3
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
"""Summarize the per-strategy application tables."""

from __future__ import annotations

import csv
import re
import statistics as st
import sys
from pathlib import Path

STRATEGIES = ("degree", "spectral", "recursive_bisection")
QUASI_RANDOM = re.compile(r"ising|dnn|random|qaoa|variational|vqe|sat|dnn")


def number(row: dict[str, str], key: str) -> float | None:
    value = row.get(key, "")
    if value in ("", None, "-"):
        return None
    try:
        return float(value)
    except ValueError:
        return None


def strategySection(handle, rows, title: str) -> None:
    handle.write(f"### {title}\n\n")
    handle.write(
        "| strategy | median node ratio | circuits smaller | median time ratio | circuits faster | worst node ratio |\n"
    )
    handle.write("| --- | --- | --- | --- | --- | --- |\n")
    for strategy in STRATEGIES:
        nodes = [number(r, f"{strategy}_nodes_ratio") for r in rows]
        nodes = [value for value in nodes if value is not None]
        times = [number(r, f"{strategy}_time_ratio") for r in rows]
        times = [value for value in times if value is not None]
        if not nodes:
            continue
        handle.write(
            f"| {strategy} | {st.median(nodes):.3f} | "
            f"{sum(1 for value in nodes if value < 1)}/{len(nodes)} | "
            f"{st.median(times):.3f} | {sum(1 for value in times if value < 1)}/{len(times)} | "
            f"{max(nodes):.2f} |\n"
        )
    handle.write("\n")


def perCircuitTable(handle, rows, limit: int | None = None) -> None:
    handle.write(
        "| circuit | qubits | original | degree | spectral | recursive bisection | best |\n"
    )
    handle.write("| --- | --- | --- | --- | --- | --- | --- |\n")
    ordered = sorted(
        rows, key=lambda r: -(number(r, "original_nodes") or 0)
    )
    for row in ordered[:limit] if limit else ordered:
        handle.write(
            f"| {row['circuit']} | {row['qubits']} | {row['original_nodes']} | "
            f"{row['degree_nodes']} | {row['spectral_nodes']} | "
            f"{row['recursive_bisection_nodes']} | {row['best_strategy']} {row['best_nodes']} |\n"
        )
    handle.write("\n")


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: summarize_application_experiments.py <results-dir> <output.md>")
        return 2
    results = Path(sys.argv[1])
    output = Path(sys.argv[2])

    e1 = [
        row
        for row in csv.DictReader((results / "application_e1_statevector.csv").open())
        if row["status"] == "ok"
    ]
    e3 = [
        row
        for row in csv.DictReader((results / "application_e3_unitary.csv").open())
        if row["status"] == "ok"
    ]

    with output.open("w") as handle:
        handle.write("# 静态变量重排的应用层实验\n\n")
        handle.write(
            "每个变体在独立进程中测量，每个电路重复多次取中位。"
            "节点比 = 该策略节点数 / 原始节点数；时间比 = (构建 + 排序) / 原始构建时间。\n\n"
        )

        handle.write("## E1 态矢量仿真\n\n")
        strategySection(handle, e1, "全部电路")
        structured = [r for r in e1 if not QUASI_RANDOM.search(r["circuit"])]
        quasi = [r for r in e1 if QUASI_RANDOM.search(r["circuit"])]
        strategySection(handle, structured, f"结构化电路（{len(structured)} 个）")
        strategySection(handle, quasi, f"准随机电路（{len(quasi)} 个）")
        handle.write("### 逐电路（节点数）\n\n")
        perCircuitTable(handle, e1)

        handle.write("## E3 酉矩阵构建\n\n")
        strategySection(handle, e3, "全部电路")
        for name, low, high in (
            ("小（<1e3）", 0, 1000),
            ("中（1e3–1e4）", 1000, 10000),
            ("大（>1e4）", 10000, float("inf")),
        ):
            group = [
                row
                for row in e3
                if low <= (number(row, "original_nodes") or 0) < high
            ]
            if group:
                strategySection(handle, group, f"{name}（{len(group)} 个）")
        handle.write("### 逐电路（节点数）\n\n")
        perCircuitTable(handle, e3)
    print(f"wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
