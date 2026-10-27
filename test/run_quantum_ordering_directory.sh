#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run static variable-ordering experiments for every QASM file below a
# directory. The seven ordering strategies of one circuit and DD mode run in
# parallel. Results are appended to CSV after each circuit completes.
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
TOTAL_MEMORY_LIMIT_GB="${MQT_MEMORY_LIMIT_GB:-32}"
WORKER_MEMORY_LIMIT_GB_OVERRIDE="${MQT_WORKER_MEMORY_LIMIT_GB:-}"
MAX_QUBITS="${MQT_MAX_QUBITS:-32}"
JOBS="${MQT_BUILD_JOBS:-8}"
RAW="${OUT}/raw"
DATA="${OUT}/data"
MANIFEST="${OUT}/circuits_manifest.tsv"
RESULTS="${DATA}/results.csv"
FAILURES="${DATA}/failures.csv"
PROGRESS_LOG="${OUT}/progress.log"
STRATEGIES=(identity degree spectral recursive_bisection force mince quantum_score)
MODES=(functionality state)

# The memory limit is the total budget for one circuit/mode wave. Dividing it
# between the seven simultaneously running strategies prevents a 7x overshoot.
TOTAL_MEMORY_LIMIT_KB=$((TOTAL_MEMORY_LIMIT_GB * 1024 * 1024))
if [[ -n "${WORKER_MEMORY_LIMIT_GB_OVERRIDE}" ]]; then
  WORKER_MEMORY_LIMIT_KB=$(awk \
    -v gb="${WORKER_MEMORY_LIMIT_GB_OVERRIDE}" \
    'BEGIN { printf "%d", gb * 1024 * 1024 }')
else
  WORKER_MEMORY_LIMIT_KB=$((TOTAL_MEMORY_LIMIT_KB / ${#STRATEGIES[@]}))
fi
WORKER_MEMORY_LIMIT_GB=$(awk \
  -v kb="${WORKER_MEMORY_LIMIT_KB}" \
  'BEGIN { printf "%.2f", kb / 1024 / 1024 }')

mkdir -p "${RAW}" "${DATA}"
python3 "${ROOT}/test/build_circuit_manifest.py" \
  "${CIRCUIT_DIRECTORY}" "${MANIFEST}"
python3 "${ROOT}/test/verify_quantum_corpus.py" "${MANIFEST}"

CIRCUIT_COUNT=$(($(wc -l <"${MANIFEST}") - 1))
TOTAL_JOBS=$((REPS * ${#MODES[@]} * CIRCUIT_COUNT * ${#STRATEGIES[@]}))
printf 'circuits=%d experiments=%d strategies=%d timeout=%ss total-memory=%sGiB worker-memory=%sGiB max-qubits=%d\n' \
  "${CIRCUIT_COUNT}" "${TOTAL_JOBS}" "${#STRATEGIES[@]}" "${TIMEOUT_SEC}" \
  "${TOTAL_MEMORY_LIMIT_GB}" "${WORKER_MEMORY_LIMIT_GB}" "${MAX_QUBITS}" |
  tee "${PROGRESS_LOG}"

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  cmake -S "${ROOT}" -B "${BUILD_DIR}" \
    -DBUILD_MQT_CORE_APPS=ON \
    -DBUILD_MQT_CORE_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "${BUILD_DIR}" --target mqt-core-order-bench -j "${JOBS}"

printf 'rep,mode,circuit,benchmark,level,qubits,gates,strategy,final_nodes,peak_nodes,build_time_ms,order_time_ms,total_time_ms,peak_rss_mb,status\n' \
  >"${RESULTS}"
printf 'rep,mode,circuit,strategy,status\n' >"${FAILURES}"

job_index=0
for ((rep = 1; rep <= REPS; rep++)); do
  circuit_index=0
  while IFS= read -r path; do
    ((circuit_index += 1))
    circuit_name="$(basename "${path}")"
    work_dir="$(mktemp -d "${RAW}/circuit.XXXXXX")"
    circuit_start_ms=$(date +%s%3N)

    for mode in "${MODES[@]}"; do
      mode_start_ms=$(date +%s%3N)
      worker_pids=()
      for strategy in "${STRATEGIES[@]}"; do
        ((job_index += 1))
        order_variant="${strategy}"
        if [[ "${strategy}" == "identity" ]]; then
          order_variant="original"
        fi
        printf '[%d/%d] rep=%d/%d mode=%s circuit=%d/%d strategy=%s file=%s starting\n' \
          "${job_index}" "${TOTAL_JOBS}" "${rep}" "${REPS}" "${mode}" \
          "${circuit_index}" "${CIRCUIT_COUNT}" "${strategy}" "${circuit_name}" |
          tee -a "${PROGRESS_LOG}" >&2

        (
          set +e
          (
            ulimit -v "${WORKER_MEMORY_LIMIT_KB}"
            timeout --kill-after=30s "${TIMEOUT_SEC}" "${BENCH}" \
              --mode "${mode}" \
              --static-only \
              --only "${order_variant}" \
              --max-qubits "${MAX_QUBITS}" \
              "${path}"
          ) >"${work_dir}/${mode}.${strategy}.row" \
            2>"${work_dir}/${mode}.${strategy}.err"
          printf '%s\n' "$?" >"${work_dir}/${mode}.${strategy}.rc"
        ) &
        worker_pids+=("$!")
      done

      for pid in "${worker_pids[@]}"; do
        wait "${pid}"
      done
      elapsed_ms=$(($(date +%s%3N) - mode_start_ms))
      printf '[%d/%d] completed mode=%s circuit=%d/%d elapsed=%dms file=%s\n' \
        "${job_index}" "${TOTAL_JOBS}" "${mode}" "${circuit_index}" \
        "${CIRCUIT_COUNT}" "${elapsed_ms}" "${circuit_name}" |
        tee -a "${PROGRESS_LOG}" >&2
    done

    python3 "${ROOT}/test/append_quantum_ordering_results.py" \
      "${MANIFEST}" \
      --results "${RESULTS}" \
      --failures "${FAILURES}" \
      --work-dir "functionality=${work_dir}" \
      --work-dir "state=${work_dir}" \
      --rep "${rep}" \
      --circuit "${circuit_name}"
    elapsed_ms=$(($(date +%s%3N) - circuit_start_ms))
    printf '[%d/%d] CSV updated circuit=%d/%d elapsed=%dms file=%s\n' \
      "${job_index}" "${TOTAL_JOBS}" "${circuit_index}" \
      "${CIRCUIT_COUNT}" "${elapsed_ms}" "${circuit_name}" |
      tee -a "${PROGRESS_LOG}" >&2
    rm -rf "${work_dir}"
  done < <(tail -n +2 "${MANIFEST}" | cut -f7)
done

printf 'results written to %s\n' "${RESULTS}"
printf 'failures written to %s\n' "${FAILURES}"
