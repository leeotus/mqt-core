/*
 * Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
 * Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Licensed under the MIT License
 */

#include "dd/FunctionalityConstruction.hpp"
#include "dd/Package.hpp"
#include "dd/Simulation.hpp"
#include "dd/StateGeneration.hpp"
#include "dd/VariableOrdering.hpp"
#include "ir/Definitions.hpp"
#include "ir/Permutation.hpp"
#include "ir/QuantumComputation.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

using namespace qc;
using namespace dd;

namespace {

constexpr std::array<OrderingStrategy, 3> NON_DEFAULT_STRATEGIES{
    OrderingStrategy::Degree, OrderingStrategy::Spectral,
    OrderingStrategy::RecursiveBisection};

/**
 * @brief A four-qubit circuit whose interaction graph has four different
 * degrees
 */
QuantumComputation makeAsymmetricCircuit() {
  QuantumComputation qc(4);
  qc.h(0);
  qc.cx(0, 1);
  qc.cx(0, 2);
  qc.cx(0, 3);
  qc.ry(qc::PI / 3., 1);
  return qc;
}

/**
 * @brief A five-qubit circuit that mixes single-qubit and two-qubit gates
 */
QuantumComputation makeTestCircuit() {
  QuantumComputation qc(5);
  qc.h(0);
  qc.cx(0, 1);
  qc.cx(1, 2);
  qc.cx(2, 3);
  qc.ry(qc::PI / 3., 1);
  qc.cx(3, 0);
  qc.h(2);
  qc.cx(0, 4);
  return qc;
}

/**
 * @brief Build a deterministic pseudo-random circuit
 * @details The test uses a fixed seed, so the circuit sequence is the same on
 * every run. The gate set mixes diagonal, entangling, and parameterized gates.
 */
QuantumComputation makeRandomCircuit(std::mt19937_64& rng,
                                     const std::size_t numQubits,
                                     const std::size_t numGates) {
  QuantumComputation qc(numQubits);
  std::uniform_int_distribution<int> kind(0, 5);
  std::uniform_int_distribution<std::size_t> pick(0, numQubits - 1);
  std::uniform_real_distribution<double> angle(0.0, 2.0 * qc::PI);
  const auto distinctPair = [&pick, &rng]() {
    auto first = pick(rng);
    auto second = pick(rng);
    while (second == first) {
      second = pick(rng);
    }
    return std::pair{first, second};
  };

  for (std::size_t gate = 0; gate < numGates; ++gate) {
    switch (kind(rng)) {
    case 0:
      qc.h(pick(rng));
      break;
    case 1:
      qc.x(pick(rng));
      break;
    case 2:
      qc.ry(angle(rng), pick(rng));
      break;
    case 3:
    case 4: {
      const auto [first, second] = distinctPair();
      qc.cx(first, second);
      break;
    }
    default: {
      const auto [first, second] = distinctPair();
      qc.swap(first, second);
      break;
    }
    }
  }
  return qc;
}

/**
 * @brief Map a physical index to the index of the same basis state under a
 * level order
 */
std::size_t levelIndex(const std::size_t physicalIndex,
                       const Permutation& layout, const std::size_t numQubits) {
  std::size_t index = 0U;
  for (qc::Qubit qubit = 0; qubit < numQubits; ++qubit) {
    if ((physicalIndex & (static_cast<std::size_t>(1U) << qubit)) != 0U) {
      index |= static_cast<std::size_t>(1U) << layout.at(qubit);
    }
  }
  return index;
}

} // namespace

TEST(VariableOrdering, IdentityReturnsTheIdentityPermutation) {
  const auto qc = makeTestCircuit();
  const auto permutation = computeVariableOrder(qc, OrderingStrategy::Identity);
  ASSERT_EQ(permutation.size(), qc.getNqubits());
  for (const auto& [qubit, level] : permutation) {
    EXPECT_EQ(qubit, level);
  }
}

TEST(VariableOrdering, EveryStrategyReturnsABijection) {
  const auto qc = makeTestCircuit();
  const auto numQubits = qc.getNqubits();
  for (const auto strategy : NON_DEFAULT_STRATEGIES) {
    const auto permutation = computeVariableOrder(qc, strategy);
    ASSERT_EQ(permutation.size(), numQubits) << toString(strategy);
    std::vector<bool> seen(numQubits, false);
    for (const auto& [qubit, level] : permutation) {
      EXPECT_LT(qubit, numQubits) << toString(strategy);
      ASSERT_LT(level, numQubits) << toString(strategy);
      EXPECT_FALSE(seen[level]) << toString(strategy);
      seen[level] = true;
    }
    for (const auto level : seen) {
      EXPECT_TRUE(level) << toString(strategy);
    }
  }
}

TEST(VariableOrdering, ComputesInteractionDegrees) {
  const auto qc = makeAsymmetricCircuit();
  const auto degrees = interactionDegrees(qc);
  ASSERT_EQ(degrees.size(), 4U);
  EXPECT_EQ(degrees[0], 3U);
  EXPECT_EQ(degrees[1], 1U);
  EXPECT_EQ(degrees[2], 1U);
  EXPECT_EQ(degrees[3], 1U);
}

TEST(VariableOrdering, StrategiesDifferOnAsymmetricCircuit) {
  QuantumComputation qc(5);
  qc.cx(0, 1);
  qc.cx(0, 2);
  qc.cx(0, 3);
  qc.cx(0, 4);

  const auto degree = computeVariableOrder(qc, OrderingStrategy::Degree);
  const auto spectral = computeVariableOrder(qc, OrderingStrategy::Spectral);

  // The qubit that every gate touches belongs at the root under the degree
  // strategy.
  EXPECT_EQ(degree.at(0), 4U);
  EXPECT_NE(degree.at(0), spectral.at(0));
}

TEST(VariableOrdering, ReorderingPreservesTheStateVector) {
  const auto qc = makeTestCircuit();
  const auto numQubits = qc.getNqubits();

  const auto referenceQuantumComputation = qc;
  auto referencePackage = std::make_unique<Package>(numQubits);
  const auto referenceState =
      simulate(referenceQuantumComputation,
               makeZeroState(numQubits, *referencePackage), *referencePackage);
  const auto referenceVector = referenceState.getVector();
  ASSERT_EQ(referenceVector.size(), static_cast<std::size_t>(1U) << numQubits);

  for (const auto strategy : NON_DEFAULT_STRATEGIES) {
    auto reorderedCircuit = qc;
    reorderedCircuit.initialLayout = computeVariableOrder(qc, strategy);

    auto package = std::make_unique<Package>(numQubits);
    const auto state = simulate(reorderedCircuit,
                                makeZeroState(numQubits, *package), *package);

    // dd::simulate restores the output permutation, so the state vector refers
    // to physical qubits even though the levels were reordered internally.
    const auto vector = state.getVector();
    ASSERT_EQ(vector.size(), referenceVector.size()) << toString(strategy);
    for (std::size_t i = 0U; i < vector.size(); ++i) {
      EXPECT_NEAR(std::abs(vector[i] - referenceVector[i]), 0.0, 1e-9)
          << toString(strategy) << " at amplitude " << i;
    }
  }
}

TEST(VariableOrdering, LevelsCarryTheExpectedQubits) {
  const auto qc = makeTestCircuit();
  const auto numQubits = qc.getNqubits();

  const auto referenceQuantumComputation = qc;
  auto referencePackage = std::make_unique<Package>(numQubits);
  const auto referenceState =
      simulate(referenceQuantumComputation,
               makeZeroState(numQubits, *referencePackage), *referencePackage);
  const auto referenceVector = referenceState.getVector();

  for (const auto strategy : NON_DEFAULT_STRATEGIES) {
    auto reorderedCircuit = qc;
    reorderedCircuit.initialLayout = computeVariableOrder(qc, strategy);
    // Keeping the output permutation equal to the input layout suppresses the
    // final restoration step, so the state vector stays in level order.
    reorderedCircuit.outputPermutation = reorderedCircuit.initialLayout;

    auto package = std::make_unique<Package>(numQubits);
    const auto state = simulate(reorderedCircuit,
                                makeZeroState(numQubits, *package), *package);
    const auto vector = state.getVector();
    ASSERT_EQ(vector.size(), referenceVector.size()) << toString(strategy);

    for (std::size_t i = 0U; i < referenceVector.size(); ++i) {
      std::size_t levelIndex = 0U;
      for (qc::Qubit qubit = 0; qubit < numQubits; ++qubit) {
        if ((i & (static_cast<std::size_t>(1U) << qubit)) != 0U) {
          levelIndex |= static_cast<std::size_t>(1U)
                        << reorderedCircuit.initialLayout.at(qubit);
        }
      }
      EXPECT_NEAR(std::abs(vector[levelIndex] - referenceVector[i]), 0.0, 1e-9)
          << toString(strategy) << " at amplitude " << i;
    }
  }
}

TEST(VariableOrdering, RandomCircuitsPreserveTheStateVector) {
  std::mt19937_64 rng(0x5EED5EEDULL);
  for (std::size_t trial = 0U; trial < 8U; ++trial) {
    const auto numQubits = 2U + (trial % 4U);
    const auto qc = makeRandomCircuit(rng, numQubits, 12U);

    auto referenceCircuit = qc;
    referenceCircuit.initialLayout =
        computeVariableOrder(qc, OrderingStrategy::Identity);
    referenceCircuit.outputPermutation = referenceCircuit.initialLayout;
    auto referencePackage = std::make_unique<Package>(numQubits);
    const auto referenceState =
        simulate(referenceCircuit, makeZeroState(numQubits, *referencePackage),
                 *referencePackage);
    const auto referenceVector = referenceState.getVector();

    for (const auto strategy : NON_DEFAULT_STRATEGIES) {
      auto circuit = qc;
      circuit.initialLayout = computeVariableOrder(qc, strategy);
      circuit.outputPermutation = circuit.initialLayout;

      auto package = std::make_unique<Package>(numQubits);
      const auto state =
          simulate(circuit, makeZeroState(numQubits, *package), *package);
      const auto vector = state.getVector();
      ASSERT_EQ(vector.size(), referenceVector.size()) << toString(strategy);

      for (std::size_t i = 0U; i < referenceVector.size(); ++i) {
        const auto index = levelIndex(i, circuit.initialLayout, numQubits);
        EXPECT_NEAR(std::abs(vector[index] - referenceVector[i]), 0.0, 1e-9)
            << toString(strategy) << " trial " << trial << " at amplitude "
            << i;
      }
    }
  }
}

TEST(VariableOrdering, RandomCircuitsPreserveTheUnitary) {
  std::mt19937_64 rng(0xC0FFEEULL);
  for (std::size_t trial = 0U; trial < 6U; ++trial) {
    const auto numQubits = 2U + (trial % 3U);
    const auto qc = makeRandomCircuit(rng, numQubits, 10U);

    auto referenceCircuit = qc;
    referenceCircuit.initialLayout =
        computeVariableOrder(qc, OrderingStrategy::Identity);
    referenceCircuit.outputPermutation = referenceCircuit.initialLayout;
    auto referencePackage = std::make_unique<Package>(numQubits);
    const auto referenceUnitary =
        buildFunctionality(referenceCircuit, *referencePackage);
    const auto referenceMatrix = referenceUnitary.getMatrix(numQubits);

    for (const auto strategy : NON_DEFAULT_STRATEGIES) {
      auto circuit = qc;
      circuit.initialLayout = computeVariableOrder(qc, strategy);
      circuit.outputPermutation = circuit.initialLayout;

      auto package = std::make_unique<Package>(numQubits);
      const auto unitary = buildFunctionality(circuit, *package);
      const auto matrix = unitary.getMatrix(numQubits);
      ASSERT_EQ(matrix.size(), referenceMatrix.size()) << toString(strategy);

      for (std::size_t i = 0U; i < referenceMatrix.size(); ++i) {
        for (std::size_t j = 0U; j < referenceMatrix.size(); ++j) {
          const auto row = levelIndex(i, circuit.initialLayout, numQubits);
          const auto col = levelIndex(j, circuit.initialLayout, numQubits);
          EXPECT_NEAR(std::abs(matrix[row][col] - referenceMatrix[i][j]), 0.0,
                      1e-9)
              << toString(strategy) << " trial " << trial << " at entry " << i
              << "," << j;
        }
      }
    }
  }
}
