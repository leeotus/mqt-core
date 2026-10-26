#!/usr/bin/env python3

# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT

"""Download every completed configuration from the MQT Bench website."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import re
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from typing import Any

BROWSER_URL = "https://mqt-bench.app/api/v1/benchmark-browser"
DOWNLOAD_URL = "https://mqt-bench.app/api/v1/download-configurations.zip"
VERSION_PATTERN = re.compile(r"^// MQT Bench version:\s*(\S+)\s*$", re.MULTILINE)


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments.

    Returns:
        Parsed arguments.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--batch-size", type=int, default=50)
    parser.add_argument("--retries", type=int, default=3)
    parser.add_argument("--max-qubits", type=int, default=32)
    parser.add_argument(
        "--refresh",
        action="store_true",
        help="download configurations even when their files already exist",
    )
    return parser.parse_args()


def request_json(url: str, retries: int) -> dict[str, Any]:
    """Fetch and decode a JSON document.

    Args:
        url: URL to request.
        retries: Number of attempts after the first failure.

    Returns:
        Decoded JSON object.
    """
    last_error: Exception | None = None
    for attempt in range(retries + 1):
        try:
            with urllib.request.urlopen(url, timeout=120) as response:
                return json.load(response)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            last_error = error
            if attempt < retries:
                time.sleep(2**attempt)
    msg = f"failed to fetch {url}: {last_error}"
    raise RuntimeError(msg)


def configuration(record: dict[str, Any]) -> dict[str, Any]:
    """Convert a browser record to a download configuration.

    Args:
        record: MQT Bench browser record.

    Returns:
        Configuration accepted by the download endpoint.
    """
    options: dict[str, Any] = {
        "algorithm": record["algorithm"],
        "circuit_size": int(record["circuit_size"]),
        "opt_level": int(record["opt_level"]),
        "mirror": record["mirror"],
        "symbolic_parameters": record["symbolic_parameters"],
    }
    if record["target"]:
        options["target"] = record["target"]
    return {"group": record["level"], "options": options}


def configuration_key(record: dict[str, Any]) -> tuple[Any, ...]:
    """Return the fields that uniquely identify a benchmark configuration.

    Args:
        record: MQT Bench browser record.

    Returns:
        Hashable configuration key.
    """
    return (
        record["algorithm"],
        record["level"],
        record["target"] or None,
        record["circuit_size"],
        record["opt_level"],
        record["mirror"],
        record["symbolic_parameters"],
    )


def enumerate_records(retries: int) -> list[dict[str, Any]]:
    """Fetch all completed benchmark records from the website.

    Args:
        retries: Number of retries per page request.

    Returns:
        Deduplicated records, newest first.
    """
    first = request_json(f"{BROWSER_URL}?page=1&per_page=100", retries)
    total = int(first["total_count"])
    records = first["history"]
    pages = (total + 99) // 100
    for page in range(2, pages + 1):
        response = request_json(
            f"{BROWSER_URL}?page={page}&per_page=100",
            retries,
        )
        records.extend(response["history"])
        print(f"enumerated {len(records)}/{total}", flush=True)

    unique: dict[tuple[Any, ...], dict[str, Any]] = {}
    for record in records:
        if record["status"] == "completed" and record["has_result"]:
            unique.setdefault(configuration_key(record), record)
    return list(unique.values())


def download_batch(
    records: list[dict[str, Any]],
    retries: int,
) -> tuple[bytes, str]:
    """Download a batch archive from MQT Bench.

    Args:
        records: Records to include in the batch.
        retries: Number of attempts after the first failure.

    Returns:
        Zip archive bytes and response filename.
    """
    payload = [configuration(record) for record in records]
    request = urllib.request.Request(
        DOWNLOAD_URL,
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    last_error: Exception | None = None
    for attempt in range(retries + 1):
        try:
            with urllib.request.urlopen(request, timeout=300) as response:
                content = response.read()
                disposition = response.headers.get("Content-Disposition", "")
                match = re.search(r'filename="([^"]+)"', disposition)
                filename = match.group(1) if match else "results.zip"
            if not zipfile.is_zipfile(io.BytesIO(content)):
                msg = content[:500].decode(errors="replace")
                raise RuntimeError(f"response is not a ZIP archive: {msg}")
            return content, filename
        except (urllib.error.URLError, TimeoutError, RuntimeError) as error:
            last_error = error
            if attempt < retries:
                time.sleep(2**attempt)
    msg = f"failed to download batch of {len(records)}: {last_error}"
    raise RuntimeError(msg)


def member_name(record: dict[str, Any], server_name: str, used: set[str]) -> str:
    """Create a unique local filename for one archive member.

    Args:
        record: Benchmark record associated with the member.
        server_name: Filename stored in the ZIP archive.
        used: Filenames already used in the destination directory.

    Returns:
        Unique local filename.
    """
    candidate = Path(server_name).name
    if candidate not in used:
        return candidate
    digest = hashlib.sha256(repr(configuration_key(record)).encode()).hexdigest()[:10]
    return f"{Path(candidate).stem}__{digest}{Path(candidate).suffix}"


def write_manifest(path: Path, rows: list[dict[str, str]]) -> None:
    """Write the download manifest atomically.

    Args:
        path: Manifest destination.
        rows: Manifest rows.
    """
    temporary = path.with_suffix(".tmp")
    columns = [
        "id",
        "benchmark",
        "level",
        "target",
        "opt_level",
        "num_qubits",
        "mirror",
        "symbolic_parameters",
        "sha256",
        "path",
        "server_filename",
        "source_url",
        "mqt_bench_version",
    ]
    with temporary.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns, delimiter="\t")
        writer.writeheader()
        writer.writerows(sorted(rows, key=lambda row: int(row["id"])))
    temporary.replace(path)


def write_failures(path: Path, rows: list[dict[str, str]]) -> None:
    """Write download failures atomically.

    Args:
        path: Failure report destination.
        rows: Failure rows.
    """
    temporary = path.with_suffix(".tmp")
    columns = [
        "id",
        "benchmark",
        "level",
        "target",
        "opt_level",
        "num_qubits",
        "mirror",
        "symbolic_parameters",
        "status",
        "message",
        "payload_path",
    ]
    with temporary.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns, delimiter="\t")
        writer.writeheader()
        writer.writerows(sorted(rows, key=lambda row: int(row["id"])))
    temporary.replace(path)


def write_catalogue(path: Path, records: list[dict[str, Any]]) -> None:
    """Write all discovered website configurations.

    Args:
        path: Catalogue destination.
        records: Benchmark records.
    """
    temporary = path.with_suffix(".tmp")
    columns = [
        "id",
        "benchmark",
        "level",
        "target",
        "opt_level",
        "num_qubits",
        "mirror",
        "symbolic_parameters",
    ]
    with temporary.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns, delimiter="\t")
        writer.writeheader()
        for record in sorted(records, key=lambda item: int(item["id"])):
            writer.writerow({
                "id": str(record["id"]),
                "benchmark": record["algorithm"],
                "level": record["level"],
                "target": record["target"] or "",
                "opt_level": str(record["opt_level"]),
                "num_qubits": str(record["circuit_size"]),
                "mirror": record["mirror"],
                "symbolic_parameters": record["symbolic_parameters"],
            })
    temporary.replace(path)


def main() -> int:
    """Download all completed MQT Bench configurations.

    Returns:
        Process exit code.
    """
    args = parse_args()
    if not 1 <= args.batch_size <= 50:
        msg = "--batch-size must be between 1 and 50"
        raise ValueError(msg)
    args.output.mkdir(parents=True, exist_ok=True)
    manifest_path = args.output / "all_circuits_manifest.tsv"
    failures_path = args.output / "download_failures.tsv"
    catalogue_path = args.output / "available_configurations.tsv"
    error_directory = args.output / "errors"
    error_directory.mkdir(exist_ok=True)
    all_records = enumerate_records(args.retries)
    records = [
        record
        for record in all_records
        if int(record["circuit_size"]) <= args.max_qubits
    ]
    print(
        f"unique completed configurations: {len(all_records)}; "
        f"at most {args.max_qubits} qubits: {len(records)}",
        flush=True,
    )
    write_catalogue(catalogue_path, records)

    existing_rows: dict[tuple[Any, ...], dict[str, str]] = {}
    if not args.refresh and manifest_path.is_file():
        with manifest_path.open(newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle, delimiter="\t"):
                key = (
                    row["benchmark"],
                    row["level"],
                    row["target"] or None,
                    int(row["num_qubits"]),
                    int(row["opt_level"]),
                    row["mirror"],
                    row["symbolic_parameters"],
                )
                if Path(row["path"]).is_file():
                    existing_rows[key] = row

    pending = [record for record in records if configuration_key(record) not in existing_rows]
    print(f"already downloaded: {len(existing_rows)}", flush=True)
    print(f"pending: {len(pending)}", flush=True)

    rows = list(existing_rows.values())
    failures: list[dict[str, str]] = []
    used_names: dict[str, set[str]] = {}
    for batch_start in range(0, len(pending), args.batch_size):
        batch = pending[batch_start : batch_start + args.batch_size]
        archive_bytes, _ = download_batch(batch, args.retries)
        with zipfile.ZipFile(io.BytesIO(archive_bytes)) as archive:
            members = [entry for entry in archive.infolist() if not entry.is_dir()]
            if len(members) != len(batch):
                msg = f"expected {len(batch)} files, archive contains {len(members)}"
                raise RuntimeError(msg)
            for record, member in zip(batch, members, strict=True):
                content = archive.read(member)
                if b"OPENQASM" not in content:
                    digest = hashlib.sha256(
                        repr(configuration_key(record)).encode()
                    ).hexdigest()[:10]
                    payload_path = (
                        error_directory
                        / f"{Path(member.filename).stem}__{digest}.invalid.txt"
                    )
                    payload_path.write_bytes(content)
                    failures.append({
                        "id": str(record["id"]),
                        "benchmark": record["algorithm"],
                        "level": record["level"],
                        "target": record["target"] or "",
                        "opt_level": str(record["opt_level"]),
                        "num_qubits": str(record["circuit_size"]),
                        "mirror": record["mirror"],
                        "symbolic_parameters": record["symbolic_parameters"],
                        "status": "invalid-payload",
                        "message": (
                            "server returned non-QASM content: "
                            f"{content[:200].decode(errors='replace')}"
                        ),
                        "payload_path": str(payload_path),
                    })
                    continue
                level_directory = args.output / record["level"]
                level_directory.mkdir(parents=True, exist_ok=True)
                used = used_names.setdefault(record["level"], set())
                filename = member_name(record, member.filename, used)
                used.add(filename)
                destination = level_directory / filename
                destination.write_bytes(content)
                version_match = VERSION_PATTERN.search(
                    content.decode(errors="replace")
                )
                rows.append({
                    "id": str(record["id"]),
                    "benchmark": record["algorithm"],
                    "level": record["level"],
                    "target": record["target"] or "",
                    "opt_level": str(record["opt_level"]),
                    "num_qubits": str(record["circuit_size"]),
                    "mirror": record["mirror"],
                    "symbolic_parameters": record["symbolic_parameters"],
                    "sha256": hashlib.sha256(content).hexdigest(),
                    "path": str(destination),
                    "server_filename": member.filename,
                    "source_url": DOWNLOAD_URL,
                    "mqt_bench_version": (
                        version_match.group(1) if version_match else ""
                    ),
                })
        write_manifest(manifest_path, rows)
        write_failures(failures_path, failures)
        print(
            f"downloaded {min(batch_start + len(batch), len(pending))}/{len(pending)} "
            f"new configurations",
            flush=True,
        )

    write_manifest(manifest_path, rows)
    write_failures(failures_path, failures)
    print(f"wrote {len(rows)} circuits to {args.output}", flush=True)
    print(f"manifest: {manifest_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
