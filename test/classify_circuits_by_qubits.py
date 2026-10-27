#!/usr/bin/env python3

# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT

"""Classify QASM circuits into q<N> directories by qubit count."""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
import re
import shutil
from pathlib import Path

QREG = re.compile(r"^\s*qreg\s+[A-Za-z_]\w*\s*\[\s*(\d+)\s*\]\s*;")
QUBIT = re.compile(
    r"^\s*qubit\s*(?:\[\s*(\d+)\s*\])?\s+[A-Za-z_]\w*"
    r"(?:\s*\[\s*(\d+)\s*\])?\s*;",
)
IMPLICIT_QUBIT = re.compile(r"\$(\d+)")


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments.

    Returns:
        Parsed arguments.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument(
        "--source",
        action="append",
        default=[],
        help="source directory below root; defaults to all non-qN directories",
    )
    return parser.parse_args()


def qubit_count(path: Path) -> int:
    """Return the number of qubits declared by a QASM file.

    Args:
        path: QASM file.

    Returns:
        Number of declared qubits.

    Raises:
        ValueError: If no qubit declaration can be parsed.
    """
    count = 0
    highest_implicit_index = -1
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if match := QREG.match(line):
            count += int(match.group(1))
            continue
        if match := QUBIT.match(line):
            size = match.group(1) or match.group(2)
            count += int(size) if size else 1
            continue
        for implicit in IMPLICIT_QUBIT.finditer(line):
            highest_implicit_index = max(highest_implicit_index, int(implicit.group(1)))
    count += highest_implicit_index + 1
    if count == 0:
        msg = f"could not determine qubit count for {path}"
        raise ValueError(msg)
    return count


def source_directories(root: Path, configured: list[str]) -> list[Path]:
    """Return source directories to classify.

    Args:
        root: Quantum circuit root.
        configured: Explicit source directory names.

    Returns:
        Existing source directories.
    """
    if configured:
        directories = [root / name for name in configured]
    else:
        directories = [
            path
            for path in root.iterdir()
            if path.is_dir() and not re.fullmatch(r"q\d+", path.name)
        ]
    missing = [path for path in directories if not path.is_dir()]
    if missing:
        msg = "missing source directories: " + ", ".join(map(str, missing))
        raise ValueError(msg)
    return directories


def main() -> int:
    """Classify all QASM files below the configured source directories.

    Returns:
        Process exit code.
    """
    args = parse_args()
    sources = source_directories(args.root, args.source)
    files = sorted(
        path
        for source in sources
        for path in source.rglob("*.qasm")
        if path.is_file()
    )

    rows = []
    failures = []
    seen_content: dict[tuple[int, str], Path] = {}
    used_names: dict[int, set[str]] = {}
    skipped_duplicates = 0
    for source_path in files:
        content = source_path.read_bytes()
        digest = hashlib.sha256(content).hexdigest()
        try:
            num_qubits = qubit_count(source_path)
        except ValueError as error:
            failures.append({
                "path": str(source_path),
                "status": "unresolved-qubit-count",
                "message": str(error),
            })
            continue
        content_key = (num_qubits, digest)
        if content_key in seen_content:
            skipped_duplicates += 1
            continue

        destination_dir = args.root / f"q{num_qubits}"
        destination_dir.mkdir(exist_ok=True)
        names = used_names.setdefault(num_qubits, set())
        filename = source_path.name
        if filename in names:
            filename = f"{source_path.stem}__{digest[:10]}.qasm"
        names.add(filename)
        destination = destination_dir / filename
        if destination.exists():
            destination.unlink()
        try:
            os.link(source_path, destination)
        except OSError:
            shutil.copy2(source_path, destination)
        seen_content[content_key] = destination
        rows.append({
            "qubits": str(num_qubits),
            "circuit": filename,
            "sha256": digest,
            "path": str(destination),
            "source_path": str(source_path),
        })

    manifest = args.root / "qubit_classification.tsv"
    with manifest.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]), delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)
    failure_manifest = args.root / "qubit_classification_failures.tsv"
    with failure_manifest.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=["path", "status", "message"],
            delimiter="\t",
        )
        writer.writeheader()
        writer.writerows(failures)
    print(
        f"classified {len(rows)} circuits; skipped {skipped_duplicates} "
        f"identical duplicates"
    )
    print(f"unclassified files: {len(failures)}")
    print(f"manifest: {manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
