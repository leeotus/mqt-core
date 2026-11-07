# MQT Core — QST experiment build and run notes

This branch adds a maximum-likelihood quantum state tomography (QST) module
(`include/mqt-core/dd/QST.hpp`, `src/dd/QST.cpp`) plus a command line probe. It
rebuilds an n-qubit density matrix from Pauli-basis measurement records with the
dilute MLE iteration `R = sum (f/p) Pi`, `rho <- R rho R / Tr(R rho R)`. The
default QMDD backend keeps `rho` and `R` as matrix decision diagrams. The dense
backend materializes full matrices and serves as the comparison baseline.

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

Separate comparison builds:

```bash
cmake --preset release -B build/matrix_qst_build -DCMAKE_UNITY_BUILD=OFF
cmake --build build/matrix_qst_build \
    --target mqt-core-qst-probe mqt-core-dd-test -j 8

cmake --preset release -B build/qmdd_qst_build -DCMAKE_UNITY_BUILD=OFF
cmake --build build/qmdd_qst_build \
    --target mqt-core-qst-probe mqt-core-dd-test -j 8
```

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
| `--qubits` | 2 | 1 to 24 |
| `--shots` | 2000 | shots per basis |
| `--random-bases` | 0 | number of random Pauli bases, 0 selects all 3^n |
| `--iterations` | 200 | maximum MLE iterations |
| `--max-outcomes` | 0 | retained outcomes per basis, 0 keeps every observed outcome |
| `--backend` | `qmdd` | `qmdd` or `dense` reconstruction |
| `--seed` | 42 | seed for the random state, the bases, and the sampling |

The probe prints one CSV line to stdout and `true_purity=` to stderr:

```text
state,backend,n,bases,shots,iterations,fidelity,purity,dd_size,time_ms,
converged,max_iterations,max_outcomes,true_purity,trace_distance,trace,
trace_error,log_likelihood,observed_outcomes,retained_outcomes,
initial_dd_size,peak_dd_size,peak_r_dd_size,density_ms,basis_ms,
sampling_ms,reconstruct_init_ms,reconstruct_iter_ms,metrics_ms,total_ms
ghz,qmdd,3,10,100,3,0.733216,0.602308,22,2.472,0,3,4,1.000000,
0.516511,1.000000,0.000000,-13.724498,60,40,22,22,22,0.018,0.006,
0.099,0.587,1.854,0.124,5.327
```

`trace_distance` uses `sqrt(max(0, 1 - fidelity))`. It is exact for two pure
states and is an approximation for a mixed reconstruction.

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
    --backend qmdd \
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
| `--max-outcomes` | 0 | retained outcomes per basis, 0 keeps all |
| `--backend` | `qmdd` | `qmdd` or `dense` reconstruction |
| `--seed` | 42 | sampling seed |
| `--limit` | 0 | only the first N circuits, 0 = all |
| `--full-path` | off | write the full circuit path to the CSV instead of the file name |
| `--probe` | `build/release/bin/mqt-core-qst-probe` | probe binary |
| `-h`, `--help` | — | print the built-in help |

The probe binary can also be set with `MQT_CORE_QST_PROBE`.

## CSV columns

```text
circuit,backend,n,bases,shots,iterations,fidelity,purity,time_ms,converged,
dd_size,max_iterations,max_outcomes,true_purity,trace_distance,trace,
trace_error,log_likelihood,observed_outcomes,retained_outcomes,
initial_dd_size,peak_dd_size,peak_r_dd_size,density_ms,basis_ms,sampling_ms,
reconstruct_init_ms,reconstruct_iter_ms,metrics_ms,total_ms,elapsed_s,
peak_rss_mb,status
```

The detailed fields have these meanings:

| Field | Meaning |
| --- | --- |
| `dd_size` | final QMDD node count; 0 for the dense backend |
| `initial_dd_size` | QMDD node count after linear-inversion initialization |
| `peak_dd_size` | largest QMDD density matrix during MLE |
| `peak_r_dd_size` | largest QMDD MLE update matrix `R` |
| `observed_outcomes` | distinct non-zero `(basis, outcome)` records |
| `retained_outcomes` | records actually used by the backend |
| `trace` | reconstructed density matrix trace |
| `trace_error` | `abs(trace - 1)` |
| `log_likelihood` | likelihood of the retained outcomes |
| `density_ms` | circuit-to-density-matrix time |
| `basis_ms` | measurement-basis generation time |
| `sampling_ms` | simulated measurement time |
| `reconstruct_init_ms` | reconstruction setup time |
| `reconstruct_iter_ms` | MLE iteration time |
| `metrics_ms` | fidelity, purity, and trace evaluation time |
| `total_ms` | probe time including input and setup |

The batch adds process-level `elapsed_s` and `peak_rss_mb`. The comparison
script writes the complete dense and QMDD metric sets side by side.

`status` is one of `ok`, `timeout`, `memory`, `error`. For every status other
than `ok`, probe-derived fields hold `-`; `elapsed_s` and `peak_rss_mb` still
report what was measured before the abort.

```text
circuit,backend,n,bases,shots,iterations,fidelity,purity,time_ms,converged,...
"ae_alg_2.qasm",qmdd,2,9,1000,50,0.991182,0.982939,1.282,0,...
"adder_n10.qasm",qmdd,-,-,-,-,-,-,-,-,...
```

The `circuit` column holds the file name only. Pass `--full-path` to record the
path as it was found instead.

Rows are appended as each circuit finishes, so an interrupted run keeps the
results collected so far. The script exits with status 1 when at least one
circuit ended in `error`; `timeout` and `memory` are not treated as errors.

Run both reconstruction backends on the same input with:

```bash
./test/compare_qst_backends.py \
    --circuits /home/flareon/workshop/quantum_circuits/q13/ghz_alg_13.qasm \
    --probe build/qmdd_qst_build/bin/mqt-core-qst-probe \
    --output test/qst_qmdd_results/qst-qmdd-q13-ghz.csv \
    --shots 1 --random-bases 5 --iterations 1 --max-outcomes 1
```

The QMDD/dense measurements are in
[`test/qst_qmdd_results/qst-qmdd.md`](test/qst_qmdd_results/qst-qmdd.md).

## How the limits work

The memory limit is enforced on the resident set size (`VmRSS`) of the probe
process, polled every 0.2 s. `ulimit -v` is not used because it limits the
virtual address space, which OpenMP runs exceed long before they use that much
resident memory. The sampling interval means the reported peak can slightly
exceed the configured limit.

The time limit uses the same poll loop with millisecond timestamps, so the
resolution is about 0.2 s.

## Dense baseline measurements

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

Fidelity depends on the state, not only on n: the maximally entangled GHZ state
spreads over all 2^n outcomes in a random basis, while an adder state stays
concentrated near a few basis states and converges much faster.

## Scale limits

The dense path needs four full `16 * 4^n` byte matrices, so the peak memory is
`64 * 4^n` bytes. n = 13 needs 4.1 GB, n = 14 needs 16.4 GB and completes in
about 37 s, n = 15 would need about 66 GB. Time grows by about a factor of four
per qubit. On a 32 GB budget the dense backend therefore reaches n = 14 and
stops below n = 15.

The QMDD backend has no fixed qubit limit from dense matrix storage. Its cost
depends on the number of QMDD nodes and on how strongly the measurements mix
bases. Structured 13-qubit states can stay compact. Random measurement bases can
make `R` dense in the matrix DD and make QMDD slower than BLAS.

The qubit limit of the QMDD backend therefore depends on the number of retained
measurement outcomes, not on `n`. With five retained outcomes it stayed below
150 MB up to n = 22, while the dense backend stops between n = 14 and n = 15.
With ten retained outcomes at n = 15 the reconstruction took 86 s. With about
fifty retained outcomes at n = 16 it did not finish within 5.4 min. The
measurements are in
[`test/qst_qmdd_results/qst-qmdd-scaling.md`](test/qst_qmdd_results/qst-qmdd-scaling.md).

## Notes

- `--iterations` is an upper bound. Set it well above the number of iterations
the MLE actually needs; n = 8 needs about 100 to reach fidelity 0.988.
- `--random-bases` trades accuracy for time. A full 3^n basis gives the best
result; 50 random bases are a reasonable default for n >= 6.
