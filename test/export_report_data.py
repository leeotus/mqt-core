#!/usr/bin/env python3
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
"""Write the CSVs that go with the progress report.

The report lives in ``docs/report/research-progress.md`` and cites the tables
that this script writes into ``docs/report/data``. Every table is a projection
of the raw data under ``/home/flareon/workshop/results``: columns are selected
and renamed for printing, no row is aggregated away.

    python3 test/export_report_data.py
"""

from __future__ import annotations

import csv
import pathlib

RESULTS = pathlib.Path("/home/flareon/workshop/results")
OUT = pathlib.Path("/home/flareon/workshop/mqt-core/docs/report/data")


def read(name: str, keep=None) -> list[dict[str, str]]:
    """Read one result table, optionally keeping only some columns."""
    with (RESULTS / name).open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if keep is None:
        return rows
    return [{column: row.get(column, "") for column in keep} for row in rows]


def write(name: str, columns: list[str], rows: list[dict[str, str]]) -> None:
    """Write one table and report how large it is.

    The byte order mark lets spreadsheet programs on Windows read the Chinese
    text, which matters because these tables are meant to be printed.
    """
    with (OUT / name).open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {name}: {len(rows)} rows")


def number(row: dict[str, str], field: str) -> float:
    """Read a numeric field, treating anything unparsable as zero."""
    try:
        return float(row.get(field, "") or 0)
    except ValueError:
        return 0.0


def syrecPerCircuit() -> list[dict[str, str]]:
    """Compare every circuit of the final SyReC dataset with its natural order."""
    rows = [
        row
        for row in read("syrec_dd_order_reps149_force.csv")
        if row["status"] == "ok"
    ]
    byCircuit: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        byCircuit.setdefault(row["circuit"], []).append(row)
    base = {
        circuit: next(
            (
                number(row, "gates_median")
                for row in entries
                if row["order"] == "natural"
            ),
            None,
        )
        for circuit, entries in byCircuit.items()
    }
    base = {c: g for c, g in base.items() if g}

    def gates(entries, orders=None):
        picked = [
            row for row in entries if orders is None or row["order"] in orders
        ]
        return min(picked, key=lambda row: number(row, "gates_median")) if picked else None

    structural = {"degree", "spectral", "recursive_bisection", "adaptive"}
    records = []
    for circuit, entries in byCircuit.items():
        if circuit not in base:
            continue
        natural = base[circuit]
        overall = gates(entries)
        winner = overall["order"]
        if winner == "random":
            winner = f"random({overall['seed']})"
        # Several configurations often reach the same minimum, and the shortest
        # table then reports whichever one comes first. Say so instead: a row
        # whose minimum is reached by more than one configuration carries
        # `tie(n)` and the reader knows the name is not a ranking.
        minimum = number(overall, "gates_median")
        tied = sum(
            1 for row in entries if number(row, "gates_median") == minimum
        )
        records.append(
            {
                "circuit": circuit,
                "n_vars": entries[0]["n_vars"],
                "natural_gates": int(natural),
                "degree_gates": int(number(gates(entries, {"degree"}), "gates_median"))
                if gates(entries, {"degree"})
                else "",
                "spectral_gates": int(number(gates(entries, {"spectral"}), "gates_median"))
                if gates(entries, {"spectral"})
                else "",
                "recursive_bisection_gates": int(
                    number(gates(entries, {"recursive_bisection"}), "gates_median")
                )
                if gates(entries, {"recursive_bisection"})
                else "",
                "adaptive_gates": int(number(gates(entries, {"adaptive"}), "gates_median"))
                if gates(entries, {"adaptive"})
                else "",
                "force_gates": int(number(gates(entries, {"force"}), "gates_median"))
                if gates(entries, {"force"})
                else "",
                "random_best_gates": int(number(gates(entries, {"random"}), "gates_median"))
                if gates(entries, {"random"})
                else "",
                "best_of_four_gates": int(
                    number(gates(entries, structural), "gates_median")
                ),
                "best_gates": int(number(overall, "gates_median")),
                "best_order": winner if tied == 1 else f"tie({tied})",
                "configs_at_best": tied,
                "best_ratio": round(
                    number(overall, "gates_median") / natural, 4
                ),
                "improved": "yes" if number(overall, "gates_median") < natural else "no",
            }
        )
    records.sort(key=lambda record: (float(record["n_vars"]), record["circuit"]))
    return records


DIRECTIONS = [
    {
        "direction": "mqt-core 静态变量序（degree / spectral / recursive bisection）",
        "experiment": "317 条 RevLib 电路上构建功能 DD（算符级），与原始序比较",
        "verdict": "部分可行",
        "key_metric": "节点数中位比 0.900，258 条里 184 条变小；谱排序中位比 1.000",
        "evidence": "02-ordering-317-per-circuit.csv",
    },
    {
        "direction": "mqt-core 动态重排（构建中途触发）",
        "experiment": "同上，节点数超过阈值就地 sift",
        "verdict": "可行",
        "key_metric": "节点数中位比 0.750，228/258 条变小；199 条优于最佳静态序，中位增益 1.23x、最大 50.5x",
        "evidence": "02-ordering-317-per-circuit.csv",
    },
    {
        "direction": "策略 min_fill 与 gate_dependency",
        "experiment": "同上，作为静态候选参与比较",
        "verdict": "不可行（已否决）",
        "key_metric": "不进入最终候选池；min_fill 在算符级无稳定收益，gate_dependency 与调用图绑定、收益最小",
        "evidence": "02-ordering-317-per-circuit.csv",
    },
    {
        "direction": "应用到 QCEC：默认流程",
        "experiment": "20 对电路 x 5 种序 x 仿真/交替/构造三个检查器 x 3 次重复",
        "verdict": "不可行",
        "key_metric": "默认流程（向量态仿真 + 交替）各策略中位比 0.96–1.04，无收益；构造检查器上最大加速 1182x，但它默认关闭",
        "evidence": "03-qcec-per-pair.csv",
    },
    {
        "direction": "应用到 DDSIM 密度矩阵 / 噪声仿真",
        "experiment": "16 条电路 x 5 种序 + 10 种动态臂",
        "verdict": "不可行",
        "key_metric": "局部噪声把密度矩阵推成对角，规模锁死在 2^n；静态序耗时中位比 0.93–2.07，动态臂 2.9–13.2 倍",
        "evidence": "04-ddsim-density-matrix.csv, 05-ddsim-dynamic-arms.csv",
    },
    {
        "direction": "应用层 E1：态矢量仿真的 DD 构建",
        "experiment": "29 条电路 x 3 种静态序",
        "verdict": "不可行",
        "key_metric": "节点数中位比 1.000，只有 8/29 条被改善；算序耗时本身可忽略（<0.02 ms）",
        "evidence": "06-application-e1-e3.csv",
    },
    {
        "direction": "应用层 E3：算符级 DD 构建",
        "experiment": "40 条 RevLib 电路 x 3 种静态序",
        "verdict": "可行",
        "key_metric": "degree 节点数中位比 0.665、耗时中位比 0.555，29/40 条改善；34/40 条被某个策略改善",
        "evidence": "06-application-e1-e3.csv",
    },
    {
        "direction": "SyReC 的 QMDD 综合：单一静态序",
        "experiment": "149 条电路 x 13 种配置 x 5 次重复",
        "verdict": "取决于函数结构",
        "key_metric": "整体中位比 0.987；小型可逆基准上 1.000（等于没用），MCNC 真实函数上 0.841（门数求和 0.607）",
        "evidence": "07-syrec-149-per-circuit.csv",
    },
    {
        "direction": "SyReC：多策略组合",
        "experiment": "同上，四个结构序取最好",
        "verdict": "可行，但候选池必须含自然序",
        "key_metric": "整体中位 0.829／求和 1.290；MCNC 上 0.714／0.463 且最坏仅 1.065；不含自然序时被 hamming_15 拖到 3.214 倍",
        "evidence": "07-syrec-149-per-circuit.csv",
    },
    {
        "direction": "SyReC：racing portfolio（可部署的选序方法）",
        "experiment": "83 条 MCNC 真实函数 x 2 种预算，顺序跑与剪枝跑对照",
        "verdict": "可行",
        "key_metric": "中位时间比 0.443（5 候选）/ 0.308（8 候选），最差 0.823；166/166 个配置与顺序跑结果一致",
        "evidence": "08-syrec-racing.csv",
    },
    {
        "direction": "FORCE 静态序（2026-10-21 新增）",
        "experiment": "149 条 SyReC + 317 条 RevLib；单跑并加入候选池",
        "verdict": "可行（作为候选池成员）",
        "key_metric": (
            "SyReC 单独中位 0.962；六序池 0.812/0.791（五序 0.829/0.799）。"
            "算符级 DD：三策略池中位 0.901 → 加 FORCE 0.821，改善 183→196"
        ),
        "evidence": "force_ordering_summary.md",
    },
    {
        "direction": "MINCE 静态序（2026-10-21 新增）",
        "experiment": "149 条 SyReC + 317 条 RevLib；单跑并加入候选池",
        "verdict": "可行（作为候选池成员）",
        "key_metric": (
            "SyReC 单独中位 0.927、改善 80/147；七序池 0.800/0.785（六序 0.812/0.791）。"
            "算符级 DD：中位 0.996，最差 21.8x；加入池后 0.821→0.820、求和 0.085→0.083"
        ),
        "evidence": "force_ordering_summary.md",
    },
    {
        "direction": "合成器非确定性（上游缺陷）",
        "experiment": "同电路同序重复合成",
        "verdict": "已定位并修复",
        "key_metric": "修复前 18% 的配置抖动、最差 21.8%；修复后 3.6%、最差 3.28%，代价约 +22% 时间",
        "evidence": "syrec_determinism.md",
    },
    {
        "direction": "工具崩溃 std::out_of_range（自研工具缺陷）",
        "experiment": "MCNC 电路建序时崩溃",
        "verdict": "已定位并修复",
        "key_metric": "9 个配置由崩溃恢复为正常综合；原因是策略返回的置换不含无门触及的比特",
        "evidence": "mydocs/syrec-dd-order.md 第 10.1 节",
    },
]


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)

    write(
        "01-direction-summary.csv",
        ["direction", "experiment", "verdict", "key_metric", "evidence"],
        DIRECTIONS,
    )

    ordering = [
        row
        for row in read("ordering_full_summary.csv")
        if row["status"] == "ok"
    ]
    write(
        "02-ordering-317-per-circuit.csv",
        [
            "circuit",
            "qubits",
            "gates",
            "original_nodes",
            "best_static_strategy",
            "best_static_nodes",
            "dynamic_best_nodes",
            "gain_vs_original",
            "gain_vs_best_static",
        ],
        [
            {
                "circuit": pathlib.Path(row["circuit"]).name,
                **{k: row[k] for k in [
                    "qubits", "gates", "original_nodes", "best_static_strategy",
                    "best_static_nodes", "dynamic_best_nodes", "gain_vs_original",
                    "gain_vs_best_static",
                ]},
            }
            for row in sorted(ordering, key=lambda r: int(r["qubits"]))
        ],
    )

    write(
        "03-qcec-per-pair.csv",
        [
            "pair",
            "checker",
            "ref_qubits",
            "impl_qubits",
            "none_time_ms",
            "gate_dependency_time_ms",
            "degree_time_ms",
            "spectral_time_ms",
            "recursive_bisection_time_ms",
            "best_policy",
            "best_time_ms",
            "best_ratio",
        ],
        sorted(
            read("qcec_static_ordering_by_policy.csv"),
            key=lambda row: (row["pair"], row["checker"]),
        ),
    )

    write(
        "04-ddsim-density-matrix.csv",
        [
            "circuit",
            "qubits",
            "gates",
            "none_ms",
            "gate_dependency_ms",
            "degree_ms",
            "spectral_ms",
            "recursive_bisection_ms",
            "best_order",
            "best_speedup",
        ],
        read("ddsim_density_matrix_by_order.csv"),
    )

    arms = []
    for line in (RESULTS / "ddsim_dynamic_reordering_summary.md").read_text().splitlines():
        if line.startswith("| ") and not line.startswith("| arm") and "---" not in line:
            cells = [cell.strip() for cell in line.strip("|").split("|")]
            if len(cells) == 8:
                arms.append(
                    dict(
                        zip(
                            [
                                "arm", "circuits", "median_peak_ratio",
                                "median_final_ratio", "wins_peak", "median_time_ratio",
                                "median_tvd", "median_tvd_sorted",
                            ],
                            cells,
                        )
                    )
                )
    write(
        "05-ddsim-dynamic-arms.csv",
        [
            "arm", "circuits", "median_peak_ratio", "median_final_ratio",
            "wins_peak", "median_time_ratio", "median_tvd", "median_tvd_sorted",
        ],
        arms,
    )

    application = []
    for name, label in [
        ("application_e1_statevector.csv", "E1 statevector"),
        ("application_e3_unitary.csv", "E3 unitary"),
    ]:
        for row in read(name):
            if row["status"] != "ok":
                continue
            application.append(
                {
                    "application": label,
                    "circuit": pathlib.Path(row["circuit"]).name,
                    "qubits": row["qubits"],
                    "gates": row["gates"],
                    "original_nodes": row["original_nodes"],
                    "original_time_ms": row["original_time_ms"],
                    "degree_nodes": row["degree_nodes"],
                    "degree_time_ms": row["degree_time_ms"],
                    "degree_order_ms": row["degree_order_ms"],
                    "degree_nodes_ratio": row["degree_nodes_ratio"],
                    "spectral_nodes": row["spectral_nodes"],
                    "spectral_nodes_ratio": row["spectral_nodes_ratio"],
                    "recursive_bisection_nodes": row["recursive_bisection_nodes"],
                    "recursive_bisection_nodes_ratio": row["recursive_bisection_nodes_ratio"],
                    "best_strategy": row["best_strategy"],
                    "best_nodes": row["best_nodes"],
                }
            )
    write(
        "06-application-e1-e3.csv",
        [
            "application", "circuit", "qubits", "gates", "original_nodes",
            "original_time_ms", "degree_nodes", "degree_time_ms", "degree_order_ms",
            "degree_nodes_ratio", "spectral_nodes", "spectral_nodes_ratio",
            "recursive_bisection_nodes", "recursive_bisection_nodes_ratio",
            "best_strategy", "best_nodes",
        ],
        application,
    )

    write(
        "07-syrec-149-per-circuit.csv",
        [
            "circuit", "n_vars", "natural_gates", "degree_gates", "spectral_gates",
            "recursive_bisection_gates", "adaptive_gates", "random_best_gates",
            "force_gates",
            "best_of_four_gates", "best_gates", "best_order", "configs_at_best",
            "best_ratio", "improved",
        ],
        syrecPerCircuit(),
    )

    racing = read("syrec_dd_portfolio_149.csv")
    write(
        "08-syrec-racing.csv",
        [
            "circuit", "candidates", "best_sequential", "best_raced", "match",
            "sequential_ms", "raced_ms", "race_speedup", "aborted",
        ],
        sorted(
            racing,
            key=lambda row: (int(row["candidates"]), row["circuit"]),
        ),
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
