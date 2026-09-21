#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
#
# Measure the effect of a static variable order on the deterministic noise
# simulator of MQT DDSIM. That simulator builds a density matrix, so the
# decision diagram is a matrix-like object and the order can matter.
#
# The script converts a set of RevLib circuits to OpenQASM 3, runs every
# circuit with every order in its own process, and stores the statistics of
# every run as JSON.
#
#   screen -dmS mqt_ddsim_dm -L -Logfile /tmp/mqt_ddsim_dm.log \
#     bash /home/flareon/workshop/mqt-core/test/run_ddsim_dm_experiments.sh
#
# A run is skipped when its result file exists, so the script can be stopped
# and started again at any time.

set -uo pipefail

ROOT="${MQT_CORE_ROOT:-/home/flareon/workshop/mqt-core}"
DDSIM="${MQT_DDSIM_ROOT:-/home/flareon/workshop/mqt-ddsim}"
BENCH="${MQT_DDSIM_BENCH:-${DDSIM}/build-local/apps/mqt-ddsim-noise_aware}"
ASSETS="${MQT_DDSIM_ASSETS:-/home/flareon/workshop/igqmdd/assets}"
WORK="${MQT_WORK:-/tmp/mqt_ddsim_dm}"
RESULTS="${MQT_RESULTS:-/home/flareon/workshop/results}"

TIMEOUT_SEC="${MQT_DDSIM_TIMEOUT_SEC:-600}"
MEM_MAX="${MQT_MEM_MAX:-40G}"
REPEATS="${MQT_REPEATS:-3}"

ORDERS="${MQT_DDSIM_ORDERS:-none gate_dependency degree spectral recursive_bisection}"
NOISE_EFFECTS="${MQT_DDSIM_NOISE_EFFECTS:-APD}"
NOISE_PROB="${MQT_DDSIM_NOISE_PROB:-0.001}"

# Circuits with at most this many qubits. A density matrix needs far more
# memory than a state vector, so the set stays small.
MAX_QUBITS="${MQT_DDSIM_MAX_QUBITS:-18}"

# Optional list of circuit names, one stem per line. Without it the script
# keeps every circuit that satisfies MAX_QUBITS.
CIRCUIT_LIST="${MQT_DDSIM_CIRCUITS:-}"

log() { printf '[%s] %s\n' "$(date '+%F %T')" "$*"; }

mkdir -p "${WORK}/qasm" "${WORK}/json" "${WORK}/logs" "${RESULTS}"

if [[ ! -x "${BENCH}" ]]; then
  log "benchmark tool ${BENCH} is missing"
  exit 1
fi

# Pick the circuits by qubit count and convert them to OpenQASM 3.
pickSources() {
  local source out
  source="$(mktemp -d "${WORK}/sources.XXXXXX")"
  for real in "${ASSETS}"/*/*.real; do
    [[ -f "${real}" ]] || continue
    if [[ -n "${CIRCUIT_LIST}" ]] && ! grep -qx "$(basename "${real}" .real)" \
      "${CIRCUIT_LIST}"; then
      continue
    fi
    local qubits
    qubits="$(sed -n 's/^\.numvars[[:space:]]*\([0-9]*\).*/\1/p' "${real}" | head -n 1)"
    [[ -n "${qubits}" ]] || continue
    if [[ "${qubits}" -le "${MAX_QUBITS}" ]]; then
      ln -sf "${real}" "${source}/"
    fi
  done
  python3 "${ROOT}/test/real_to_qasm3.py" "${source}" "${WORK}/qasm" \
    > "${WORK}/convert.log" 2>&1
  echo "${source}"
}

sourceDir="$(pickSources)"
# Derive the circuit set from the selected sources. The QASM directory may
# still hold files from an earlier selection, so listing it would mix sets.
mapfile -t circuits < <(cd "${sourceDir}" && ls ./*.real | sed 's|^\./||; s|\.real$|.qasm|')
log "converted $(wc -l < "${WORK}/convert.log" | tr -d ' ') lines of conversion output; found ${#circuits[@]} circuits with at most ${MAX_QUBITS} qubits (sources in ${sourceDir})"

runOne() {
  local outfile="$1" logfile="$2" circuit="$3" order="$4"
  local raw="${outfile}.raw"
  systemd-run --user --scope -q -p "MemoryMax=${MEM_MAX}" -p MemorySwapMax=0 -- \
    timeout "${TIMEOUT_SEC}" "${BENCH}" \
    --use_density_matrix_simulator \
    --noise_effects "${NOISE_EFFECTS}" --noise_prob "${NOISE_PROB}" \
    --simulate_file "${WORK}/qasm/${circuit}" --ps \
    --initial_reorder "${order}" \
    > "${raw}" 2> "${logfile}"
  local rc=$?
  if [[ ${rc} -eq 0 && -s "${raw}" ]] && grep -q '"statistics"' "${raw}"; then
    mv "${raw}" "${outfile}"
    return 0
  fi
  rm -f "${raw}"
  local status="error"
  [[ ${rc} -eq 124 ]] && status="timeout"
  [[ ${rc} -eq 137 ]] && status="out-of-memory"
  printf '%s\n' "${status}" > "${outfile}"
  return 1
}

for circuit in "${circuits[@]}"; do
  for order in ${ORDERS}; do
    for ((rep = 1; rep <= REPEATS; rep++)); do
      stem="${circuit%.qasm}__${order}__rep${rep}"
      outfile="${WORK}/json/${stem}.json"
      logfile="${WORK}/logs/${stem}.log"
      if [[ -s "${outfile}" ]]; then
        continue
      fi
      if runOne "${outfile}" "${logfile}" "${circuit}" "${order}"; then
        log "[ok] ${stem}"
      else
        log "[$(cat "${outfile}")] ${stem}"
      fi
    done
  done
done

log "merging"
python3 "${ROOT}/test/merge_ddsim_dm_experiments.py" "${WORK}" "${RESULTS}"
log "done"
