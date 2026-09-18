/*
 * Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
 * Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Licensed under the MIT License
 */

#include "dd/VariableOrdering.hpp"

#include "ir/QuantumComputation.hpp"
#include "ir/operations/CompoundOperation.hpp"
#include "ir/operations/OpType.hpp"
#include "ir/operations/Operation.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace dd {

namespace {

/**
 * @brief The weighted interaction graph of a circuit
 */
struct InteractionGraph {
  /// The number of qubits, which is also the number of vertices
  std::size_t numQubits = 0U;
  /// The gate count between every pair of qubits
  std::vector<std::vector<std::size_t>> weight;
  /// The sum of the incident gate counts of every qubit
  std::vector<std::size_t> degree;
};

/**
 * @brief Add the interactions of one operation to a graph
 * @details The function recurses into compound operations and ignores
 * operations that are not standard operations, such as measurements and
 * resets.
 */
void addOperation(InteractionGraph& graph, const qc::Operation& op) {
  if (op.isCompoundOperation()) {
    const auto* compound =
        dynamic_cast<const qc::CompoundOperation*>(&op); // NOLINT
    if (compound != nullptr) {
      for (const auto& child : *compound) {
        addOperation(graph, *child);
      }
    }
    return;
  }

  if (!op.isStandardOperation()) {
    return;
  }

  std::vector<std::size_t> involved;
  for (const auto& control : op.getControls()) {
    involved.emplace_back(control.qubit);
  }
  for (const auto target : op.getTargets()) {
    involved.emplace_back(target);
  }

  for (std::size_t i = 0U; i < involved.size(); ++i) {
    for (std::size_t j = i + 1U; j < involved.size(); ++j) {
      const auto a = involved[i];
      const auto b = involved[j];
      if (a == b || a >= graph.numQubits || b >= graph.numQubits) {
        continue;
      }
      ++graph.weight[a][b];
      ++graph.weight[b][a];
      ++graph.degree[a];
      ++graph.degree[b];
    }
  }
}

/**
 * @brief Build the weighted interaction graph of a circuit
 */
[[nodiscard]] InteractionGraph
buildInteractionGraph(const qc::QuantumComputation& circuit) {
  InteractionGraph graph;
  graph.numQubits = circuit.getNqubits();
  graph.weight.assign(graph.numQubits,
                      std::vector<std::size_t>(graph.numQubits, 0U));
  graph.degree.assign(graph.numQubits, 0U);
  for (const auto& op : circuit) {
    if (op) {
      addOperation(graph, *op);
    }
  }
  return graph;
}

/**
 * @brief The result of a symmetric eigenvalue computation
 */
struct EigenDecomposition {
  /// The eigenvalues
  std::vector<double> values;
  /// The eigenvectors stored by column
  std::vector<std::vector<double>> vectors;
};

/**
 * @brief Compute all eigenvalues and eigenvectors of a symmetric matrix
 * @details The cyclic Jacobi method applies a sequence of plane rotations
 * until the off-diagonal entries vanish. The method needs no external library
 * and is stable for the small dense matrices that interaction graphs produce.
 * @param matrix The symmetric matrix, which is modified in place
 * @return The eigenvalues and the eigenvectors stored by column
 */
[[nodiscard]] EigenDecomposition
jacobiEigen(std::vector<std::vector<double>> matrix) {
  const auto n = matrix.size();
  EigenDecomposition result;
  result.vectors.assign(n, std::vector<double>(n, 0.0));
  for (std::size_t i = 0U; i < n; ++i) {
    result.vectors[i][i] = 1.0;
  }

  constexpr double EPSILON = 1e-14;
  constexpr std::size_t MAX_SWEEPS = 100U;
  for (std::size_t sweep = 0U; sweep < MAX_SWEEPS; ++sweep) {
    double offDiagonal = 0.0;
    for (std::size_t i = 0U; i < n; ++i) {
      for (std::size_t j = i + 1U; j < n; ++j) {
        offDiagonal += matrix[i][j] * matrix[i][j];
      }
    }
    if (offDiagonal <= EPSILON) {
      break;
    }

    for (std::size_t p = 0U; p + 1U < n; ++p) {
      for (std::size_t q = p + 1U; q < n; ++q) {
        if (std::abs(matrix[p][q]) <= EPSILON) {
          continue;
        }
        const double theta =
            (matrix[q][q] - matrix[p][p]) / (2.0 * matrix[p][q]);
        const double sign = theta >= 0.0 ? 1.0 : -1.0;
        const double t =
            sign / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;

        for (std::size_t k = 0U; k < n; ++k) {
          if (k == p || k == q) {
            continue;
          }
          const auto akp = matrix[k][p];
          const auto akq = matrix[k][q];
          matrix[k][p] = c * akp - s * akq;
          matrix[p][k] = matrix[k][p];
          matrix[k][q] = s * akp + c * akq;
          matrix[q][k] = matrix[k][q];
        }

        const auto app = matrix[p][p];
        const auto aqq = matrix[q][q];
        const auto apq = matrix[p][q];
        matrix[p][p] = (c * c * app) - (2.0 * s * c * apq) + (s * s * aqq);
        matrix[q][q] = (s * s * app) + (2.0 * s * c * apq) + (c * c * aqq);
        matrix[p][q] = 0.0;
        matrix[q][p] = 0.0;

        for (std::size_t k = 0U; k < n; ++k) {
          const auto vkp = result.vectors[p][k];
          const auto vkq = result.vectors[q][k];
          result.vectors[p][k] = (c * vkp) - (s * vkq);
          result.vectors[q][k] = (s * vkp) + (c * vkq);
        }
      }
    }
  }

  result.values.resize(n);
  for (std::size_t i = 0U; i < n; ++i) {
    result.values[i] = matrix[i][i];
  }
  return result;
}

/**
 * @brief Build the Laplacian of the subgraph induced by a vertex set
 */
[[nodiscard]] std::vector<std::vector<double>>
inducedLaplacian(const InteractionGraph& graph,
                 const std::vector<std::size_t>& vertices) {
  const auto size = vertices.size();
  std::vector<std::vector<double>> laplacian(size,
                                             std::vector<double>(size, 0.0));
  for (std::size_t i = 0U; i < size; ++i) {
    for (std::size_t j = i + 1U; j < size; ++j) {
      const auto w =
          static_cast<double>(graph.weight[vertices[i]][vertices[j]]);
      laplacian[i][j] = -w;
      laplacian[j][i] = -w;
      laplacian[i][i] += w;
      laplacian[j][j] += w;
    }
  }
  return laplacian;
}

/**
 * @brief Compute the Fiedler vector of the subgraph induced by a vertex set
 * @details The Fiedler vector is the eigenvector of the second-smallest
 * eigenvalue of the graph Laplacian. Sorting vertices by the Fiedler vector
 * keeps strongly connected vertices close in the resulting order.
 */
[[nodiscard]] std::vector<double>
fiedlerVector(const InteractionGraph& graph,
              const std::vector<std::size_t>& vertices) {
  const auto size = vertices.size();
  std::vector<double> values(size, 0.0);
  if (size <= 1U) {
    return values;
  }

  auto decomposition = jacobiEigen(inducedLaplacian(graph, vertices));
  std::vector<std::size_t> order(size);
  std::iota(order.begin(), order.end(), 0U);
  std::ranges::sort(order, {}, [&decomposition](const std::size_t index) {
    return decomposition.values[index];
  });

  const auto fiedler = order[1U];
  for (std::size_t i = 0U; i < size; ++i) {
    values[i] = decomposition.vectors[fiedler][i];
  }
  return values;
}

/**
 * @brief Return whether a vector is numerically constant
 */
[[nodiscard]] bool isDegenerate(const std::vector<double>& values) {
  if (values.size() <= 1U) {
    return true;
  }
  const auto [minIt, maxIt] = std::ranges::minmax_element(values);
  return (*maxIt - *minIt) <= 1e-9;
}

/**
 * @brief Sort qubits by descending interaction degree
 */
[[nodiscard]] std::vector<std::size_t>
degreeOrder(const InteractionGraph& graph,
            const std::vector<std::size_t>& vertices) {
  auto order = vertices;
  std::ranges::stable_sort(order,
                           [&graph](const std::size_t a, const std::size_t b) {
                             return graph.degree[a] > graph.degree[b];
                           });
  return order;
}

/**
 * @brief Recursively bisect a vertex set and append the resulting level order
 */
void bisect(const InteractionGraph& graph,
            const std::vector<std::size_t>& vertices,
            std::vector<std::size_t>& order) {
  if (vertices.size() <= 1U) {
    order.insert(order.end(), vertices.begin(), vertices.end());
    return;
  }

  const auto fiedler = fiedlerVector(graph, vertices);
  std::vector<std::pair<double, std::size_t>> keyed;
  keyed.reserve(vertices.size());

  if (isDegenerate(fiedler)) {
    const auto byDegree = degreeOrder(graph, vertices);
    for (std::size_t i = 0U; i < byDegree.size(); ++i) {
      keyed.emplace_back(-static_cast<double>(i), byDegree[i]);
    }
  } else {
    for (std::size_t i = 0U; i < vertices.size(); ++i) {
      keyed.emplace_back(fiedler[i], vertices[i]);
    }
  }
  std::ranges::sort(keyed, {}, &std::pair<double, std::size_t>::first);

  const auto mid = keyed.size() / 2U;
  std::vector<std::size_t> left;
  std::vector<std::size_t> right;
  left.reserve(mid);
  right.reserve(keyed.size() - mid);
  for (std::size_t i = 0U; i < mid; ++i) {
    left.emplace_back(keyed[i].second);
  }
  for (std::size_t i = mid; i < keyed.size(); ++i) {
    right.emplace_back(keyed[i].second);
  }

  std::vector<std::size_t> leftOrder;
  std::vector<std::size_t> rightOrder;
  bisect(graph, left, leftOrder);
  bisect(graph, right, rightOrder);

  if (!leftOrder.empty() && !rightOrder.empty()) {
    const auto boundary = leftOrder.back();
    const auto connectFront = graph.weight[boundary][rightOrder.front()];
    const auto connectBack = graph.weight[boundary][rightOrder.back()];
    if (connectBack > connectFront) {
      std::ranges::reverse(rightOrder);
    }
  }

  order.insert(order.end(), leftOrder.begin(), leftOrder.end());
  order.insert(order.end(), rightOrder.begin(), rightOrder.end());
}

/**
 * @brief Append qubits that no strategy placed, in physical order
 */
void completeOrder(std::vector<std::size_t>& order, const std::size_t n) {
  std::vector<bool> placed(n, false);
  for (const auto qubit : order) {
    if (qubit < n) {
      placed[qubit] = true;
    }
  }
  for (std::size_t qubit = 0U; qubit < n; ++qubit) {
    if (!placed[qubit]) {
      order.emplace_back(qubit);
    }
  }
}

} // namespace

std::vector<std::size_t>
interactionDegrees(const qc::QuantumComputation& circuit) {
  return buildInteractionGraph(circuit).degree;
}

qc::Permutation computeVariableOrder(const qc::QuantumComputation& circuit,
                                     const OrderingStrategy strategy) {
  const auto numQubits = circuit.getNqubits();
  if (numQubits == 0U) {
    return {};
  }

  const auto graph = buildInteractionGraph(circuit);
  std::vector<std::size_t> order; // order[level] = qubit

  switch (strategy) {
  case OrderingStrategy::Identity: {
    // Return the complete identity map instead of an empty map. An empty map
    // would crash the simulator when a circuit contains an uncontrolled SWAP,
    // because dd::applyVirtualOperation calls Permutation::at and is marked
    // noexcept.
    order.resize(numQubits);
    std::iota(order.begin(), order.end(), 0U);
    break;
  }
  case OrderingStrategy::Degree: {
    std::vector<std::size_t> vertices(numQubits);
    std::iota(vertices.begin(), vertices.end(), 0U);
    const auto byDegree = degreeOrder(graph, vertices);
    order.assign(byDegree.rbegin(), byDegree.rend());
    break;
  }
  case OrderingStrategy::Spectral: {
    std::vector<std::size_t> vertices(numQubits);
    std::iota(vertices.begin(), vertices.end(), 0U);
    auto fiedler = fiedlerVector(graph, vertices);
    if (isDegenerate(fiedler)) {
      const auto byDegree = degreeOrder(graph, vertices);
      order.assign(byDegree.rbegin(), byDegree.rend());
    } else {
      std::vector<std::pair<double, std::size_t>> keyed;
      keyed.reserve(numQubits);
      for (std::size_t i = 0U; i < numQubits; ++i) {
        keyed.emplace_back(fiedler[i], i);
      }
      std::ranges::sort(keyed, {}, &std::pair<double, std::size_t>::first);
      for (const auto& [value, qubit] : keyed) {
        order.emplace_back(qubit);
      }
    }
    break;
  }
  case OrderingStrategy::RecursiveBisection: {
    std::vector<std::size_t> vertices(numQubits);
    std::iota(vertices.begin(), vertices.end(), 0U);
    bisect(graph, vertices, order);
    break;
  }
  }

  completeOrder(order, numQubits);

  qc::Permutation permutation;
  for (std::size_t level = 0U; level < order.size(); ++level) {
    permutation[static_cast<qc::Qubit>(order[level])] =
        static_cast<qc::Qubit>(level);
  }
  return permutation;
}

const char* toString(const OrderingStrategy strategy) {
  switch (strategy) {
  case OrderingStrategy::Identity:
    return "identity";
  case OrderingStrategy::Degree:
    return "degree";
  case OrderingStrategy::Spectral:
    return "spectral";
  case OrderingStrategy::RecursiveBisection:
    return "recursive-bisection";
  }
  return "unknown";
}

} // namespace dd
