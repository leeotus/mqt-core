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
 * @brief Options for threshold-triggered reordering during construction
 * @details The policy follows the idea of the igqmdd tool: after every
 * operation, compare the size of the current decision diagram with a
 * threshold. When the size exceeds the threshold, reorder the diagram with
 * sifting and raise the threshold so that the next reordering does not happen
 * immediately. The idea is to keep intermediate decision diagrams small so
 * that the final diagram is small as well.
 */
struct DynamicReorderingOptions {
  /// Whether to reorder during construction at all
  bool enabled = false;
  /// The size that triggers the first reordering
  std::size_t initialThreshold = 1000U;
  /// The factor by which the threshold grows after a reordering
  double growthFactor = 2.0;
  /// The factor of the current size that the threshold is raised to
  double trackingFactor = 1.5;
  /// The options that are passed to sifting
  SiftingOptions sifting{};
};

/**
 * @brief Options for building a decision diagram incrementally
 */
struct ConstructionOptions {
  /// Threshold-triggered reordering during construction
  DynamicReorderingOptions dynamic{};
};

/**
 * @brief Measurements of a construction run
 */
struct ConstructionStatistics {
  /// The number of times that the threshold triggered a reordering
  std::size_t reorderings = 0U;
  /// The total number of adjacent level exchanges
  std::size_t exchanges = 0U;
  /// The largest decision diagram that was observed
  std::size_t peakSize = 0U;
  /// The size of the final decision diagram
  std::size_t finalSize = 0U;
  /// The total time spent in reordering
  double reorderTimeMs = 0.0;
  /// The level order of the returned decision diagram
  /// @note The construction functions fill this field only when dynamic
  /// reordering is enabled, because then the level order is not known before
  /// the construction.
  qc::Permutation finalLayout;
};

/**
 * @brief Threshold-triggered reordering policy
 * @details Keep one instance per construction. The call operator inspects the
 * current decision diagram after an operation and reorders it when its size
 * exceeds the threshold. It owns the reference that the caller holds on the
 * diagram, so the caller keeps its invariant of holding exactly one reference.
 * The class is independent of the construction loop, which makes it reusable
 * for any incremental construction, for example @ref buildFunctionality or
 * @ref simulate.
 */
class DynamicReorderer final {
public:
  explicit DynamicReorderer(DynamicReorderingOptions options = {});

  /**
   * @brief Inspect a matrix decision diagram and reorder it when needed
   * @return Whether a reordering was applied
   */
  bool operator()(Package& dd, mEdge& root, qc::Permutation& layout,
                  ConstructionStatistics* statistics = nullptr);

  /**
   * @brief Inspect a vector decision diagram and reorder it when needed
   * @return Whether a reordering was applied
   */
  bool operator()(Package& dd, vEdge& root, qc::Permutation& layout,
                  ConstructionStatistics* statistics = nullptr);

  /// The threshold that is currently in use
  [[nodiscard]] std::size_t threshold() const noexcept { return threshold_; }

private:
  template <class Node>
  bool inspect(Package& dd, Edge<Node>& root, qc::Permutation& layout,
               ConstructionStatistics* statistics);

  DynamicReorderingOptions options_;
  std::size_t threshold_ = 0U;
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
