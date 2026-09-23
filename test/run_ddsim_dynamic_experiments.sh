#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
#
# Compare static and dynamic variable reordering for the deterministic
# (density matrix) noise simulator of MQT DDSIM.
#
# The static arms pick a variable order before the simulation starts. The
# dynamic arms start from a fixed order and sift the density matrix whenever it
# grows past a threshold. Every arm is a separate process so that a crash or an
# out-of-memory kill cannot affect the other arms.
#
#   screen -dmS mqt_ddsim_dyn -L -Logfile /tmp/mqt_ddsim_dyn.log \
#     bash /home/flareon/workshop/mqt-core/test/run_ddsim_dynamic_experiments.sh
#
# A run is skipped when its result file exists, so the script can be stopped
# and started again at any time.

set -uo pipefail

ROOT="${MQT_CORE_ROOT:-/home/flareon/workshop/mqt-core}"
DDSIM="${MQT_DDSIM_ROOT:-/home/flareon/workshop/mqt-ddsim}"
BENCH="${MQT_DDSIM_BENCH:-${DDSIM}/build-local/apps/mqt-ddsim-noise_aware}"
ASSETS="${MQT_DDSIM_ASSETS:-/home/flareon/workshop/igqmdd/assets}"
WORK="${MQT_WORK:-/tmp/mqt_ddsim_dynamic}"
RESULTS="${MQT_RESULTS:-/home/flareon/workshop/results}"

TIMEOUT_SEC="${MQT_DDSIM_TIMEOUT_SEC:-3600}"
MEM_MAX="${MQT_MEM_MAX:-40G}"
REPEATS="${MQT_REPEATS:-1}"
SHOTS="${MQT_DDSIM_SHOTS:-20000}"

NOISE_EFFECTS="${MQT_DDSIM_NOISE_EFFECTS:-APD}"
NOISE_PROB="${MQT_DDSIM_NOISE_PROB:-0.001}"
MAX_QUBITS="${MQT_DDSIM_MAX_QUBITS:-18}"
CIRCUIT_LIST="${MQT_DDSIM_CIRCUITS:-}"

# Every arm has a name and a flag string. The name is used for the result
# files, the flag string is passed to the simulator unchanged.
# The cheap arms come first so that an early stop still leaves a usable
# comparison. The last two arms restrict the sift to the qubits of the next
# few gates, which makes one sift much cheaper.
ARM_NAMES=(none spectral rb degree dyn2000 dyn500 dyn2000_w4 dyn500_w4
  spectral_dyn500 dyn500_node)
ARM_FLAGS=(
  ""
  "--initial_reorder spectral"
  "--initial_reorder recursive_bisection"
  "--initial_reorder degree"
  "--initial_reorder none --density_dynamic_reorder --density_threshold 2000"
  "--initial_reorder none --density_dynamic_reorder --density_threshold 500"
  "--initial_reorder none --density_dynamic_reorder --density_threshold 2000 --density_reorder_window 4"
  "--initial_reorder none --density_dynamic_reorder --density_threshold 500 --density_reorder_window 4"
  "--initial_reorder spectral --density_dynamic_reorder --density_threshold 500"
  "--initial_reorder none --density_dynamic_reorder --density_threshold 500 --density_reorder_window 4 --density_reorder_exchange rebuild"
)

# Stop starting new runs after this many seconds. The script always merges the
# runs that finished. Default: ten hours.
DEADLINE_SEC="${MQT_DDSIM_DEADLINE_SEC:-36000}"
startEpoch="$(date +%s)"

log() { printf '[%s] %s\n' "$(date '+%F %T')" "$*"; }

mkdir -p "${WORK}/qasm" "${WORK}/json" "${WORK}/logs" "${RESULTS}"

if [[ ! -x "${BENCH}" ]]; then
  log "benchmark tool ${BENCH} is missing"
  exit 1
fi

# Pick the circuits by qubit count and convert them to OpenQASM 3.
pickSources() {
  local source
  source="$(mktemp -d "${WORK}/sources.XXXXXX")"
  for real in "${ASSETS}"/*/*.real; do
    [[ -f "${real}" ]] || continue
    if [[ -n "${CIRCUIT_LIST}" ]] &&
      ! grep -qx "$(basename "${real}" .real)" "${CIRCUIT_LIST}"; then
      continue
    fi
    local qubits
    qubits="$(sed -n 's/^\.numvars[[:space:]]*\([0-9]*\).*/\1/p' "${real}" |
      head -n 1)"
    [[ -n "${qubits}" ]] || continue
    if [[ "${qubits}" -le "${MAX_QUBITS}" ]]; then
      ln -sf "${real}" "${source}/"
    fi
  done
  python3 "${ROOT}/test/real_to_qasm3.py" "${source}" "${WORK}/qasm" \
    >"${WORK}/convert.log" 2>&1
  echo "${source}"
}

sourceDir="$(pickSources)"
mapfile -t circuits < <(cd "${sourceDir}" && ls ./*.real |
  sed 's|^\./||; s|\.real$|.qasm|')
log "found ${#circuits[@]} circuits with at most ${MAX_QUBITS} qubits"

runOne() {
  local outfile="$1" logfile="$2" circuit="$3" flags="$4"
  local raw="${outfile}.raw"
  # shellcheck disable=SC2086
  systemd-run --user --scope -q -p "MemoryMax=${MEM_MAX}" \
    -p MemorySwapMax=0 -- \
    timeout "${TIMEOUT_SEC}" "${BENCH}" \
    --use_density_matrix_simulator \
    --noise_effects "${NOISE_EFFECTS}" --noise_prob "${NOISE_PROB}" \
    --simulate_file "${WORK}/qasm/${circuit}" --ps --pm --shots "${SHOTS}" \
    ${flags} \
    >"${raw}" 2>"${logfile}"
  local rc=$?
  if [[ ${rc} -eq 0 && -s "${raw}" ]] && grep -q '"statistics"' "${raw}"; then
    mv "${raw}" "${outfile}"
    return 0
  fi
  rm -f "${raw}"
  local status="error"
  [[ ${rc} -eq 124 ]] && status="timeout"
  [[ ${rc} -eq 137 ]] && status="out-of-memory"
  printf '%s\n' "${status}" >"${outfile}"
  return 1
}

for ((armIndex = 0; armIndex < ${#ARM_NAMES[@]}; armIndex++)); do
  name="${ARM_NAMES[armIndex]}"
  flags="${ARM_FLAGS[armIndex]}"
  for circuit in "${circuits[@]}"; do
    for ((rep = 1; rep <= REPEATS; rep++)); do
      stem="${circuit%.qasm}__${name}__rep${rep}"
      outfile="${WORK}/json/${stem}.json"
      logfile="${WORK}/logs/${stem}.log"
      [[ -s "${outfile}" ]] && continue
      if [[ $(( $(date +%s) - startEpoch )) -ge ${DEADLINE_SEC} ]]; then
        log "deadline reached, stopping before ${stem}"
        break 3
      fi
      if runOne "${outfile}" "${logfile}" "${circuit}" "${flags}"; then
        log "[ok] ${stem}"
      else
        log "[$(cat "${outfile}")] ${stem}"
      fi
    done
  done
done

log "merging"
python3 "${ROOT}/test/merge_ddsim_dynamic_experiments.py" "${WORK}" "${RESULTS}"
log "done"
