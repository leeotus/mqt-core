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
#include "dd/StateGeneration.hpp"
#include "ir/QuantumComputation.hpp"

#include <algorithm>
#include <array>
#include <bit>
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

/// Visit the diagonal entries of a matrix DD without visiting off-diagonal
/// entries.
template <typename Callback>
void traverseDiagonal(const mEdge& edge, const std::complex<fp>& amplitude,
                      const std::size_t index, const std::size_t level,
                      Callback&& callback) {
  const auto value = amplitude * static_cast<std::complex<fp>>(edge.w);
  if (std::abs(value) == 0.0) {
    return;
  }
  if (level == 0U) {
    callback(index, value);
    return;
  }

  const auto nextLevel = static_cast<Qubit>(level - 1U);
  if (edge.isTerminal() || edge.p->v < nextLevel) {
    traverseDiagonal(edge, amplitude, index, nextLevel, callback);
    traverseDiagonal(edge, amplitude, index | (1ULL << nextLevel), nextLevel,
                     callback);
    return;
  }

  const auto upperIndex = index | (1ULL << nextLevel);
  traverseDiagonal(edge.p->e[0], value, index, nextLevel, callback);
  traverseDiagonal(edge.p->e[3], value, upperIndex, nextLevel, callback);
}

/// Return the real part of the trace without materializing the matrix.
double traceReal(const mEdge& edge, const std::size_t nQubits) {
  double value = 0.0;
  traverseDiagonal(edge, {1.0, 0.0}, 0U, nQubits,
                   [&value](const std::size_t, const std::complex<fp>& entry) {
                     value += entry.real();
                   });
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

/// Return |v> as a vector DD for the rank-one projector |v><v|.
vEdge projectorVectorDD(Package& dd, const std::size_t nQubits,
                        const QST::MeasurementBasis& basis,
                        const std::vector<int>& bits) {
  std::vector<BasisStates> state(nQubits, BasisStates::zero);
  for (std::size_t qubit = 0U; qubit < nQubits; ++qubit) {
    const bool one = bits[qubit] != 0;
    switch (basis.bases[qubit]) {
    case QST::Basis::Z:
      state[qubit] = one ? BasisStates::one : BasisStates::zero;
      break;
    case QST::Basis::X:
      state[qubit] = one ? BasisStates::minus : BasisStates::plus;
      break;
    case QST::Basis::Y:
      state[qubit] = one ? BasisStates::left : BasisStates::right;
      break;
    }
  }
  return makeBasisState(nQubits, state, dd);
}

/// Return <v|rho|v> without materializing rho.
double expectationFromVector(Package& dd, const mEdge& rho,
                             const vEdge& state) {
  auto product = dd.multiply(rho, state);
  dd.incRef(product);
  const auto value = dd.innerProduct(state, product).r;
  dd.decRef(product);
  return std::max(0.0, value);
}

mCachedEdge makeDiagonalDDRec(Package& dd, const std::vector<double>& diagonal,
                              const std::size_t offset,
                              const std::size_t level) {
  if (level == 0U) {
    return dd.makeDDNode<mNode, CachedEdge>(
        0U, {mCachedEdge::terminal(dd.cn.lookup(diagonal[offset])),
             mCachedEdge::zero(), mCachedEdge::zero(),
             mCachedEdge::terminal(dd.cn.lookup(diagonal[offset + 1U]))});
  }
  const auto half = 1ULL << level;
  const auto lower = static_cast<Qubit>(level);
  return dd.makeDDNode<mNode, CachedEdge>(
      lower, {makeDiagonalDDRec(dd, diagonal, offset, level - 1U),
              mCachedEdge::zero(), mCachedEdge::zero(),
              makeDiagonalDDRec(dd, diagonal, offset + half, level - 1U)});
}

/// Build a diagonal matrix DD from a dense diagonal. The input has 2^n
/// entries; the resulting matrix has only the same number of leaves.
mEdge makeDiagonalDD(Package& dd, const std::vector<double>& diagonal) {
  if (diagonal.empty()) {
    return mEdge::one();
  }
  if (diagonal.size() == 1U) {
    return mEdge::terminal(dd.cn.lookup(diagonal.front()));
  }
  const auto level = std::bit_width(diagonal.size()) - 1U;
  const auto root = makeDiagonalDDRec(dd, diagonal, 0U, level - 1U);
  return {.p = root.p, .w = dd.cn.lookup(root.w)};
}

/// Return diag(U rho U^dagger) without materializing the rotated matrix.
std::vector<double> probabilitiesFromRotation(Package& dd, const mEdge& rho,
                                              const mEdge& operation,
                                              const std::size_t nQubits) {
  auto dagger = dd.conjugateTranspose(operation);
  dd.incRef(dagger);
  auto left = dd.multiply(operation, rho);
  dd.incRef(left);
  auto rotated = dd.multiply(left, dagger);
  dd.incRef(rotated);
  dd.decRef(left);
  dd.decRef(dagger);

  const std::size_t dimension = 1ULL << nQubits;
  std::vector<double> probabilities(dimension, 0.0);
  traverseDiagonal(rotated, {1.0, 0.0}, 0U, nQubits,
                   [&probabilities](const std::size_t index,
                                    const std::complex<fp>& amplitude) {
                     probabilities[index] += std::max(0.0, amplitude.real());
                   });
  dd.decRef(rotated);
  dd.garbageCollect();
  return probabilities;
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
#pragma omp parallel for schedule(static)                                      \
    reduction(+ : value) if (dimension >= 64U)
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

mEdge QST::basisChange(Package& dd, const std::size_t nQubits,
                       const MeasurementBasis& basis) {
  auto result = dd.createInitialMatrix({});
  dd.incRef(result);
  for (std::size_t qubit = 0U; qubit < nQubits; ++qubit) {
    if (basis.bases[qubit] == Basis::Z) {
      continue;
    }
    auto local = basisChange(dd, qubit, basis.bases[qubit]);
    dd.incRef(local);
    auto product = dd.multiply(result, local);
    dd.incRef(product);
    dd.decRef(result);
    dd.decRef(local);
    result = product;
  }
  dd.decRef(result);
  return result;
}

std::vector<double> QST::basisProbabilities(Package& dd, const mEdge& rho,
                                            const MeasurementBasis& basis,
                                            const std::size_t nQubits) {
  auto operation = basisChange(dd, nQubits, basis);
  dd.incRef(operation);
  auto probabilities = probabilitiesFromRotation(dd, rho, operation, nQubits);
  dd.decRef(operation);
  dd.garbageCollect();
  return probabilities;
}

double QST::probability(Package& dd, const mEdge& rho,
                        const MeasurementBasis& basis,
                        const std::vector<int>& bits) {
  auto state = projectorVectorDD(dd, basis.bases.size(), basis, bits);
  const auto value = expectationFromVector(dd, rho, state);
  dd.decRef(state);
  dd.garbageCollect();
  return value;
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
  std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
  std::uniform_real_distribution<double> uniform(0.0, 1.0);

  for (std::size_t basisIndex = 0U; basisIndex < bases.size(); ++basisIndex) {
    auto probabilities =
        basisProbabilities(dd, rho, bases[basisIndex], nQubits);
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
  result.backend = Backend::Dense;
  const auto start = std::chrono::steady_clock::now();
  const auto nQubits = bases.front().bases.size();
  const std::size_t dimension = 1ULL << nQubits;

  std::vector<std::size_t> shotsPerBasis(bases.size(), 0U);
  for (const auto& record : records) {
    if (record.basisIndex < bases.size() && record.count > 0U) {
      shotsPerBasis[record.basisIndex] += record.count;
      ++result.observedOutcomes;
    }
  }

  std::vector<std::vector<const MeasurementRecord*>> perBasis(bases.size());
  for (const auto& record : records) {
    if (record.basisIndex < bases.size() && record.count > 0U) {
      perBasis[record.basisIndex].push_back(&record);
    }
  }

  // Each projector |v><v| is stored as the vector |v>. The accumulation of R
  // costs 4^n operations per entry, so the entry count follows a work budget.
  constexpr std::size_t WORK_BUDGET = 4000000000ULL;
  const std::size_t maxEntries =
      std::min<std::size_t>(20000U, WORK_BUDGET / (dimension * dimension));
  const auto budgetLimit = std::max<std::size_t>(2U, maxEntries / bases.size());
  const std::size_t perBasisLimit =
      config.maxOutcomesPerBasis == 0U
          ? budgetLimit
          : std::min(config.maxOutcomesPerBasis, budgetLimit);

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
  result.retainedOutcomes = projectors.size();

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
  result.initializationMs = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - start)
                                .count();

  double previousLogL = -std::numeric_limits<double>::infinity();
  const auto iterationStart = std::chrono::steady_clock::now();
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
  result.iterationMs = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - iterationStart)
                           .count();

  result.rhoMatrix = toCMat(rho, dimension);
  const auto stop = std::chrono::steady_clock::now();
  result.elapsedMs =
      std::chrono::duration<double, std::milli>(stop - start).count();
  return result;
}

QST::Result QST::reconstructQMDD(Package& dd,
                                 const std::vector<MeasurementRecord>& records,
                                 const std::vector<MeasurementBasis>& bases,
                                 const Config& config) {
  Result result;
  result.backend = Backend::QMDD;
  const auto start = std::chrono::steady_clock::now();
  if (bases.empty() || records.empty()) {
    return result;
  }
  const auto nQubits = bases.front().bases.size();
  if (nQubits == 0U) {
    return result;
  }

  std::vector<std::size_t> shotsPerBasis(bases.size(), 0U);
  for (const auto& record : records) {
    if (record.basisIndex < bases.size() && record.count > 0U) {
      shotsPerBasis[record.basisIndex] += record.count;
      ++result.observedOutcomes;
    }
  }

  std::vector<std::vector<const MeasurementRecord*>> perBasis(bases.size());
  for (const auto& record : records) {
    if (record.basisIndex < bases.size() && record.count > 0U) {
      perBasis[record.basisIndex].push_back(&record);
    }
  }

  std::vector<std::size_t> limits(bases.size(), 0U);
  bool hasRecords = false;
  for (std::size_t basisIndex = 0U; basisIndex < bases.size(); ++basisIndex) {
    auto& group = perBasis[basisIndex];
    std::ranges::sort(group, [](const auto* a, const auto* b) {
      return a->count > b->count;
    });
    limits[basisIndex] =
        config.maxOutcomesPerBasis == 0U
            ? group.size()
            : std::min(config.maxOutcomesPerBasis, group.size());
    result.retainedOutcomes += limits[basisIndex];
    hasRecords = hasRecords || limits[basisIndex] > 0U;
  }
  if (!hasRecords) {
    return result;
  }

  std::vector<mEdge> operations(bases.size());
  std::vector<mEdge> daggers(bases.size());
  for (std::size_t basisIndex = 0U; basisIndex < bases.size(); ++basisIndex) {
    if (limits[basisIndex] == 0U) {
      continue;
    }
    operations[basisIndex] = basisChange(dd, nQubits, bases[basisIndex]);
    dd.incRef(operations[basisIndex]);
    daggers[basisIndex] = dd.conjugateTranspose(operations[basisIndex]);
    dd.incRef(daggers[basisIndex]);
  }

  const std::size_t dimension = 1ULL << nQubits;
  const auto addTransformedDiagonal = [&](mEdge& target,
                                          const std::size_t basisIndex,
                                          const std::vector<double>& diagonal) {
    auto diagonalDD = makeDiagonalDD(dd, diagonal);
    dd.incRef(diagonalDD);
    auto left = dd.multiply(daggers[basisIndex], diagonalDD);
    dd.incRef(left);
    auto term = dd.multiply(left, operations[basisIndex]);
    dd.incRef(term);
    dd.decRef(left);
    dd.decRef(diagonalDD);

    if (target.p == nullptr) {
      target = term;
    } else {
      auto sum = dd.add(target, term);
      dd.incRef(sum);
      dd.decRef(target);
      dd.decRef(term);
      target = sum;
    }
  };

  const auto outcomeIndex = [nQubits](const std::vector<int>& bits) {
    std::size_t outcome = 0U;
    for (std::size_t qubit = 0U; qubit < nQubits; ++qubit) {
      outcome |= static_cast<std::size_t>(bits[qubit]) << qubit;
    }
    return outcome;
  };

  // Linear inversion: R0 = sum_b U_b^dagger D_b U_b with D_b holding the
  // observed frequencies in basis b.
  mEdge rho{};
  for (std::size_t basisIndex = 0U; basisIndex < bases.size(); ++basisIndex) {
    if (limits[basisIndex] == 0U) {
      continue;
    }
    std::vector<double> diagonal(dimension, 0.0);
    const auto shots = shotsPerBasis[basisIndex];
    for (std::size_t index = 0U; index < limits[basisIndex]; ++index) {
      const auto* record = perBasis[basisIndex][index];
      diagonal[outcomeIndex(record->bits)] =
          static_cast<double>(record->count) / static_cast<double>(shots);
    }
    addTransformedDiagonal(rho, basisIndex, diagonal);
  }
  const auto initialTrace = traceReal(rho, nQubits);
  if (!std::isfinite(initialTrace) || initialTrace <= 0.0) {
    for (auto& operation : operations) {
      if (operation.p != nullptr) {
        dd.decRef(operation);
      }
    }
    for (auto& dagger : daggers) {
      if (dagger.p != nullptr) {
        dd.decRef(dagger);
      }
    }
    dd.garbageCollect();
    return result;
  }
  auto initial = scaled(dd, rho, 1.0 / initialTrace);
  dd.incRef(initial);
  dd.decRef(rho);
  rho = initial;
  dd.garbageCollect();
  result.initialDdSize = rho.size();
  result.peakDdSize = result.initialDdSize;
  result.initializationMs = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - start)
                                .count();
  double previousLogL = -std::numeric_limits<double>::infinity();
  const auto iterationStart = std::chrono::steady_clock::now();

  for (std::size_t iter = 0U; iter < config.maxIterations; ++iter) {
    mEdge R{};
    double logL = 0.0;
    for (std::size_t basisIndex = 0U; basisIndex < bases.size(); ++basisIndex) {
      if (limits[basisIndex] == 0U) {
        continue;
      }
      const auto probabilities =
          probabilitiesFromRotation(dd, rho, operations[basisIndex], nQubits);
      std::vector<double> diagonal(dimension, 0.0);
      const auto shots = shotsPerBasis[basisIndex];
      for (std::size_t index = 0U; index < limits[basisIndex]; ++index) {
        const auto* record = perBasis[basisIndex][index];
        const auto frequency =
            static_cast<double>(record->count) / static_cast<double>(shots);
        const double p =
            std::max(1e-12, probabilities[outcomeIndex(record->bits)]);
        double weight = frequency / p;
        if (!std::isfinite(weight)) {
          weight = 1e12;
        }
        diagonal[outcomeIndex(record->bits)] = std::min(weight, 1e12);
        logL += frequency * std::log(p);
      }
      addTransformedDiagonal(R, basisIndex, diagonal);
    }
    if (R.p != nullptr) {
      result.peakRSize = std::max(result.peakRSize, R.size());
    }
    dd.garbageCollect();

    auto rhoR = dd.multiply(rho, R);
    dd.incRef(rhoR);
    auto candidate = dd.multiply(R, rhoR);
    dd.incRef(candidate);
    dd.decRef(rhoR);
    result.peakDdSize = std::max(result.peakDdSize, candidate.size());

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
    result.peakDdSize = std::max(result.peakDdSize, rho.size());
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
  result.iterationMs = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - iterationStart)
                           .count();

  for (auto& operation : operations) {
    if (operation.p != nullptr) {
      dd.decRef(operation);
    }
  }
  for (auto& dagger : daggers) {
    if (dagger.p != nullptr) {
      dd.decRef(dagger);
    }
  }
  dd.garbageCollect();

  result.rho = rho;
  result.ddSize = rho.size();
  const auto stop = std::chrono::steady_clock::now();
  result.elapsedMs =
      std::chrono::duration<double, std::milli>(stop - start).count();
  return result;
}

QST::Result QST::reconstruct(Package& dd,
                             const std::vector<MeasurementRecord>& records,
                             const std::vector<MeasurementBasis>& bases,
                             const Config& config) {
  if (config.backend == Backend::Dense) {
    if (bases.empty() || records.empty()) {
      Result result;
      result.backend = Backend::Dense;
      return result;
    }
    return reconstructMatrix(dd, records, bases, config);
  }
  return reconstructQMDD(dd, records, bases, config);
}

} // namespace dd
