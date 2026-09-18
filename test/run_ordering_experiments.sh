#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
#
# Collect the variable ordering experiment table over a directory of RevLib
# circuits. For every circuit the script records the size and the construction
# time of the functional decision diagram for
#   1. the original physical order,
#   2. several static variable orders,
#   3. dynamic reordering with sifting.
#
# The script is written to run inside a screen session, for example
#
#   screen -dmS mqt_order_exp -L -Logfile /tmp/mqt_ordering_screen.log \
#     bash /home/flareon/workshop/mqt-core/test/run_ordering_experiments.sh
#
# Every circuit runs under a wall clock limit of one hour (MQT_TIMEOUT_SEC) and
# under a memory limit of forty gigabytes (MQT_MEM_MAX). Per circuit logs and
# other intermediate files go to /tmp, the CSV table goes to
# /home/flareon/workshop/results. The script resumes: circuits that already
# appear in the CSV are skipped.

set -uo pipefail

ROOT="${MQT_CORE_ROOT:-/home/flareon/workshop/mqt-core}"
BUILD="${MQT_CORE_BUILD:-${ROOT}/build}"
ASSETS="${MQT_ASSETS:-/home/flareon/workshop/igqmdd/assets}"
WORK="${MQT_WORK:-/tmp/mqt_ordering_full}"
OUT="${MQT_OUT:-/home/flareon/workshop/results/ordering_full_experiments.csv}"
BENCH="${BUILD}/bin/mqt-core-order-bench"
TIMEOUT_SEC="${MQT_TIMEOUT_SEC:-3600}"
MEM_MAX="${MQT_MEM_MAX:-40G}"
MAX_QUBITS="${MQT_MAX_QUBITS:-40}"

log() { printf '[%s] %s\n' "$(date '+%F %T')" "$*"; }

mkdir -p "${WORK}/logs" "$(dirname "${OUT}")"

if [[ ! -x "${BENCH}" ]]; then
  log "building the benchmark tool"
  cmake -S "${ROOT}" -B "${BUILD}" -DBUILD_MQT_CORE_APPS=ON > /dev/null || exit 1
  cmake --build "${BUILD}" --target mqt-core-order-bench -j "$(nproc)" \
    > "${WORK}/build.log" 2>&1 || { log "build failed, see ${WORK}/build.log"; exit 1; }
fi

if [[ ! -s "${OUT}" ]]; then
  "${BENCH}" --header > "${OUT}"
  log "wrote the header to ${OUT}"
fi

# Order the circuits by qubit count so that the small, feasible circuits run
# first and the large ones fill the table at the end.
mapfile -t CIRCUITS < <(
  for circuit in "${ASSETS}"/*/*.real; do
    [[ -f "${circuit}" ]] || continue
    qubits=$(sed -n 's/^\.numvars[[:space:]]*\([0-9]*\).*/\1/p' "${circuit}" | head -n 1)
    printf '%05d %s\n' "${qubits:-0}" "${circuit}"
  done | sort -n | cut -d' ' -f2-
)

total="${#CIRCUITS[@]}"
log "running ${total} circuits from ${ASSETS}"
ok=0
failed=0
index=0

for circuit in "${CIRCUITS[@]}"; do
  index=$((index + 1))
  name="$(basename "${circuit}" .real)"

  if grep -q "^${circuit}," "${OUT}" 2> /dev/null; then
    log "[${index}/${total}] skip ${name}, already recorded"
    continue
  fi

  qubits="$(sed -n 's/^\.numvars[[:space:]]*\([0-9]*\).*/\1/p' "${circuit}" | head -n 1)"
  logfile="${WORK}/logs/${name}.log"
  rowfile="${WORK}/logs/${name}.csv"
  start="$(date +%s)"

  if command -v systemd-run > /dev/null 2>&1; then
    systemd-run --user --scope -q -p "MemoryMax=${MEM_MAX}" \
      -p MemorySwapMax=0 -- timeout "${TIMEOUT_SEC}" "${BENCH}" \
      --max-qubits "${MAX_QUBITS}" "${circuit}" \
      > "${rowfile}" 2> "${logfile}"
    rc=$?
  else
    ( ulimit -v 41943040
      timeout "${TIMEOUT_SEC}" "${BENCH}" --max-qubits "${MAX_QUBITS}" \
        "${circuit}" ) > "${rowfile}" 2> "${logfile}"
    rc=$?
  fi

  elapsed=$(( $(date +%s) - start ))
  if [[ ${rc} -eq 0 && -s "${rowfile}" ]]; then
    cat "${rowfile}" >> "${OUT}"
    ok=$((ok + 1))
    status="$(cut -d, -f4 "${rowfile}")"
    log "[${index}/${total}] ${status:-ok} ${name} (${elapsed}s)"
  else
    status="error"
    [[ ${rc} -eq 124 ]] && status="timeout"
    [[ ${rc} -eq 137 ]] && status="out-of-memory"
    pad="$(printf ',%.0s' $(seq 1 22))"
    printf '%s,%s,,%s%s\n' "${circuit}" "${qubits:-}" "${status}" "${pad}" \
      >> "${OUT}"
    failed=$((failed + 1))
    log "[${index}/${total}] ${status} ${name} rc=${rc} (${elapsed}s)"
  fi
done

log "finished: ${ok} circuits ok, ${failed} circuits failed or skipped"
log "table: ${OUT}"
