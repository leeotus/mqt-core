#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run static variable-ordering experiments for every QASM file below a
# directory. Each circuit and strategy is measured in a separate process.
#
# Usage:
#   bash test/run_quantum_ordering_directory.sh CIRCUIT_DIRECTORY [OUTPUT_DIRECTORY]

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  printf 'usage: %s CIRCUIT_DIRECTORY [OUTPUT_DIRECTORY]\n' "$0" >&2
  exit 2
fi

ROOT="${MQT_CORE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CIRCUIT_DIRECTORY="$(cd "$1" && pwd)"
OUT="${2:-/home/flareon/workshop/results/quantum_ordering_directory}"
BUILD_DIR="${MQT_BUILD_DIR:-${ROOT}/build/release}"
BENCH="${MQT_CORE_ORDER_BENCH:-${BUILD_DIR}/bin/mqt-core-order-bench}"
REPS="${MQT_REPS:-1}"
TIMEOUT_SEC="${MQT_TIMEOUT_SEC:-7200}"
MEMORY_LIMIT_GB="${MQT_MEMORY_LIMIT_GB:-32}"
MAX_QUBITS="${MQT_MAX_QUBITS:-32}"
JOBS="${MQT_BUILD_JOBS:-8}"
MEMORY_LIMIT_KB=$((MEMORY_LIMIT_GB * 1024 * 1024))
RAW="${OUT}/raw"
DATA="${OUT}/data"
MANIFEST="${OUT}/circuits_manifest.tsv"
PROGRESS_LOG="${OUT}/progress.log"
STRATEGIES=(identity degree spectral recursive_bisection force mince quantum_score)
MODES=(functionality state)

mkdir -p "${RAW}" "${DATA}"
python3 "${ROOT}/test/build_circuit_manifest.py" \
  "${CIRCUIT_DIRECTORY}" "${MANIFEST}"
python3 "${ROOT}/test/verify_quantum_corpus.py" "${MANIFEST}"
CIRCUIT_COUNT=$(($(wc -l <"${MANIFEST}") - 1))
TOTAL_JOBS=$((REPS * ${#MODES[@]} * CIRCUIT_COUNT * ${#STRATEGIES[@]}))
printf 'experiments=%d files=%d timeout=%ss memory=%sGiB max-qubits=%d\n' \
  "${TOTAL_JOBS}" "${CIRCUIT_COUNT}" "${TIMEOUT_SEC}" \
  "${MEMORY_LIMIT_GB}" "${MAX_QUBITS}" | tee "${PROGRESS_LOG}"

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  cmake -S "${ROOT}" -B "${BUILD_DIR}" \
    -DBUILD_MQT_CORE_APPS=ON \
    -DBUILD_MQT_CORE_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "${BUILD_DIR}" --target mqt-core-order-bench -j "${JOBS}"

printf 'rep,circuit,mode,strategy,status,qubits,gates\n' >"${DATA}/failures.csv"

for ((rep = 1; rep <= REPS; rep++)); do
  mode_position=0
  for mode in "${MODES[@]}"; do
    raw_file="${RAW}/${mode}_rep${rep}.csv"
    "${BENCH}" --header >"${raw_file}"
    circuit_index=0
    job_index=$((((rep - 1) * ${#MODES[@]} + mode_position) *
      CIRCUIT_COUNT * ${#STRATEGIES[@]}))

    tail -n +2 "${MANIFEST}" |
      cut -f7 |
      while IFS= read -r path; do
        ((circuit_index += 1))
        circuit_name="$(basename "${path}")"
        for strategy in "${STRATEGIES[@]}"; do
          ((job_index += 1))
          job_start_ms=$(date +%s%3N)
          printf -v progress \
            '[%d/%d] rep=%d/%d mode=%s circuit=%d/%d strategy=%s file=%s starting' \
            "${job_index}" "${TOTAL_JOBS}" "${rep}" "${REPS}" \
            "${mode}" "${circuit_index}" "${CIRCUIT_COUNT}" \
            "${strategy}" "${circuit_name}"
          printf '%s\n' "${progress}" | tee -a "${PROGRESS_LOG}" >&2
          order_variant="${strategy}"
          if [[ "${strategy}" == "identity" ]]; then
            order_variant="original"
          fi
          rc=0
          error_log="${RAW}/last_error.log"
          row="$(
            ulimit -v "${MEMORY_LIMIT_KB}"
            timeout --kill-after=30s "${TIMEOUT_SEC}" "${BENCH}" \
              --mode "${mode}" \
              --static-only \
              --only "${order_variant}" \
              --max-qubits "${MAX_QUBITS}" \
              "${path}"
          )" 2>"${error_log}" || rc=$?
          if ((rc != 0)); then
            if ((rc == 124)); then
              failure_status="timeout"
            elif grep -Eqi \
              'bad_alloc|bad array new length|cannot allocate memory|memory exhausted|out of memory' \
              "${error_log}"; then
              failure_status="memory-limit"
            elif ((rc == 134 || rc == 137)); then
              failure_status="memory-limit-or-killed"
            else
              failure_status="runtime-error-${rc}"
            fi
            printf '%s,%s,%s,%s,%s,-,-\n' \
              "${rep}" "${circuit_name}" "${mode}" "${strategy}" \
              "${failure_status}" >>"${DATA}/failures.csv"
            elapsed_ms=$(($(date +%s%3N) - job_start_ms))
            printf '[%d/%d] status=%s elapsed=%dms file=%s\n' \
              "${job_index}" "${TOTAL_JOBS}" "${failure_status}" \
              "${elapsed_ms}" "${circuit_name}" |
              tee -a "${PROGRESS_LOG}" >&2
            continue
          fi
          IFS=',' read -r -a fields <<<"${row}"
          if [[ "${fields[3]:-missing_status}" != "ok" ]]; then
            printf '%s,%s,%s,%s,%s,%s,%s\n' \
              "${rep}" "${circuit_name}" "${mode}" "${strategy}" \
              "${fields[3]:-missing_status}" "${fields[1]:--}" "${fields[2]:--}" \
              >>"${DATA}/failures.csv"
          fi
          printf '%s\n' "${row}" >>"${raw_file}"
          elapsed_ms=$(($(date +%s%3N) - job_start_ms))
          printf '[%d/%d] status=%s elapsed=%dms file=%s\n' \
            "${job_index}" "${TOTAL_JOBS}" "${fields[3]:-missing_status}" \
            "${elapsed_ms}" "${circuit_name}" |
            tee -a "${PROGRESS_LOG}" >&2
        done
      done
    mode_position=$((mode_position + 1))
  done
done

for mode in "${MODES[@]}"; do
  python3 "${ROOT}/test/normalize_quantum_ordering.py" \
    "${MANIFEST}" \
    "${RAW}/${mode}"_rep*.csv \
    "${DATA}/${mode}_full.csv" \
    --mode "${mode}" \
    --failures "${DATA}/failures.csv"
done

printf 'results written below %s\n' "${DATA}"
