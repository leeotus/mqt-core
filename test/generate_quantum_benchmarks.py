#!/usr/bin/env python3

# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT

"""Generate deterministic MQT Bench circuits for ordering experiments."""

from __future__ import annotations

import argparse
import csv
import hashlib
from pathlib import Path

import qiskit.qasm3
from mqt.bench import BenchmarkLevel, get_benchmark
from mqt.bench.output import OutputFormat, generate_filename, write_circuit


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments.

    Returns:
        Parsed command-line arguments.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    return parser.parse_args()


def main() -> int:
    """Generate all circuits in a manifest.

    Returns:
        Process exit code.
    """
    args = parse_args()
    level = BenchmarkLevel.INDEP
    args.output.mkdir(parents=True, exist_ok=True)
    generated = args.output / "generated_circuits.tsv"

    with args.manifest.open(newline="", encoding="utf-8") as handle:
        entries = list(csv.DictReader(handle))

    rows = []
    for entry in entries:
        benchmark = entry["benchmark"]
        num_qubits = int(entry["num_qubits"])
        opt_level = int(entry["opt_level"])
        seed = int(entry["seed"])
        seed_options = {"seed": seed} if benchmark in {"graphstate", "qaoa"} else {}
        circuit = get_benchmark(
            benchmark=benchmark,
            level=level,
            circuit_size=num_qubits,
            opt_level=opt_level,
            random_parameters=True,
            **seed_options,
        )

        filename = generate_filename(
            benchmark_name=benchmark,
            level=level,
            num_qubits=num_qubits,
            opt_level=opt_level,
        )
        path = args.output / f"{filename}.qasm"
        write_circuit(circuit, path, level, OutputFormat.QASM3)
        canonical = qiskit.qasm3.dumps(circuit).encode()
        rows.append((
            benchmark,
            level.name.lower(),
            str(opt_level),
            str(num_qubits),
            str(seed),
            hashlib.sha256(canonical).hexdigest(),
            str(path),
        ))

    with generated.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        writer.writerow([
            "benchmark",
            "level",
            "opt_level",
            "num_qubits",
            "seed",
            "sha256",
            "path",
        ])
        writer.writerows(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
