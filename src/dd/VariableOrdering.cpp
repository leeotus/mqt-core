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
#include <set>
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
 * @brief Compute a greedy minimum-fill elimination order
 * @details The function repeatedly eliminates the qubit whose elimination adds
 * the fewest edges between its remaining neighbors. Minimum-fill elimination
 * approximates the treewidth of the interaction graph, which bounds the number
 * of decision diagram nodes.
 */
[[nodiscard]] std::vector<std::size_t>
minFillEliminationOrder(const InteractionGraph& graph) {
  const auto n = graph.numQubits;
  std::vector<std::set<std::size_t>> adjacency(n);
  for (std::size_t a = 0U; a < n; ++a) {
    for (std::size_t b = a + 1U; b < n; ++b) {
      if (graph.weight[a][b] > 0U) {
        adjacency[a].insert(b);
        adjacency[b].insert(a);
      }
    }
  }

  std::vector<bool> eliminated(n, false);
  std::vector<std::size_t> order;
  order.reserve(n);

  for (std::size_t step = 0U; step < n; ++step) {
    std::size_t best = n;
    std::size_t bestFill = std::numeric_limits<std::size_t>::max();
    std::size_t bestDegree = std::numeric_limits<std::size_t>::max();
    for (std::size_t candidate = 0U; candidate < n; ++candidate) {
      if (eliminated[candidate]) {
        continue;
      }
      std::size_t fill = 0U;
      const auto& neighbors = adjacency[candidate];
      for (auto it = neighbors.begin(); it != neighbors.end(); ++it) {
        auto jt = it;
        for (++jt; jt != neighbors.end(); ++jt) {
          if (adjacency[*it].find(*jt) == adjacency[*it].end()) {
            ++fill;
          }
        }
      }
      const auto degree = neighbors.size();
      if (fill < bestFill || (fill == bestFill && degree < bestDegree)) {
        best = candidate;
        bestFill = fill;
        bestDegree = degree;
      }
    }

    const std::vector<std::size_t> neighbors(adjacency[best].begin(),
                                             adjacency[best].end());
    for (std::size_t i = 0U; i < neighbors.size(); ++i) {
      for (std::size_t j = i + 1U; j < neighbors.size(); ++j) {
        adjacency[neighbors[i]].insert(neighbors[j]);
        adjacency[neighbors[j]].insert(neighbors[i]);
      }
    }
    for (const auto neighbor : neighbors) {
      adjacency[neighbor].erase(best);
    }
    adjacency[best].clear();
    eliminated[best] = true;
    order.emplace_back(best);
  }
  return order;
}

/**
 * @brief Compute the control-dependency order used by MQT DDSIM
 * @details The function mirrors `DDMinimizer::createGateBasedPermutation` from
 * MQT DDSIM. A qubit that no other qubit controls is ready first, so targets
 * receive lower levels than the controls that act on them.
 */
[[nodiscard]] std::vector<std::size_t>
gateDependencyOrder(const qc::QuantumComputation& circuit) {
  const auto n = circuit.getNqubits();
  std::vector<std::size_t> identity(n);
  std::iota(identity.begin(), identity.end(), 0U);
  if (n < 2U) {
    return identity;
  }

  std::vector<std::set<std::size_t>> targetToControls(n);
  std::vector<std::size_t> openControls(n, 0U);

  const auto collect = [&targetToControls, &openControls](
                           const auto& self, const qc::Operation& op) -> void {
    if (op.isCompoundOperation()) {
      const auto* compound =
          dynamic_cast<const qc::CompoundOperation*>(&op); // NOLINT
      if (compound != nullptr) {
        for (const auto& child : *compound) {
          self(self, *child);
        }
      }
      return;
    }
    if (!op.isStandardOperation() || op.getType() == qc::Z) {
      return;
    }
    for (const auto& control : op.getControls()) {
      for (const auto target : op.getTargets()) {
        if (control.qubit == target) {
          continue;
        }
        if (targetToControls[target].insert(control.qubit).second) {
          ++openControls[control.qubit];
        }
      }
    }
  };

  for (const auto& op : circuit) {
    if (op) {
      collect(collect, *op);
    }
  }

  std::set<std::size_t> ready;
  for (std::size_t qubit = 0U; qubit < n; ++qubit) {
    if (openControls[qubit] == 0U) {
      ready.insert(qubit);
    }
  }
  if (ready.size() == n) {
    return identity;
  }

  std::vector<std::size_t> position(n, 0U);
  std::size_t next = 0U;
  while (!ready.empty()) {
    const auto current = *ready.begin();
    ready.erase(ready.begin());
    position[current] = next++;
    for (const auto control : targetToControls[current]) {
      if (--openControls[control] == 0U) {
        ready.insert(control);
      }
    }
  }
  if (next != n) {
    return identity;
  }
  return position;
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
  case OrderingStrategy::MinFill: {
    order = minFillEliminationOrder(graph);
    std::ranges::reverse(order);
    break;
  }
  case OrderingStrategy::GateDependency: {
    order = gateDependencyOrder(circuit);
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
  case OrderingStrategy::MinFill:
    return "min-fill";
  case OrderingStrategy::GateDependency:
    return "gate-dependency";
  }
  return "unknown";
}

} // namespace dd
