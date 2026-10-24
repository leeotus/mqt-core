#!/usr/bin/env python3
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
"""Write one row per circuit, with every configuration as a column.

The tables under ``docs/experiment-data`` hold one row per measurement. This
script pivots them into the view a reader wants when checking a single circuit:
one row per circuit, one column per variable order, plus the ratios that the
report quotes.

    python3 test/export_per_circuit_matrices.py
"""

from __future__ import annotations

import csv
import pathlib
import statistics

RESULTS = pathlib.Path("/home/flareon/workshop/results")
OUT = pathlib.Path("/home/flareon/workshop/mqt-core/docs/report/full-data")

ORDERS = ["natural", "degree", "spectral", "recursive_bisection", "adaptive"]
RANDOM_SEEDS = [str(seed) for seed in range(1, 9)]


def read(name: str) -> list[dict[str, str]]:
    """Read one CSV from the results directory."""
    with (RESULTS / name).open(newline="") as handle:
        return list(csv.DictReader(handle))


def number(row: dict[str, str], field: str) -> float:
    """Read a numeric field, treating anything unparsable as zero."""
    try:
        return float(row.get(field, "") or 0)
    except ValueError:
        return 0.0


def cell(value) -> str:
    """Format one table cell."""
    if value is None or value == "":
        return "-"
    if isinstance(value, float):
        if abs(value) >= 1:
            return f"{value:.3f}".rstrip("0").rstrip(".")
        return f"{value:.4g}"
    return str(value)


def write(name: str, title: str, intro: str, columns: list[str],
          rows: list[dict]) -> None:
    """Write one markdown table."""
    lines = [f"# {title}", "", intro.strip(), ""]
    lines.append("| " + " | ".join(columns) + " |")
    lines.append("| " + " | ".join("---" for _ in columns) + " |")
    for row in rows:
        lines.append("| " + " | ".join(cell(row.get(c)) for c in columns) + " |")
    lines.append("")
    (OUT / name).write_text("\n".join(lines))
    print(f"wrote {name}: {len(rows)} circuits")


def groupMap() -> dict[str, str]:
    """Map every SyReC circuit to the set it came from."""
    groups: dict[str, str] = {}
    for name, label in [
        ("syrec_benchmark_mcnc.txt", "MCNC"),
        ("syrec_benchmark_revlib.txt", "RevLib"),
    ]:
        for line in (RESULTS / name).read_text().splitlines():
            if line.strip():
                groups[pathlib.Path(line.strip()).name] = label
    return groups


def syrecMatrix() -> None:
    """One row per circuit, one column per configuration."""
    groups = groupMap()
    rows = [
        r
        for r in read("syrec_dd_order_reps149_force_mince.csv")
        if r["status"] == "ok"
    ]
    byCircuit: dict[str, dict[str, float]] = {}
    varsOf: dict[str, str] = {}
    for row in rows:
        key = row["order"] if row["order"] != "random" else f"random{row['seed']}"
        byCircuit.setdefault(row["circuit"], {})[key] = number(row, "gates_median")
        varsOf[row["circuit"]] = row["n_vars"]
    circuits = sorted(byCircuit, key=lambda c: (int(varsOf[c]), c))
    table = []
    for circuit in circuits:
        values = byCircuit[circuit]
        natural = values.get("natural")
        best = min(values.values())
        winner = min(values, key=values.get)
        spread = max(
            (
                (number(r, "gates_max") - number(r, "gates_min"))
                / max(number(r, "gates_median"), 1.0)
                for r in rows
                if r["circuit"] == circuit
            ),
            default=0.0,
        )
        entry = {
            "circuit": circuit,
            "group": groups.get(
                circuit,
                "random" if pathlib.Path(circuit).stem.startswith("rand")
                else "SyReC seal",
            ),
            "n_vars": varsOf[circuit],
            "natural": natural,
            "degree": values.get("degree"),
            "spectral": values.get("spectral"),
            "recursive_bisection": values.get("recursive_bisection"),
            "adaptive": values.get("adaptive"),
            "force": values.get("force"),
            "mince": values.get("mince"),
        }
        for seed in RANDOM_SEEDS:
            entry[f"random{seed}"] = values.get(f"random{seed}")
        entry.update(
            {
                "best": best,
                "best_order": winner,
                "best_ratio": best / natural if natural else "",
                "spread_max": spread,
            }
        )
        table.append(entry)
    write(
        "syrec-149.md",
        "SyReC 最终数据集：每个电路的全部配置",
        """149 条电路，每行一条；列是 14 种配置各自的门数中位数（5 次重复）。
`-` 表示该配置在这条电路上没跑完。`best_ratio` = 最好配置 ÷ 自然序；
`spread_max` 是这条电路上所有配置里最大的相对离散度（(max-min)/median）。
原始逐次重复数据见 `docs/experiment-data/syrec.md`。""",
        ["circuit", "group", "n_vars", "natural", "degree", "spectral",
         "recursive_bisection", "adaptive", "force", "mince"] +
        [f"random{s}" for s in RANDOM_SEEDS] +
        ["best", "best_order", "best_ratio", "spread_max"],
        table,
    )


def syrecFiveOrders() -> None:
    """One row per circuit, only the five structural orders."""
    groups = groupMap()
    rows = [
        r
        for r in read("syrec_dd_order_reps149_force_mince.csv")
        if r["status"] == "ok"
    ]
    byCircuit: dict[str, dict[str, float]] = {}
    varsOf: dict[str, str] = {}
    for row in rows:
        if row["order"] == "random":
            continue
        byCircuit.setdefault(row["circuit"], {})[row["order"]] = number(
            row, "gates_median"
        )
        varsOf[row["circuit"]] = row["n_vars"]
    table = []
    for circuit in sorted(byCircuit, key=lambda c: (int(varsOf[c]), c)):
        values = byCircuit[circuit]
        natural = values.get("natural")
        best = min(values.values())
        winner = min(values, key=values.get)
        table.append(
            {
                "circuit": circuit,
                "group": groups.get(
                    circuit,
                    "random" if pathlib.Path(circuit).stem.startswith("rand")
                    else "SyReC seal",
                ),
                "n_vars": varsOf[circuit],
                **values,
                "best": best,
                "best_order": winner,
                "best_ratio": best / natural if natural else "",
            }
        )
    write(
        "syrec-149-7orders.md",
        "SyReC 最终数据集：只看 7 个结构序",
        """与 `syrec-149.md` 相同的电路，但只保留 natural / degree / spectral /
recursive_bisection / adaptive / force / mince 七列；`best` 与 `best_ratio` **只在这七列里取**，
所以不会引用未显示的随机序。`-` 表示该配置没跑完。
去掉 8 个随机序后，整体中位比 0.800、门数求和比 0.785（六序时是 0.812 / 0.791）；
MCNC 组 0.714 / 0.450。""",
        ["circuit", "group", "n_vars", "natural", "degree", "spectral",
         "recursive_bisection", "adaptive", "force", "mince", "best", "best_order",
         "best_ratio"],
        table,
    )


def orderingMatrix() -> None:
    """One row per circuit, one column group per strategy."""
    def loadNodes(names: list[str], column: str) -> dict[str, float]:
        nodes: dict[str, float] = {}
        for name in names:
            for row in read(name):
                if row.get("status") != "ok":
                    continue
                try:
                    nodes[pathlib.Path(row["circuit"]).name] = float(
                        row[column]
                    )
                except (KeyError, ValueError):
                    continue
        return nodes

    force = loadNodes(
        ["force_317_bench2.csv", "force_cycle10.csv"], "static_force_nodes"
    )
    mince = loadNodes(
        ["mince_317_bench.csv", "mince_cycle10.csv"], "static_mince_nodes"
    )
    rows = read("ordering_full_experiments.csv")
    table = []
    for row in rows:
        table.append(
            {
                "circuit": pathlib.Path(row["circuit"]).name,
                "qubits": row["qubits"],
                "gates": row["gates"],
                "status": row["status"],
                "original": row["original_nodes"],
                "degree": row["static_degree_nodes"],
                "spectral": row["static_spectral_nodes"],
                "rb": row["static_recursive_bisection_nodes"],
                "min_fill": row["static_min_fill_nodes"],
                "gate_dep": row["static_gate_dependency_nodes"],
                "best_static": row["static_best_nodes"],
                "sift": row["dynamic_sifting_nodes"],
                "sift_spectral": row["dynamic_sifting_spectral_nodes"],
                "force": force.get(pathlib.Path(row["circuit"]).name),
                "mince": mince.get(pathlib.Path(row["circuit"]).name),
            }
        )
    table.sort(key=lambda r: (int(r["qubits"] or 0), r["circuit"]))
    write(
        "ordering-317.md",
        "mqt-core 决策图构建：每个电路的全部策略",
        """317 条 RevLib 电路，每行一条；值是功能 DD 的节点数。
`original` 是原始比特顺序，其余列是各静态策略；`sift` 是建完再 sifting，
`sift_spectral` 以谱序为起点；`force` 与 `mince` 是 2026-10-21 新增的策略。
`-` 表示读取失败、超时或超过 40 比特。
逐策略耗时与峰值内存见 `docs/experiment-data/ordering.md`。""",
        ["circuit", "qubits", "gates", "status", "original", "degree", "spectral",
         "rb", "min_fill", "gate_dep", "best_static", "sift", "sift_spectral",
         "force", "mince"],
        table,
    )


def qcecMatrix() -> None:
    """One row per circuit pair and checker."""
    rows = read("qcec_static_ordering_by_policy.csv")
    table = []
    for row in sorted(rows, key=lambda r: (r["pair"], r["checker"])):
        table.append(
            {
                "pair": row["pair"],
                "checker": row["checker"],
                "ref_qubits": row["ref_qubits"],
                "impl_qubits": row["impl_qubits"],
                "none_ms": row["none_time_ms"],
                "gate_dep_ms": row["gate_dependency_time_ms"],
                "degree_ms": row["degree_time_ms"],
                "spectral_ms": row["spectral_time_ms"],
                "rb_ms": row["recursive_bisection_time_ms"],
                "best": row["best_policy"],
                "best_ms": row["best_time_ms"],
                "best_ratio": row["best_ratio"],
            }
        )
    write(
        "qcec-20-pairs.md",
        "QCEC 等价性检查：每对电路的全部策略",
        """20 对电路 × 3 个检查器，每行一个组合；值是检查耗时（毫秒，3 次重复的中位）。
`construction` 是构造决策图的检查器（默认关闭），`simulation` 与 `alternating`
是默认流程会走的检查器。逐次运行的 900 行数据见 `docs/experiment-data/qcec.md`。""",
        ["pair", "checker", "ref_qubits", "impl_qubits", "none_ms", "gate_dep_ms",
         "degree_ms", "spectral_ms", "rb_ms", "best", "best_ms", "best_ratio"],
        table,
    )


def ddsimMatrix() -> None:
    """One row per circuit, one column per order."""
    rows = read("ddsim_density_matrix.csv")
    grouped: dict[str, dict[str, list[float]]] = {}
    for row in rows:
        if row["status"] != "ok":
            continue
        grouped.setdefault(row["circuit"], {}).setdefault(row["order"], []).append(
            number(row, "simulation_time_ms")
        )
    table = []
    for circuit in sorted(grouped):
        orders = grouped[circuit]
        entry = {"circuit": circuit}
        for order in ["none", "gate_dependency", "degree", "spectral",
                      "recursive_bisection"]:
            values = orders.get(order)
            entry[order] = statistics.median(values) if values else None
        base = entry.get("none")
        best = min((v for v in entry.values() if isinstance(v, float)), default=None)
        entry["best_ratio"] = best / base if base and best else None
        table.append(entry)
    write(
        "ddsim-16.md",
        "DDSIM 密度矩阵仿真：每个电路的全部变量序",
        """16 条电路，每行一条；值是仿真耗时（毫秒，10 次重复的中位）。
`none` 是原始顺序。有 1 条电路在 5 种序下全部超时。
逐次运行的 160 行数据见 `docs/experiment-data/ddsim.md`。""",
        ["circuit", "none", "gate_dependency", "degree", "spectral",
         "recursive_bisection", "best_ratio"],
        table,
    )


def applicationMatrix() -> None:
    """One row per circuit for E1 and E3."""
    table = []
    for name, label in [
        ("application_e1_statevector.csv", "E1 statevector"),
        ("application_e3_unitary.csv", "E3 unitary"),
    ]:
        for row in read(name):
            if row["status"] != "ok":
                continue
            table.append(
                {
                    "application": label,
                    "circuit": pathlib.Path(row["circuit"]).name,
                    "qubits": row["qubits"],
                    "gates": row["gates"],
                    "original_nodes": row["original_nodes"],
                    "degree_nodes": row["degree_nodes"],
                    "spectral_nodes": row["spectral_nodes"],
                    "rb_nodes": row["recursive_bisection_nodes"],
                    "best": row["best_strategy"],
                    "best_nodes": row["best_nodes"],
                    "best_ratio": (
                        number(row, "best_nodes") / number(row, "original_nodes")
                        if number(row, "original_nodes")
                        else None
                    ),
                }
            )
    write(
        "application-e1-e3.md",
        "应用层 E1/E3：每个电路的全部变量序",
        """E1 是态矢量仿真的 DD 构建（29 条），E3 是算符级构建（40 条）；值是节点数。
`best_ratio` = 最好的策略 ÷ 原始顺序。逐策略耗时与算序时间见
`docs/experiment-data/application.md`。""",
        ["application", "circuit", "qubits", "gates", "original_nodes",
         "degree_nodes", "spectral_nodes", "rb_nodes", "best", "best_nodes",
         "best_ratio"],
        table,
    )


def racingMatrix() -> None:
    """One row per circuit, both budgets side by side."""
    rows = read("syrec_dd_portfolio_149.csv")
    grouped: dict[str, dict[str, dict[str, str]]] = {}
    for row in rows:
        grouped.setdefault(row["circuit"], {})[row["candidates"]] = row
    table = []
    for circuit in sorted(grouped):
        budgets = grouped[circuit]
        five = budgets.get("5", {})
        eight = budgets.get("8", {})
        table.append(
            {
                "circuit": circuit,
                "seq5_s": number(five, "sequential_ms") / 1000 if five else None,
                "raced5_s": number(five, "raced_ms") / 1000 if five else None,
                "ratio5": number(five, "race_speedup") if five else None,
                "match5": five.get("match"),
                "seq8_s": number(eight, "sequential_ms") / 1000 if eight else None,
                "raced8_s": number(eight, "raced_ms") / 1000 if eight else None,
                "ratio8": number(eight, "race_speedup") if eight else None,
                "match8": eight.get("match"),
            }
        )
    write(
        "racing-147.md",
        "racing portfolio：每个电路的两种预算",
        """147 条电路（149 条里 `hamming_15`/`urf6` 的候选跑不完），每行一条。
`seq` 是顺序跑完所有候选的耗时，`raced` 是剪枝后的耗时，`ratio` 是两者之比
（小于 1 表示 racing 更快），`match` = 1 表示两侧最优门数相同。
逐配置的 294 行数据见 `docs/experiment-data/syrec-final.md`。""",
        ["circuit", "seq5_s", "raced5_s", "ratio5", "match5",
         "seq8_s", "raced8_s", "ratio8", "match8"],
        table,
    )


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    syrecMatrix()
    syrecFiveOrders()
    orderingMatrix()
    qcecMatrix()
    ddsimMatrix()
    applicationMatrix()
    racingMatrix()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
