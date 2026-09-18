#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
#
# Compare static ordering against construction-time dynamic reordering in a
# Release build. Every arm is a (start order, threshold) pair, for example
# "identity:500" or "spectral:1000". The script writes one CSV per arm and
# merges them into a single table.
#
#   screen -dmS mqt_order_arms -L -Logfile /tmp/mqt_arms_screen.log \
#     bash /home/flareon/workshop/mqt-core/test/run_ordering_arms.sh

set -uo pipefail

ROOT="${MQT_CORE_ROOT:-/home/flareon/workshop/mqt-core}"
RELEASE_BUILD="${MQT_RELEASE_BUILD:-${ROOT}/build/release}"
LIST="${MQT_LIST:-/home/flareon/workshop/results/ordering_ablation_circuits.txt}"
OUT="${MQT_OUT:-/home/flareon/workshop/results/ordering_static_vs_dynamic_release.csv}"
WORK="${MQT_WORK:-/tmp/mqt_ordering_arms}"
ARMS="${MQT_ARMS:-identity:500 identity:1000 spectral:500 spectral:1000}"
BENCH="${RELEASE_BUILD}/bin/mqt-core-order-bench"
TIMEOUT_SEC="${MQT_TIMEOUT_SEC:-3600}"
MEM_MAX="${MQT_MEM_MAX:-40G}"
SIFT_PASSES="${MQT_SIFT_PASSES:-1}"

log() { printf '[%s] %s\n' "$(date '+%F %T')" "$*"; }

mkdir -p "${WORK}/logs" "$(dirname "${OUT}")"

if [[ ! -x "${BENCH}" ]]; then
  log "configuring the Release build in ${RELEASE_BUILD}"
  cmake -S "${ROOT}" -B "${RELEASE_BUILD}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_MQT_CORE_APPS=ON \
    -DBUILD_MQT_CORE_TESTS=OFF \
    -DBUILD_MQT_CORE_BINDINGS=OFF \
    -DBUILD_MQT_CORE_QDMI_DDSIM_DEVICE=OFF \
    -DBUILD_MQT_CORE_QDMI_SC_DEVICE=OFF \
    > "${WORK}/configure.log" 2>&1 || { log "configure failed"; exit 1; }
  cmake --build "${RELEASE_BUILD}" --target mqt-core-order-bench \
    -j "$(nproc)" > "${WORK}/build.log" 2>&1 || { log "build failed"; exit 1; }
fi

mapfile -t CIRCUITS < <(grep -v '^[[:space:]]*$' "${LIST}")

for arm in ${ARMS}; do
  start="${arm%%:*}"
  threshold="${arm##*:}"
  table="${WORK}/arm_${start}_${threshold}.csv"
  if [[ ! -s "${table}" ]]; then
    "${BENCH}" --header > "${table}"
  fi
  index=0
  log "arm ${start}/${threshold}: ${#CIRCUITS[@]} circuits"
  for circuit in "${CIRCUITS[@]}"; do
    index=$((index + 1))
    name="$(basename "${circuit}" .real)"
    if grep -q "^${circuit}," "${table}" 2> /dev/null; then
      continue
    fi
    qubits="$(sed -n 's/^\.numvars[[:space:]]*\([0-9]*\).*/\1/p' "${circuit}" | head -n 1)"
    rowfile="${WORK}/logs/${name}_${start}_${threshold}.csv"
    logfile="${WORK}/logs/${name}_${start}_${threshold}.log"
    startTime="$(date +%s)"

    systemd-run --user --scope -q -p "MemoryMax=${MEM_MAX}" \
      -p MemorySwapMax=0 -- timeout "${TIMEOUT_SEC}" "${BENCH}" \
      --skip-post-hoc --dynamic-threshold "${threshold}" \
      --dynamic-start "${start}" --sift-passes "${SIFT_PASSES}" "${circuit}" \
      > "${rowfile}" 2> "${logfile}"
    rc=$?
    elapsed=$(( $(date +%s) - startTime ))

    if [[ ${rc} -eq 0 && -s "${rowfile}" ]]; then
      cat "${rowfile}" >> "${table}"
      log "[${start}/${threshold}] [${index}/${#CIRCUITS[@]}] ok ${name} (${elapsed}s)"
    else
      status="error"
      [[ ${rc} -eq 124 ]] && status="timeout"
      [[ ${rc} -eq 137 ]] && status="out-of-memory"
      pad="$(printf ',%.0s' $(seq 1 28))"
      printf '%s,%s,,%s%s\n' "${circuit}" "${qubits:-}" "${status}" "${pad}" \
        >> "${table}"
      log "[${start}/${threshold}] [${index}/${#CIRCUITS[@]}] ${status} ${name} rc=${rc} (${elapsed}s)"
    fi
  done
done

log "merging the arms"
python3 "${ROOT}/test/merge_ordering_arms.py" "${WORK}" "${OUT}" ${ARMS}
log "table: ${OUT}"
