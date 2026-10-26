#!/usr/bin/env python3

# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT

"""Create a verified manifest from a directory of QASM circuits."""

from __future__ import annotations

import argparse
import csv
import hashlib
from pathlib import Path


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments.

    Returns:
        Parsed arguments.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument("circuit_directory", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument(
        "--extension",
        action="append",
        default=[],
        help="file extension to include; defaults to .qasm and .qasm2",
    )
    return parser.parse_args()


def main() -> int:
    """Build a manifest for all matching files under a directory.

    Returns:
        Process exit code.

    Raises:
        ValueError: If no matching files or duplicate basenames exist.
    """
    args = parse_args()
    extensions = {
        extension if extension.startswith(".") else f".{extension}"
        for extension in (args.extension or ["qasm", "qasm2", "qasm3"])
    }
    files = sorted(
        path
        for path in args.circuit_directory.rglob("*")
        if path.is_file() and path.suffix.lower() in extensions
    )
    if not files:
        msg = f"no circuit files found below {args.circuit_directory}"
        raise ValueError(msg)

    names: dict[str, Path] = {}
    rows = []
    for path in files:
        if path.name in names:
            msg = (
                f"duplicate circuit basename {path.name}: "
                f"{names[path.name]} and {path}"
            )
            raise ValueError(msg)
        names[path.name] = path
        content = path.read_bytes()
        rows.append({
            "circuit": path.name,
            "benchmark": path.stem,
            "level": "directory",
            "qubits": "",
            "gates": "",
            "sha256": hashlib.sha256(content).hexdigest(),
            "path": str(path.resolve()),
            "source_url": "",
            "mqt_bench_version": "",
        })

    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    with args.manifest.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]), delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {len(rows)} circuits to {args.manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
