#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
#
# Measure the effect of a static variable order on the MQT QCEC equivalence
# checkers.
#
# The experiment pairs every circuit of the QCEC benchmark set with its
# transpiled implementation. Each pair runs with the natural order and with
# four reordering policies. Every run uses its own process, so the allocation
# warm-up of an earlier run cannot bias the measurement, and every run repeats.
#
#   screen -dmS mqt_qcec_exp -L -Logfile /tmp/mqt_qcec_screen.log \
#     bash /home/flareon/workshop/mqt-core/test/run_qcec_experiments.sh
#
# The script stores one file per run. A run is skipped when its file exists, so
# the script can be stopped and started again at any time. A later pass with a
# larger MQT_QCEc_TIMEOUT_SEC and MQT_RETRY_TIMEOUTS=1 redoes the runs that hit
# the wall clock cap.
#
# Environment overrides:
#   MQT_QCEc_TIMEOUT_SEC  wall clock cap of one equivalence check (default 1800)
#   MQT_QCEC_DD_TIMEOUT_SEC  wall clock cap of one decision diagram build
#                            (default 300)
#   MQT_RETRY_TIMEOUTS    set to 1 to redo failed runs

set -uo pipefail

ROOT="${MQT_CORE_ROOT:-/home/flareon/workshop/mqt-core}"
QCEC="${MQT_QCEC_ROOT:-/home/flareon/workshop/qcec}"
CIRCUITS="${MQT_QCEC_CIRCUITS:-${QCEC}/test/circuits}"
BENCH="${MQT_QCEC_BENCH:-${QCEC}/build-local/apps/qcec_order_experiment}"
WORK="${MQT_WORK:-/tmp/mqt_qcec_experiments}"
RESULTS="${MQT_RESULTS:-/home/flareon/workshop/results}"

TIMEOUT_SEC="${MQT_QCEc_TIMEOUT_SEC:-1800}"
DD_TIMEOUT_SEC="${MQT_QCEC_DD_TIMEOUT_SEC:-300}"
MEM_MAX="${MQT_MEM_MAX:-40G}"
REPEATS="${MQT_REPEATS:-3}"
RETRY="${MQT_RETRY_TIMEOUTS:-0}"

POLICIES="${MQT_QCEC_POLICIES:-none gate_dependency degree spectral recursive_bisection}"
CHECKERS="${MQT_QCEC_CHECKERS:-construction simulation alternating}"
DD_SIZE_LIMIT="${MQT_QCEC_DD_SIZE_LIMIT:-64}"

log() { printf '[%s] %s\n' "$(date '+%F %T')" "$*"; }

mkdir -p "${WORK}/rows/check" "${WORK}/rows/dd" "${WORK}/logs" "${RESULTS}"

if [[ ! -x "${BENCH}" ]]; then
  log "benchmark tool ${BENCH} is missing"
  exit 1
fi

mapfile -t pairs < <(cd "${CIRCUITS}/original" && ls ./*.qasm | sed 's|^\./||; s|\.qasm$||')
log "found ${#pairs[@]} circuit pairs, ${#pairs[@]}x$(wc -w <<< "${POLICIES}")x$(wc -w <<< "${CHECKERS}")x${REPEATS} check runs"

# Run one benchmark invocation and store the single CSV row.
# Arguments: rowfile, logfile, then the tool arguments.
runOne() {
  local rowfile="$1" logfile="$2" limit="$3"
  shift 3
  local rawfile="${rowfile}.raw"
  systemd-run --user --scope -q -p "MemoryMax=${MEM_MAX}" \
    -p MemorySwapMax=0 -- timeout "${limit}" "${BENCH}" "$@" \
    > "${rawfile}" 2> "${logfile}"
  local rc=$?
  if [[ ${rc} -eq 0 && -s "${rawfile}" ]]; then
    mv "${rawfile}" "${rowfile}"
    return 0
  fi
  rm -f "${rawfile}"
  local status="error"
  [[ ${rc} -eq 124 ]] && status="timeout"
  [[ ${rc} -eq 137 ]] && status="out-of-memory"
  printf '%s\n' "${status}" > "${rowfile}"
  return 1
}

runChecks() {
  local pair checker policy rep rowfile logfile
  for pair in "${pairs[@]}"; do
    for checker in ${CHECKERS}; do
      for ((rep = 1; rep <= REPEATS; rep++)); do
        for policy in ${POLICIES}; do
          rowfile="${WORK}/rows/check/${pair}__${policy}__${checker}__rep${rep}.csv"
          logfile="${WORK}/logs/${pair}__${policy}__${checker}__rep${rep}.log"
          if [[ -s "${rowfile}" ]]; then
            if [[ "${RETRY}" == "1" ]] && ! grep -q ',' "${rowfile}"; then
              :
            else
              continue
            fi
          fi
          if runOne "${rowfile}" "${logfile}" "${TIMEOUT_SEC}" \
            --policy "${policy}" --checker "${checker}" --timeout 0 \
            "${CIRCUITS}/original/${pair}.qasm" \
            "${CIRCUITS}/transpiled/${pair}_transpiled.qasm"; then
            log "[check] ${pair} ${policy} ${checker} rep${rep} ok"
          else
            log "[check] ${pair} ${policy} ${checker} rep${rep} $(cat "${rowfile}")"
          fi
        done
      done
    done
  done
}

runDdSizes() {
  local pair policy rep rowfile logfile
  for pair in "${pairs[@]}"; do
    for ((rep = 1; rep <= REPEATS; rep++)); do
      for policy in ${POLICIES}; do
        rowfile="${WORK}/rows/dd/${pair}__${policy}__rep${rep}.csv"
        logfile="${WORK}/logs/dd_${pair}__${policy}__rep${rep}.log"
        if [[ -s "${rowfile}" ]]; then
          if [[ "${RETRY}" == "1" ]] && ! grep -q ',' "${rowfile}"; then
            :
          else
            continue
          fi
        fi
        if runOne "${rowfile}" "${logfile}" "${DD_TIMEOUT_SEC}" \
          --mode dd --dd-target ref --policy "${policy}" \
          --dd-size-limit "${DD_SIZE_LIMIT}" \
          "${CIRCUITS}/original/${pair}.qasm" \
          "${CIRCUITS}/transpiled/${pair}_transpiled.qasm"; then
          log "[dd] ${pair} ${policy} rep${rep} ok"
        else
          log "[dd] ${pair} ${policy} rep${rep} $(cat "${rowfile}")"
        fi
      done
    done
  done
}

runChecks
runDdSizes

log "merging"
python3 "${ROOT}/test/merge_qcec_experiments.py" "${WORK}" "${RESULTS}"
log "done"
