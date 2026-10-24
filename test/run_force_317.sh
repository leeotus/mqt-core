#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License
#
# Run the FORCE ordering over a list of RevLib circuits, one process per
# circuit, so that one slow circuit cannot block the rest.
#
#   MQT_FORCE_JOBS=8 MQT_FORCE_TIMEOUT=900 \
#     bash test/run_force_317.sh LIST.txt OUT.csv [force|mince]

set -uo pipefail

ROOT="${MQT_CORE_ROOT:-/home/flareon/workshop/mqt-core}"
BENCH="${MQT_CORE_BENCH:-${ROOT}/build/bin/mqt-core-order-bench}"
LIST="${1:?usage: run_force_317.sh LIST.txt OUT.csv [STRATEGY]}"
OUT="${2:?usage: run_force_317.sh LIST.txt OUT.csv [STRATEGY]}"
STRATEGY="${3:-force}"
JOBS="${MQT_FORCE_JOBS:-8}"
TIMEOUT_SEC="${MQT_FORCE_TIMEOUT:-900}"
MAX_QUBITS="${MQT_FORCE_MAX_QUBITS:-40}"

"${BENCH}" --header >"${OUT}"

xargs -P "${JOBS}" -I{} bash -c '
  circuit="$1"
  row="$(timeout "$2" "$3" --only "$4" --skip-post-hoc \
    --dynamic-threshold 1000000000 --max-qubits "$5" "${circuit}" 2>/dev/null |
    tail -n 1)"
  if [[ -n "${row}" ]]; then
    printf "%s\n" "${row}"
  else
    padding="$(printf ",%.0s" {1..36})"
    printf "%s%s\n" "${circuit}" "${padding}"
  fi
' _ {} "${TIMEOUT_SEC}" "${BENCH}" "${STRATEGY}" "${MAX_QUBITS}" <"${LIST}" >>"${OUT}"

echo "wrote ${OUT}"
