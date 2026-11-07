/*
 * Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
 * Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Licensed under the MIT License
 */

#include "dd/Package.hpp"
#include "dd/QST.hpp"
#include "ir/QuantumComputation.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

using namespace dd;

namespace {

constexpr double TOLERANCE = 1e-9;

/// Build a Pauli basis from characters 'Z', 'X', 'Y'.
QST::MeasurementBasis makeBasis(const std::string& specification) {
  QST::MeasurementBasis basis;
  for (const char symbol : specification) {
    switch (symbol) {
    case 'Z':
      basis.bases.push_back(QST::Basis::Z);
      break;
    case 'X':
      basis.bases.push_back(QST::Basis::X);
      break;
    default:
      basis.bases.push_back(QST::Basis::Y);
      break;
    }
  }
  return basis;
}

/// Build a two-qubit GHZ state circuit.
qc::QuantumComputation ghzCircuit() {
  qc::QuantumComputation circuit(2);
  circuit.h(0);
  circuit.cx(0, 1);
  return circuit;
}

/// Return Tr(first * second) for two dense matrices.
double matrixOverlap(const CMat& first, const CMat& second) {
  double value = 0.0;
  for (std::size_t row = 0U; row < first.size(); ++row) {
    for (std::size_t col = 0U; col < first.size(); ++col) {
      value += (first[row][col] * second[col][row]).real();
    }
  }
  return value;
}

} // namespace

TEST(QST, ProbabilitiesOfComputationalBasisState) {
  Package dd(1);
  qc::QuantumComputation circuit(1);
  auto rho = QST::densityMatrix(dd, circuit);

  EXPECT_NEAR(QST::probability(dd, rho, makeBasis("Z"), {0}), 1.0, TOLERANCE);
  EXPECT_NEAR(QST::probability(dd, rho, makeBasis("Z"), {1}), 0.0, TOLERANCE);
  EXPECT_NEAR(QST::probability(dd, rho, makeBasis("X"), {0}), 0.5, TOLERANCE);
  EXPECT_NEAR(QST::probability(dd, rho, makeBasis("X"), {1}), 0.5, TOLERANCE);
  EXPECT_NEAR(QST::probability(dd, rho, makeBasis("Y"), {0}), 0.5, TOLERANCE);

  dd.decRef(rho);
  dd.garbageCollect();
}

TEST(QST, BellStateCorrelations) {
  Package dd(2);
  auto circuit = ghzCircuit();
  auto rho = QST::densityMatrix(dd, circuit);

  const auto zz = makeBasis("ZZ");
  const auto xx = makeBasis("XX");
  EXPECT_NEAR(QST::probability(dd, rho, zz, {0, 0}), 0.5, TOLERANCE);
  EXPECT_NEAR(QST::probability(dd, rho, zz, {1, 1}), 0.5, TOLERANCE);
  EXPECT_NEAR(QST::probability(dd, rho, zz, {0, 1}), 0.0, TOLERANCE);
  EXPECT_NEAR(QST::probability(dd, rho, xx, {0, 0}), 0.5, TOLERANCE);
  EXPECT_NEAR(QST::probability(dd, rho, xx, {1, 1}), 0.5, TOLERANCE);
  EXPECT_NEAR(QST::probability(dd, rho, xx, {0, 1}), 0.0, TOLERANCE);

  dd.decRef(rho);
  dd.garbageCollect();
}

TEST(QST, DensityMatrixIsNormalizedAndPure) {
  Package dd(2);
  auto circuit = ghzCircuit();
  auto rho = QST::densityMatrix(dd, circuit);

  auto identity = dd.createInitialMatrix({});
  dd.incRef(identity);
  EXPECT_NEAR(QST::overlap(dd, rho, identity, 2), 1.0, TOLERANCE);
  EXPECT_NEAR(QST::purity(dd, rho, 2), 1.0, TOLERANCE);

  dd.decRef(identity);
  dd.decRef(rho);
  dd.garbageCollect();
}

TEST(QST, MaximallyMixedStateHasHalfPurity) {
  Package dd(1);
  auto rho = QST::maximallyMixedState(dd, 1);

  EXPECT_NEAR(QST::purity(dd, rho, 1), 0.5, TOLERANCE);
  EXPECT_NEAR(QST::probability(dd, rho, makeBasis("Z"), {0}), 0.5, TOLERANCE);
  EXPECT_NEAR(QST::probability(dd, rho, makeBasis("X"), {0}), 0.5, TOLERANCE);

  dd.decRef(rho);
  dd.garbageCollect();
}

TEST(QST, SampleKeepsTheBasisIdentity) {
  Package dd(1);
  qc::QuantumComputation circuit(1);
  auto rho = QST::densityMatrix(dd, circuit);

  const auto bases = QST::allPauliBases(1);
  ASSERT_EQ(bases.size(), 3U);

  const auto records = QST::sample(dd, rho, bases, 100U, 7U);
  ASSERT_FALSE(records.empty());
  for (const auto& record : records) {
    ASSERT_LT(record.basisIndex, bases.size());
    if (bases[record.basisIndex].bases[0] == QST::Basis::Z) {
      EXPECT_EQ(record.bits[0], 0);
    }
  }

  dd.decRef(rho);
  dd.garbageCollect();
}

TEST(QST, ReconstructsComputationalBasisState) {
  Package dd(1);
  qc::QuantumComputation circuit(1);
  auto rhoTrue = QST::densityMatrix(dd, circuit);
  const auto bases = QST::allPauliBases(1);

  QST::Config config;
  config.backend = QST::Backend::Dense;
  config.shotsPerBasis = 4000U;
  config.maxIterations = 200U;
  config.tolerance = 1e-10;
  config.seed = 11U;

  const auto records =
      QST::sample(dd, rhoTrue, bases, config.shotsPerBasis, config.seed);
  auto result = QST::reconstruct(dd, records, bases, config);
  ASSERT_FALSE(result.rhoMatrix.empty());
  const auto trueMatrix = rhoTrue.getMatrix(1);
  EXPECT_NEAR(matrixOverlap(trueMatrix, result.rhoMatrix), 1.0, 1e-3);

  if (result.rho.p != nullptr) {
    dd.decRef(result.rho);
  }
  dd.decRef(rhoTrue);
  dd.garbageCollect();
}

TEST(QST, ReconstructsBellState) {
  Package dd(2);
  auto circuit = ghzCircuit();
  auto rhoTrue = QST::densityMatrix(dd, circuit);
  const auto bases = QST::allPauliBases(2);

  QST::Config config;
  config.backend = QST::Backend::Dense;
  config.shotsPerBasis = 2000U;
  config.maxIterations = 200U;
  config.tolerance = 1e-10;
  config.seed = 5U;

  const auto records =
      QST::sample(dd, rhoTrue, bases, config.shotsPerBasis, config.seed);
  auto result = QST::reconstruct(dd, records, bases, config);
  ASSERT_FALSE(result.rhoMatrix.empty());
  const auto trueMatrix = rhoTrue.getMatrix(2);
  EXPECT_NEAR(matrixOverlap(trueMatrix, result.rhoMatrix), 1.0, 5e-2);

  if (result.rho.p != nullptr) {
    dd.decRef(result.rho);
  }
  dd.decRef(rhoTrue);
  dd.garbageCollect();
}

/// bits[q] must be the outcome of qubit q. The GHZ state is symmetric and
/// cannot detect a reversed order, so this test uses |01> instead.
TEST(QST, OutcomeBitsFollowQubitIndex) {
  Package dd(2);
  qc::QuantumComputation circuit(2);
  circuit.x(1);
  auto rho = QST::densityMatrix(dd, circuit);

  const auto zz = makeBasis("ZZ");
  EXPECT_NEAR(QST::probability(dd, rho, zz, {0, 1}), 1.0, TOLERANCE);
  EXPECT_NEAR(QST::probability(dd, rho, zz, {1, 0}), 0.0, TOLERANCE);

  dd.decRef(rho);
  dd.garbageCollect();
}

/// A state without permutation symmetry would keep only half of its overlap
/// if the projector index and the outcome encoding disagreed.
TEST(QST, ReconstructsAsymmetricState) {
  Package dd(2);
  qc::QuantumComputation circuit(2);
  circuit.ry(0.7, 0);
  circuit.cx(0, 1);
  circuit.ry(0.9, 1);
  auto rhoTrue = QST::densityMatrix(dd, circuit);
  const auto bases = QST::allPauliBases(2);

  QST::Config config;
  config.backend = QST::Backend::Dense;
  config.shotsPerBasis = 4000U;
  config.maxIterations = 300U;
  config.tolerance = 1e-10;
  config.seed = 7U;

  const auto records =
      QST::sample(dd, rhoTrue, bases, config.shotsPerBasis, config.seed);
  auto result = QST::reconstruct(dd, records, bases, config);
  ASSERT_FALSE(result.rhoMatrix.empty());
  const auto trueMatrix = rhoTrue.getMatrix(2);
  EXPECT_NEAR(matrixOverlap(trueMatrix, result.rhoMatrix), 1.0, 2e-2);

  if (result.rho.p != nullptr) {
    dd.decRef(result.rho);
  }
  dd.decRef(rhoTrue);
  dd.garbageCollect();
}

TEST(QST, QMDDReconstructionStaysOnDecisionDiagrams) {
  Package dd(1);
  qc::QuantumComputation circuit(1);
  auto rhoTrue = QST::densityMatrix(dd, circuit);
  const auto bases = QST::allPauliBases(1);

  QST::Config config;
  config.backend = QST::Backend::QMDD;
  config.shotsPerBasis = 4000U;
  config.maxIterations = 100U;
  config.tolerance = 1e-10;
  config.seed = 13U;

  const auto records =
      QST::sample(dd, rhoTrue, bases, config.shotsPerBasis, config.seed);
  auto result = QST::reconstruct(dd, records, bases, config);

  EXPECT_EQ(result.backend, QST::Backend::QMDD);
  EXPECT_TRUE(result.rhoMatrix.empty());
  ASSERT_NE(result.rho.p, nullptr);
  EXPECT_GT(QST::overlap(dd, rhoTrue, result.rho, 1), 0.99);
  EXPECT_NEAR(QST::purity(dd, result.rho, 1), 1.0, 1e-2);
  EXPECT_GT(result.observedOutcomes, 0U);
  EXPECT_GT(result.retainedOutcomes, 0U);
  EXPECT_GT(result.initialDdSize, 0U);
  EXPECT_GE(result.peakDdSize, result.initialDdSize);
  EXPECT_GE(result.peakDdSize, result.ddSize);
  EXPECT_GT(result.peakRSize, 0U);
  EXPECT_GE(result.initializationMs, 0.0);
  EXPECT_GT(result.iterationMs, 0.0);
  EXPECT_GT(result.elapsedMs, 0.0);

  dd.decRef(result.rho);
  dd.decRef(rhoTrue);
  dd.garbageCollect();
}

TEST(QST, QMDDAndDenseBackendsReconstructTheSameState) {
  Package dd(2);
  qc::QuantumComputation circuit(2);
  circuit.ry(0.4, 0);
  circuit.cx(0, 1);
  circuit.ry(0.8, 1);
  auto rhoTrue = QST::densityMatrix(dd, circuit);
  const auto bases = QST::allPauliBases(2);

  QST::Config denseConfig;
  denseConfig.backend = QST::Backend::Dense;
  denseConfig.shotsPerBasis = 2000U;
  denseConfig.maxIterations = 40U;
  denseConfig.tolerance = 1e-10;
  denseConfig.seed = 17U;

  const auto records = QST::sample(dd, rhoTrue, bases,
                                   denseConfig.shotsPerBasis, denseConfig.seed);
  auto denseResult = QST::reconstruct(dd, records, bases, denseConfig);
  auto qmddConfig = denseConfig;
  qmddConfig.backend = QST::Backend::QMDD;
  auto qmddResult = QST::reconstruct(dd, records, bases, qmddConfig);

  EXPECT_EQ(denseResult.backend, QST::Backend::Dense);
  EXPECT_EQ(qmddResult.backend, QST::Backend::QMDD);
  EXPECT_GT(denseResult.observedOutcomes, 0U);
  EXPECT_GT(denseResult.retainedOutcomes, 0U);
  EXPECT_EQ(denseResult.initialDdSize, 0U);
  EXPECT_EQ(denseResult.peakDdSize, 0U);
  ASSERT_FALSE(denseResult.rhoMatrix.empty());
  ASSERT_NE(qmddResult.rho.p, nullptr);
  const auto qmddMatrix = qmddResult.rho.getMatrix(2);
  ASSERT_EQ(qmddMatrix.size(), denseResult.rhoMatrix.size());
  for (std::size_t row = 0U; row < qmddMatrix.size(); ++row) {
    ASSERT_EQ(qmddMatrix[row].size(), denseResult.rhoMatrix[row].size());
    for (std::size_t col = 0U; col < qmddMatrix[row].size(); ++col) {
      EXPECT_LT(
          std::abs(qmddMatrix[row][col] - denseResult.rhoMatrix[row][col]),
          1e-8);
    }
  }
  EXPECT_GT(dd::QST::overlap(dd, rhoTrue, qmddResult.rho, 2), 0.99);

  dd.decRef(qmddResult.rho);
  dd.decRef(rhoTrue);
  dd.garbageCollect();
}
