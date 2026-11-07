# How far the QMDD backend scales beyond the dense limit

The dense backend stores four full `2^n x 2^n` complex matrices, so its peak
memory is `64 * 4^n` bytes: 4.1 GB at n = 13 and 16.4 GB at n = 14. A 32 GB
budget therefore stops the dense backend between n = 14 and n = 15. This note
measures where the QMDD backend stops instead.

Measurements use `build/release` on a 16-core machine with 54 GB of RAM and a
32 GB per-run budget. Every circuit is a GHZ circuit from
`/home/flareon/workshop/quantum_circuits`. Peak RSS comes from
`/usr/bin/time -v` or from the poll loop in `test/run_qst_batch.sh`.

## Memory at the dense limit

| n | backend | bases / shots / iterations | wall | peak RSS |
| ---: | --- | --- | ---: | ---: |
| 13 | dense | 5 / 1 / 1 | 17.3 s | 4.1 GB |
| 14 | dense | 1 / 1 / 1 | 36.6 s | 16.4 GB |
| 15 | dense | — | not run | about 66 GB, extrapolated |
| 13 | QMDD | 5 / 1 / 1 | 6.4 s, 0.6 s of it reconstruction | 87 MB |
| 14 | QMDD | 5 / 1 / 1 | 0.15 s to 40 s, see below | 29 MB to 153 MB |

The dense peak does not depend on the number of measurement records, because the
four flat matrices dominate. The QMDD peak does not depend on `n` in the same
way, because the density matrix stays a decision diagram.

## QMDD beyond the dense limit

Five random bases, one shot per basis, one MLE iteration, so five retained
outcomes:

| n | 16 | 17 | 18 | 20 | 22 |
| --- | ---: | ---: | ---: | ---: | ---: |
| reconstruction | 52 ms | 79 ms | 57 ms | 350 ms | 2.4 s |
| peak RSS | 27 MB | 31 MB | 30 MB | 49 MB | 141 MB |
| peak density DD size | 483 | 973 | 601 | 1200 | 971 |

The memory cost is two orders of magnitude below the dense backend at the same
`n`, and it stays below 150 MB up to n = 22.

## The cost follows the retained-outcome count, not `n`

n = 14, one shot per basis, one iteration, varying the number of bases:

| bases | retained outcomes | reconstruction | peak density DD size |
| ---: | ---: | ---: | ---: |
| 1 | 1 | 1.1 ms | 15 |
| 2 | 2 | 1.4 ms | 28 |
| 3 | 3 | 3.9 ms | 84 |
| 4 | 4 | 8.2 ms | 150 |
| 5 | 5 | 150 ms to more than 200 s | 490 to 10 700 |

The last row depends on the sampling seed. The same circuit with the same
settings, five random bases, one shot, one iteration, one seed per run:

| seed | 7 | 3 | 1 | 2 | 6 | 4 | 5 | 8 | 42 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| reconstruction | 0.15 s | 0.18 s | 0.83 s | 1.1 s | 4.7 s | 8.7 s | 21 s | 27 s | over 200 s |
| peak density DD size | 490 | 1001 | 504 | 701 | 1524 | 2961 | 4041 | 10700 | not reached |

The cost grows faster than the density-DD size grows. A density DD of 500 nodes
costs milliseconds; 10 700 nodes cost 27 s. The blow-up is in
`Package::multiply` for the MLE update `rho <- R rho R`, not in sampling or in
the initial linear inversion. The initial state costs about 2 ms and a few
hundred nodes at every `n` from 12 to 15.

Raising the retained-outcome count at a larger `n` shows the same wall:

| n | bases / shots | retained outcomes | reconstruction |
| ---: | --- | ---: | ---: |
| 16 | 5 / 1 | 5 | 52 ms |
| 16 | 5 / 10 | about 50 | over 5.4 min, aborted |
| 15 | 5 / 100, two kept per basis | 10 | 86 s |

The n = 15 row took 370 s of wall clock in total. The MLE iterations account for
86 s of that and the fidelity evaluation accounts for 284 s, because the probe
compares the reconstruction with the true state through another matrix product
on decision diagrams. The reconstruction itself kept 317 MB of peak RSS.

## Conclusion

The QMDD backend removes the dense memory wall. At n = 15 the dense backend
would need about 66 GB, while the QMDD backend stays at tens of megabytes, and
it keeps working up to n = 22 in this study. It does not remove the time wall.
The dilute MLE update multiplies the density matrix with the matrix `R`, and the
cost of that product grows much faster than linearly in the number of retained
outcomes. With five retained outcomes the QMDD backend is fast at every `n` that
was tested. With about fifty retained outcomes a single n = 16 reconstruction
exceeds 5.4 min, where five outcomes take 52 ms.

A real tomography run keeps hundreds to thousands of outcomes, because a random
Pauli basis spreads a GHZ state over most of the `2^n` outcomes. In that regime
the density matrix is far from low rank, the decision diagram is large, and the
dense BLAS path is faster; the `ae_alg_6` row in
[`qst-qmdd.md`](qst-qmdd.md) shows a factor of 17.

So the QMDD backend raises the qubit limit only where the reconstruction stays
low rank: few retained outcomes, few bases, or structured states. It does not
make multi-basis tomography of highly entangled states feasible above the dense
limit.

The seed dependence also means that any claim of the form "the QMDD backend
handles n qubits" must be repeated over several seeds. One seed at n = 14 took
0.15 s and another took more than 200 s with identical settings.
