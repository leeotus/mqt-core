#!/usr/bin/env python3
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
"""Merge the raw density-matrix runs into one table and a short report.

The runner stores one JSON file per circuit, arm and repetition. This script
reads those files, compares every arm against the run without reordering, and
writes a CSV plus a Markdown summary.
"""

from __future__ import annotations

import json
import pathlib
import statistics
import sys
from typing import Any

ARMS = [
    "none",
    "degree",
    "spectral",
    "rb",
    "dyn500",
    "dyn2000",
    "dyn2000_w4",
    "dyn500_w4",
    "spectral_dyn500",
    "dyn500_node",
]

ARM_LABELS = {
    "none": "natural order",
    "degree": "static degree",
    "spectral": "static spectral",
    "rb": "static recursive bisection",
    "dyn500": "dynamic, threshold 500",
    "dyn2000": "dynamic, threshold 2000",
    "dyn2000_w4": "dynamic, threshold 2000, window 4",
    "dyn500_w4": "dynamic, threshold 500, window 4",
    "spectral_dyn500": "static spectral + dynamic, threshold 500",
    "dyn500_node": "dynamic, threshold 500, window 4, rebuild exchange",
}

FIELDS = [
    "circuit",
    "arm",
    "rep",
    "n_qubits",
    "applied_gates",
    "status",
    "simulation_time_ms",
    "peak_nodes",
    "final_nodes",
    "reorderings",
    "reorder_time_ms",
    "reorder_share",
    "peak_ratio",
    "final_ratio",
    "time_ratio",
    "ref_top20_share",
    "tvd_informative",
    "tvd_vs_natural",
    "sorted_vs_natural",
]


def distribution(raw: dict[str, Any]) -> dict[str, float]:
    counts = raw.get("measurement_results", {})
    total = sum(counts.values())
    if total == 0:
        return {}
    return {key: value / total for key, value in counts.items()}


def total_variation(
    left: dict[str, float], right: dict[str, float]
) -> float:
    keys = set(left) | set(right)
    return 0.5 * sum(
        abs(left.get(key, 0.0) - right.get(key, 0.0)) for key in keys
    )


def sorted_difference(
    left: dict[str, float], right: dict[str, float]
) -> float:
    """Largest gap between the sorted probability lists.

    The simulator samples in decision-diagram level order, so a reordering
    permutes the histogram. This measure ignores that relabelling. A state
    that one run draws and the other does not is padded with a zero
    probability, because the number of drawn states is itself subject to
    sampling noise.
    """
    a = sorted(left.values())
    b = sorted(right.values())
    length = max(len(a), len(b))
    a = [0.0] * (length - len(a)) + a
    b = [0.0] * (length - len(b)) + b
    return max((abs(x - y) for x, y in zip(a, b)), default=0.0)


def ratio(current: Any, base: Any) -> float:
    if not isinstance(current, (int, float)) or not base:
        return float("nan")
    return current / base


def topShare(counts: dict[str, Any], top: int = 20) -> float:
    """Share of the sampled mass that the `top` most likely states carry."""
    values = sorted(counts.values(), reverse=True)
    total = sum(values)
    if total == 0:
        return 0.0
    return sum(values[:top]) / total


def loadRuns(work: pathlib.Path) -> dict[tuple[str, str, int], dict[str, Any]]:
    runs: dict[tuple[str, str, int], dict[str, Any]] = {}
    for path in sorted((work / "json").glob("*.json")):
        stem = path.stem
        parts = stem.split("__")
        if len(parts) != 3:
            print(f"skipping unexpected file name {path}", file=sys.stderr)
            continue
        circuit, arm, rep = parts
        runs[(circuit, arm, int(rep.removeprefix("rep")))] = {
            "text": path.read_text(),
        }
    return runs


def parse(raw: str) -> dict[str, Any] | None:
    if not raw.lstrip().startswith("{"):
        return None
    return json.loads(raw)


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} WORK RESULTS", file=sys.stderr)
        return 2
    work = pathlib.Path(sys.argv[1])
    results = pathlib.Path(sys.argv[2])
    results.mkdir(parents=True, exist_ok=True)

    runs = loadRuns(work)
    parsed: dict[tuple[str, str, int], dict[str, Any] | None] = {
        key: parse(value["text"]) for key, value in runs.items()
    }

    rows: list[dict[str, Any]] = []
    for (circuit, arm, rep), data in sorted(parsed.items()):
        if data is None:
            rows.append(
                {
                    "circuit": circuit,
                    "arm": arm,
                    "rep": rep,
                    "status": runs[(circuit, arm, rep)]["text"].strip(),
                }
            )
            continue
        stats = data.get("statistics", data.get("stats", {}))
        baseline = parsed.get((circuit, "none", rep))
        tvd = float("nan")
        sortedTvd = float("nan")
        peakRatio = float("nan")
        finalRatio = float("nan")
        timeRatio = float("nan")
        if baseline is not None:
            baseStats = baseline.get("statistics", baseline.get("stats", {}))
            tvd = total_variation(
                distribution(baseline), distribution(data)
            )
            sortedTvd = sorted_difference(
                distribution(baseline), distribution(data)
            )
            referenceTopShare = topShare(
                baseline.get("measurement_results", {})
            )
            peakRatio = ratio(stats.get("peak_nodes"), baseStats.get("peak_nodes"))
            finalRatio = ratio(
                stats.get("final_nodes"), baseStats.get("final_nodes")
            )
            timeRatio = ratio(
                stats.get("simulation_time"), baseStats.get("simulation_time")
            )
        simulationMs = 1000.0 * float(stats.get("simulation_time", float("nan")))
        reorderMs = float(stats.get("reorder_time_ms", 0.0))
        rows.append(
            {
                "circuit": circuit,
                "arm": arm,
                "rep": rep,
                "n_qubits": stats.get("n_qubits"),
                "applied_gates": stats.get("applied_gates"),
                "status": "ok",
                "simulation_time_ms": round(simulationMs, 3),
                "peak_nodes": stats.get("peak_nodes"),
                "final_nodes": stats.get("final_nodes"),
                "reorderings": stats.get("reorderings"),
                "reorder_time_ms": round(reorderMs, 3),
                "reorder_share": (
                    round(reorderMs / simulationMs, 4) if simulationMs else 0.0
                ),
                "peak_ratio": (
                    round(peakRatio, 4) if peakRatio == peakRatio else ""
                ),
                "final_ratio": (
                    round(finalRatio, 4) if finalRatio == finalRatio else ""
                ),
                "time_ratio": (
                    round(timeRatio, 4) if timeRatio == timeRatio else ""
                ),
                "tvd_vs_natural": round(tvd, 5) if tvd == tvd else "",
                "sorted_vs_natural": (
                    round(sortedTvd, 5) if sortedTvd == sortedTvd else ""
                ),
                "ref_top20_share": round(referenceTopShare, 5),
                "tvd_informative": "yes" if referenceTopShare >= 0.99 else "no",
            }
        )

    csvPath = results / "ddsim_dynamic_reordering.csv"
    with csvPath.open("w") as handle:
        handle.write(",".join(FIELDS) + "\n")
        for row in rows:
            handle.write(
                ",".join(str(row.get(field, "")) for field in FIELDS) + "\n"
            )

    ok = [row for row in rows if row.get("status") == "ok"]
    circuits = sorted({row["circuit"] for row in ok})
    byKey = {(row["circuit"], row["arm"]): row for row in ok}

    lines = ["# Density-matrix reordering experiment", ""]
    lines.append(
        f"{len(circuits)} circuits, {len(ARMS)} arms, "
        f"{len(ok)} completed runs."
    )
    lines.append("")
    lines.append(
        "`peak_ratio` and `final_ratio` divide by the run with the natural "
        "order. `time_ratio` divides the wall time of the simulation by the "
        "wall time of the natural order. `reorder_share` is the part of the "
        "simulation time that the reordering itself used. `tvd_vs_natural` "
        "compares the sampled distribution with the natural order. The "
        "sampling order follows the decision-diagram level order, so a "
        "reordering relabels the histogram; `sorted_vs_natural` ignores that "
        "relabelling and is the value that decides correctness."
    )
    lines.append("")
    lines.append(
        "The TVD columns are only meaningful when the reference run "
        "concentrates its mass. With 20000 shots, a circuit that spreads its "
        "mass over thousands of basis states has a sampling noise floor far "
        "above 0.02, so two independent samples of the *same* distribution "
        "also differ by 0.1 to 1.0. `ref_top20_share` records the share of "
        "the mass that the 20 most likely states carry, and `tvd_informative` "
        "is `yes` only when that share is at least 0.99. The TVD medians "
        "below use the informative rows only. Correctness is established by "
        "the unit tests and by `ddsim_dynamic_acceptance.txt`, not by this "
        "column."
    )
    lines.append("")
    lines.append(
        "| arm | circuits | median peak | median final | wins peak | "
        "median time | median TVD | median sorted |"
    )
    lines.append("| --- | --- | --- | --- | --- | --- | --- | --- |")
    for arm in ARMS:
        peak = [
            byKey[(circuit, arm)]["peak_ratio"]
            for circuit in circuits
            if (circuit, arm) in byKey
            and byKey[(circuit, arm)]["peak_ratio"] != ""
        ]
        final = [
            byKey[(circuit, arm)]["final_ratio"]
            for circuit in circuits
            if (circuit, arm) in byKey
            and byKey[(circuit, arm)]["final_ratio"] != ""
        ]
        time = [
            byKey[(circuit, arm)]["time_ratio"]
            for circuit in circuits
            if (circuit, arm) in byKey
            and byKey[(circuit, arm)]["time_ratio"] != ""
        ]
        tvd = [
            byKey[(circuit, arm)]["tvd_vs_natural"]
            for circuit in circuits
            if (circuit, arm) in byKey
            and byKey[(circuit, arm)]["tvd_vs_natural"] != ""
            and byKey[(circuit, arm)]["tvd_informative"] == "yes"
        ]
        sortedTvd = [
            byKey[(circuit, arm)]["sorted_vs_natural"]
            for circuit in circuits
            if (circuit, arm) in byKey
            and byKey[(circuit, arm)]["sorted_vs_natural"] != ""
            and byKey[(circuit, arm)]["tvd_informative"] == "yes"
        ]
        if not final:
            continue
        wins = sum(1 for value in final if value < 1.0)
        lines.append(
            "| {} | {} | {} | {} | {}/{} | {} | {} | {} |".format(
                ARM_LABELS.get(arm, arm),
                len(final),
                f"{statistics.median(peak):.3f}" if peak else "",
                f"{statistics.median(final):.3f}",
                wins,
                len(final),
                f"{statistics.median(time):.2f}" if time else "",
                f"{statistics.median(tvd):.5f}" if tvd else "",
                f"{statistics.median(sortedTvd):.5f}" if sortedTvd else "",
            )
        )
    lines.append("")
    summaryPath = results / "ddsim_dynamic_reordering_summary.md"
    summaryPath.write_text("\n".join(lines) + "\n")
    print(f"wrote {csvPath} ({len(rows)} rows) and {summaryPath}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
