#!/usr/bin/env python3

# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT

"""Copy selected QASMBench circuits and append them to an MQT Bench manifest."""

from __future__ import annotations

import argparse
import csv
import hashlib
import re
import subprocess
from pathlib import Path

REGISTER_DECLARATION = re.compile(r"^\s*(qreg|creg)\s+([A-Za-z_]\w*)\s*\[\d+\]\s*;")
MEASUREMENT = re.compile(
    r"^\s*measure\s+([A-Za-z_]\w*)\s*\[.*\]\s*->\s*"
    r"([A-Za-z_]\w*)\s*\[.*\]\s*;",
)


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments.

    Returns:
        Parsed arguments.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument("mqt_manifest", type=Path)
    parser.add_argument("qasmbench_root", type=Path)
    parser.add_argument("selection", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("output_manifest", type=Path)
    return parser.parse_args()


def git_revision(repository: Path) -> str:
    """Return the current Git revision of a repository.

    Args:
        repository: Git repository path.

    Returns:
        Git commit hash.
    """
    return subprocess.run(
        ["git", "-C", str(repository), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def main() -> int:
    """Collect QASMBench circuits and merge manifests.

    Returns:
        Process exit code.
    """
    args = parse_args()
    args.destination.mkdir(parents=True, exist_ok=True)

    with args.mqt_manifest.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle, delimiter="\t"))

    with args.selection.open(newline="", encoding="utf-8") as handle:
        entries = list(csv.DictReader(handle))
    revision = git_revision(args.qasmbench_root)

    for entry in entries:
        source = args.qasmbench_root / entry["relative_path"]
        destination = args.destination / source.name
        if destination.exists():
            destination.chmod(0o644)

        source_lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
        qregs = {
            match.group(2)
            for line in source_lines
            if (match := REGISTER_DECLARATION.match(line))
            and match.group(1) == "qreg"
        }
        cregs = {
            match.group(2)
            for line in source_lines
            if (match := REGISTER_DECLARATION.match(line))
            and match.group(1) == "creg"
        }
        sanitized_lines = []
        removed_measurements = 0
        for line in source_lines:
            match = MEASUREMENT.match(line)
            if match and (match.group(1) not in qregs or match.group(2) not in cregs):
                removed_measurements += 1
                continue
            sanitized_lines.append(line)
        destination.write_text("".join(sanitized_lines), encoding="utf-8")
        if removed_measurements:
            print(
                f"{source.name}: removed {removed_measurements} measurement(s) "
                "with undeclared registers"
            )
        checksum = hashlib.sha256(destination.read_bytes()).hexdigest()
        rows.append({
            "benchmark": f"qasmbench_{entry['benchmark']}",
            "level": "qasmbench",
            "opt_level": "NA",
            "num_qubits": "NA",
            "seed": "NA",
            "sha256": checksum,
            "path": str(destination),
            "source_url": (
                "https://github.com/pnnl/QASMBench/blob/"
                f"{revision}/{entry['relative_path']}"
            ),
            "mqt_bench_version": f"QASMBench@{revision}",
        })

    with args.output_manifest.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=list(rows[0]),
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
