#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Download every completed MQT Bench configuration with at most 32 qubits.
#
# Usage:
#   bash test/download_all_mqt_bench.sh [OUTPUT_DIRECTORY] [--refresh]

set -euo pipefail

ROOT="${MQT_CORE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
OUT="${1:-/home/flareon/workshop/quantum_circuits/mqt_bench_all}"
ARGS=()

if [[ "${2:-}" == "--refresh" ]]; then
  ARGS=(--refresh)
elif [[ $# -gt 1 ]]; then
  printf 'usage: %s [OUTPUT_DIRECTORY] [--refresh]\n' "$0" >&2
  exit 2
fi

python3 "${ROOT}/test/download_all_mqt_bench.py" "${OUT}" "${ARGS[@]}"
