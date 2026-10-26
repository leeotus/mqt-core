#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Populate the local quantum circuit corpus. This is deliberately separate from
# benchmark execution: experiments must consume a frozen local snapshot.
#
# Usage:
#   bash test/bootstrap_quantum_corpus.sh [--refresh]

set -euo pipefail

ROOT="${MQT_CORE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CIRCUIT_ROOT="${MQT_CIRCUIT_ROOT:-/home/flareon/workshop/quantum_circuits}"
MQT_CIRCUITS="${CIRCUIT_ROOT}/mqt_bench_web"
QASM_CIRCUITS="${CIRCUIT_ROOT}/qasmbench"
QASMBENCH_ROOT="${QASMBENCH_ROOT:-/home/flareon/workshop/QASMBench}"
REFRESH_ARGS=()

if [[ "${1:-}" == "--refresh" ]]; then
  REFRESH_ARGS=(--refresh)
elif [[ $# -gt 0 ]]; then
  printf 'usage: %s [--refresh]\n' "$0" >&2
  exit 2
fi

mkdir -p "${MQT_CIRCUITS}" "${QASM_CIRCUITS}"
python3 "${ROOT}/test/download_mqt_bench_circuits.py" \
  "${ROOT}/test/quantum_benchmarks.csv" \
  "${MQT_CIRCUITS}" \
  "${REFRESH_ARGS[@]}"

if [[ -d "${QASMBENCH_ROOT}" ]]; then
  python3 "${ROOT}/test/collect_qasmbench.py" \
    "${MQT_CIRCUITS}/generated_circuits.tsv" \
    "${QASMBENCH_ROOT}" \
    "${ROOT}/test/qasmbench_circuits.csv" \
    "${QASM_CIRCUITS}" \
    "${CIRCUIT_ROOT}/circuits_manifest.tsv"
else
  cp "${MQT_CIRCUITS}/generated_circuits.tsv" \
    "${CIRCUIT_ROOT}/circuits_manifest.tsv"
fi

python3 "${ROOT}/test/verify_quantum_corpus.py" \
  "${CIRCUIT_ROOT}/circuits_manifest.tsv"
