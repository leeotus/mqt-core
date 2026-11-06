/*
 * Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
 * Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Licensed under the MIT License
 */

#include "dd/QST.hpp"

#include "dd/FunctionalityConstruction.hpp"
#include "dd/Package.hpp"
#include "ir/QuantumComputation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
#include <random>
#include <set>
#include <utility>

namespace dd {

#ifdef MQT_CORE_QST_USE_BLAS
extern "C" {
/// CBLAS entry point. Declared locally so that the build does not depend on a
/// CBLAS header being installed.
void cblas_zgemm(int layout, int transA, int transB, int m, int n, int k,
                 const void* alpha, const void* a, int lda, const void* b,
                 int ldb, const void* beta, void* c, int ldc);
}
#endif

namespace {

/// Multiply the root weight of an edge by a real factor.
mEdge scaled(Package& dd, mEdge edge, const double factor) {
  if (edge.p == nullptr) {
    return edge;
  }
  const auto real = RealNumber::val(edge.w.r) * factor;
  const auto imag = RealNumber::val(edge.w.i) * factor;
  edge.w = dd.cn.lookup(real, imag);
  return edge;
}

/// Return the real part of the trace.
/// @details The trace is accumulated from the non-zero matrix entries, so the
/// computation follows the DD paths instead of materializing the full matrix.
double traceReal(const mEdge& edge, const std::size_t nQubits) {
  double value = 0.0;
  edge.traverseMatrix(
      std::complex<fp>{1.0, 0.0}, 0U, 0U,
      [&value](const std::size_t i, const std::size_t j,
               const std::complex<fp>& amplitude) {
        if (i == j) {
          value += amplitude.real();
        }
      },
      nQubits);
  return value;
}

/// Return 3^n, capped so that callers can bound the work.
std::size_t pauliBasisCount(const std::size_t nQubits) {
  std::size_t count = 1U;
  for (std::size_t i = 0U; i < nQubits; ++i) {
    if (count > (std::numeric_limits<std::size_t>::max() / 3U)) {
      return std::numeric_limits<std::size_t>::max();
    }
    count *= 3U;
  }
  return count;
}

/// Return the state vector of one projector. Every Pauli projector is a
/// rank-one matrix |v><v|, so storing |v> instead of the full matrix
/// shrinks the memory by a factor of 4^n.
CVec projectorVector(const std::size_t nQubits,
                     const QST::MeasurementBasis& basis,
                     const std::vector<int>& bits) {
  constexpr double invSqrt2 = 0.70710678118654752440;
  CVec result(1U, {1.0, 0.0});
  for (std::size_t qubit = 0U; qubit < nQubits; ++qubit) {
    CVec single(2U, {0.0, 0.0});
    switch (basis.bases[qubit]) {
    case QST::Basis::Z:
      single[static_cast<std::size_t>(bits[qubit])] = 1.0;
      break;
    case QST::Basis::X:
      single[0] = invSqrt2;
      single[1] = bits[qubit] == 0 ? invSqrt2 : -invSqrt2;
      break;
    case QST::Basis::Y:
      single[0] = invSqrt2;
      single[1] = bits[qubit] == 0 ? CVec::value_type{0.0, invSqrt2}
                                   : CVec::value_type{0.0, -invSqrt2};
      break;
    }
    // Qubit 0 must end up in the least significant bit so that the index
    // matches the outcome encoding used by sample(). Each new qubit takes the
    // most significant position, so the last qubit ends up on top.
    CVec product(2U * result.size(), {0.0, 0.0});
    for (std::size_t i = 0U; i < result.size(); ++i) {
      product[i] = result[i] * single[0];
      product[result.size() + i] = result[i] * single[1];
    }
    result = std::move(product);
  }
  return result;
}

/// A dense matrix stored as interleaved real and imaginary parts.
using FlatMatrix = std::vector<fp>;

/// Convert a flat matrix back to the complex layout.
CMat toCMat(const FlatMatrix& flat, const std::size_t dimension) {
  CMat matrix(dimension, CVec(dimension, {0.0, 0.0}));
  for (std::size_t row = 0U; row < dimension; ++row) {
    for (std::size_t col = 0U; col < dimension; ++col) {
      const auto index = 2U * (row * dimension + col);
      matrix[row][col] = {flat[index], flat[index + 1U]};
    }
  }
  return matrix;
}

/// Multiply two flat matrices.
FlatMatrix flatMultiply(const FlatMatrix& left, const FlatMatrix& right,
                        const std::size_t dimension) {
  FlatMatrix product(2U * dimension * dimension, 0.0);
#if defined(MQT_CORE_QST_USE_BLAS) && !defined(MQT_CORE_QST_USE_OPENMP)
  // The interleaved real/imaginary layout matches std::complex<fp>.
  if (dimension >= 64U) {
    const std::complex<fp> alpha{1.0, 0.0};
    const std::complex<fp> beta{0.0, 0.0};
    const auto size = static_cast<int>(dimension);
    // CblasRowMajor = 101, CblasNoTrans = 111.
    cblas_zgemm(101, 111, 111, size, size, size, &alpha, left.data(), size,
                right.data(), size, &beta, product.data(), size);
    return product;
  }
#endif
#ifdef MQT_CORE_QST_USE_OPENMP
#pragma omp parallel for schedule(static) if (dimension >= 64U)
#endif
  for (std::size_t row = 0U; row < dimension; ++row) {
    for (std::size_t inner = 0U; inner < dimension; ++inner) {
      const auto leftIndex = 2U * (row * dimension + inner);
      const auto leftReal = left[leftIndex];
      const auto leftImag = left[leftIndex + 1U];
      if (leftReal == 0.0 && leftImag == 0.0) {
        continue;
      }
      for (std::size_t col = 0U; col < dimension; ++col) {
        const auto rightIndex = 2U * (inner * dimension + col);
        const auto rightReal = right[rightIndex];
        const auto rightImag = right[rightIndex + 1U];
        if (rightReal == 0.0 && rightImag == 0.0) {
          continue;
        }
        const auto productIndex = 2U * (row * dimension + col);
        product[productIndex] += leftReal * rightReal - leftImag * rightImag;
        product[productIndex + 1U] +=
            leftReal * rightImag + leftImag * rightReal;
      }
    }
  }
  return product;
}

/// Return the real part of the trace of a flat matrix.
double flatTrace(const FlatMatrix& matrix, const std::size_t dimension) {
  double value = 0.0;
  for (std::size_t i = 0U; i < dimension; ++i) {
    value += matrix[2U * (i * dimension + i)];
  }
  return value;
}

/// Add weight * |v><v| to a flat matrix.
void addOuterProduct(FlatMatrix& target, const CVec& vector,
                     const double weight, const std::size_t dimension) {
#ifdef MQT_CORE_QST_USE_OPENMP
#pragma omp parallel for schedule(static) if (dimension >= 64U)
#endif
  for (std::size_t row = 0U; row < dimension; ++row) {
    const auto realRow = vector[row].real();
    const auto imagRow = vector[row].imag();
    for (std::size_t col = 0U; col < dimension; ++col) {
      const auto realCol = vector[col].real();
      const auto imagCol = vector[col].imag();
      const auto index = 2U * (row * dimension + col);
      target[index] += weight * (realRow * realCol + imagRow * imagCol);
      target[index + 1U] += weight * (imagRow * realCol - realRow * imagCol);
    }
  }
}

/// Return the real part of <v|matrix|v>.
double expectationValue(const FlatMatrix& matrix, const CVec& vector,
                        const std::size_t dimension) {
  double value = 0.0;
#ifdef MQT_CORE_QST_USE_OPENMP
#pragma omp parallel for schedule(static) reduction(+ : value) if (dimension >= 64U)
#endif
  for (std::size_t row = 0U; row < dimension; ++row) {
    fp realPart = 0.0;
    fp imagPart = 0.0;
    for (std::size_t col = 0U; col < dimension; ++col) {
      const auto index = 2U * (row * dimension + col);
      const auto realEntry = matrix[index];
      const auto imagEntry = matrix[index + 1U];
      const auto realVector = vector[col].real();
      const auto imagVector = vector[col].imag();
      realPart += realEntry * realVector - imagEntry * imagVector;
      imagPart += realEntry * imagVector + imagEntry * realVector;
    }
    value += vector[row].real() * realPart + vector[row].imag() * imagPart;
  }
  return value;
}

} // namespace

// ============================================================================
// Measurement bases
// ============================================================================

std::vector<QST::MeasurementBasis>
QST::allPauliBases(const std::size_t nQubits) {
  const auto total = pauliBasisCount(nQubits);
  std::vector<MeasurementBasis> bases;
  if (total == std::numeric_limits<std::size_t>::max()) {
    return bases;
  }
  bases.reserve(total);
  for (std::size_t index = 0U; index < total; ++index) {
    MeasurementBasis basis;
    basis.bases.resize(nQubits);
    auto remaining = index;
    for (std::size_t qubit = 0U; qubit < nQubits; ++qubit) {
      basis.bases[qubit] = static_cast<Basis>(remaining % 3U);
      remaining /= 3U;
    }
    bases.push_back(std::move(basis));
  }
  return bases;
}

std::vector<QST::MeasurementBasis>
QST::randomPauliBases(const std::size_t nQubits, const std::size_t count,
                      const std::size_t seed) {
  const auto total = pauliBasisCount(nQubits);
  const auto target = std::min(count, total);
  std::vector<MeasurementBasis> bases;
  if (target == 0U) {
    return bases;
  }
  if (target == total) {
    return allPauliBases(nQubits);
  }

  std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
  std::uniform_int_distribution<int> dist(0, 2);
  std::set<MeasurementBasis> unique;
  while (unique.size() < target) {
    MeasurementBasis basis;
    basis.bases.resize(nQubits);
    for (std::size_t qubit = 0U; qubit < nQubits; ++qubit) {
      basis.bases[qubit] = static_cast<Basis>(dist(rng));
    }
    unique.insert(std::move(basis));
  }
  bases.assign(unique.begin(), unique.end());
  return bases;
}

// ============================================================================
// Projectors and outcome probabilities
// ============================================================================

mEdge QST::singleQubitProjector(Package& dd, const std::size_t qubit,
                                const Basis basis, const int outcome) {
  GateMatrix matrix{};
  const std::complex<fp> half{0.5, 0.0};
  const std::complex<fp> minusHalf{-0.5, 0.0};
  const std::complex<fp> iHalf{0.0, 0.5};
  const std::complex<fp> minusIHalf{0.0, -0.5};

  switch (basis) {
  case Basis::Z:
    matrix[outcome == 0 ? 0U : 3U] = {1.0, 0.0};
    break;
  case Basis::X:
    matrix = {half, outcome == 0 ? half : minusHalf,
              outcome == 0 ? half : minusHalf, half};
    break;
  case Basis::Y:
    matrix = {half, outcome == 0 ? minusIHalf : iHalf,
              outcome == 0 ? iHalf : minusIHalf, half};
    break;
  }
  return dd.makeGateDD(matrix, static_cast<qc::Qubit>(qubit));
}

mEdge QST::projector(Package& dd, const std::size_t nQubits,
                     const MeasurementBasis& basis,
                     const std::vector<int>& bits) {
  auto result = dd.createInitialMatrix({});
  dd.incRef(result);
  for (std::size_t qubit = 0U; qubit < nQubits; ++qubit) {
    auto single =
        singleQubitProjector(dd, qubit, basis.bases[qubit], bits[qubit]);
    dd.incRef(single);
    auto product = dd.multiply(result, single);
    dd.incRef(product);
    dd.decRef(result);
    dd.decRef(single);
    result = product;
  }
  return result;
}

mEdge QST::basisChange(Package& dd, const std::size_t qubit,
                       const Basis basis) {
  GateMatrix matrix{};
  const double scale = 1.0 / std::sqrt(2.0);
  switch (basis) {
  case Basis::Z:
    matrix = {1.0, 0.0, 0.0, 1.0};
    break;
  case Basis::X:
    matrix = {scale, scale, scale, -scale};
    break;
  case Basis::Y:
    matrix = {scale, {0.0, -scale}, scale, {0.0, scale}};
    break;
  }
  return dd.makeGateDD(matrix, static_cast<qc::Qubit>(qubit));
}

std::vector<double> QST::basisProbabilities(Package& dd, const mEdge& rho,
                                            const MeasurementBasis& basis,
                                            const std::size_t nQubits) {
  (void)dd;
  auto matrix = rho.getMatrix(nQubits);
  const std::size_t dimension = 1ULL << nQubits;
  const double scale = 1.0 / std::sqrt(2.0);

  for (std::size_t qubit = 0U; qubit < nQubits; ++qubit) {
    if (basis.bases[qubit] == Basis::Z) {
      continue;
    }
    std::array<std::complex<fp>, 4> unitary{};
    if (basis.bases[qubit] == Basis::X) {
      unitary = {scale, scale, scale, -scale};
    } else {
      unitary = {scale, {0.0, -scale}, scale, {0.0, scale}};
    }

    const std::size_t mask = 1ULL << qubit;
    for (std::size_t row = 0U; row < dimension; ++row) {
      if ((row & mask) != 0U) {
        continue;
      }
      const auto other = row | mask;
      for (std::size_t col = 0U; col < dimension; ++col) {
        const auto a = matrix[row][col];
        const auto b = matrix[other][col];
        matrix[row][col] = unitary[0] * a + unitary[1] * b;
        matrix[other][col] = unitary[2] * a + unitary[3] * b;
      }
    }

    for (std::size_t col = 0U; col < dimension; ++col) {
      if ((col & mask) != 0U) {
        continue;
      }
      const auto other = col | mask;
      for (std::size_t row = 0U; row < dimension; ++row) {
        const auto a = matrix[row][col];
        const auto b = matrix[row][other];
        matrix[row][col] =
            a * std::conj(unitary[0]) + b * std::conj(unitary[1]);
        matrix[row][other] =
            a * std::conj(unitary[2]) + b * std::conj(unitary[3]);
      }
    }
  }

  std::vector<double> probabilities(dimension, 0.0);
  for (std::size_t i = 0U; i < dimension; ++i) {
    probabilities[i] = matrix[i][i].real();
  }
  return probabilities;
}

double QST::probability(Package& dd, const mEdge& rho,
                        const MeasurementBasis& basis,
                        const std::vector<int>& bits) {
  auto proj = projector(dd, basis.bases.size(), basis, bits);
  auto product = dd.multiply(rho, proj);
  double value = 0.0;
  if (product.p != nullptr) {
    dd.incRef(product);
    value = traceReal(product, basis.bases.size());
    dd.decRef(product);
  }
  dd.decRef(proj);
  dd.garbageCollect();
  return std::max(0.0, value);
}

// ============================================================================
// Sampling and density matrices
// ============================================================================

std::vector<QST::MeasurementRecord>
QST::sample(Package& dd, const mEdge& rho,
            const std::vector<MeasurementBasis>& bases,
            const std::size_t shotsPerBasis, const std::size_t seed) {
  std::vector<MeasurementRecord> records;
  if (bases.empty()) {
    return records;
  }
  const auto nQubits = bases.front().bases.size();
  const std::size_t outcomes = 1ULL << nQubits;
  // The bulk path needs one 2^n by 2^n matrix (16 * 4^n bytes). At n = 14 that
  // is 4 GB, which still fits the memory budget of the dense MLE.
  const bool useBulk = nQubits <= 15U;
  std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
  std::uniform_real_distribution<double> uniform(0.0, 1.0);

  for (std::size_t basisIndex = 0U; basisIndex < bases.size(); ++basisIndex) {
    std::vector<double> probabilities;
    if (useBulk) {
      probabilities = basisProbabilities(dd, rho, bases[basisIndex], nQubits);
    } else {
      probabilities.assign(outcomes, 0.0);
      for (std::size_t outcome = 0U; outcome < outcomes; ++outcome) {
        std::vector<int> bits(nQubits, 0);
        for (std::size_t qubit = 0U; qubit < nQubits; ++qubit) {
          bits[qubit] = static_cast<int>((outcome >> qubit) & 1ULL);
        }
        probabilities[outcome] = probability(dd, rho, bases[basisIndex], bits);
      }
    }
    double total = 0.0;
    for (const auto value : probabilities) {
      total += value;
    }
    if (total <= 0.0) {
      continue;
    }
    for (auto& value : probabilities) {
      value /= total;
    }

    std::vector<std::size_t> counts(outcomes, 0U);
    for (std::size_t shot = 0U; shot < shotsPerBasis; ++shot) {
      const double draw = uniform(rng);
      double cumulative = 0.0;
      std::size_t chosen = outcomes - 1U;
      for (std::size_t outcome = 0U; outcome < outcomes; ++outcome) {
        cumulative += probabilities[outcome];
        if (draw <= cumulative) {
          chosen = outcome;
          break;
        }
      }
      ++counts[chosen];
    }
    for (std::size_t outcome = 0U; outcome < outcomes; ++outcome) {
      if (counts[outcome] == 0U) {
        continue;
      }
      std::vector<int> bits(nQubits, 0);
      for (std::size_t qubit = 0U; qubit < nQubits; ++qubit) {
        bits[qubit] = static_cast<int>((outcome >> qubit) & 1ULL);
      }
      records.push_back({basisIndex, std::move(bits), counts[outcome]});
    }
  }
  return records;
}

mEdge QST::densityMatrix(Package& dd, qc::QuantumComputation& circuit) {
  const auto nQubits = circuit.getNqubits();
  if (nQubits == 0U) {
    return dd.createInitialMatrix({});
  }

  auto unitary = buildFunctionality(circuit, dd);
  dd.incRef(unitary);

  // rho0 = |0...0><0...0| as a product of single-qubit projectors.
  auto rho0 = dd.createInitialMatrix({});
  dd.incRef(rho0);
  for (std::size_t qubit = 0U; qubit < nQubits; ++qubit) {
    GateMatrix matrix{};
    matrix[0] = {1.0, 0.0};
    auto proj = dd.makeGateDD(matrix, static_cast<qc::Qubit>(qubit));
    dd.incRef(proj);
    auto product = dd.multiply(rho0, proj);
    dd.incRef(product);
    dd.decRef(rho0);
    dd.decRef(proj);
    rho0 = product;
  }

  auto dagger = dd.conjugateTranspose(unitary);
  dd.incRef(dagger);
  auto left = dd.multiply(unitary, rho0);
  dd.incRef(left);
  auto rho = dd.multiply(left, dagger);
  dd.incRef(rho);

  dd.decRef(left);
  dd.decRef(dagger);
  dd.decRef(rho0);
  dd.decRef(unitary);
  dd.garbageCollect();
  return rho;
}

double QST::overlap(Package& dd, const mEdge& first, const mEdge& second,
                    const std::size_t nQubits) {
  auto product = dd.multiply(first, second);
  dd.incRef(product);
  const auto value = traceReal(product, nQubits);
  dd.decRef(product);
  dd.garbageCollect();
  return value;
}

double QST::purity(Package& dd, const mEdge& rho, const std::size_t nQubits) {
  return overlap(dd, rho, rho, nQubits);
}

mEdge QST::maximallyMixedState(Package& dd, const std::size_t nQubits) {
  auto mixed = dd.createInitialMatrix({});
  dd.incRef(mixed);
  for (std::size_t qubit = 0U; qubit < nQubits; ++qubit) {
    GateMatrix half{};
    half[0] = {0.5, 0.0};
    half[3] = {0.5, 0.0};
    auto gate = dd.makeGateDD(half, static_cast<qc::Qubit>(qubit));
    dd.incRef(gate);
    auto product = dd.multiply(mixed, gate);
    dd.incRef(product);
    dd.decRef(mixed);
    dd.decRef(gate);
    mixed = product;
  }
  return mixed;
}

// ============================================================================
// Dilute MLE
// ============================================================================

QST::Result QST::reconstructMatrix(
    Package& dd, const std::vector<MeasurementRecord>& records,
    const std::vector<MeasurementBasis>& bases, const Config& config) {
  (void)dd;
  Result result;
  const auto start = std::chrono::steady_clock::now();
  const auto nQubits = bases.front().bases.size();
  const std::size_t dimension = 1ULL << nQubits;

  std::vector<std::size_t> shotsPerBasis(bases.size(), 0U);
  for (const auto& record : records) {
    if (record.basisIndex < bases.size()) {
      shotsPerBasis[record.basisIndex] += record.count;
    }
  }

  std::vector<std::vector<const MeasurementRecord*>> perBasis(bases.size());
  for (const auto& record : records) {
    if (record.basisIndex < bases.size()) {
      perBasis[record.basisIndex].push_back(&record);
    }
  }

  // Each projector |v><v| is stored as the vector |v>. The accumulation of R
  // costs 4^n operations per entry, so the entry count follows a work budget.
  constexpr std::size_t WORK_BUDGET = 4000000000ULL;
  const std::size_t maxEntries =
      std::min<std::size_t>(20000U, WORK_BUDGET / (dimension * dimension));
  const std::size_t perBasisLimit =
      std::max<std::size_t>(2U, maxEntries / bases.size());

  std::vector<CVec> projectors;
  std::vector<double> frequencies;
  for (auto& group : perBasis) {
    std::ranges::sort(group, [](const auto* a, const auto* b) {
      return a->count > b->count;
    });
    const auto limit = std::min(perBasisLimit, group.size());
    for (std::size_t index = 0U; index < limit; ++index) {
      const auto* record = group[index];
      const auto shots = shotsPerBasis[record->basisIndex];
      if (shots == 0U) {
        continue;
      }
      projectors.push_back(
          projectorVector(nQubits, bases[record->basisIndex], record->bits));
      frequencies.push_back(static_cast<double>(record->count) /
                            static_cast<double>(shots));
    }
  }
  if (projectors.empty()) {
    return result;
  }

  // Linear inversion initial value: rho = sum f_k |v_k><v_k| / Tr(...).
  FlatMatrix rho(2U * dimension * dimension, 0.0);
  for (std::size_t index = 0U; index < projectors.size(); ++index) {
    addOuterProduct(rho, projectors[index], frequencies[index], dimension);
  }
  const double initialTrace = flatTrace(rho, dimension);
  if (initialTrace > 0.0) {
    for (auto& value : rho) {
      value /= initialTrace;
    }
  } else {
    for (std::size_t i = 0U; i < dimension; ++i) {
      rho[2U * (i * dimension + i)] = 1.0 / static_cast<double>(dimension);
    }
  }

  double previousLogL = -std::numeric_limits<double>::infinity();
  for (std::size_t iter = 0U; iter < config.maxIterations; ++iter) {
    FlatMatrix R(2U * dimension * dimension, 0.0);
    double logL = 0.0;
    for (std::size_t index = 0U; index < projectors.size(); ++index) {
      const auto expected = expectationValue(rho, projectors[index], dimension);
      const double p = std::max(1e-12, expected);
      addOuterProduct(R, projectors[index], frequencies[index] / p, dimension);
      logL += frequencies[index] * std::log(p);
    }

    const auto rhoR = flatMultiply(rho, R, dimension);
    auto candidate = flatMultiply(R, rhoR, dimension);
    const double trace = flatTrace(candidate, dimension);
    if (!std::isfinite(trace) || trace <= 0.0) {
      break;
    }
    for (auto& value : candidate) {
      value /= trace;
    }
    rho = std::move(candidate);

    result.iterations = iter + 1U;
    result.logLikelihood = logL;
    const auto change = std::abs(logL - previousLogL);
    if (iter > 0U &&
        change < config.tolerance * std::max(1.0, std::abs(logL))) {
      result.converged = true;
      break;
    }
    previousLogL = logL;
  }

  result.rhoMatrix = toCMat(rho, dimension);
  const auto stop = std::chrono::steady_clock::now();
  result.elapsedMs =
      std::chrono::duration<double, std::milli>(stop - start).count();
  return result;
}

QST::Result QST::reconstruct(Package& dd,
                             const std::vector<MeasurementRecord>& records,
                             const std::vector<MeasurementBasis>& bases,
                             const Config& config) {
  Result result;
  const auto start = std::chrono::steady_clock::now();
  if (bases.empty() || records.empty()) {
    return result;
  }
  const auto nQubits = bases.front().bases.size();
  if (nQubits == 0U) {
    return result;
  }
  if (nQubits <= 13U) {
    return reconstructMatrix(dd, records, bases, config);
  }

  std::vector<std::size_t> shotsPerBasis(bases.size(), 0U);
  for (const auto& record : records) {
    if (record.basisIndex < bases.size()) {
      shotsPerBasis[record.basisIndex] += record.count;
    }
  }

  std::vector<std::vector<const MeasurementRecord*>> perBasis(bases.size());
  for (const auto& record : records) {
    if (record.basisIndex < bases.size()) {
      perBasis[record.basisIndex].push_back(&record);
    }
  }

  // Keep only the most frequent outcomes per basis. Every entry costs one DD
  // multiplication per iteration, so an unbounded entry list does not scale.
  constexpr std::size_t MAX_ENTRIES_PER_BASIS = 32U;
  struct Entry {
    mEdge projector;
    double frequency;
  };
  std::vector<Entry> entries;
  for (auto& group : perBasis) {
    std::ranges::sort(group, [](const auto* a, const auto* b) {
      return a->count > b->count;
    });
    const auto limit = std::min(MAX_ENTRIES_PER_BASIS, group.size());
    for (std::size_t index = 0U; index < limit; ++index) {
      const auto* record = group[index];
      const auto shots = shotsPerBasis[record->basisIndex];
      if (shots == 0U) {
        continue;
      }
      auto proj =
          projector(dd, nQubits, bases[record->basisIndex], record->bits);
      dd.incRef(proj);
      entries.push_back({proj, static_cast<double>(record->count) /
                                   static_cast<double>(shots)});
    }
  }
  if (entries.empty()) {
    return result;
  }

  // Linear inversion initial value: rho = (1 / #bases) sum f_k Pi_k.
  mEdge rho{};
  for (const auto& entry : entries) {
    auto term = scaled(dd, entry.projector, entry.frequency);
    dd.incRef(term);
    if (rho.p == nullptr) {
      rho = term;
    } else {
      auto sum = dd.add(rho, term);
      dd.incRef(sum);
      dd.decRef(rho);
      dd.decRef(term);
      rho = sum;
    }
  }
  auto initial = scaled(dd, rho, 1.0 / static_cast<double>(bases.size()));
  dd.incRef(initial);
  dd.decRef(rho);
  rho = initial;
  double previousLogL = -std::numeric_limits<double>::infinity();

  for (std::size_t iter = 0U; iter < config.maxIterations; ++iter) {
    mEdge R{};
    double logL = 0.0;
    for (const auto& entry : entries) {
      auto product = dd.multiply(rho, entry.projector);
      double probabilityValue = 0.0;
      if (product.p != nullptr) {
        dd.incRef(product);
        probabilityValue = traceReal(product, nQubits);
        dd.decRef(product);
      }
      const double p = std::max(1e-12, probabilityValue);

      double weight = entry.frequency / p;
      if (!std::isfinite(weight)) {
        weight = 1e12;
      }
      weight = std::min(weight, 1e12);

      auto term = scaled(dd, entry.projector, weight);
      dd.incRef(term);
      if (R.p == nullptr) {
        R = term;
      } else {
        auto sum = dd.add(R, term);
        dd.incRef(sum);
        dd.decRef(R);
        dd.decRef(term);
        R = sum;
      }
      logL += entry.frequency * std::log(p);
    }
    dd.garbageCollect();

    auto rhoR = dd.multiply(rho, R);
    dd.incRef(rhoR);
    auto candidate = dd.multiply(R, rhoR);
    dd.incRef(candidate);
    dd.decRef(rhoR);

    const double trace = traceReal(candidate, nQubits);
    if (!std::isfinite(trace) || trace <= 0.0) {
      dd.decRef(candidate);
      dd.decRef(R);
      dd.garbageCollect();
      break;
    }

    auto normalized = scaled(dd, candidate, 1.0 / trace);
    dd.incRef(normalized);
    dd.decRef(candidate);
    dd.decRef(rho);
    dd.decRef(R);
    rho = normalized;
    dd.garbageCollect();

    result.iterations = iter + 1U;
    result.logLikelihood = logL;
    const auto change = std::abs(logL - previousLogL);
    if (iter > 0U &&
        change < config.tolerance * std::max(1.0, std::abs(logL))) {
      result.converged = true;
      break;
    }
    previousLogL = logL;
  }

  for (auto& entry : entries) {
    dd.decRef(entry.projector);
  }
  dd.garbageCollect();

  result.rho = rho;
  result.ddSize = rho.size();
  const auto stop = std::chrono::steady_clock::now();
  result.elapsedMs =
      std::chrono::duration<double, std::milli>(stop - start).count();
  return result;
}

} // namespace dd
