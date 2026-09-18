#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
#
# Compare static variable ordering against construction-time dynamic
# reordering. The script runs the benchmark tool once per threshold over the
# circuits listed in a file, and merges the results into one CSV.
#
# Run it inside a screen session, for example
#
#   screen -dmS mqt_order_ablation -L -Logfile /tmp/mqt_ablation_screen.log \
#     bash /home/flareon/workshop/mqt-core/test/run_ordering_ablation.sh

set -uo pipefail

ROOT="${MQT_CORE_ROOT:-/home/flareon/workshop/mqt-core}"
BUILD="${MQT_CORE_BUILD:-${ROOT}/build}"
LIST="${MQT_LIST:-/home/flareon/workshop/results/ordering_ablation_circuits.txt}"
OUT="${MQT_OUT:-/home/flareon/workshop/results/ordering_static_vs_dynamic.csv}"
WORK="${MQT_WORK:-/tmp/mqt_ordering_ablation}"
BENCH="${BUILD}/bin/mqt-core-order-bench"
THRESHOLDS="${MQT_THRESHOLDS:-500 1000}"
SIFT_PASSES="${MQT_SIFT_PASSES:-1}"
TIMEOUT_SEC="${MQT_TIMEOUT_SEC:-3600}"
MEM_MAX="${MQT_MEM_MAX:-40G}"

log() { printf '[%s] %s\n' "$(date '+%F %T')" "$*"; }

mkdir -p "${WORK}/logs" "$(dirname "${OUT}")"

if [[ ! -x "${BENCH}" ]]; then
  log "building the benchmark tool"
  cmake -S "${ROOT}" -B "${BUILD}" -DBUILD_MQT_CORE_APPS=ON > /dev/null || exit 1
  cmake --build "${BUILD}" --target mqt-core-order-bench -j "$(nproc)" \
    > "${WORK}/build.log" 2>&1 || { log "build failed"; exit 1; }
fi

mapfile -t CIRCUITS < <(grep -v '^[[:space:]]*$' "${LIST}")
log "running ${#CIRCUITS[@]} circuits for thresholds: ${THRESHOLDS}"

for threshold in ${THRESHOLDS}; do
  perThreshold="${WORK}/threshold_${threshold}.csv"
  if [[ ! -s "${perThreshold}" ]]; then
    "${BENCH}" --header > "${perThreshold}"
  fi
  index=0
  for circuit in "${CIRCUITS[@]}"; do
    index=$((index + 1))
    name="$(basename "${circuit}" .real)"
    if grep -q "^${circuit}," "${perThreshold}" 2> /dev/null; then
      continue
    fi
    qubits="$(sed -n 's/^\.numvars[[:space:]]*\([0-9]*\).*/\1/p' "${circuit}" | head -n 1)"
    rowfile="${WORK}/logs/${name}_${threshold}.csv"
    logfile="${WORK}/logs/${name}_${threshold}.log"
    start="$(date +%s)"

    systemd-run --user --scope -q -p "MemoryMax=${MEM_MAX}" \
      -p MemorySwapMax=0 -- timeout "${TIMEOUT_SEC}" "${BENCH}" \
      --skip-post-hoc --dynamic-threshold "${threshold}" "${circuit}" \
      --sift-passes "${SIFT_PASSES}" \
      > "${rowfile}" 2> "${logfile}"
    rc=$?
    elapsed=$(( $(date +%s) - start ))

    if [[ ${rc} -eq 0 && -s "${rowfile}" ]]; then
      cat "${rowfile}" >> "${perThreshold}"
      log "[threshold ${threshold}] [${index}/${#CIRCUITS[@]}] ok ${name} (${elapsed}s)"
    else
      status="error"
      [[ ${rc} -eq 124 ]] && status="timeout"
      [[ ${rc} -eq 137 ]] && status="out-of-memory"
      pad="$(printf ',%.0s' $(seq 1 27))"
      printf '%s,%s,,%s%s\n' "${circuit}" "${qubits:-}" "${status}" "${pad}" \
        >> "${perThreshold}"
      log "[threshold ${threshold}] [${index}/${#CIRCUITS[@]}] ${status} ${name} rc=${rc} (${elapsed}s)"
    fi
  done
done

log "merging the per-threshold tables"
python3 "${ROOT}/test/merge_ordering_ablation.py" "${WORK}" "${OUT}" \
  ${THRESHOLDS}
log "table: ${OUT}"
