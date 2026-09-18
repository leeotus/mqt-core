/*
 * Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
 * Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Licensed under the MIT License
 */

/**
 * @file Reordering.hpp
 * @brief Dynamic variable reordering for decision diagrams
 */

#pragma once

#include "dd/Package_fwd.hpp"
#include "ir/Permutation.hpp"

#include <cstddef>

namespace dd {

class Package;

/**
 * @brief Options that control sifting
 */
struct SiftingOptions {
  /// The maximum number of sweeps over all qubits
  std::size_t maxPasses = 3U;
  /// Stop early when a sweep does not reduce the decision diagram
  bool stopWhenUnchanged = true;
  /// Run garbage collection after every sifted qubit
  /// @note Garbage collection keeps only the edges that the caller has
  /// registered with @ref Package::incRef. Track every edge that you want to
  /// keep before calling @ref sift.
  bool collectGarbage = true;
};

/**
 * @brief Measurements that sifting produces
 */
struct SiftingStatistics {
  /// The number of nodes before sifting
  std::size_t initialSize = 0U;
  /// The number of nodes after sifting
  std::size_t finalSize = 0U;
  /// The number of adjacent level exchanges
  std::size_t exchanges = 0U;
  /// The number of completed sweeps
  std::size_t passes = 0U;
  /// The number of sifted qubits
  std::size_t qubits = 0U;
};

/**
 * @brief Swap two adjacent levels of a matrix decision diagram
 * @details The function returns a diagram that represents the same operator
 * with the variables of @p level and `level - 1` exchanged. It builds new
 * nodes instead of changing existing nodes in place, because a node may be
 * shared and because the diagram may skip levels.
 * @param dd The decision diagram package
 * @param root The root of the diagram
 * @param level The higher of the two levels to exchange. It must be at least
 * one.
 * @return The diagram with the two levels exchanged
 */
[[nodiscard]] mEdge exchangeMatrixLevels(Package& dd, const mEdge& root,
                                         std::size_t level);

/**
 * @brief Swap two adjacent levels of a vector decision diagram
 * @param dd The decision diagram package
 * @param root The root of the diagram
 * @param level The higher of the two levels to exchange. It must be at least
 * one.
 * @return The diagram with the two levels exchanged
 */
[[nodiscard]] vEdge exchangeVectorLevels(Package& dd, const vEdge& root,
                                         std::size_t level);

/**
 * @brief Reorder a matrix decision diagram with sifting
 * @details Sifting moves one qubit at a time through all positions, keeps the
 * position with the fewest nodes, and repeats for every qubit. The function
 * updates @p layout to describe the new level order and registers the returned
 * edge in the root set. Call @ref Package::decRef when the edge is no longer
 * needed.
 * @param dd The decision diagram package
 * @param root The root of the diagram
 * @param layout The physical-qubit-to-level map, updated in place
 * @param options The sifting options
 * @param statistics Optional measurements
 * @return The reordered diagram
 */
[[nodiscard]] mEdge sift(Package& dd, mEdge root, qc::Permutation& layout,
                         const SiftingOptions& options = {},
                         SiftingStatistics* statistics = nullptr);

/**
 * @brief Reorder a vector decision diagram with sifting
 * @param dd The decision diagram package
 * @param root The root of the diagram
 * @param layout The physical-qubit-to-level map, updated in place
 * @param options The sifting options
 * @param statistics Optional measurements
 * @return The reordered diagram
 */
[[nodiscard]] vEdge sift(Package& dd, vEdge root, qc::Permutation& layout,
                         const SiftingOptions& options = {},
                         SiftingStatistics* statistics = nullptr);

} // namespace dd
