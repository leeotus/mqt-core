/*
 * Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
 * Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Licensed under the MIT License
 */

#include "dd/Reordering.hpp"

#include "dd/ComplexNumbers.hpp"
#include "dd/Edge.hpp"
#include "dd/Node.hpp"
#include "dd/Package.hpp"
#include "ir/Permutation.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

namespace dd {

namespace {

/**
 * @brief The matrix of the SWAP gate
 */
constexpr TwoQubitGateMatrix SWAP_MATRIX{
    {{1, 0, 0, 0}, {0, 0, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}}};

/**
 * @brief Exchange two adjacent levels of a decision diagram
 * @details Swapping two levels of an operator is a similarity transformation
 * with the SWAP gate, that is `U' = S U S`. A state vector is transformed by
 * `v' = S v`. The function uses the multiplication routine that the library
 * already provides, so it inherits its handling of skipped levels, shared
 * nodes, and edge weights.
 * @param dd The decision diagram package
 * @param root The edge to transform
 * @param level The higher of the two levels to exchange
 * @return The transformed edge
 */
template <class Node>
[[nodiscard]] Edge<Node> swapAdjacentLevels(Package& dd, const Edge<Node>& root,
                                            const std::size_t level) {
  const auto swap = dd.makeTwoQubitGateDD(
      SWAP_MATRIX, static_cast<Qubit>(level), static_cast<Qubit>(level - 1U));
  if constexpr (std::is_same_v<Node, vNode>) {
    return dd.multiply(swap, root);
  } else {
    return dd.multiply(swap, dd.multiply(root, swap));
  }
}

/**
 * @brief Fill missing entries of a level order
 */
void completeLayout(qc::Permutation& layout, const std::size_t numQubits) {
  std::vector<bool> used(numQubits, false);
  for (const auto& [qubit, level] : layout) {
    if (level < numQubits) {
      used[level] = true;
    }
  }
  std::size_t next = 0U;
  for (qc::Qubit qubit = 0; qubit < numQubits; ++qubit) {
    if (layout.find(qubit) == layout.end()) {
      while (next < numQubits && used[next]) {
        ++next;
      }
      layout[qubit] = static_cast<qc::Qubit>(next);
      used[next] = true;
    }
  }
}

/// Return the level of a qubit.
[[nodiscard]] std::size_t levelOf(const qc::Permutation& layout,
                                  const qc::Qubit qubit) {
  return layout.at(qubit);
}

/**
 * @brief Swap the two qubits that occupy two adjacent levels
 */
void swapLayoutLevels(qc::Permutation& layout, const std::size_t level) {
  qc::Qubit lower = 0;
  qc::Qubit upper = 0;
  bool haveLower = false;
  bool haveUpper = false;
  for (const auto& [qubit, assigned] : layout) {
    if (assigned == level) {
      upper = qubit;
      haveUpper = true;
    } else if (assigned == level - 1U) {
      lower = qubit;
      haveLower = true;
    }
  }
  if (haveLower && haveUpper) {
    std::swap(layout.at(lower), layout.at(upper));
  }
}

/**
 * @brief Sift a decision diagram
 */
template <class Node>
[[nodiscard]] Edge<Node>
siftImpl(Package& dd, Edge<Node> root, qc::Permutation& layout,
         const SiftingOptions& options, SiftingStatistics* statistics) {
  const auto numQubits = dd.qubits();
  SiftingStatistics stats;
  stats.initialSize = root.size();
  completeLayout(layout, numQubits);

  if (numQubits < 2U) {
    stats.finalSize = stats.initialSize;
    if (statistics != nullptr) {
      *statistics = stats;
    }
    return root;
  }

  dd.incRef(root);
  if (options.collectGarbage) {
    dd.garbageCollect(true);
  }

  const auto exchange = [&dd, &root](const std::size_t level) {
    auto swapped = swapAdjacentLevels(dd, root, level);
    dd.incRef(swapped);
    dd.decRef(root);
    root = swapped;
  };

  std::size_t previousSize = root.size();
  for (std::size_t pass = 0U; pass < options.maxPasses; ++pass) {
    ++stats.passes;

    for (qc::Qubit qubit = 0; qubit < numQubits; ++qubit) {
      ++stats.qubits;

      // Move the qubit to the lowest level.
      while (levelOf(layout, qubit) > 0U) {
        const auto level = levelOf(layout, qubit);
        exchange(level);
        swapLayoutLevels(layout, level);
        ++stats.exchanges;
      }

      // Sweep the qubit upward and remember the smallest diagram.
      std::size_t bestSize = root.size();
      std::size_t bestLevel = 0U;
      for (std::size_t level = 0U; level + 1U < numQubits; ++level) {
        exchange(level + 1U);
        swapLayoutLevels(layout, level + 1U);
        ++stats.exchanges;
        const auto size = root.size();
        if (size < bestSize) {
          bestSize = size;
          bestLevel = level + 1U;
        }
      }

      // Move the qubit back to the best position.
      while (levelOf(layout, qubit) > bestLevel) {
        const auto level = levelOf(layout, qubit);
        exchange(level);
        swapLayoutLevels(layout, level);
        ++stats.exchanges;
      }

      if (options.collectGarbage) {
        dd.garbageCollect(true);
      }
    }

    const auto size = root.size();
    if (options.stopWhenUnchanged && size >= previousSize) {
      break;
    }
    previousSize = size;
  }

  stats.finalSize = root.size();
  if (statistics != nullptr) {
    *statistics = stats;
  }
  return root;
}

} // namespace

DynamicReorderer::DynamicReorderer(DynamicReorderingOptions options)
    : options_(std::move(options)), threshold_(options_.initialThreshold) {}

bool DynamicReorderer::operator()(Package& dd, mEdge& root,
                                  qc::Permutation& layout,
                                  ConstructionStatistics* statistics) {
  return inspect(dd, root, layout, statistics);
}

bool DynamicReorderer::operator()(Package& dd, vEdge& root,
                                  qc::Permutation& layout,
                                  ConstructionStatistics* statistics) {
  return inspect(dd, root, layout, statistics);
}

template <class Node>
bool DynamicReorderer::inspect(Package& dd, Edge<Node>& root,
                               qc::Permutation& layout,
                               ConstructionStatistics* statistics) {
  const auto size = root.size();
  if (statistics != nullptr) {
    statistics->peakSize = std::max(statistics->peakSize, size);
  }
  if (!options_.enabled || size <= threshold_) {
    return false;
  }

  const auto start = std::chrono::steady_clock::now();
  SiftingStatistics sifting;
  auto reordered = sift(dd, root, layout, options_.sifting, &sifting);

  // sift leaves exactly one reference on the result. Transfer the reference
  // that the caller holds on the old root to the reordered diagram.
  const auto& roots = dd.getRootSet<Node>();
  if (roots.find(root) != roots.end()) {
    dd.decRef(root);
  }
  root = reordered;
  const auto stop = std::chrono::steady_clock::now();

  if (statistics != nullptr) {
    ++statistics->reorderings;
    statistics->exchanges += sifting.exchanges;
    statistics->reorderTimeMs +=
        std::chrono::duration<double, std::milli>(stop - start).count();
  }

  // Raise the threshold so that the next reordering does not happen
  // immediately: grow it by the growth factor, and keep it above the tracking
  // factor of the current size.
  const auto grown = static_cast<double>(threshold_) * options_.growthFactor;
  const auto tracked =
      options_.trackingFactor * static_cast<double>(root.size());
  threshold_ = static_cast<std::size_t>(std::max(grown, tracked));
  return true;
}

mEdge exchangeMatrixLevels(Package& dd, const mEdge& root,
                           const std::size_t level) {
  if (level == 0U || level >= dd.qubits()) {
    return root;
  }
  return swapAdjacentLevels(dd, root, level);
}

vEdge exchangeVectorLevels(Package& dd, const vEdge& root,
                           const std::size_t level) {
  if (level == 0U || level >= dd.qubits()) {
    return root;
  }
  return swapAdjacentLevels(dd, root, level);
}

mEdge sift(Package& dd, mEdge root, qc::Permutation& layout,
           const SiftingOptions& options, SiftingStatistics* statistics) {
  return siftImpl<mNode>(dd, std::move(root), layout, options, statistics);
}

vEdge sift(Package& dd, vEdge root, qc::Permutation& layout,
           const SiftingOptions& options, SiftingStatistics* statistics) {
  return siftImpl<vNode>(dd, std::move(root), layout, options, statistics);
}

} // namespace dd
