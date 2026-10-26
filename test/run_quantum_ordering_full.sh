#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run static variable ordering measurements. Every circuit and strategy uses a
# separate process, so peak RSS can be attributed to that strategy.
#
# Usage:
#   bash test/run_quantum_ordering_full.sh [OUTPUT_DIRECTORY]

set -euo pipefail

ROOT="${MQT_CORE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
OUT="${1:-/home/flareon/workshop/results/quantum_ordering_full}"
CIRCUIT_ROOT="${MQT_CIRCUIT_ROOT:-/home/flareon/workshop/quantum_circuits}"
BUILD_DIR="${MQT_BUILD_DIR:-${ROOT}/build/release}"
BENCH="${MQT_CORE_ORDER_BENCH:-${BUILD_DIR}/bin/mqt-core-order-bench}"
REPS="${MQT_REPS:-1}"
TIMEOUT_SEC="${MQT_TIMEOUT_SEC:-600}"
MAX_QUBITS="${MQT_MAX_QUBITS:-20}"
JOBS="${MQT_BUILD_JOBS:-8}"

RAW="${OUT}/raw"
DATA="${OUT}/data"
mkdir -p "${RAW}" "${DATA}"
printf 'rep,circuit,mode,strategy,status\n' >"${DATA}/failures.csv"

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  cmake -S "${ROOT}" -B "${BUILD_DIR}" \
    -DBUILD_MQT_CORE_APPS=ON \
    -DBUILD_MQT_CORE_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "${BUILD_DIR}" --target mqt-core-order-bench -j "${JOBS}"

MANIFEST_PATH="${CIRCUIT_ROOT}/circuits_manifest.tsv"
if [[ ! -f "${MANIFEST_PATH}" ]]; then
  printf 'missing circuit corpus manifest: %s\n' "${MANIFEST_PATH}" >&2
  printf 'run test/bootstrap_quantum_corpus.sh first\n' >&2
  exit 1
fi
python3 "${ROOT}/test/verify_quantum_corpus.py" "${MANIFEST_PATH}"

strategies=(identity degree spectral recursive_bisection force mince quantum_score)
modes=(functionality state)

for ((rep = 1; rep <= REPS; rep++)); do
  for mode in "${modes[@]}"; do
    raw_file="${RAW}/${mode}_rep${rep}.csv"
    "${BENCH}" --header >"${raw_file}"

    tail -n +2 "${CIRCUIT_ROOT}/circuits_manifest.tsv" |
      cut -f7 |
      while IFS= read -r path; do
        path="${path%$'\r'}"
        for strategy in "${strategies[@]}"; do
          order_variant="${strategy}"
          if [[ "${strategy}" == "identity" ]]; then
            order_variant="original"
          fi
          if ! row="$(
            timeout "${TIMEOUT_SEC}" "${BENCH}" \
              --mode "${mode}" \
              --static-only \
              --only "${order_variant}" \
              --max-qubits "${MAX_QUBITS}" \
              "${path}" 2>/dev/null
          )"; then
            printf '%s,%s,%s,%s,timeout_or_error\n' \
              "${rep}" "$(basename "${path}")" "${mode}" "${strategy}" \
              >>"${DATA}/failures.csv"
            continue
          fi
          IFS=',' read -r -a fields <<<"${row}"
          if [[ "${fields[3]:-missing_status}" != "ok" ]]; then
            printf '%s,%s,%s,%s,%s\n' \
              "${rep}" "$(basename "${path}")" "${mode}" "${strategy}" \
              "${fields[3]:-missing_status}" >>"${DATA}/failures.csv"
          fi
          printf '%s\n' "${row}" >>"${raw_file}"
        done
      done
  done
done

for mode in "${modes[@]}"; do
  python3 "${ROOT}/test/normalize_quantum_ordering.py" \
    "${CIRCUIT_ROOT}/circuits_manifest.tsv" \
    "${RAW}/${mode}"_rep*.csv \
    "${DATA}/${mode}_full.csv"
done

printf 'full data written below %s\n' "${DATA}"
find "${DATA}" -maxdepth 1 -type f -printf '%f\n' | sort
