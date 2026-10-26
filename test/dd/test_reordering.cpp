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
#include "dd/Reordering.hpp"
#include "dd/Simulation.hpp"
#include "dd/StateGeneration.hpp"
#include "dd/VariableOrdering.hpp"
#include "ir/Permutation.hpp"
#include "ir/QuantumComputation.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>

using namespace qc;
using namespace dd;

namespace {

QuantumComputation makeAsymmetricCircuit() {
  QuantumComputation qc(4);
  qc.h(0);
  qc.cx(0, 1);
  qc.cx(0, 2);
  qc.cx(0, 3);
  qc.ry(qc::PI / 3., 1);
  qc.cx(1, 2);
  return qc;
}

Permutation identityPermutation(const std::size_t n) {
  Permutation permutation;
  for (qc::Qubit qubit = 0; qubit < n; ++qubit) {
    permutation[qubit] = qubit;
  }
  return permutation;
}

/// Map a physical basis index to the index of the same basis state in level
/// order.
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

/// Move a qubit to a target level with adjacent exchanges.
mEdge moveQubit(Package& dd, mEdge root, Permutation& layout,
                const qc::Qubit qubit, const std::size_t target) {
  while (layout.at(qubit) > target) {
    const auto level = layout.at(qubit);
    root = exchangeMatrixLevels(dd, root, level);
    qc::Qubit lower = 0;
    for (const auto& [q, l] : layout) {
      if (l == level - 1U) {
        lower = q;
      }
    }
    std::swap(layout.at(qubit), layout.at(lower));
  }
  while (layout.at(qubit) < target) {
    const auto level = layout.at(qubit);
    root = exchangeMatrixLevels(dd, root, level + 1U);
    qc::Qubit upper = 0;
    for (const auto& [q, l] : layout) {
      if (l == level + 1U) {
        upper = q;
      }
    }
    std::swap(layout.at(qubit), layout.at(upper));
  }
  return root;
}

} // namespace

TEST(Reordering, ExchangeTwiceRestoresTheDiagram) {
  const auto circuit = makeAsymmetricCircuit();
  const auto numQubits = circuit.getNqubits();
  auto package = std::make_unique<Package>(numQubits);

  auto layout = identityPermutation(numQubits);
  auto root = buildFunctionality(circuit, *package);
  const auto initialSize = root.size();

  root = exchangeMatrixLevels(*package, root, 1U);
  root = exchangeMatrixLevels(*package, root, 1U);

  EXPECT_EQ(root.size(), initialSize);
}

TEST(Reordering, ExchangeMatchesAStaticPermutation) {
  const auto circuit = makeAsymmetricCircuit();
  const auto numQubits = circuit.getNqubits();
  const auto target = computeVariableOrder(circuit, OrderingStrategy::Degree);

  // Build the unitary in the target order directly.
  auto staticCircuit = circuit;
  staticCircuit.initialLayout = target;
  staticCircuit.outputPermutation = target;
  auto staticPackage = std::make_unique<Package>(numQubits);
  const auto staticRoot = buildFunctionality(staticCircuit, *staticPackage);

  // Build in the identity order and reach the target order with exchanges.
  auto package = std::make_unique<Package>(numQubits);
  auto root = buildFunctionality(circuit, *package);
  auto layout = identityPermutation(numQubits);
  for (qc::Qubit qubit = 0; qubit < numQubits; ++qubit) {
    root = moveQubit(*package, root, layout, qubit, target.at(qubit));
  }

  for (const auto& [qubit, level] : target) {
    EXPECT_EQ(layout.at(qubit), level);
  }
  EXPECT_EQ(root.size(), staticRoot.size());
}

TEST(Reordering, SiftPreservesTheUnitary) {
  const auto circuit = makeAsymmetricCircuit();
  const auto numQubits = circuit.getNqubits();

  auto referencePackage = std::make_unique<Package>(numQubits);
  const auto referenceMatrix =
      buildFunctionality(circuit, *referencePackage).getMatrix(numQubits);

  auto package = std::make_unique<Package>(numQubits);
  auto layout = identityPermutation(numQubits);
  const auto root =
      sift(*package, buildFunctionality(circuit, *package), layout);
  const auto matrix = root.getMatrix(numQubits);

  for (std::size_t i = 0U; i < referenceMatrix.size(); ++i) {
    for (std::size_t j = 0U; j < referenceMatrix.size(); ++j) {
      const auto row = levelIndex(i, layout, numQubits);
      const auto col = levelIndex(j, layout, numQubits);
      EXPECT_NEAR(std::abs(matrix[row][col] - referenceMatrix[i][j]), 0.0,
                  1e-9);
    }
  }
}

TEST(Reordering, SiftPreservesTheStateVector) {
  const auto circuit = makeAsymmetricCircuit();
  const auto numQubits = circuit.getNqubits();

  auto referencePackage = std::make_unique<Package>(numQubits);
  const auto referenceVector =
      simulate(circuit, makeZeroState(numQubits, *referencePackage),
               *referencePackage)
          .getVector();

  auto package = std::make_unique<Package>(numQubits);
  const auto state =
      simulate(circuit, makeZeroState(numQubits, *package), *package);
  auto layout = identityPermutation(numQubits);
  const auto root = sift(*package, state, layout);
  const auto vector = root.getVector();

  ASSERT_EQ(vector.size(), referenceVector.size());
  for (std::size_t i = 0U; i < referenceVector.size(); ++i) {
    const auto index = levelIndex(i, layout, numQubits);
    EXPECT_NEAR(std::abs(vector[index] - referenceVector[i]), 0.0, 1e-9);
  }
}

TEST(Reordering, SiftNeverIncreasesTheSize) {
  const auto circuit = makeAsymmetricCircuit();
  const auto numQubits = circuit.getNqubits();
  auto package = std::make_unique<Package>(numQubits);

  auto layout = identityPermutation(numQubits);
  auto root = buildFunctionality(circuit, *package);
  const auto initialSize = root.size();
  SiftingStatistics statistics;
  root = sift(*package, root, layout, {}, &statistics);

  EXPECT_LE(root.size(), initialSize);
  EXPECT_EQ(statistics.initialSize, initialSize);
  EXPECT_EQ(statistics.finalSize, root.size());
}

TEST(Reordering, ConstructionStatisticsIncludePeakWithoutReordering) {
  const auto circuit = makeAsymmetricCircuit();
  const auto numQubits = circuit.getNqubits();
  auto package = std::make_unique<Package>(numQubits);

  ConstructionStatistics statistics;
  const auto root =
      buildFunctionality(circuit, *package, ConstructionOptions{}, &statistics);

  EXPECT_EQ(statistics.reorderings, 0U);
  EXPECT_EQ(statistics.finalSize, root.size());
  EXPECT_GE(statistics.peakSize, statistics.finalSize);
}

TEST(Reordering, DynamicConstructionTriggersReorderings) {
  const auto circuit = makeAsymmetricCircuit();
  const auto numQubits = circuit.getNqubits();
  auto package = std::make_unique<Package>(numQubits);

  ConstructionOptions options;
  options.dynamic.enabled = true;
  options.dynamic.initialThreshold = 4U;
  ConstructionStatistics statistics;
  const auto root = buildFunctionality(circuit, *package, options, &statistics);

  EXPECT_GT(statistics.reorderings, 0U);
  EXPECT_GT(statistics.exchanges, 0U);
  EXPECT_EQ(statistics.finalSize, root.size());
  EXPECT_EQ(statistics.finalLayout.size(), numQubits);
}

TEST(Reordering, DynamicConstructionPreservesTheUnitary) {
  const auto circuit = makeAsymmetricCircuit();
  const auto numQubits = circuit.getNqubits();

  auto referencePackage = std::make_unique<Package>(numQubits);
  const auto referenceMatrix =
      buildFunctionality(circuit, *referencePackage).getMatrix(numQubits);

  auto package = std::make_unique<Package>(numQubits);
  ConstructionOptions options;
  options.dynamic.enabled = true;
  options.dynamic.initialThreshold = 4U;
  ConstructionStatistics statistics;
  const auto root = buildFunctionality(circuit, *package, options, &statistics);
  const auto matrix = root.getMatrix(numQubits);

  for (std::size_t i = 0U; i < referenceMatrix.size(); ++i) {
    for (std::size_t j = 0U; j < referenceMatrix.size(); ++j) {
      const auto row = levelIndex(i, statistics.finalLayout, numQubits);
      const auto col = levelIndex(j, statistics.finalLayout, numQubits);
      EXPECT_NEAR(std::abs(matrix[row][col] - referenceMatrix[i][j]), 0.0,
                  1e-9);
    }
  }
}

TEST(Reordering, DynamicConstructionPreservesTheStateVector) {
  const auto circuit = makeAsymmetricCircuit();
  const auto numQubits = circuit.getNqubits();

  auto referencePackage = std::make_unique<Package>(numQubits);
  const auto referenceVector =
      simulate(circuit, makeZeroState(numQubits, *referencePackage),
               *referencePackage)
          .getVector();

  auto package = std::make_unique<Package>(numQubits);
  ConstructionOptions options;
  options.dynamic.enabled = true;
  options.dynamic.initialThreshold = 4U;
  ConstructionStatistics statistics;
  const auto root = simulate(circuit, makeZeroState(numQubits, *package),
                             *package, options, &statistics);
  const auto vector = root.getVector();

  ASSERT_EQ(vector.size(), referenceVector.size());
  for (std::size_t i = 0U; i < referenceVector.size(); ++i) {
    const auto index = levelIndex(i, statistics.finalLayout, numQubits);
    EXPECT_NEAR(std::abs(vector[index] - referenceVector[i]), 0.0, 1e-9);
  }
}
