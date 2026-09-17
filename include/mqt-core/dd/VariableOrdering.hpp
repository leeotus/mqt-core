/*
 * Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
 * Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Licensed under the MIT License
 */

#pragma once

#include "ir/Permutation.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qc {
class QuantumComputation;
} // namespace qc

namespace dd {

/**
 * @brief A heuristic for choosing the level order of a decision diagram
 * @details The level order assigns one decision diagram level to each qubit of
 * a circuit. MQT Core numbers decision diagram levels with @ref NodeBase::v.
 * Level 0 is the level nearest to the terminals and the largest level is the
 * level nearest to the root. Two gates that act on the same qubit, or on qubits
 * that are far apart in the level order, tend to produce more decision diagram
 * nodes. The strategies in this enumeration trade the cost of computing an
 * order against the decision diagram size that the order produces.
 */
enum class OrderingStrategy : std::uint8_t {
  /// Keep the physical qubit order.
  Identity,
  /// Place qubits with many two-qubit interactions near the root.
  Degree,
  /// Order qubits by the Fiedler vector of the interaction graph Laplacian.
  Spectral,
  /// Recursively bisect the interaction graph to keep related qubits adjacent.
  RecursiveBisection,
  /// Order qubits by reversing a minimum-fill elimination order.
  MinFill,
  /// Order targets below the controls that act on them, as MQT DDSIM does.
  GateDependency,
};

/**
 * @brief Return the weighted interaction degree of every qubit
 * @details The interaction graph of a circuit has one vertex per qubit and one
 * weighted edge per pair of qubits that occur together in a gate. The degree of
 * a vertex is the sum of the weights of its edges. Single-qubit gates and
 * non-unitary operations do not contribute.
 * @param circuit The circuit to inspect
 * @return The weighted degree of every qubit in physical qubit order
 */
[[nodiscard]] std::vector<std::size_t>
interactionDegrees(const qc::QuantumComputation& circuit);

/**
 * @brief Compute a physical-qubit-to-decision-diagram-level order
 * @details The result is a complete permutation of the qubits that is suitable
 * for @ref qc::QuantumComputation::initialLayout. Setting that field before
 * calling @ref simulate or @ref buildFunctionality makes the decision diagram
 * package use the computed order. The function always returns a map for every
 * qubit, including for @ref OrderingStrategy::Identity. An incomplete map
 * crashes the simulator when a circuit contains an uncontrolled SWAP.
 * @param circuit The circuit to inspect
 * @param strategy The heuristic that chooses the order
 * @return The level order
 */
[[nodiscard]] qc::Permutation
computeVariableOrder(const qc::QuantumComputation& circuit,
                     OrderingStrategy strategy);

/**
 * @brief Return a human-readable name for an ordering strategy
 * @param strategy The strategy to name
 * @return A constant string that names the strategy
 */
[[nodiscard]] const char* toString(OrderingStrategy strategy);

} // namespace dd
