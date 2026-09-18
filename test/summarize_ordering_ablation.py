#!/usr/bin/env python3
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
"""Summarize the static-versus-dynamic ordering comparison."""

from __future__ import annotations

import csv
import statistics as st
import sys
from pathlib import Path


def asInt(row: dict[str, str], key: str) -> int | None:
    value = row.get(key, "")
    if value in ("", None, "-"):
        return None
    try:
        return int(float(value))
    except ValueError:
        return None


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: summarize_ordering_ablation.py <input.csv> <output.md>")
        return 2
    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    rows = [row for row in csv.DictReader(source.open()) if row["status"] == "ok"]

    thresholds = sorted(
        {
            key[len("dynamic") : -len("_nodes")]
            for key in rows[0]
            if key.startswith("dynamic") and key.endswith("_nodes")
        },
        key=int,
    )

    entries = []
    for row in rows:
        original = asInt(row, "original_nodes")
        staticBest = asInt(row, "static_best_nodes")
        dynamic = {
            threshold: asInt(row, f"dynamic{threshold}_nodes")
            for threshold in thresholds
        }
        peak = {
            threshold: asInt(row, f"dynamic{threshold}_peak")
            for threshold in thresholds
        }
        available = {
            threshold: size
            for threshold, size in dynamic.items()
            if size is not None
        }
        if not available or original is None:
            continue
        bestThreshold = min(available, key=lambda t: available[t])
        entries.append(
            {
                "circuit": row["circuit"],
                "qubits": int(row["qubits"]),
                "original": original,
                "static": staticBest,
                "dynamic": available[bestThreshold],
                "threshold": bestThreshold,
                "peak": peak[bestThreshold],
                "time": float(row[f"dynamic{bestThreshold}_time_ms"]),
            }
        )

    with output.open("w") as handle:
        handle.write("# 静态排序 vs 构建期动态重排（对比实验）\n\n")
        handle.write(f"数据文件：`{source}`。共 {len(entries)} 个电路。\n\n")

        wins = sum(1 for e in entries if e["dynamic"] < e["static"])
        ties = sum(1 for e in entries if e["dynamic"] == e["static"])
        losses = len(entries) - wins - ties
        gains = [
            e["static"] / e["dynamic"] for e in entries if e["dynamic"] and e["static"]
        ]
        handle.write("## 一、总体\n\n")
        handle.write(f"- 动态重排最终 DD 更小：**{wins}** 个电路；持平 {ties}；更大 {losses}\n")
        handle.write(f"- 动态相对静态的倍数：中位 {st.median(gains):.2f}×，最大 {max(gains):.2f}×\n")

        peaks = [
            (e["original"] / e["peak"], e)
            for e in entries
            if e["peak"]
        ]
        if peaks:
            best = max(peaks, key=lambda pair: pair[0])
            handle.write(
                f"- 中间态峰值相对原始构建的最大压缩：**{best[0]:.0f}×**"
                f"（{best[1]['circuit']}，原始 {best[1]['original']}，峰值 {best[1]['peak']}）\n"
            )
            handle.write(
                f"- 峰值压缩比中位数：**{st.median([p for p, _ in peaks]):.1f}×**\n"
            )
        handle.write("\n")

        handle.write("## 二、逐电路结果\n\n")
        handle.write(
            "| 电路 | 比特 | 原始 | 最佳静态 | 最佳动态 | 动态阈值 | 动态峰值 | 动态时间 | 动态/静态 |\n"
        )
        handle.write("| --- | --- | --- | --- | --- | --- | --- | --- | --- |\n")
        for e in sorted(entries, key=lambda e: -e["original"]):
            ratio = e["static"] / e["dynamic"] if e["dynamic"] else float("nan")
            handle.write(
                f"| {e['circuit']} | {e['qubits']} | {e['original']} | {e['static']} | "
                f"{e['dynamic']} | {e['threshold']} | {e['peak']} | "
                f"{e['time'] / 1000:.2f} s | {ratio:.2f}× |\n"
            )
        handle.write("\n")

        handle.write("## 三、按规模分组（中位倍数：原始/最终、原始/峰值）\n\n")
        handle.write("| 原始节点数 | 电路数 | 静态：原始/最终 | 动态：原始/最终 | 动态：原始/峰值 |\n")
        handle.write("| --- | --- | --- | --- | --- |\n")
        buckets = {
            "1e3–1e4": (1000, 10000),
            "1e4–1e5": (10000, 100000),
            "1e5+": (100000, 10**12),
        }
        for name, (low, high) in buckets.items():
            group = [e for e in entries if low <= e["original"] < high]
            if not group:
                continue
            staticRatio = st.median([e["original"] / e["static"] for e in group])
            dynamicRatio = st.median([e["original"] / e["dynamic"] for e in group])
            peakRatio = st.median(
                [e["original"] / e["peak"] for e in group if e["peak"]]
            )
            handle.write(
                f"| {name} | {len(group)} | {staticRatio:.1f}× | {dynamicRatio:.1f}× | "
                f"{peakRatio:.1f}× |\n"
            )
    print(f"wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
