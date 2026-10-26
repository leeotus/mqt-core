#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Measure static variable orders on the generated quantum benchmark circuits.
#
# Usage:
#   bash test/run_quantum_ordering.sh [GENERATED_TSV] [OUTPUT_CSV]

set -euo pipefail

ROOT="${MQT_CORE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
GENERATED="${1:-/home/flareon/workshop/results/quantum_ordering/circuits/generated_circuits.tsv}"
OUT="${2:-/home/flareon/workshop/results/quantum_ordering/runs/functionality.csv}"
BENCH="${MQT_CORE_ORDER_BENCH:-${ROOT}/build/release/bin/mqt-core-order-bench}"
MODE="${MQT_MODE:-functionality}"
REPS="${MQT_REPS:-1}"
MAX_QUBITS="${MQT_MAX_QUBITS:-20}"
TIMEOUT_SEC="${MQT_TIMEOUT_SEC:-600}"

mkdir -p "$(dirname "${OUT}")"
"${BENCH}" --header >"${OUT}"

for ((rep = 1; rep <= REPS; rep++)); do
  while IFS=$'\t' read -r benchmark level opt_level num_qubits seed checksum path; do
    [[ "${benchmark}" == "benchmark" ]] && continue
    [[ -n "${path}" ]] || continue

    if ! row="$(
      timeout "${TIMEOUT_SEC}" "${BENCH}" \
        --mode "${MODE}" \
        --static-only \
        --max-qubits "${MAX_QUBITS}" \
        "${path}" 2>/dev/null
    )"; then
      padding="$(printf ',%.0s' {1..43})"
      printf '%s,%s,,timeout%s\n' \
        "${path}" "${num_qubits}" "${padding}" >>"${OUT}"
      continue
    fi

    printf '%s\n' "${row}" >>"${OUT}"
  done <"${GENERATED}"
  printf 'completed repetition %s/%s\n' "${rep}" "${REPS}" >&2
done

printf 'wrote %s\n' "${OUT}"
