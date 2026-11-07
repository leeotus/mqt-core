#!/usr/bin/env bash
# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Run the QST probe over one QASM file or a directory of QASM files and write
# one CSV row per circuit. A circuit that exceeds the wall-clock or memory limit
# is aborted; the fields that the probe would have filled are written as "-".
#
# The memory limit is enforced on the resident set size (VmRSS) of the probe
# process. This differs from `ulimit -v`, which limits the virtual address
# space and would reject OpenMP runs because every thread reserves stack.
#
# Usage:
#   bash test/run_qst_batch.sh [options] CIRCUIT_PATH
#
# Options:
#   --output FILE          write the CSV to FILE (required)
#   --probe PATH           probe binary
#                          (default: build/release/bin/mqt-core-qst-probe)
#   --time-limit SECONDS   wall-clock limit per circuit (0 = unlimited)
#   --memory-limit MB      peak RSS limit per circuit (0 = unlimited)
#   --shots N              shots per basis (default: 1000)
#   --random-bases K       number of random Pauli bases, 0 selects all 3^n
#                          (default: 50)
#   --iterations I         maximum MLE iterations (default: 50)
#   --max-outcomes K       outcomes kept per basis, 0 keeps all (default: 0)
#   --backend NAME         qmdd or dense (default: qmdd)
#   --seed S               sampling seed (default: 42)
#   --limit N              only run the first N circuits (0 = all)
#   --full-path            write the full circuit path to the CSV instead of
#                          the file name
#   -h, --help             print this help
#
# CSV columns:
#   circuit,backend,n,bases,shots,iterations,fidelity,purity,time_ms,converged,
#   dd_size,max_iterations,max_outcomes,true_purity,trace_distance,trace,
#   trace_error,log_likelihood,observed_outcomes,retained_outcomes,
#   initial_dd_size,peak_dd_size,peak_r_dd_size,density_ms,basis_ms,sampling_ms,
#   reconstruct_init_ms,reconstruct_iter_ms,metrics_ms,total_ms,elapsed_s,
#   peak_rss_mb,status
#
# status is one of ok, timeout, memory, error. For every status other than ok,
# probe-derived fields hold "-"; elapsed_s and peak_rss_mb still report what was
# measured before the abort.
#
# Examples:
#   bash test/run_qst_batch.sh --output out.csv \
#       --time-limit 600 --memory-limit 4096 \
#       /home/flareon/workshop/quantum_circuits/q8
#
#   bash test/run_qst_batch.sh --output one.csv \
#       /home/flareon/workshop/quantum_circuits/q10/adder_n10.qasm

set -uo pipefail

ROOT="${MQT_CORE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
BUILD_DIR="${MQT_BUILD_DIR:-${ROOT}/build/release}"
PROBE="${MQT_CORE_QST_PROBE:-${BUILD_DIR}/bin/mqt-core-qst-probe}"

OUTPUT=""
TIME_LIMIT_SEC=0
MEMORY_LIMIT_MB=0
SHOTS=1000
RANDOM_BASES=50
ITERATIONS=50
MAX_OUTCOMES=0
BACKEND=qmdd
SEED=42
LIMIT=0
FULL_PATH=0
POLL_INTERVAL=0.2
INPUT=""

usage() {
  cat <<'EOF'
Run the QST probe over one QASM file or a directory of QASM files and write
one CSV row per circuit. A circuit that exceeds the wall-clock or memory limit
is aborted; the fields that the probe would have filled are written as "-".

The memory limit is enforced on the resident set size (VmRSS) of the probe
process. This differs from `ulimit -v`, which limits the virtual address
space and would reject OpenMP runs because every thread reserves stack.

Usage:
  bash test/run_qst_batch.sh [options] CIRCUIT_PATH

Options:
  --output FILE          write the CSV to FILE (required)
  --probe PATH           probe binary
                         (default: build/release/bin/mqt-core-qst-probe)
  --time-limit SECONDS   wall-clock limit per circuit (0 = unlimited)
  --memory-limit MB      peak RSS limit per circuit (0 = unlimited)
  --shots N              shots per basis (default: 1000)
  --random-bases K       number of random Pauli bases, 0 selects all 3^n
                         (default: 50)
  --iterations I         maximum MLE iterations (default: 50)
  --max-outcomes K       outcomes kept per basis, 0 keeps all (default: 0)
  --backend NAME         qmdd or dense (default: qmdd)
  --seed S               sampling seed (default: 42)
  --limit N              only run the first N circuits (0 = all)
  --full-path            write the full circuit path to the CSV instead of
                         just the file name
  -h, --help             print this help

CSV columns:
  circuit,backend,n,bases,shots,iterations,fidelity,purity,time_ms,converged,
  dd_size,max_iterations,max_outcomes,true_purity,trace_distance,trace,
  trace_error,log_likelihood,observed_outcomes,retained_outcomes,
  initial_dd_size,peak_dd_size,peak_r_dd_size,density_ms,basis_ms,sampling_ms,
  reconstruct_init_ms,reconstruct_iter_ms,metrics_ms,total_ms,elapsed_s,
  peak_rss_mb,status

The circuit column holds the file name only, for example adder_n10.qasm.
Pass --full-path to store the path as it was found instead.

status is one of ok, timeout, memory, error. For every status other than ok,
probe-derived fields hold "-"; elapsed_s and peak_rss_mb still report what was
measured before the abort.

Examples:
  bash test/run_qst_batch.sh --output out.csv \
      --time-limit 600 --memory-limit 4096 \
      /home/flareon/workshop/quantum_circuits/q8

  bash test/run_qst_batch.sh --output one.csv \
      /home/flareon/workshop/quantum_circuits/q10/adder_n10.qasm
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --output) [[ $# -ge 2 ]] || die "--output needs a value"; OUTPUT="$2"; shift 2 ;;
    --probe) [[ $# -ge 2 ]] || die "--probe needs a value"; PROBE="$2"; shift 2 ;;
    --time-limit) [[ $# -ge 2 ]] || die "--time-limit needs a value"; TIME_LIMIT_SEC="$2"; shift 2 ;;
    --memory-limit) [[ $# -ge 2 ]] || die "--memory-limit needs a value"; MEMORY_LIMIT_MB="$2"; shift 2 ;;
    --shots) [[ $# -ge 2 ]] || die "--shots needs a value"; SHOTS="$2"; shift 2 ;;
    --random-bases) [[ $# -ge 2 ]] || die "--random-bases needs a value"; RANDOM_BASES="$2"; shift 2 ;;
    --iterations) [[ $# -ge 2 ]] || die "--iterations needs a value"; ITERATIONS="$2"; shift 2 ;;
    --max-outcomes) [[ $# -ge 2 ]] || die "--max-outcomes needs a value"; MAX_OUTCOMES="$2"; shift 2 ;;
    --backend) [[ $# -ge 2 ]] || die "--backend needs a value"; BACKEND="$2"; shift 2 ;;
    --seed) [[ $# -ge 2 ]] || die "--seed needs a value"; SEED="$2"; shift 2 ;;
    --limit) [[ $# -ge 2 ]] || die "--limit needs a value"; LIMIT="$2"; shift 2 ;;
    --full-path) FULL_PATH=1; shift ;;
    -h | --help) usage; exit 0 ;;
    -*) die "unknown option: $1" ;;
    *) [[ -z "$INPUT" ]] || die "only one circuit path is allowed"; INPUT="$1"; shift ;;
  esac
done

[[ -n "$INPUT" ]] || die "missing CIRCUIT_PATH"
[[ -n "$OUTPUT" ]] || die "missing --output"
[[ -x "$PROBE" ]] || die "probe is not executable: $PROBE"
[[ "$OUTPUT" != "-" ]] || die "--output must be a path, not -"

for value in "$TIME_LIMIT_SEC" "$MEMORY_LIMIT_MB" "$SHOTS" "$RANDOM_BASES" \
             "$ITERATIONS" "$MAX_OUTCOMES" "$SEED" "$LIMIT"; do
  [[ "$value" =~ ^[0-9]+$ ]] || die "not a non-negative integer: $value"
done
[[ "$BACKEND" == "qmdd" || "$BACKEND" == "dense" ]] ||
  die "backend must be qmdd or dense: $BACKEND"

now_ms() { date +%s%3N; }

# Quote a field for CSV. Paths may contain spaces or commas.
csv_field() {
  local value="$1"
  value="${value//\"/\"\"}"
  printf '"%s"' "$value"
}

ms_to_s() { awk -v ms="$1" 'BEGIN { printf "%.2f", ms / 1000 }'; }
kb_to_mb() { awk -v kb="$1" 'BEGIN { printf "%.1f", kb / 1024 }'; }

TMP_OUT=""
TMP_ERR=""

cleanup() {
  [[ -n "$TMP_OUT" ]] && rm -f "$TMP_OUT"
  [[ -n "$TMP_ERR" ]] && rm -f "$TMP_ERR"
}
trap cleanup EXIT INT TERM

collect_circuits() {
  local input="$1"
  if [[ -d "$input" ]]; then
    find "$input" -type f -name '*.qasm' -print0 | sort -z
  elif [[ -f "$input" ]]; then
    printf '%s\0' "$input"
  else
    printf 'no such file or directory: %s\n' "$input" >&2
    return 1
  fi
}

# Run the probe on one circuit and print a single CSV line.
run_one() {
  local circuit="$1"
  # The CSV holds the file name only; the progress lines keep the full path.
  local label
  if (( FULL_PATH )); then
    label="$circuit"
  else
    label=$(basename -- "$circuit")
  fi
  local pid rc start_ms elapsed_ms status max_rss_kb rss line
  local reported_backend=- n=- bases=- shots=- iterations=- fidelity=- purity=-
  local time_ms=- converged=-
  local -a fields=()
  local -a details=()
  for ((i = 0; i < 20; ++i)); do
    details+=("-")
  done

  TMP_OUT=$(mktemp) || die "cannot create a temporary file"
  TMP_ERR=$(mktemp) || die "cannot create a temporary file"

  status="ok"
  max_rss_kb=0
  start_ms=$(now_ms)

  "$PROBE" --circuit "$circuit" \
      --shots "$SHOTS" \
      --random-bases "$RANDOM_BASES" \
      --iterations "$ITERATIONS" \
      --max-outcomes "$MAX_OUTCOMES" \
      --backend "$BACKEND" \
      --seed "$SEED" >"$TMP_OUT" 2>"$TMP_ERR" &
  pid=$!

  while kill -0 "$pid" 2>/dev/null; do
    rss=$(awk '/^VmRSS:/ { print $2 }' "/proc/$pid/status" 2>/dev/null)
    if [[ -n "$rss" ]]; then
      if (( rss > max_rss_kb )); then
        max_rss_kb=$rss
      fi
      if (( MEMORY_LIMIT_MB > 0 && rss > MEMORY_LIMIT_MB * 1024 )); then
        status="memory"
        kill -9 "$pid" 2>/dev/null
        break
      fi
    fi
    elapsed_ms=$(( $(now_ms) - start_ms ))
    if (( TIME_LIMIT_SEC > 0 && elapsed_ms > TIME_LIMIT_SEC * 1000 )); then
      status="timeout"
      kill -9 "$pid" 2>/dev/null
      break
    fi
    sleep "$POLL_INTERVAL"
  done

  wait "$pid" 2>/dev/null
  rc=$?
  elapsed_ms=$(( $(now_ms) - start_ms ))

  if [[ "$status" == "ok" && $rc -ne 0 ]]; then
    status="error"
  fi

  if [[ "$status" == "ok" ]]; then
    line=$(tail -n 1 "$TMP_OUT")
    IFS=',' read -r -a fields <<< "$line"
    if (( ${#fields[@]} < 30 )); then
      status="error"
      reported_backend=- n=- bases=- shots=- iterations=- fidelity=- purity=-
      time_ms=- converged=-
    else
      reported_backend="${fields[1]}"
      n="${fields[2]}"
      bases="${fields[3]}"
      shots="${fields[4]}"
      iterations="${fields[5]}"
      fidelity="${fields[6]}"
      purity="${fields[7]}"
      time_ms="${fields[9]}"
      converged="${fields[10]}"
      details=("${fields[8]}" "${fields[@]:11:19}")
    fi
  fi

  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s' "$(csv_field "$label")" \
      "$reported_backend" "$n" "$bases" "$shots" "$iterations" "$fidelity" \
      "$purity" "$time_ms" "$converged"
  printf ',%s' "${details[@]}"
  printf ',%s,%s,%s\n' "$(ms_to_s "$elapsed_ms")" \
      "$(kb_to_mb "$max_rss_kb")" "$status"

  if [[ "$status" == "error" ]]; then
    printf '      probe stderr: %s' "$(tail -n 1 "$TMP_ERR" 2>/dev/null)" >&2
    printf '\n' >&2
  fi

  rm -f "$TMP_OUT" "$TMP_ERR"
  TMP_OUT=""
  TMP_ERR=""
}

CIRCUITS=()
while IFS= read -r -d '' file; do
  CIRCUITS+=("$file")
done < <(collect_circuits "$INPUT")

if (( ${#CIRCUITS[@]} == 0 )); then
  die "no QASM files found under $INPUT"
fi

TOTAL=${#CIRCUITS[@]}
if (( LIMIT > 0 && LIMIT < TOTAL )); then
  TOTAL=$LIMIT
fi

printf 'circuit,backend,n,bases,shots,iterations,fidelity,purity,time_ms,converged,dd_size,max_iterations,max_outcomes,true_purity,trace_distance,trace,trace_error,log_likelihood,observed_outcomes,retained_outcomes,initial_dd_size,peak_dd_size,peak_r_dd_size,density_ms,basis_ms,sampling_ms,reconstruct_init_ms,reconstruct_iter_ms,metrics_ms,total_ms,elapsed_s,peak_rss_mb,status\n' \
    >"$OUTPUT"

printf 'probe=%s backend=%s circuits=%d time-limit=%ss memory-limit=%sMiB shots=%d bases=%s iterations=%d max-outcomes=%s seed=%d\n' \
    "$PROBE" "$BACKEND" "$TOTAL" "$TIME_LIMIT_SEC" "$MEMORY_LIMIT_MB" "$SHOTS" \
    "$RANDOM_BASES" "$ITERATIONS" "$MAX_OUTCOMES" "$SEED" >&2

count_ok=0
count_timeout=0
count_memory=0
count_error=0
index=0

for circuit in "${CIRCUITS[@]}"; do
  index=$((index + 1))
  if (( LIMIT > 0 && index > LIMIT )); then
    break
  fi
  printf '[%d/%d] %s ... ' "$index" "$TOTAL" "$circuit" >&2
  line=$(run_one "$circuit")
  printf '%s\n' "$line" >>"$OUTPUT"
  status=${line##*,}
  case "$status" in
    ok) count_ok=$((count_ok + 1)) ;;
    timeout) count_timeout=$((count_timeout + 1)) ;;
    memory) count_memory=$((count_memory + 1)) ;;
    *) count_error=$((count_error + 1)) ;;
  esac
  printf '%s\n' "$status" >&2
done

printf 'done ok=%d timeout=%d memory=%d error=%d csv=%s\n' \
    "$count_ok" "$count_timeout" "$count_memory" "$count_error" "$OUTPUT" >&2

if (( count_error > 0 )); then
  exit 1
fi
exit 0
