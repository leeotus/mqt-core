#!/usr/bin/env python3
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
"""Write the experiment data into one Excel workbook.

One sheet per experiment. The per-circuit sheets hold one row per circuit with
every configuration as a column; the raw sheets hold one row per measurement.
The first sheet explains the workbook and repeats the caveats that apply to the
data, so the file can be read without the report next to it.

    python3 test/export_excel_workbook.py
"""

from __future__ import annotations

import csv
import pathlib
import statistics

from openpyxl import Workbook
from openpyxl.formatting.rule import CellIsRule
from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
from openpyxl.utils import get_column_letter

RESULTS = pathlib.Path("/home/flareon/workshop/results")
OUT = pathlib.Path("/home/flareon/workshop/mqt-core/docs/report/research-data.xlsx")

HEADER_FILL = PatternFill("solid", fgColor="1F3864")
HEADER_FONT = Font(bold=True, color="FFFFFF")
TITLE_FONT = Font(bold=True, size=14)
GROUP_FILL = PatternFill("solid", fgColor="D9E2F3")
GREEN_FILL = PatternFill("solid", fgColor="C6EFCE")
RED_FILL = PatternFill("solid", fgColor="FFC7CE")
THIN = Side(style="thin", color="BFBFBF")
BORDER = Border(left=THIN, right=THIN, top=THIN, bottom=THIN)

ORDERS = ["natural", "degree", "spectral", "recursive_bisection", "adaptive"]
SEEDS = [str(seed) for seed in range(1, 9)]
RATIO_COLUMNS = {
    "best_ratio", "ratio5", "ratio8", "best", "speedup", "race_speedup",
    "gain_vs_original", "gain_vs_best_static", "nodes_ratio", "time_ratio",
}


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


def value(text: str):
    """Turn a CSV cell into a number where possible."""
    text = (text or "").strip()
    if text in {"", "-"}:
        return None
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text


def groupOf() -> dict[str, str]:
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


def addSheet(workbook: Workbook, title: str, columns: list[str],
             rows: list[list], start: int = 1, note: str | None = None) -> None:
    """Write one formatted sheet."""
    sheet = workbook.create_sheet(title[:31])
    first = start
    if note:
        sheet.cell(row=1, column=1, value=note).alignment = Alignment(
            wrap_text=True, vertical="center"
        )
        sheet.merge_cells(
            start_row=1, start_column=1, end_row=1,
            end_column=max(len(columns), 1),
        )
        sheet.row_dimensions[1].height = 32
        first = 3
    for column, name in enumerate(columns, start=1):
        cell = sheet.cell(row=first, column=column, value=name)
        cell.fill = HEADER_FILL
        cell.font = HEADER_FONT
        cell.alignment = Alignment(horizontal="center", vertical="center",
                                   wrap_text=True)
        cell.border = BORDER
    for offset, row in enumerate(rows, start=1):
        for column, item in enumerate(row, start=1):
            cell = sheet.cell(row=first + offset, column=column, value=item)
            cell.border = BORDER
            if isinstance(item, float):
                cell.number_format = "0.000"
    sheet.freeze_panes = sheet.cell(row=first + 1, column=1)
    sheet.auto_filter.ref = (
        f"A{first}:{get_column_letter(len(columns))}{first + len(rows)}"
    )
    for column, name in enumerate(columns, start=1):
        width = max(
            [len(str(name))]
            + [len(str(row[column - 1])) if row[column - 1] is not None else 0
               for row in rows[:200]]
        )
        sheet.column_dimensions[get_column_letter(column)].width = min(
            max(width + 2, 9), 42
        )
    for column, name in enumerate(columns, start=1):
        if name in RATIO_COLUMNS:
            letter = get_column_letter(column)
            area = f"{letter}{first + 1}:{letter}{first + len(rows)}"
            sheet.conditional_formatting.add(
                area, CellIsRule(operator="lessThan", formula=["1"],
                                 fill=GREEN_FILL)
            )
            sheet.conditional_formatting.add(
                area, CellIsRule(operator="greaterThan", formula=["1"],
                                 fill=RED_FILL)
            )


def syrecSheet(workbook: Workbook) -> None:
    """One row per circuit, one column per configuration."""
    groups = groupOf()
    rows = [
        r
        for r in read("syrec_dd_order_reps149_force_mince.csv")
        if r["status"] == "ok"
    ]
    byCircuit: dict[str, dict[str, float]] = {}
    varsOf: dict[str, int] = {}
    spreadOf: dict[str, float] = {}
    for row in rows:
        key = row["order"] if row["order"] != "random" else f"random{row['seed']}"
        byCircuit.setdefault(row["circuit"], {})[key] = number(row, "gates_median")
        varsOf[row["circuit"]] = int(row["n_vars"])
        spread = (
            (number(row, "gates_max") - number(row, "gates_min"))
            / max(number(row, "gates_median"), 1.0)
        )
        spreadOf[row["circuit"]] = max(spreadOf.get(row["circuit"], 0.0), spread)
    table = []
    for circuit in sorted(byCircuit, key=lambda c: (varsOf[c], c)):
        values = byCircuit[circuit]
        natural = values.get("natural")
        best = min(values.values())
        winner = min(values, key=values.get)
        table.append(
            [
                circuit,
                groups.get(
                    circuit,
                    "random" if pathlib.Path(circuit).stem.startswith("rand")
                    else "SyReC seal",
                ),
                varsOf[circuit],
                natural,
                values.get("degree"),
                values.get("spectral"),
                values.get("recursive_bisection"),
                values.get("adaptive"),
                values.get("force"),
                values.get("mince"),
                *[values.get(f"random{seed}") for seed in SEEDS],
                best,
                winner,
                best / natural if natural else None,
                spreadOf[circuit],
            ]
        )
    addSheet(
        workbook,
        "SyReC-149 逐电路",
        ["circuit", "group", "n_vars", "natural", "degree", "spectral",
         "recursive_bisection", "adaptive",
         "force", "mince",
         *[f"random{seed}" for seed in SEEDS],
         "best", "best_order", "best_ratio", "spread_max"],
        table,
        note=("149 条电路（表中 148 行：urf6 全部超时）。值是门数中位数（5 次重复）；"
              "- 表示该配置没跑完。best_order 并列时写 tie(n)。"),
    )


def syrecFiveSheet(workbook: Workbook) -> None:
    """The same table without the eight random orders."""
    groups = groupOf()
    rows = [
        r
        for r in read("syrec_dd_order_reps149_force_mince.csv")
        if r["status"] == "ok" and r["order"] != "random"
    ]
    byCircuit: dict[str, dict[str, float]] = {}
    varsOf: dict[str, int] = {}
    for row in rows:
        byCircuit.setdefault(row["circuit"], {})[row["order"]] = number(
            row, "gates_median"
        )
        varsOf[row["circuit"]] = int(row["n_vars"])
    table = []
    for circuit in sorted(byCircuit, key=lambda c: (varsOf[c], c)):
        values = byCircuit[circuit]
        natural = values.get("natural")
        best = min(values.values())
        winner = min(values, key=values.get)
        table.append(
            [
                circuit,
                groups.get(
                    circuit,
                    "random" if pathlib.Path(circuit).stem.startswith("rand")
                    else "SyReC seal",
                ),
                varsOf[circuit],
                natural,
                values.get("degree"),
                values.get("spectral"),
                values.get("recursive_bisection"),
                values.get("adaptive"),
                values.get("force"),
                values.get("mince"),
                best,
                winner,
                best / natural if natural else None,
            ]
        )
    addSheet(
        workbook,
        "SyReC-149 七序",
        ["circuit", "group", "n_vars", "natural", "degree", "spectral",
         "recursive_bisection", "adaptive", "force", "mince", "best", "best_order",
         "best_ratio"],
        table,
        note=("只保留 7 个结构序；best 与 best_ratio 只在这七列里取，不引用随机序。"
              "整体中位 0.800、求和 0.785；MCNC 组 0.714 / 0.450。"),
    )


def orderingSheet(workbook: Workbook) -> None:
    """One row per circuit, every strategy as a column."""
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
    columns = [
        ("circuit", "circuit"), ("qubits", "qubits"), ("gates", "gates"),
        ("status", "status"), ("original", "original_nodes"),
        ("degree", "static_degree_nodes"), ("spectral", "static_spectral_nodes"),
        ("rb", "static_recursive_bisection_nodes"),
        ("min_fill", "static_min_fill_nodes"),
        ("gate_dep", "static_gate_dependency_nodes"),
        ("best_static", "static_best_nodes"),
        ("sift", "dynamic_sifting_nodes"),
        ("sift_spectral", "dynamic_sifting_spectral_nodes"),
        ("original_ms", "original_time_ms"),
        ("sift_ms", "dynamic_sifting_time_ms"),
    ]
    table = []
    for row in sorted(rows, key=lambda r: (int(r["qubits"] or 0), r["circuit"])):
        circuit = pathlib.Path(row["circuit"]).name
        cells = [
            pathlib.Path(row[key]).name if name == "circuit" else value(row[key])
            for name, key in columns
        ]
        insertAt = [name for name, _ in columns].index("sift_spectral") + 1
        cells[insertAt:insertAt] = [force.get(circuit), mince.get(circuit)]
        table.append(cells)
    headers = [name for name, _ in columns]
    headers.insert(headers.index("sift_spectral") + 1, "force")
    headers.insert(headers.index("sift_spectral") + 2, "mince")
    addSheet(
        workbook,
        "Ordering-317 逐电路",
        headers,
        table,
        note=("317 条 RevLib 电路，值是功能 DD 的节点数。- 表示读入失败、超时或超过 40 比特。"
              "sift 是建完再重排；sift_spectral 以谱序为起点；force 与 mince 是新增策略。"),
    )


def qcecSheet(workbook: Workbook) -> None:
    """One row per pair and checker."""
    columns = [
        ("pair", "pair"), ("checker", "checker"), ("ref_qubits", "ref_qubits"),
        ("impl_qubits", "impl_qubits"), ("none_ms", "none_time_ms"),
        ("gate_dep_ms", "gate_dependency_time_ms"),
        ("degree_ms", "degree_time_ms"), ("spectral_ms", "spectral_time_ms"),
        ("rb_ms", "recursive_bisection_time_ms"), ("best", "best_policy"),
        ("best_ms", "best_time_ms"), ("best", "best_ratio"),
    ]
    rows = read("qcec_static_ordering_by_policy.csv")
    table = [
        [value(row[key]) for _, key in columns]
        for row in sorted(rows, key=lambda r: (r["pair"], r["checker"]))
    ]
    addSheet(
        workbook,
        "QCEC 逐对",
        ["pair", "checker", "ref_qubits", "impl_qubits", "none_ms",
         "gate_dep_ms", "degree_ms", "spectral_ms", "rb_ms", "best",
         "best_ms", "best_ratio"],
        table,
        note=("20 对电路 x 3 个检查器，耗时单位毫秒（3 次重复的中位）。"
              "construction 是构造 DD 的检查器（默认关闭）；simulation 与 alternating 是默认流程。"),
    )


def ddsimSheet(workbook: Workbook) -> None:
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
        table.append(
            [circuit, *(entry[o] for o in ["none", "gate_dependency", "degree",
                                           "spectral", "recursive_bisection"]),
             best / base if base and best else None]
        )
    addSheet(
        workbook,
        "DDSIM-16 逐电路",
        ["circuit", "none_ms", "gate_dependency_ms", "degree_ms", "spectral_ms",
         "recursive_bisection_ms", "best_ratio"],
        table,
        note=("16 条电路（表中 15 条：1 条在 5 种序下全部超时）。耗时单位毫秒，10 次重复的中位。"),
    )


def applicationSheet(workbook: Workbook) -> None:
    """E1 and E3 side by side."""
    table = []
    for name, label in [
        ("application_e1_statevector.csv", "E1 statevector"),
        ("application_e3_unitary.csv", "E3 unitary"),
    ]:
        for row in read(name):
            if row["status"] != "ok":
                continue
            original = number(row, "original_nodes")
            table.append(
                [
                    label, pathlib.Path(row["circuit"]).name,
                    value(row["qubits"]), value(row["gates"]), original,
                    value(row["degree_nodes"]), value(row["spectral_nodes"]),
                    value(row["recursive_bisection_nodes"]),
                    row["best_strategy"], value(row["best_nodes"]),
                    number(row, "best_nodes") / original if original else None,
                ]
            )
    addSheet(
        workbook,
        "应用 E1E3",
        ["application", "circuit", "qubits", "gates", "original_nodes",
         "degree_nodes", "spectral_nodes", "rb_nodes", "best", "best_nodes",
         "best_ratio"],
        table,
        note="E1 是态矢量构建（29 条），E3 是算符级构建（40 条）；值是节点数。",
    )


def racingSheet(workbook: Workbook) -> None:
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
            [
                circuit,
                number(five, "sequential_ms") / 1000 if five else None,
                number(five, "raced_ms") / 1000 if five else None,
                number(five, "race_speedup") if five else None,
                value(five.get("match", "")),
                number(eight, "sequential_ms") / 1000 if eight else None,
                number(eight, "raced_ms") / 1000 if eight else None,
                number(eight, "race_speedup") if eight else None,
                value(eight.get("match", "")),
            ]
        )
    addSheet(
        workbook,
        "Racing-147 逐电路",
        ["circuit", "seq5_s", "raced5_s", "ratio5", "match5", "seq8_s",
         "raced8_s", "ratio8", "match8"],
        table,
        note=("147 条电路（hamming_15/urf6 的候选跑不完，排除）。单位秒；"
              "ratio 小于 1 表示 racing 更快；match=1 表示两侧最优门数相同。"),
    )


def syrecRawSheet(workbook: Workbook) -> None:
    """The raw SyReC measurements."""
    rows = read("syrec_dd_order_reps149_force_mince.csv")
    columns = ["circuit", "n_vars", "order", "seed", "status", "reps",
               "gates_median", "gates_min", "gates_max", "dd_nodes",
               "elapsed_ms", "verified"]
    addSheet(
        workbook,
        "SyReC 全部配置",
        columns,
        [[value(row[c]) for c in columns] for row in rows],
        note="2235 行，每个（电路 x 配置）一行，含 FORCE 与 MINCE；gates_median 是 5 次重复的中位。",
    )


def racingRawSheet(workbook: Workbook) -> None:
    """The raw racing measurements."""
    rows = read("syrec_dd_portfolio_149.csv")
    columns = ["circuit", "n_vars", "candidates", "best_sequential",
               "best_raced", "aborted", "sequential_ms", "raced_ms",
               "race_speedup", "match"]
    addSheet(
        workbook,
        "Racing 全部配置",
        columns,
        [[value(row[c]) for c in columns] for row in rows],
        note="294 行，每个（电路 x 预算）一行。",
    )


def coverSheet(workbook: Workbook) -> None:
    """Explain the workbook."""
    sheet = workbook.active
    sheet.title = "说明"
    sheet["A1"] = "MQT Core 变量排序研究：完整实验数据"
    sheet["A1"].font = TITLE_FONT
    sheet["A2"] = "导出日期 2026-10-19 · 数据截止 2026-10-11"
    sheet["A4"] = "本工作簿包含 11 张表（说明 + 10 张数据表），按“结论 → 逐电路 → 原始数据”组织："
    sheet["A4"].font = Font(bold=True)
    contents = [
        ("方向总表", "12 个研究方向的结论、关键指标与证据文件"),
        ("SyReC-149 逐电路", "149 条电路，一行一条，15 种配置作为列（含 FORCE、MINCE）"),
        ("SyReC-149 七序", "同上，但只保留 7 个结构序（含 FORCE、MINCE），best 只在这七列里取"),
        ("SyReC 全部配置", "1937 行原始测量（每配置 5 次重复）"),
        ("Ordering-317 逐电路", "317 条 RevLib 电路，各静态/动态策略的 DD 节点数"),
        ("QCEC 逐对", "20 对电路 x 3 个检查器，各变量序的检查耗时"),
        ("DDSIM-16 逐电路", "密度矩阵仿真，各变量序的耗时"),
        ("应用 E1E3", "态矢量与算符级构建，各变量序的节点数"),
        ("Racing-147 逐电路", "顺序跑 vs 剪枝跑，两种预算的耗时与一致性"),
        ("Racing 全部配置", "294 行原始对照"),
    ]
    for index, (name, text) in enumerate(contents, start=5):
        sheet.cell(row=index, column=1, value=name).font = Font(bold=True)
        sheet.cell(row=index, column=2, value=text)
    sheet["A16"] = "读表要点"
    sheet["A16"].font = Font(bold=True)
    notes = [
        "比值列（best_ratio / ratio5 / ratio8 等）小于 1 表示更好；绿色背景为改善，红色为劣化。",
        "- 表示该配置在这条电路上没有可用结果（超时、读入失败或超过规模上限）。",
        "SyReC 表 148 行：urf6 的 13 个配置全部超时，没有可比数据。",
        "DDSIM 表 15 行：1 条电路在 5 种序下全部超时。",
        "Racing 表 147 行：hamming_15 与 urf6 的候选本身跑不完，无法做顺序/剪枝对照。",
        "MCNC 组有 10 个文件名沿用了正版本的基名（实际是 fr/ 取反变体），来源见 manifest.csv 的 source_path 列。",
        "逐次重复与全部列在 docs/experiment-data/ 的 CSV 里；本工作簿是其透视与摘录。",
    ]
    for index, text in enumerate(notes, start=17):
        sheet.cell(row=index, column=1, value="·")
        sheet.cell(row=index, column=2, value=text)
    sheet.column_dimensions["A"].width = 30
    sheet.column_dimensions["B"].width = 95


def main() -> int:
    workbook = Workbook()
    coverSheet(workbook)
    directions = list(csv.DictReader(
        open("/home/flareon/workshop/mqt-core/docs/report/data/01-direction-summary.csv",
             encoding="utf-8-sig")
    ))
    addSheet(
        workbook,
        "方向总表",
        ["direction", "experiment", "verdict", "key_metric", "evidence"],
        [[row[c] for c in ["direction", "experiment", "verdict", "key_metric",
                            "evidence"]] for row in directions],
    )
    syrecSheet(workbook)
    syrecFiveSheet(workbook)
    syrecRawSheet(workbook)
    orderingSheet(workbook)
    qcecSheet(workbook)
    ddsimSheet(workbook)
    applicationSheet(workbook)
    racingSheet(workbook)
    racingRawSheet(workbook)
    # 把说明保持在最前，其余按上面的添加顺序
    workbook.move_sheet("说明", offset=-workbook.index(workbook["说明"]))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    workbook.save(OUT)
    print(f"wrote {OUT}")
    for sheet in workbook.sheetnames:
        print(f"   {sheet}: {workbook[sheet].max_row} rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
