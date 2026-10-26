#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Download the quantum benchmark circuits used by the variable-ordering
# experiments from the MQT Bench website.
#
# Usage:
#   bash test/fetch_quantum_benchmarks.sh \
#     [MANIFEST] [OUTPUT_DIRECTORY] [--refresh]

set -euo pipefail

ROOT="${MQT_CORE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
MANIFEST="${1:-${ROOT}/test/quantum_benchmarks.csv}"
OUT_DIR="${2:-/home/flareon/workshop/results/quantum_ordering/circuits}"
REFRESH_ARGS=()
if [[ "${3:-}" == "--refresh" ]]; then
  REFRESH_ARGS=(--refresh)
elif [[ $# -gt 2 ]]; then
  printf 'usage: %s [MANIFEST] [OUTPUT_DIRECTORY] [--refresh]\n' "$0" >&2
  exit 2
fi

mkdir -p "${OUT_DIR}"
python3 "${ROOT}/test/download_mqt_bench_circuits.py" \
  "${MANIFEST}" "${OUT_DIR}" "${REFRESH_ARGS[@]}"
printf 'wrote %s\n' "${OUT_DIR}/generated_circuits.tsv"
