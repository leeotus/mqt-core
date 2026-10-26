#!/usr/bin/env python3

# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT

"""Verify that every circuit in a corpus manifest exists and matches its hash."""

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
    parser.add_argument("manifest", type=Path)
    return parser.parse_args()


def main() -> int:
    """Validate a corpus manifest.

    Returns:
        Process exit code.

    Raises:
        RuntimeError: If the manifest contains duplicate names, missing files,
            or checksum mismatches.
    """
    args = parse_args()
    with args.manifest.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle, delimiter="\t"))

    failures = []
    seen_names = set()
    for row in rows:
        path = Path(row["path"])
        if path.name in seen_names:
            failures.append(f"duplicate circuit basename: {path.name}")
        seen_names.add(path.name)
        if not path.is_file():
            failures.append(f"missing file: {path}")
            continue
        checksum = hashlib.sha256(path.read_bytes()).hexdigest()
        if checksum != row["sha256"]:
            failures.append(
                f"checksum mismatch: {path} expected {row['sha256']}, got {checksum}"
            )

    if failures:
        raise RuntimeError("\n".join(failures))
    print(f"verified {len(rows)} circuits from {args.manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
