# QMDD and dense QST comparison

The comparison uses the `qst_qmdd` branch probe with both reconstruction
backends. Each run samples the same simulated measurement model with the same
seed, keeps the same number of outcomes per basis, and uses the same MLE
iteration limit.

Build the two experiment directories:

```bash
cmake --preset release -B build/matrix_qst_build -DCMAKE_UNITY_BUILD=OFF
cmake --build build/matrix_qst_build \
    --target mqt-core-qst-probe mqt-core-dd-test -j 4

cmake --preset release -B build/qmdd_qst_build -DCMAKE_UNITY_BUILD=OFF
cmake --build build/qmdd_qst_build \
    --target mqt-core-qst-probe mqt-core-dd-test -j 4
```

Both build directories are ignored by the existing `/build/` rule in
`.gitignore`.

Run one comparison:

```bash
./test/compare_qst_backends.py \
    --circuits /home/flareon/workshop/quantum_circuits/q13/ghz_alg_13.qasm \
    --probe build/qmdd_qst_build/bin/mqt-core-qst-probe \
    --output test/qst_qmdd_results/qst-qmdd-q13-ghz.csv \
    --shots 1 --random-bases 5 --iterations 1 --max-outcomes 1
```

## Results

| Circuit | n | bases/shots/iters | observed/retained | fidelity | trace distance | purity | dense recon | QMDD recon | QMDD initial/peak/final/R | dense wall | QMDD wall | dense RSS | QMDD RSS |
| --- | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |
| `ae_alg_6.qasm` | 6 | 20 / 100 / 5 | 671 / 317 | 0.443842 | 0.745760 | 0.358119 | 187.3 ms | 3222.8 ms | 940 / 1366 / 1366 / 940 | 0.41 s | 3.69 s | 25.0 MB | 61.3 MB |
| `ae_alg_8.qasm` | 8 | 5 / 1 / 1 | 5 / 5 | 0.028886 | 0.985451 | 0.239579 | 18.5 ms | 22.7 ms | 114 / 366 / 366 / 114 | 3.07 s | 3.28 s | 82.8 MB | 82.5 MB |
| `ghz_alg_12.qasm` | 12 | 5 / 1 / 1 | 5 / 5 | 0.004049 | 0.997974 | 0.201395 | 2134.0 ms | 57.5 ms | 271 / 546 / 546 / 271 | 2.70 s | 0.21 s | 1043.5 MB | 5.6 MB |
| `ghz_alg_13.qasm` | 13 | 5 / 1 / 1 | 5 / 5 | 0.004170 | 0.997913 | 0.201574 | 15264.2 ms | 528.9 ms | 273 / 4385 / 4385 / 274 | 17.28 s | 5.13 s | 4116.4 MB | 84.6 MB |

The fidelity values are identical because both backends use the same sampled
records and the same dilute MLE update. The low-shot rows test representation
cost; they are not full-tomography accuracy measurements. The `ae_alg_6` row
uses 16 retained outcomes per basis. The other rows use one retained outcome per
basis.

`trace distance` is `sqrt(max(0, 1 - fidelity))`. It is exact when both states
are pure and is an approximation for a mixed reconstruction.

The QMDD backend uses `U_b^dagger D_b U_b` to build each measurement-basis term
of `R`. It never constructs a dense density matrix. The 12- and 13-qubit GHZ
circuits keep the intermediate QMDD compact and show the intended memory and
time reduction. The `ae_alg_6` case shows the opposite regime: mixing 20 random
measurement bases produces a large intermediate QMDD, so the DD operations are
much slower than the BLAS path.

Raw comparison data is stored in the adjacent `qst-qmdd-*.csv` files. Each row
contains all dense and QMDD detail fields, including phase times, observed and
retained outcomes, trace and likelihood values, initial/final/peak DD sizes,
wall time, and peak RSS.
