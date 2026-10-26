#!/usr/bin/env python3

# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT

"""Download deterministic QASM files from the MQT Bench website."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import urllib.error
import urllib.request
from pathlib import Path

DOWNLOAD_URL = "https://mqt-bench.app/api/v1/download-configuration.qasm"
VERSION_PATTERN = re.compile(r"^// MQT Bench version:\s*(\S+)\s*$", re.MULTILINE)


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments.

    Returns:
        Parsed arguments.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--refresh",
        action="store_true",
        help="download existing files again instead of reusing them",
    )
    return parser.parse_args()


def download(configuration: dict[str, object]) -> tuple[bytes, str]:
    """Download one QASM file from the MQT Bench website.

    Args:
        configuration: MQT Bench configuration dictionary.

    Returns:
        Downloaded bytes and the server-provided filename.

    Raises:
        RuntimeError: If the download fails or does not contain QASM.
    """
    request = urllib.request.Request(
        DOWNLOAD_URL,
        data=json.dumps({"configuration": configuration}).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=300) as response:
            content = response.read()
            disposition = response.headers.get("Content-Disposition", "")
            match = re.search(r'filename="([^"]+)"', disposition)
            filename = match.group(1) if match else ""
    except (urllib.error.URLError, TimeoutError) as error:
        msg = f"failed to download configuration {configuration}: {error}"
        raise RuntimeError(msg) from error

    if b"OPENQASM" not in content:
        preview = content[:500].decode(errors="replace")
        msg = f"download did not contain QASM: {preview}"
        raise RuntimeError(msg)
    return content, filename


def main() -> int:
    """Download all configurations listed in an experiment manifest.

    Returns:
        Process exit code.
    """
    args = parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    generated_path = args.output / "generated_circuits.tsv"

    with args.manifest.open(newline="", encoding="utf-8") as handle:
        entries = list(csv.DictReader(handle))

    rows = []
    for entry in entries:
        benchmark = entry["benchmark"]
        level = entry["level"]
        circuit_size = int(entry["num_qubits"])
        opt_level = int(entry["opt_level"])
        configuration = {
            "group": level,
            "options": {
                "algorithm": benchmark,
                "circuit_size": circuit_size,
                "opt_level": opt_level,
            },
        }

        expected_name = f"{benchmark}_{level}_opt{opt_level}_{circuit_size}.qasm"
        destination = args.output / expected_name
        if args.refresh or not destination.exists():
            content, server_name = download(configuration)
            if server_name and expected_name != server_name:
                print(
                    f"using server filename {server_name} instead of {expected_name}"
                )
                destination = args.output / server_name
            destination.write_bytes(content)

        content = destination.read_bytes()
        if b"OPENQASM" not in content:
            msg = f"{destination} does not contain QASM"
            raise RuntimeError(msg)
        version_match = VERSION_PATTERN.search(content.decode(errors="replace"))
        version = version_match.group(1) if version_match else ""
        rows.append((
            benchmark,
            level,
            str(opt_level),
            str(circuit_size),
            "site-cached",
            hashlib.sha256(content).hexdigest(),
            str(destination),
            DOWNLOAD_URL,
            version,
        ))

    with generated_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        writer.writerow([
            "benchmark",
            "level",
            "opt_level",
            "num_qubits",
            "seed",
            "sha256",
            "path",
            "source_url",
            "mqt_bench_version",
        ])
        writer.writerows(rows)
    print(f"wrote {len(rows)} circuits to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
