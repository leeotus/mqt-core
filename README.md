# MQT Core — QST experiment build and run notes

This branch adds a maximum-likelihood quantum state tomography (QST) module
(`include/mqt-core/dd/QST.hpp`, `src/dd/QST.cpp`) plus a command line probe.
It rebuilds an n-qubit density matrix from Pauli-basis measurement records with
the dilute MLE iteration `R = sum (f/p) Pi`, `rho <- R rho R / Tr(R rho R)`.

## Requirements

- CMake 3.24+
- a C++20 compiler
- Linux (the batch script reads `/proc/<pid>/status` for the memory limit)
- optionally OpenMP and BLAS; both are detected at configure time and the code
  falls back to scalar loops when they are missing

## Build

```bash
cmake --preset release
cmake --build --preset release
```

Build only the two targets needed for the experiments:

```bash
cmake --build build/release --target mqt-core-qst-probe mqt-core-dd-test -j 8
```

For a debug build, replace `release` with `debug` everywhere.

## Test

```bash
./build/release/test/dd/mqt-core-dd-test --gtest_filter='QST.*'
```

## Run the probe

Synthetic states (`zero`, `ghz`, `random`):

```bash
./build/release/bin/mqt-core-qst-probe \
    --state ghz --qubits 8 \
    --shots 1000 --random-bases 50 --iterations 100
```

Circuit from an OpenQASM 2.0 or 3.0 file (final measurements are dropped, the
qubit count comes from the circuit):

```bash
./build/release/bin/mqt-core-qst-probe \
    --circuit /home/flareon/workshop/quantum_circuits/q10/adder_n10.qasm \
    --shots 1000 --random-bases 50 --iterations 50
```

Probe options:

| Option | Default | Meaning |
| --- | --- | --- |
| `--state` | `ghz` | `zero`, `ghz`, or `random` |
| `--circuit` | — | OpenQASM file, overrides `--state` and `--qubits` |
| `--qubits` | 2 | 1 to 15 |
| `--shots` | 2000 | shots per basis |
| `--random-bases` | 0 | number of random Pauli bases, 0 selects all 3^n |
| `--iterations` | 200 | maximum MLE iterations |
| `--seed` | 42 | seed for the random state, the bases, and the sampling |

The probe prints one CSV line to stdout and `true_purity=` to stderr:

```text
state,n,bases,shots,iterations,fidelity,purity,dd_size,time_ms,converged
qasm,10,50,1000,32,0.992124,1.000000,0,227700.028,1
```

## Batch over many circuits

`test/run_qst_batch.sh` runs the probe over one QASM file or a directory tree
and writes one CSV row per circuit. It enforces a wall-clock limit and a
resident-memory limit per circuit.

```bash
bash test/run_qst_batch.sh \
    --output q8.csv \
    --time-limit 600 \
    --memory-limit 4096 \
    --shots 1000 \
    --random-bases 50 \
    --iterations 100 \
    /home/flareon/workshop/quantum_circuits/q8
```

Single file with tighter limits:

```bash
bash test/run_qst_batch.sh --output one.csv \
    --time-limit 120 --memory-limit 1024 \
    /home/flareon/workshop/quantum_circuits/q2/ae_alg_2.qasm
```

Script options:

| Option | Default | Meaning |
| --- | --- | --- |
| `--output` | required | CSV output path |
| `--time-limit` | 0 | wall-clock seconds per circuit, 0 = unlimited |
| `--memory-limit` | 0 | peak RSS in MiB per circuit, 0 = unlimited |
| `--shots` | 1000 | shots per basis |
| `--random-bases` | 50 | random Pauli bases, 0 selects all 3^n |
| `--iterations` | 50 | maximum MLE iterations |
| `--seed` | 42 | sampling seed |
| `--limit` | 0 | only the first N circuits, 0 = all |
| `--probe` | `build/release/bin/mqt-core-qst-probe` | probe binary |
| `-h`, `--help` | — | print the built-in help |

The probe binary can also be set with `MQT_CORE_QST_PROBE`.

## CSV columns

```text
circuit,n,bases,shots,iterations,fidelity,purity,time_ms,converged,elapsed_s,peak_rss_mb,status
```

`status` is one of `ok`, `timeout`, `memory`, `error`. For every status other
than `ok`, the columns `n` through `converged` hold `-`; `elapsed_s` and
`peak_rss_mb` still report what was measured before the abort.

```text
circuit,n,bases,shots,iterations,fidelity,purity,time_ms,converged,elapsed_s,peak_rss_mb,status
"/…/q2/ae_alg_2.qasm",2,9,1000,50,0.991182,0.982939,1.282,0,0.21,5.7,ok
"/…/q10/adder_n10.qasm",-,-,-,-,-,-,-,-,2.09,63.8,timeout
"/…/q10/adder_n10.qasm",-,-,-,-,-,-,-,-,0.21,34.3,memory
"/…/q16/ae_alg_16.qasm",-,-,-,-,-,-,-,-,0.41,42.0,error
```

Rows are appended as each circuit finishes, so an interrupted run keeps the
results collected so far. The script exits with status 1 when at least one
circuit ended in `error`; `timeout` and `memory` are not treated as errors.

## How the limits work

The memory limit is enforced on the resident set size (`VmRSS`) of the probe
process, polled every 0.2 s. `ulimit -v` is not used because it limits the
virtual address space, which OpenMP runs exceed long before they use that much
resident memory. The sampling interval means the reported peak can slightly
exceed the configured limit.

The time limit uses the same poll loop with millisecond timestamps, so the
resolution is about 0.2 s.

## Measured behaviour

GHZ state, 1000 shots per basis:

| n | bases | iterations | fidelity | purity | time | peak RSS |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 50 | 50 | 0.926 | 0.865 | 10.5 s | 39 MB |
| 10 | 50 | 50 | 0.770 | 0.664 | 428 s | 141 MB |
| 11 | 20 | 10 | 0.421 | 0.475 | 152 s | 311 MB |
| 12 | 20 | 5 | 0.047 | 0.085 | 252 s | 1.08 GB |
| 13 | 5 | 1 | 0.008 | 0.025 | 92 s | 4.22 GB |

Real circuits from `/home/flareon/workshop/quantum_circuits`:

| circuit | n | bases | iterations | fidelity | time |
| --- | ---: | ---: | ---: | ---: | ---: |
| ae_alg_2 | 2 | 9 (all) | 100 | 0.992 | 2.4 s |
| adder_n4 | 4 | 81 (all) | 23 | 0.9999 | 14 s |
| ae_alg_6 | 6 | 50 | 100 | 0.948 | 3.6 s |
| ae_alg_8 | 8 | 100 | 100 | 0.982 | 58 s |
| adder_n10 | 10 | 50 | 32 | 0.992 | 230 s |

Fidelity depends on the state, not only on n: the maximally entangled GHZ
state spreads over all 2^n outcomes in a random basis, while an adder state
stays concentrated near a few basis states and converges much faster.

## Scale limits

The dense path needs four full `16 * 4^n` byte matrices, so the peak memory is
`64 * 4^n` bytes. n = 13 needs 4.2 GB, n = 14 needs 16 GB, n = 15 would need
64 GB. Time grows by about a factor of four per qubit. Practical values are
n <= 13; above that a different representation is required.

## Notes

- `--iterations` is an upper bound. Set it well above the number of iterations
the MLE actually needs; n = 8 needs about 100 to reach fidelity 0.988.
- `--random-bases` trades accuracy for time. A full 3^n basis gives the best
result; 50 random bases are a reasonable default for n >= 6.
