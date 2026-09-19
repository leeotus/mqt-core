#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
#
# Measure the effect of static variable ordering in two applications:
#   E1: state vector simulation   (mode state)
#   E3: unitary construction      (mode functionality)
#
# Every variant runs in its own process, so the measurements are not biased by
# the allocation warm-up of an earlier variant. Every measurement is repeated.
#
#   screen -dmS mqt_app_exp -L -Logfile /tmp/mqt_app_screen.log \
#     bash /home/flareon/workshop/mqt-core/test/run_application_experiments.sh

set -uo pipefail

ROOT="${MQT_CORE_ROOT:-/home/flareon/workshop/mqt-core}"
BUILD="${MQT_BUILD:-${ROOT}/build/release}"
WORK="${MQT_WORK:-/tmp/mqt_application_experiments}"
RESULTS="${MQT_RESULTS:-/home/flareon/workshop/results}"
E1_LIST="${MQT_E1_LIST:-${RESULTS}/application_e1_circuits.txt}"
E3_LIST="${MQT_E3_LIST:-${RESULTS}/ordering_ablation_circuits.txt}"
BENCH="${BUILD}/bin/mqt-core-order-bench"
TIMEOUT_SEC="${MQT_TIMEOUT_SEC:-1800}"
MEM_MAX="${MQT_MEM_MAX:-40G}"
REPEATS="${MQT_REPEATS:-3}"
VARIANTS="${MQT_VARIANTS:-original degree spectral recursive_bisection}"

log() { printf '[%s] %s\n' "$(date '+%F %T')" "$*"; }

mkdir -p "${WORK}/logs" "${RESULTS}"

if [[ ! -f "${BUILD}/CMakeCache.txt" ]]; then
  log "configuring the Release build"
  cmake -S "${ROOT}" -B "${BUILD}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_MQT_CORE_APPS=ON \
    -DBUILD_MQT_CORE_TESTS=OFF \
    -DBUILD_MQT_CORE_BINDINGS=OFF \
    -DBUILD_MQT_CORE_QDMI_DDSIM_DEVICE=OFF \
    -DBUILD_MQT_CORE_QDMI_SC_DEVICE=OFF \
    > "${WORK}/configure.log" 2>&1 || { log "configure failed"; exit 1; }
fi

log "building the benchmark tool"
cmake --build "${BUILD}" --target mqt-core-order-bench -j "$(nproc)" \
  > "${WORK}/build.log" 2>&1 || { log "build failed"; exit 1; }

runCase() {
  local name="$1" mode="$2" list="$3"
  mapfile -t circuits < <(grep -v '^[[:space:]]*$' "${list}")
  local total
  total="$((${#circuits[@]} * REPEATS * $(wc -w <<< "${VARIANTS}")))"
  local done_=0
  for variant in ${VARIANTS}; do
    for rep in $(seq 1 "${REPEATS}"); do
      local table="${WORK}/${name}_${variant}_rep${rep}.csv"
      if [[ ! -s "${table}" ]]; then
        "${BENCH}" --header > "${table}"
      fi
      for circuit in "${circuits[@]}"; do
        done_=$((done_ + 1))
        if grep -q "^${circuit}," "${table}" 2> /dev/null; then
          continue
        fi
        local base rowfile logfile qubits rc status
        base="$(basename "${circuit}")"
        rowfile="${WORK}/logs/${name}_${variant}_${base}_rep${rep}.csv"
        logfile="${WORK}/logs/${name}_${variant}_${base}_rep${rep}.log"
        qubits="$(sed -n 's/^\.numvars[[:space:]]*\([0-9]*\).*/\1/p' "${circuit}" | head -n 1)"
        if [[ -z "${qubits}" ]]; then
          qubits="$(sed -n 's/^qreg[[:space:]]*q\[\([0-9]*\)\].*/\1/p' "${circuit}" | head -n 1)"
        fi

        systemd-run --user --scope -q -p "MemoryMax=${MEM_MAX}" \
          -p MemorySwapMax=0 -- timeout "${TIMEOUT_SEC}" "${BENCH}" \
          --only "${variant}" --mode "${mode}" "${circuit}" \
          > "${rowfile}" 2> "${logfile}"
        rc=$?
        if [[ ${rc} -eq 0 && -s "${rowfile}" ]]; then
          cat "${rowfile}" >> "${table}"
        else
          status="error"
          [[ ${rc} -eq 124 ]] && status="timeout"
          [[ ${rc} -eq 137 ]] && status="out-of-memory"
          printf '%s,%s,,%s' "${circuit}" "${qubits:-}" "${status}" >> "${table}"
          for _ in $(seq 1 27); do printf ',' >> "${table}"; done
          printf '\n' >> "${table}"
          log "[${name}/${variant}/rep${rep}] ${status} ${base} rc=${rc}"
        fi
      done
    done
  done
  log "${name} finished (${done_} runs)"
}

runCase e1 state "${E1_LIST}"
runCase e3 functionality "${E3_LIST}"

log "merging"
python3 "${ROOT}/test/merge_application_experiments.py" "${WORK}" "${RESULTS}" \
  "${REPEATS}" ${VARIANTS}
log "done"
