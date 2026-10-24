#!/usr/bin/env python3
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
"""Render the experiment tables of this workspace into ``docs/experiment-data``.

Every table is written from the raw CSV that the run produced: one row per
measurement, not a summary. The renderer shortens circuit paths, rounds floats
for printing and sorts the rows. It never aggregates, so a reader can look up
any single circuit and any single repetition.

    python3 test/render_experiment_data.py
"""

from __future__ import annotations

import csv
import pathlib

RESULTS = pathlib.Path("/home/flareon/workshop/results")
OUT = pathlib.Path("/home/flareon/workshop/mqt-core/docs/experiment-data")

# Circuit columns hold an absolute path in some tables and a bare file name in
# others. The report keeps the part below the benchmark directory.
CIRCUIT_PREFIXES = (
    "/home/flareon/workshop/igqmdd/assets/",
    "/home/flareon/workshop/results/ordering_experiments/revlib_qasm4/",
    "/home/flareon/workshop/results/ordering_experiments/revlib_qasm/",
    "/home/flareon/workshop/results/ordering_experiments/",
    "/home/flareon/workshop/QASMBench/",
    "/home/flareon/workshop/mqt-syrec/test/benchmark/",
    "/home/flareon/workshop/mqt-syrec/test/circuits/",
    "/home/flareon/workshop/mqt-syrec/test/",
)
CIRCUIT_COLUMNS = ("circuit", "pair")


def read_rows(path: pathlib.Path) -> list[dict[str, str]]:
    """Read one experiment table."""
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def clean_circuit(value: str) -> str:
    """Drop the benchmark prefix from a circuit path."""
    for prefix in CIRCUIT_PREFIXES:
        if value.startswith(prefix):
            return value.removeprefix(prefix)
    return value


def format_value(value: str) -> str:
    """Print a cell. Integers stay exact, floats keep three decimals."""
    text = (value or "").strip()
    if text == "":
        return ""
    text = text.replace("|", "\\|")
    try:
        number = float(text)
    except ValueError:
        return text
    if number != number or number in (float("inf"), float("-inf")):
        return text
    if number == int(number) and abs(number) < 1e15:
        return str(int(number))
    if abs(number) >= 1:
        return f"{number:.3f}".rstrip("0").rstrip(".")
    # Small values are probabilities and ratios. Four significant digits keep
    # them readable without inventing precision.
    return f"{number:.4g}"


def sort_rows(rows: list[dict[str, str]], keys: list[str]) -> list[dict[str, str]]:
    """Sort rows by the given columns, numerically where possible."""
    if not keys:
        return rows

    def key(row: dict[str, str]):
        """Build the sort key of one row."""
        parts = []
        for name in keys:
            value = (row.get(name) or "").strip()
            try:
                parts.append((0, float(value), ""))
            except ValueError:
                parts.append((1, 0.0, value))
        return parts

    return sorted(rows, key=key)


def table(columns: list[str], rows: list[dict[str, str]]) -> list[str]:
    """Render a list of rows as one Markdown table."""
    lines = ["| " + " | ".join(columns) + " |"]
    lines.append("| " + " | ".join("---" for _ in columns) + " |")
    for row in rows:
        cells = []
        for name in columns:
            value = row.get(name, "") or ""
            if name in CIRCUIT_COLUMNS:
                value = clean_circuit(value)
            cells.append(format_value(value))
        lines.append("| " + " | ".join(cells) + " |")
    return lines


def section(dataset: dict, level: int = 2) -> list[str]:
    """Render one dataset as a heading, its glossary and its table."""
    path = RESULTS / dataset["path"]
    rows = read_rows(path)
    columns = dataset.get("columns") or (list(rows[0].keys()) if rows else [])
    rows = sort_rows(rows, dataset.get("sort", []))
    lines = [
        "#" * level + " " + dataset["title"],
        "",
        f"来源：`results/{dataset['path']}`，{len(rows)} 行。",
        "",
        dataset["blurb"].strip(),
        "",
    ]
    if dataset.get("glossary"):
        lines.append("| 列 | 含义 |")
        lines.append("| --- | --- |")
        lines.extend(f"| `{name}` | {text} |" for name, text in dataset["glossary"])
        lines.append("")
    if rows:
        lines.extend(table(columns, rows))
    else:
        lines.append("（该文件没有数据行。）")
    lines.append("")
    if dataset.get("derived"):
        lines.extend(dataset["derived"](rows, level + 1))
        lines.append("")
    return lines


STRUCTURAL_ORDERS = ("degree", "spectral", "recursive_bisection", "adaptive")


def syrec_comparison(rows: list[dict[str, str]], level: int) -> list[str]:
    """Compare every circuit with its natural order, one row per circuit.

    The raw table holds the measurements. This view holds the comparison: the
    gate count of each order as a ratio against the natural order of the same
    circuit. A ratio below one means fewer gates than the natural order.
    """
    by_circuit: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        # A configuration that ran out of time carries a placeholder row with
        # zero gates. It is not a result, so the comparison leaves it out.
        if row.get("status") == "ok":
            by_circuit.setdefault(row["circuit"], []).append(row)

    def gates(row: dict[str, str]) -> float:
        try:
            return float(row.get("gates_median") or 0)
        except ValueError:
            return 0.0

    def best(entries, orders=None):
        picked = [
            row
            for row in entries
            if orders is None or row["order"] in orders
        ]
        return min(picked, key=gates) if picked else None

    out = [
        "#" * level + " 逐电路对比：每个序相对同一条电路的自然序",
        "",
        "这一张是派生表：原始测量在上一张，这里只做「除以同一条电路的自然序门数」。"
        "`< 1` 表示比自然序小。`四序最好` 是 degree、spectral、recursive_bisection、"
        "adaptive 四者取最小（**不含自然序**）；`全部最好` 是包括自然序与 8 个随机序"
        "在内的所有配置取最小，也就是 oracle。门数并列时，`最好序` 取排序在前的"
        "那一个，不代表它是唯一能赢的序。",
        "",
        "| circuit | n_vars | natural 门数 | degree | spectral | "
        "recursive_bisection | adaptive | random 最好 | 四序最好 | 全部最好 | 最好序 |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    records = []
    for circuit, entries in by_circuit.items():
        natural = next((row for row in entries if row["order"] == "natural"), None)
        if natural is None or gates(natural) == 0:
            continue
        base = gates(natural)
        ratios = {}
        for order in STRUCTURAL_ORDERS:
            entry = best(entries, {order})
            ratios[order] = gates(entry) / base if entry else None
        random_row = best(entries, {"random"})
        ratios["random"] = gates(random_row) / base if random_row else None
        structural = best(entries, STRUCTURAL_ORDERS)
        overall = best(entries)
        ratio_four = gates(structural) / base if structural else None
        ratio_all = gates(overall) / base if overall else None
        label = ""
        if overall is not None:
            label = overall["order"]
            if label == "random":
                label = f"random({overall['seed']})"
        records.append(
            {
                "circuit": circuit,
                "n_vars": natural["n_vars"],
                "natural": base,
                **ratios,
                "four": ratio_four,
                "all": ratio_all,
                "winner": label,
                "overall": gates(overall) if overall else 0,
            }
        )
    records.sort(key=lambda record: (float(record["n_vars"]), record["circuit"]))

    def show(value) -> str:
        return "-" if value is None else format_value(f"{value}")

    for record in records:
        cells = [
            clean_circuit(record["circuit"]),
            format_value(record["n_vars"]),
            format_value(f"{record['natural']}"),
            show(record.get("degree")),
            show(record.get("spectral")),
            show(record.get("recursive_bisection")),
            show(record.get("adaptive")),
            show(record.get("random")),
            show(record.get("four")),
            show(record.get("all")),
            record["winner"],
        ]
        out.append("| " + " | ".join(cells) + " |")
    out.append("")
    out.append(
        "**怎么读**：`spectral` 那一列的改善和恶化相当，中位比正好落在 1.000，"
        "所以单看这一列会觉得什么都没发生。`四序最好` 的中位是 0.848，但它在"
        "`hamming_15` 上选到 3.214 倍，按门数求和会变成 1.501 倍。只有 `全部最好`"
        "（把自然序留在候选池里）在每条电路上都不超过 1.000。"
    )
    return out


ORDERING_GLOSSARY = [
    ("circuit", "RevLib `.real` 电路，路径相对 igqmdd 的 assets 目录"),
    ("qubits", "电路的量子比特数"),
    ("gates", "电路的门的数量"),
    ("status", "`ok`；`timeout` 单电路超过 1 小时；`read-error` 文件读不进；"
               "`too-many-qubits` 超过工具上限"),
    ("original_nodes", "原始（物理）变量序下功能 DD 的节点数"),
    ("original_time_ms", "原始序下构建功能 DD 的耗时，毫秒"),
    ("static_<策略>_nodes", "先用该策略算出变量序，再用它建 DD 的节点数"),
    ("static_<策略>_time_ms", "该序下的**构建**耗时；这一版表没有单列算序耗时"),
    ("static_best_strategy", "四种静态策略里节点数最小的那个"),
    ("static_best_nodes", "上述最优静态策略的节点数"),
    ("static_best_time_ms", "上述最优静态策略的构建耗时"),
    ("dynamic_sifting_nodes", "先按原始序把 DD 建完、再对成品 sift 之后的节点数"),
    ("dynamic_sifting_time_ms", "构建加事后 sift 的总耗时"),
    ("dynamic_sifting_exchanges", "事后 sift 交换的层数"),
    ("dynamic_sifting_spectral_*", "同上，但以 spectral 序作为起点"),
    ("peak_rss_mb", "进程峰值常驻内存，兆字节"),
]

ORDERING_ARM_GLOSSARY = [
    ("static_<策略>_nodes", "用该静态策略的变量序构建 DD 的节点数"),
    ("identity<T>_nodes", "**构建期重排**：从原始序开始，节点数超过 T 就就地 sift，"
                          "结束时的节点数"),
    ("identity<T>_peak", "构建期重排过程中 DD 的峰值节点数"),
    ("identity<T>_time_ms", "整条构建（含重排）的耗时"),
    ("identity<T>_reorder_time_ms", "其中花在重排上的时间"),
    ("identity<T>_reorderings", "触发重排的次数"),
    ("identity<T>_exchanges", "重排交换的层数"),
    ("spectral<T>_*", "同上，但构建期重排的起点是 spectral 序"),
    ("best_static_nodes", "五种静态策略里最小的节点数"),
    ("best_dynamic_nodes", "四个动态臂里最小的节点数"),
    ("best_dynamic_arm", "取得最小节点数的那个动态臂"),
    ("dynamic_gain_vs_static", "`best_dynamic_nodes / best_static_nodes`，"
                               "小于 1 表示动态更好"),
    ("peak_shrink", "静态峰值节点数除以动态峰值节点数"),
]

QCEC_GLOSSARY = [
    ("pair", "QCEC 基准里的一对电路：参考电路与它的转译实现"),
    ("rep", "重复编号；每次运行一个独立进程，避免上一次运行的热身偏差"),
    ("policy", "给实现电路选变量序用的策略；`none` 是原始序"),
    ("checker", "QCEC 的检验器：`alternating` 交替判定、`simulation` 仿真、"
                "`construction` 构造 DD；`default` 是 QCEC 自己选的默认流程"),
    ("status", "`ok` 或 `timeout`（单次等价性检查 30 分钟上限）"),
    ("equivalent", "检验器的判定结果"),
    ("preprocessing_time_ms", "等价性检查的预处理耗时"),
    ("check_time_ms", "检验本身的耗时"),
    ("total_time_ms", "两者之和"),
    ("peak_rss_mb", "进程峰值常驻内存，兆字节"),
    ("started_simulations", "仿真的启动次数，只有 simulation 检验器会用到"),
    ("performed_simulations", "完整跑完的仿真次数"),
    ("selection_time_ms", "在若干仿真之间做选择花掉的时间"),
    ("ref_dd_nodes", "参考电路的功能 DD 节点数"),
    ("ref_build_ms", "构建那张 DD 的耗时"),
]

DDSIM_GLOSSARY = [
    ("circuit", "DDSIM 密度矩阵基准里的电路"),
    ("rep", "重复编号"),
    ("order", "仿真时用的变量序策略；`none` 是原始序"),
    ("status", "`ok` 或 `timeout`"),
    ("qubits", "量子比特数"),
    ("gates", "门的数量"),
    ("nodes", "仿真过程中密度矩阵 DD 的节点数"),
    ("simulation_time_ms", "仿真耗时，毫秒"),
    ("arm", "动态重排的实验臂，见「构建期动态重排」一节里的对照表"),
    ("applied_gates", "超时前已经作用到状态上的门数"),
    ("peak_nodes", "仿真过程中 DD 的峰值节点数"),
    ("final_nodes", "仿真结束时的节点数"),
    ("reorderings", "触发重排的次数"),
    ("reorder_time_ms", "花在重排上的时间"),
    ("reorder_share", "重排时间占总时间的比例"),
    ("time_ratio", "相对自然序运行的耗时比"),
    ("peak_ratio", "相对自然序运行的峰值节点比"),
    ("final_ratio", "相对自然序运行的结束时节点比"),
    ("tvd_informative", "统计量是否与非对角项有关（局部噪声会把密度矩阵推成对角）"),
    ("tvd_vs_natural", "与自然序运行之间的总变差距离"),
    ("sorted_vs_natural", "排序后分布与自然序分布的总变差距离"),
]

APPLICATION_GLOSSARY = [
    ("circuit", "`qasm` 电路（E1）或 RevLib `.real` 电路（E3）"),
    ("qubits", "量子比特数"),
    ("gates", "门的数量"),
    ("status", "`ok` 或失败原因"),
    ("original_nodes", "原始变量序下 DD 的节点数"),
    ("original_time_ms", "原始序下构建 DD 的耗时"),
    ("<策略>_nodes", "用该静态策略的变量序构建 DD 的节点数"),
    ("<策略>_time_ms", "该序下构建 DD 的耗时"),
    ("<策略>_order_ms", "**计算该变量序本身**的耗时（这一组把算序与构建分开记）"),
    ("<策略>_nodes_ratio", "相对原始序的节点数比"),
    ("<策略>_time_ratio", "相对原始序的耗时比"),
    ("best_strategy", "节点数最小的策略"),
    ("best_nodes", "该策略的节点数"),
    ("peak_rss_mb", "进程峰值常驻内存，兆字节"),
]

SYREC_GLOSSARY = [
    ("circuit", "可逆函数的 PLA 真值表；`random/` 前缀的是生成的可逆电路"),
    ("n_vars", "函数的变量数"),
    ("order", "变量序策略：`natural` / `degree` / `spectral` / "
              "`recursive_bisection` / `adaptive`（构建期贪心选序）/ `random`"),
    ("seed", "只有 `random` 会用到：1 到 8 号随机变量序"),
    ("status", "`ok` 或 `timeout`（单配置 30 分钟上限）"),
    ("reps", "同一进程里重复合成的次数"),
    ("gates_median", "重复合成的门数中位数——这一列才是产出"),
    ("gates_min", "重复合成里最少的门数"),
    ("gates_max", "重复合成里最多的门数"),
    ("dd_nodes", "合成用的功能 DD 的节点数"),
    ("elapsed_ms", "`reps` 次合成的总耗时"),
    ("verified", "1 表示合成线路的功能等于它读入的 DD"),
    ("gates", "单次运行版的门数"),
    ("dd_build_ms", "建功能 DD 的耗时"),
    ("synth_ms", "从 DD 读出电路的耗时"),
    ("total_ms", "`dd_build_ms` 加 `synth_ms`"),
    ("gate_ratio", "相对同一电路 `natural` 运行的门数比"),
    ("dd_ratio", "相对同一电路 `natural` 运行的 DD 节点数比"),
    ("verdict", "`equivalent`、`MISMATCH`，或 `not-in-dataset`"
                "（数据集里该配置没跑完）"),
    ("natural_gates", "同一条电路自然序电路的门数"),
    ("order_gates", "换序后电路的门数"),
    ("candidates", "racing 的候选变量序个数：5 个固定候选，或 8 个（再加 3 个随机序）"),
    ("best_sequential", "顺序跑完所有候选得到的最小门数"),
    ("best_raced", "racing 得到的最小门数"),
    ("aborted", "被剪枝杀掉的候选个数"),
    ("sequential_ms", "顺序跑完所有候选的耗时"),
    ("raced_ms", "racing 的耗时"),
    ("race_speedup", "`raced_ms / sequential_ms`，小于 1 表示 racing 更快"),
    ("match", "1 表示 racing 的最优门数与顺序跑相同"),
    ("max_width", "DD 中单层最多的节点数"),
    ("levels", "至少有一个节点的层数"),
    ("signature_sum", "每个节点四棵子树上的路径类数之和"),
    ("pair_sum_squares", "同上，但按两棵子树路径类数之和的平方累加"),
    ("triggers1/2/4", "会让 P1 / P2 / P4 例程发门的节点数"),
    ("estimate1/2/4", "对 P1 / P2 / P4 发门数的静态估计"),
]

EARLY_GLOSSARY = [
    ("circuit", "QASMBench 电路或从 RevLib 转出的 qasm 电路"),
    ("qubits", "量子比特数"),
    ("gates", "门的数量"),
    ("strategy", "变量序策略；`identity` 是原始序"),
    ("orientation", "序的方向：`forward` 或 `reversed`"),
    ("time_ms", "构建功能 DD 的墙钟时间"),
    ("nodes", "构建完成后 DD 的节点数"),
    ("peak_nodes", "构建过程中的峰值节点数"),
    ("inserts", "唯一表插入的节点总数，衡量真实工作量"),
    ("verified", "换序后算符是否与默认序一致；`n/a` 表示这一轮没有比对"),
]


SYREC_FINAL_DATASET = {
    "path": "syrec_dd_order_reps149.csv",
    "title": "最终数据集：149 条电路、13 种配置、5 次重复（1937 行）",
    "blurb": """每条电路、每种变量序配置各跑 5 次合成，报告门数的中位数与极值。
`random` 有 8 个种子，其余每个配置一行。149 条电路由三部分组成：29 条随 SyReC
提供的可逆函数、24 条按随机门序列生成的可逆函数、13 条从 RevLib 补筛出来的电路，
以及 83 条 MCNC/LGSynth 真实逻辑函数的可逆嵌入。每行都是测量的原始值，没有聚合。
三部分的来源见 `mcnc-dataset/README.md` 与 `syrec.md`。""",
    "glossary": SYREC_GLOSSARY,
    "sort": ["n_vars", "circuit", "order", "seed"],
    "derived": syrec_comparison,
}

SYREC_SEAL_DATASET = {
    "path": "syrec_dd_order_reps64.csv",
    "title": "53 条封版数据集（689 行，最终数据集的子集）",
    "blurb": """第一批封版的数据集：29 条随 SyReC 提供的电路加 24 条随机可逆电路，
13 种配置各跑 5 次。它现在是 149 条最终数据集的一个子集。""",
    "glossary": SYREC_GLOSSARY,
    "sort": ["n_vars", "circuit", "order", "seed"],
    "derived": syrec_comparison,
}

SYREC_PORTFOLIO_DATASET = {
    "path": "syrec_dd_portfolio_149.csv",
    "title": "racing portfolio：147 条电路、2 种预算（294 行）",
    "blurb": """顺序跑完所有候选与边跑边剪枝的对照。剪枝依据是门数单调递增：某个候选
一旦涨过当前最好已完成候选的门数就杀掉它。`best_sequential` 与 `best_raced` 是两侧
各自的最优门数，`race_speedup` 是 racing 耗时除以顺序耗时。

覆盖最终数据集 149 条里的 147 条（`hamming_15` 与 `urf6` 的候选本身跑不完，排除）。
数据由三轮组成：83 条 MCNC 真实函数、51 条封版电路（按当前二进制重跑）、13 条 RevLib
电路。""",
    "glossary": SYREC_GLOSSARY,
    "sort": ["circuit", "candidates"],
}


GROUPS: list[tuple[str, str, str, list[dict]]] = [
    (
        "ordering.md",
        "重排实验数据：mqt-core 决策图构建",
        """这一组测的是**经典决策图构建**：输入 igqmdd 的 RevLib 电路，比较原始变量序、
几种静态变量序，以及事后重排与构建期重排。每条电路一行，所有策略都保留，没有聚合。
`min_fill` 与 `gate_dependency` 两组策略后来被否决，表里保留只为存档。""",
        [
            {
                "path": "ordering_full_experiments.csv",
                "title": "静态重排主实验（全表，317 条电路）",
                "blurb": """每行一条电路。`original_*` 是原始变量序；`static_<策略>_*`
是先用该策略算序、再用它建同一张 DD；`static_best_*` 是其中节点数最小的；
`dynamic_sifting_*` 是**先把 DD 建完、再对成品调用 `dd::sift`**（事后重排），
`dynamic_sifting_spectral_*` 只是换了起点。这一版表没有单列算序耗时。""",
                "glossary": ORDERING_GLOSSARY,
                "sort": ["qubits", "circuit"],
            },
            {
                "path": "summary/ordering_full_experiments.csv",
                "title": "静态重排主实验（清洗版）",
                "blurb": """与上一张表同一批电路，两处不同：电路名只留
`q3/3_17_13.real` 这样的相对路径；失败单元格换成 `-`。""",
                "glossary": ORDERING_GLOSSARY,
                "sort": ["qubits", "circuit"],
            },
            {
                "path": "ordering_full_summary.csv",
                "title": "静态重排主实验（逐电路派生值）",
                "blurb": """同一批电路的派生表：最优静态策略、事后 sift 的两个臂，
以及逐电路的收益比。""",
                "glossary": [
                    ("best_static_strategy", "节点数最小的静态策略"),
                    ("best_static_nodes", "该策略的节点数"),
                    ("sifting_nodes", "以原始序为起点、事后 sift 的节点数"),
                    ("sifting_spectral_nodes", "以 spectral 序为起点、事后 sift 的节点数"),
                    ("dynamic_best_nodes", "上述两个事后重排臂里较小的节点数"),
                    ("gain_vs_original", "`dynamic_best_nodes / original_nodes`"),
                    ("gain_vs_best_static", "`dynamic_best_nodes / best_static_nodes`"),
                ],
                "sort": ["qubits", "circuit"],
            },
            {
                "path": "ordering_static_vs_dynamic_release.csv",
                "title": "静态与构建期动态重排（Release，40 条电路）",
                "blurb": """静态侧是五种策略；动态侧是**构建期重排**：节点数一旦超过
阈值就就地 sift，四个臂由起点与阈值组成（`identity500`、`identity1000`、
`spectral500`、`spectral1000`）。这一版是开优化的 Release 构建。""",
                "glossary": ORDERING_ARM_GLOSSARY,
                "sort": ["qubits", "circuit"],
            },
            {
                "path": "ordering_static_vs_dynamic.csv",
                "title": "静态与构建期动态重排（早期版本，40 条电路）",
                "blurb": """同一组电路的早期一版（未开优化的构建），绝对耗时与上一张表
不可直接比较，只作对照。""",
                "glossary": [
                    ("dynamic500_nodes", "阈值为 500 的构建期重排结束时的节点数"),
                    ("dynamic500_peak", "过程中的峰值节点数"),
                    ("dynamic500_reorderings", "触发重排的次数"),
                    ("dynamic500_exchanges", "交换的层数"),
                    ("dynamic1000_*", "同上，阈值为 1000"),
                    ("dynamic_gain_vs_static", "动态节点数除以静态节点数"),
                ],
                "sort": ["qubits", "circuit"],
            },
        ],
    ),
    (
        "early-rounds.md",
        "重排实验数据：最早一轮（22 条电路）",
        """这一组是接上 `dd::VariableOrdering` 之后最早跑的一批数据，比
`ordering_full_experiments.csv` 更早，电路集也小得多（22 条，且每条都跑
`forward` 与 `reversed` 两个方向）。结论已经被后面的表取代，保留是为了留下
「结论是怎么一步步来的」这条线。`unitary_dump.csv` 是一份 134 MB 的算符转储，
不放进 Markdown；它是把整张酉矩阵按元素导出的原始记录。""",
        [
            {
                "path": "ordering_experiments/ordering_results_main.csv",
                "title": "QASMBench 电路上的主实验（264 行）",
                "blurb": """22 条 QASMBench 电路 × 6 种策略（含 `identity`）× 两个方向。
`verified` 表示换序后算符按层号映回物理标号后与默认序一致。""",
                "glossary": [
                    ("strategy", "变量序策略；`identity` 是原始序"),
                    ("orientation", "序的方向：`forward` 或 `reversed`"),
                    ("time_ms", "构建功能 DD 的墙钟时间"),
                    ("nodes", "构建完成后 DD 的节点数"),
                    ("peak_nodes", "构建过程中的峰值节点数"),
                    ("inserts", "唯一表插入的节点总数，衡量真实工作量"),
                    ("verified", "换序后算符是否与默认序一致"),
                ],
                "sort": ["qubits", "circuit", "strategy", "orientation"],
            },
            {
                "path": "ordering_experiments/ordering_results_functionality.csv",
                "title": "RevLib 电路、算符模式（240 行）",
                "blurb": """把 RevLib 电路转成 qasm 之后按算符模式测量。`verified` 为
`n/a` 表示这一轮没有做逐电路的算符比对（对比结果另见
`ordering_results_functionality2.csv`）。""",
                "glossary": EARLY_GLOSSARY,
                "sort": ["qubits", "circuit", "strategy", "orientation"],
            },
            {
                "path": "ordering_experiments/ordering_results_functionality2.csv",
                "title": "RevLib 电路、算符模式（第二遍，228 行）",
                "blurb": """上一张表的重跑，补上了逐电路的校验。""",
                "glossary": EARLY_GLOSSARY,
                "sort": ["qubits", "circuit", "strategy", "orientation"],
            },
            {
                "path": "ordering_experiments/ordering_results_dynamic.csv",
                "title": "最早的动态重排轮（152 行）",
                "blurb": """把 sifting 接到构建流程上的第一版尝试，策略列里多了动态
变体。`max_deviation` 是换序结果映回物理标号后的最大偏差；`time_ms` 不含算序。""",
                "glossary": [
                    ("mode", "测量模式，这一轮都是 `functionality`"),
                    ("strategy", "变量序策略，含动态变体"),
                    ("max_deviation", "换序结果按层号映回物理标号后的最大偏差"),
                    ("verified", "偏差是否为零"),
                ],
                "sort": ["qubits", "circuit", "strategy"],
            },
            {
                "path": "ordering_experiments/smoke.csv",
                "title": "冒烟测试（22 行）",
                "blurb": """两条电路上的连通性检查，不是实验结果。""",
                "glossary": EARLY_GLOSSARY,
                "sort": ["circuit", "strategy", "orientation"],
            },
            {
                "path": "ordering_experiments/smoke2.csv",
                "title": "冒烟测试第二遍（11 行）",
                "blurb": """同上，补 `peak_nodes` 列。""",
                "glossary": EARLY_GLOSSARY,
                "sort": ["circuit", "strategy", "orientation"],
            },
            {
                "path": "ordering_experiments/ordering_results.csv",
                "title": "空表（0 行）",
                "blurb": """只有一个表头，是第一批尝试留下的空文件，列出只为说明它不是
被漏掉的数据。""",
                "glossary": EARLY_GLOSSARY,
                "sort": [],
            },
        ],
    ),
    (
        "qcec.md",
        "重排实验数据：QCEC 等价性检查",
        """这一组把静态变量序用在 **QCEC 的等价性检查**上。基准是 20 对「参考电路加
转译实现」，每对跑五种变量序策略、三个检验器与多次重复。行数多是因为每条记录只是
一次运行；整体结论见 `results/qcec_static_ordering_summary.md` 与
`results/qcec_default_flow_analysis.md`。""",
        [
            {
                "path": "qcec_static_ordering_checks.csv",
                "title": "逐次等价性检查（900 行）",
                "blurb": """一次运行等于一对电路、一种变量序策略、一个检验器与一次重复。
`checker` 决定耗时结构：`alternating` 交替判定，`simulation` 仿真，
`construction` 构造决策图。""",
                "glossary": QCEC_GLOSSARY,
                "sort": ["pair", "checker", "policy", "rep"],
            },
            {
                "path": "qcec_static_ordering_dd.csv",
                "title": "检验器用到的决策图构建（300 行）",
                "blurb": """每种变量序策略下为参考电路建决策图的节点数与耗时。""",
                "glossary": QCEC_GLOSSARY,
                "sort": ["pair", "policy", "rep"],
            },
            {
                "path": "qcec_static_ordering_by_policy.csv",
                "title": "逐电路对、逐检验器的策略对比（60 行）",
                "blurb": """按「电路对与检验器」把重复取中位数，把五种策略并到一行，
末尾给出最优策略。""",
                "glossary": QCEC_GLOSSARY,
                "sort": ["pair", "checker"],
            },
            {
                "path": "qcec_default_configuration.csv",
                "title": "QCEC 默认流程（99 行）",
                "blurb": """不让实验挑选检验器，而是让 QCEC 用它自己的默认流程
（`checker` 为 `default`，实际落在仿真加交替判定上），再看静态变量序有没有收益。""",
                "glossary": QCEC_GLOSSARY,
                "sort": ["pair", "policy"],
            },
        ],
    ),
    (
        "ddsim.md",
        "重排实验数据：DDSIM 密度矩阵仿真",
        """这一组把变量序用在 **DDSIM 的密度矩阵仿真**上，包括静态序与构建期动态重排。
局部噪声会把密度矩阵推成对角，规模上界因此是 2 的 n 次方，重排能省的地方有限。""",
        [
            {
                "path": "ddsim_density_matrix.csv",
                "title": "静态变量序的密度矩阵仿真（160 行）",
                "blurb": """一条电路、一种变量序与一次重复一行。仿真走 DDSIM 的密度
矩阵模拟器。""",
                "glossary": DDSIM_GLOSSARY,
                "sort": ["circuit", "order", "rep"],
            },
            {
                "path": "ddsim_density_matrix_by_order.csv",
                "title": "密度矩阵仿真逐电路汇总（16 行）",
                "blurb": """按电路把五种序并到一行，给出最优序与最优加速比。""",
                "glossary": DDSIM_GLOSSARY,
                "sort": ["circuit"],
            },
            {
                "path": "ddsim_dynamic_reordering.csv",
                "title": "构建期动态重排（150 行）",
                "blurb": """十种臂，每种反复重复。臂的含义见下表。

| 臂 | 含义 |
| --- | --- |
| `none` | 原始序，不重排 |
| `degree` | 静态 degree 序 |
| `spectral` | 静态 spectral 序 |
| `rb` | 静态递归二分序 |
| `dyn500` | 构建期重排，阈值 500 |
| `dyn2000` | 构建期重排，阈值 2000 |
| `dyn500_w4` | 同上，阈值 500，滑动窗口 4 |
| `dyn2000_w4` | 同上，阈值 2000，滑动窗口 4 |
| `spectral_dyn500` | 先按 spectral 序建，再做阈值 500 的构建期重排 |
| `dyn500_node` | 阈值 500，窗口 4，交换后重建 |""",
                "glossary": DDSIM_GLOSSARY,
                "sort": ["circuit", "arm", "rep"],
            },
        ],
    ),
    (
        "application.md",
        "重排实验数据：应用层 E1 与 E3",
        """这一组把静态变量序接到 mqt-core 自己的两个构建入口上：E1 是态矢量仿真的
决策图构建，E3 是算符级构建。这一组**把算变量序的时间与构建时间分开记**，
所以能看出算序本身的开销。""",
        [
            {
                "path": "application_e1_statevector.csv",
                "title": "E1：态矢量仿真的决策图构建（39 条电路）",
                "blurb": """每条电路在原始序与三种静态序下各建一次，算序耗时单列。""",
                "glossary": APPLICATION_GLOSSARY,
                "sort": ["qubits", "circuit"],
            },
            {
                "path": "application_e3_unitary.csv",
                "title": "E3：算符级决策图构建（40 条电路）",
                "blurb": """与 E1 同样的对照，但构建的是算符而不是态矢量。""",
                "glossary": APPLICATION_GLOSSARY,
                "sort": ["qubits", "circuit"],
            },
        ],
    ),
    (
        "syrec-final.md",
        "SyReC 最终实验数据",
        """这一份只留最终结果：最终数据集的逐电路原始测量、由它算出的逐电路对比，
以及最后一轮 racing portfolio。历史封版、等价性检查与静态代理统计不在这里，
它们连同早期轮次一起放在 `syrec.md`。""",
        [SYREC_FINAL_DATASET, SYREC_PORTFOLIO_DATASET],
    ),
    (
        "revlib-src.md",
        "重排实验数据：RevLib 的 SyReC 程序",
        """RevLib 的 `func.tgz` 里除真值表外还有 6 个 SyReC 程序（`.src`）。它们是
2010 年的语法，赋值写作 `^ =`、变量宽度取默认值，需要先转成现在的写法才能用当前
的 SyReC 编译；编译本身用发布版的 Python 包完成，因为上游的 `.src` 解析器需要
GCC 13 才有的 `<format>`。这一组给出编译后的规模，以及这些电路在变量序实验里的
结果。""",
        [
            {
                "path": "syrec_src_scale.csv",
                "title": "编译后的规模（6 行）",
                "blurb": """`lines` 是总共需要的量子比特数（信号线加辅助线），`gates`
是合成出的门数。决定性的是 `lines` 里有多少条信号线：真值表按信号线数指数增长。
默认位宽是 32，所以除 `arb8` 外的程序信号线都在 98 条以上。""",
                "glossary": [
                    ("circuit", "RevLib 的 SyReC 程序"),
                    ("lines", "量子比特总数（信号线 + 辅助线）"),
                    ("gates", "综合出的门的数量"),
                    ("ancilla", "其中辅助线数；`lines` 减它就是信号线数"),
                    ("quantum_cost", "SyReC 的量子代价度量"),
                    ("transistor_cost", "SyReC 的晶体管代价度量"),
                    ("seconds", "综合耗时"),
                ],
                "sort": ["circuit"],
            },
            {
                "path": "revlib_src_ordering.csv",
                "title": "这 6 个电路在变量序实验里的表现（6 行）",
                "blurb": """用决策图构建的基准工具跑这 6 个编译后的电路。工具的上限是
40 个量子比特，所以只有 `arb8`（32 比特）跑得动，其余 5 个记 `too-many-qubits`。
`arb8` 恰好是变量序影响极大的一条：原始序 12615 个节点，静态最优 1313，事后动态
重排到 77 个。""",
                "glossary": ORDERING_GLOSSARY + [
                    ("dynamic_construction_nodes", "构建期重排结束时的节点数"),
                    ("dynamic_construction_reorder_time_ms", "其中花在重排上的时间"),
                    ("dynamic_construction_reorderings", "触发重排的次数"),
                    ("dynamic_construction_peak_size", "构建期重排的峰值节点数"),
                ],
                "sort": ["qubits", "circuit"],
            },
        ],
    ),
    (
        "mcnc.md",
        "重排实验数据：MCNC 基准的可逆嵌入",
        """公开的逻辑综合基准（MCNC、LGSynth）放的是普通布尔函数，不是置换，所以
要先做一步可逆嵌入 `(x, y) -> (x, y XOR f(x))` 才能进决策图综合。这一组是嵌入后的
83 条真实函数，加上与前面数据集合并后的总表。数据集本身、清单和来源见
`mcnc-dataset/README.md`。**注意**：这里测的是"原函数的可逆嵌入"，不是原函数本身。""",
        [
            {
                "path": "syrec_dd_order_reps_mcnc.csv",
                "title": "MCNC 嵌入后的 83 条电路（1079 行）",
                "blurb": """83 条真实函数（加法器、乘法器、仲裁器、编码器、错误检测）
嵌入后的可逆版本，用同一套 13 种配置跑 5 次重复。变量数 3 到 16，门数中位 65、
最大 1222。""",
                "glossary": SYREC_GLOSSARY,
                "sort": ["n_vars", "circuit", "order", "seed"],
                "derived": syrec_comparison,
            },
            {
                "path": "syrec_dd_order_reps149.csv",
                "title": "全部合并：149 条电路（1937 行）",
                "blurb": """前面三份数据的合并：53 条封版数据集、13 条 RevLib 新电路、
83 条 MCNC 嵌入电路。这也是目前规模最大的一份数据集。""",
                "glossary": SYREC_GLOSSARY,
                "sort": ["n_vars", "circuit", "order", "seed"],
                "derived": syrec_comparison,
            },
            {
                "path": "syrec_dd_portfolio_mcnc.csv",
                "title": "MCNC 83 条上的 racing portfolio（166 行）",
                "blurb": """在 83 条真实函数上重跑 racing，两种预算各 83 个配置。
`best_sequential` 与 `best_raced` 是两侧的最优门数，`race_speedup` 是 racing 耗时
除以顺序耗时，`match` 表示两侧最优门数是否一致。**166 个配置全部 match**——这一组
电路没有 §8.2 那个残余非确定性，所以"输出与全跑完全一致"在这里是严格成立的。""",
                "glossary": SYREC_GLOSSARY,
                "sort": ["candidates", "circuit"],
            },
        ],
    ),
    (
        "syrec.md",
        "重排实验数据：SyReC 的 QMDD 综合",
        """这一组是当前主线。变量序在这里不只是性能旋钮，它决定**综合出来的电路有多大**。
最终数据集是 `syrec_dd_order_reps64.csv`，更早的两张表保留作历史。""",
        [
            SYREC_SEAL_DATASET,
            {
                "path": "syrec_dd_order_reps_revlib.csv",
                "title": "RevLib 新增的 13 条电路（169 行）",
                "blurb": """从 RevLib 的 133 个函数里筛出的、我们此前没做过的 13 个可逆
函数，用同一套 13 种配置跑 5 次重复。全部 169 个配置都成功。这些电路多数很小，
其中 9 条本来就已经是最少门数。""",
                "glossary": SYREC_GLOSSARY,
                "sort": ["n_vars", "circuit", "order", "seed"],
            },
            {
                "path": "syrec_dd_order_reps66.csv",
                "title": "与 RevLib 合并后的 66 条电路（858 行）",
                "blurb": """53 条封版数据集加 13 条 RevLib 电路的合并结果。加入的是小
电路，所以结论不变，中位比有小幅移动。""",
                "glossary": SYREC_GLOSSARY,
                "sort": ["n_vars", "circuit", "order", "seed"],
                "derived": syrec_comparison,
            },
            {
                "path": "syrec_dd_order_reps22.csv",
                "title": "第一批带重复的数据集：22 条电路（286 行）",
                "blurb": """最先做的那 22 条电路，每配置跑 5 次，已被 53 条电路的版本
取代。**这一份是按当前二进制重跑的**：原始文件在 2026-10-10 被误覆盖（见下面
§文件清单里的说明），重跑用的是同一批电路和同一套协议，但合成器已经修过非确定性，
所以离散度比当初小，不应当拿来复现当初那几个小数。""",
                "glossary": SYREC_GLOSSARY,
                "sort": ["n_vars", "circuit", "order", "seed"],
            },
            {
                "path": "syrec_dd_order_experiments.csv",
                "title": "第一次封版：单次运行（286 行）",
                "blurb": """最早的一版，每个配置只跑一次，而且跑在修 ESOP 贪心确定性
之前。只作历史对照：那一版的精细比较落在合成器自身抖动范围之内。""",
                "glossary": SYREC_GLOSSARY,
                "sort": ["n_vars", "circuit", "order", "seed"],
            },
            {
                "path": "syrec_dd_order_equivalence149.csv",
                "title": "等价性：换序电路与自然序电路的功能比对，覆盖 149 条数据集",
                "blurb": """每行一次检查：把换序电路的每条线按 `order[line]` 映回原变量，
再与自然序电路比较功能图。每条电路跑 5 种结构性序加 3 个随机序。""",
                "glossary": SYREC_GLOSSARY,
                "sort": ["circuit", "order", "seed"],
            },
            {
                "path": "syrec_dd_order_equivalence.csv",
                "title": "等价性（只覆盖原 53 条，424 行）",
                "blurb": """同一套检查跑在 53 条封版数据集上的结果，保留作对照。""",
                "glossary": SYREC_GLOSSARY,
                "sort": ["circuit", "order", "seed"],
            },
            SYREC_PORTFOLIO_DATASET,
            {
                "path": "syrec_dd_portfolio.csv",
                "title": "racing portfolio：早期 51 条电路版本（102 行）",
                "blurb": """最早那轮 racing，只覆盖 53 条封版里的 51 条，已被 147 条的
全量版本取代，保留作对照。""",
                "glossary": SYREC_GLOSSARY,
                "sort": ["circuit", "candidates"],
            },
            {
                "path": "syrec_dd_portfolio_pre_fix.csv",
                "title": "racing portfolio（修复合成器非确定性之前，40 行）",
                "blurb": """同一实验在修复之前的版本，只作历史对照。""",
                "glossary": SYREC_GLOSSARY,
                "sort": ["circuit", "candidates"],
            },
            {
                "path": "syrec_dd_order_stats.csv",
                "title": "决策图结构统计与静态代价代理（274 行）",
                "blurb": """每个「电路与序」的决策图形状，以及三组廉价静态代理：
`estimate` 只走一遍图、不发门，用来在没有跑完合成的情况下估计 P1、P2、P4
例程会发多少门。""",
                "glossary": SYREC_GLOSSARY,
                "sort": ["n_vars", "circuit", "order", "seed"],
            },
        ],
    ),
]


INDEX_INTRO = """这份目录把这一阶段的全部实验数据按「一条测量一行」摊开：

| 文件 | 内容 |
| --- | --- |
| [ordering.md](ordering.md) | mqt-core 决策图构建：原始序、静态序、事后 sift、构建期重排 |
| [early-rounds.md](early-rounds.md) | 最早一轮：22 条电路上的策略与方向对照 |
| [qcec.md](qcec.md) | QCEC 等价性检查：静态变量序对三个检验器的影响 |
| [ddsim.md](ddsim.md) | DDSIM 密度矩阵仿真：静态序与十种动态臂 |
| [application.md](application.md) | 应用层 E1（态矢量）与 E3（算符）的决策图构建 |
| [syrec-final.md](syrec-final.md) | **只含最终结果**：最终数据集、逐电路对比、racing portfolio |
| [revlib-src.md](revlib-src.md) | RevLib 的 SyReC 程序：编译规模与变量序结果 |
| [mcnc.md](mcnc.md) | MCNC 基准的可逆嵌入：83 条真实函数，以及与前面数据集的合并 |
| [syrec.md](syrec.md) | SyReC 的 QMDD 综合：最终数据集、等价性、racing portfolio |

每张表都来自 `/home/flareon/workshop/results` 下的原始 CSV，只做了三件事：把电路路径
缩成相对名、把浮点数按三位小数打印、把行排序。**没有任何聚合**，所以每条电路的每次
重复都能查到。重新生成用：

```bash
python3 test/render_experiment_data.py
```

## 这几条线的结论

**决策图构建**：变量序对**算符级**构建有效，但收益集中在少数电路；`min_fill` 与
`gate_dependency` 两种策略被否决，不再保留。

**QCEC**：变量序能大幅加快**构造检查器**建决策图的速度，但那个检查器默认关闭；
默认流程（向量态仿真加交替判定）上，静态重排的中位比在 0.97 到 1.06 之间，
**没有收益**。

**DDSIM 密度矩阵**：局部噪声把密度矩阵推成对角，规模上界是 2 的 n 次方，重排省不下来；
中位耗时比 1.33 到 2.06，**不划算**。

**应用层 E1 与 E3**：构建耗时确实能被改善，但同样依赖具体电路，而且这一层的构建成本
在整体流程里占比很小。

**SyReC**：这是唯一站得住的宿主。换序能让综合出的电路小到 **0.076 倍**，也可能大到
6.2 倍，而**价值取决于函数结构**：

| 数据集 | spectral 单独 | 四序最好 | oracle |
| --- | --- | --- | --- |
| 66 条小型可逆函数 | 1.000 / 0.998 | 0.894 / **1.500** | 0.831 / 0.780 |
| **83 条 MCNC 真实函数（可逆嵌入）** | **0.841 / 0.607** | **0.714 / 0.463** | **0.667 / 0.427** |
| 149 条合并 | 0.987 / 0.897 | 0.829 / 1.290 | 0.750 / 0.709 |

每格是「逐电路中位比 / 门数求和比」。**在真实逻辑函数上，单一启发式就有效**：
spectral 单独把总门数压到 0.607，四序组合压到 0.463，而且最坏只差 6.5%（1.065）；
在小型可逆基准上同样的方法几乎无效（1.000），四序组合按门数求和甚至更差（1.500，
因为候选池里没有自然序时 `hamming_15` 会选到 3.214 倍）。所以"变量序有没有用"取决于
函数本身有没有可利用的结构——这是目前最有价值的结论。

候选池做成 racing portfolio 后，得到的是同一批候选里的最优结果，代价降到中位
**0.427**（5 候选）与 **0.293**（8 候选）；等价性 410/410 全部通过。

两个必须一起读的前提：合成器修完之后仍有 **9.5% 的配置**在 5 次重复之间有差异
（最差 3.3%），所以 racing 在 102 个配置里有 9 个与顺序跑差几门；以及结论适用的对象是
**完全指定的双射可逆函数**（2 到 15 个变量）。

## 相关文档

| 文档 | 内容 |
| --- | --- |
| [mcnc-dataset/README.md](mcnc-dataset/README.md) | MCNC/LGSynth 基准的可逆嵌入数据集：来源、嵌入规则、清单 |
| `mydocs/阶段性成果.md` | mqt-core 侧的总报告（重排算法、QCEC、DDSIM、应用层） |
| `mydocs/variable-ordering-howto.md` | 重排工具的用法与输出列含义 |
| `mydocs/variable-ordering-strategy-sources.md` | 每个排序策略的机制、算法与代码出处 |
| `mydocs/syrec-dd-order.md` | SyReC 这条线的详细记录 |
| `mydocs/syrec-dd-ordering-paper.md` | SyReC 这条线的论文稿 |
| `results/README-syrec.md` | SyReC 数据文件的说明 |
| `results/qcec_default_flow_analysis.md` | QCEC 默认流程的收益分析 |
| `results/syrec_determinism.md` | 合成器非确定性的排查记录 |

本目录由 `test/render_experiment_data.py` 生成，不要手工修改。

## 一次数据事故的记录

2026-10-10 重跑 MCNC 的修复时，命令漏了 `MQT_REPS_OUT_NAME`，运行脚本用了默认输出名
`syrec_dd_order_reps.csv`，把 9 月那份 22 条电路的历史归档连同名汇总一起覆盖了。
被覆盖的文件移到 `results/obsolete/` 留证；其中的测量对象是 53 条数据集里的一个子集，
所以没有独有电路丢失，但当初那批数字（跑在修非确定性之前）无法复原。同一批电路随后
用修好的二进制重跑，就是 `syrec_dd_order_reps22.csv`。运行脚本现在会拒绝在未显式
指定输出名的情况下覆盖已有数据集。"""


def main() -> int:
    """Write every group file and the index."""
    OUT.mkdir(parents=True, exist_ok=True)
    index = [
        "---",
        "orphan: true",
        "---",
        "",
        "# 实验数据总表",
        "",
        INDEX_INTRO,
        "",
        "## 文件清单",
        "",
    ]
    for filename, title, intro, datasets in GROUPS:
        lines = [
            "---",
            "orphan: true",
            "---",
            "",
            f"# {title}",
            "",
            intro.strip(),
            "",
        ]
        for dataset in datasets:
            lines.extend(section(dataset))
        (OUT / filename).write_text("\n".join(lines).rstrip() + "\n")
        rows = sum(len(read_rows(RESULTS / dataset["path"])) for dataset in datasets)
        index.append(f"- `{filename}`：{len(datasets)} 张表，{rows} 行数据。")
        print(f"wrote {OUT / filename} ({rows} rows)")
    (OUT / "index.md").write_text("\n".join(index).rstrip() + "\n")
    print(f"wrote {OUT / 'index.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
