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
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <numeric>
#include <utility>
#include <vector>

namespace dd {

namespace {

/**
 * @brief Weighted circuit structure shared by all static ordering strategies
 */
struct QuantumOrderingProblem {
  /// The number of qubits, which is also the number of vertices
  std::size_t numQubits = 0U;
  /// The number of multi-qubit gates that use a qubit as a control
  std::vector<std::size_t> controlCount;
  /// The number of nontrivial parameterized gates that act on a qubit
  std::vector<std::size_t> parameterizedGateCount;
  /// The number of multi-qubit gates that act on a qubit
  std::vector<std::size_t> supportCount;
  /// One distinct support set for every multi-qubit gate
  std::vector<std::vector<std::size_t>> hyperedges;
  /// The number of gates behind every hyperedge
  std::vector<double> hyperedgeWeights;
  /// Whether the circuit contains an operation that is not unitary
  bool hasNonUnitaryOperation = false;
};

/**
 * @brief The weighted interaction graph obtained from the quantum hypergraph
 */
struct InteractionGraph {
  /// The number of qubits, which is also the number of vertices
  std::size_t numQubits = 0U;
  /// The normalized gate count between every pair of qubits
  std::vector<std::vector<double>> weight;
  /// The sum of the incident normalized gate counts of every qubit
  std::vector<double> degree;
};

/**
 * @brief Return whether all parameters are integer multiples of pi/2
 * @details The scoring method treats these rotations as trivial because their
 * matrix entries are limited to 0, 1, -1, i, and -i.
 */
[[nodiscard]] bool hasOnlyTrivialParameters(const qc::Operation& op) {
  const auto& parameters = op.getParameter();
  if (parameters.empty()) {
    return true;
  }
  constexpr double TOLERANCE = 1e-12;
  return std::ranges::all_of(parameters, [](const qc::fp value) {
    const auto multiple = value / (qc::PI / 2.0);
    return std::abs(multiple - std::nearbyint(multiple)) <= TOLERANCE;
  });
}

/**
 * @brief Add one operation to the weighted quantum interaction structure
 */
void addQuantumOperation(
    QuantumOrderingProblem& problem,
    std::map<std::vector<std::size_t>, double>& hyperedgeWeights,
    const qc::Operation& op) {
  if (op.isCompoundOperation()) {
    const auto* compound =
        dynamic_cast<const qc::CompoundOperation*>(&op); // NOLINT
    if (compound != nullptr) {
      for (const auto& child : *compound) {
        addQuantumOperation(problem, hyperedgeWeights, *child);
      }
    }
    return;
  }

  if (op.isNonUnitaryOperation() || !op.isUnitary()) {
    problem.hasNonUnitaryOperation = true;
    return;
  }
  if (!op.isStandardOperation()) {
    return;
  }

  std::vector<std::size_t> support;
  support.reserve(op.getNqubits());
  for (const auto& control : op.getControls()) {
    support.emplace_back(control.qubit);
    if (control.qubit < problem.controlCount.size()) {
      ++problem.controlCount[control.qubit];
    }
  }
  for (const auto target : op.getTargets()) {
    support.emplace_back(target);
  }
  std::ranges::sort(support);
  support.erase(std::ranges::unique(support).begin(), support.end());
  if (support.size() >= 2U) {
    ++hyperedgeWeights[support];
    for (const auto qubit : support) {
      if (qubit < problem.supportCount.size()) {
        ++problem.supportCount[qubit];
      }
    }
  }

  if (!hasOnlyTrivialParameters(op)) {
    for (const auto target : op.getTargets()) {
      if (target < problem.parameterizedGateCount.size()) {
        ++problem.parameterizedGateCount[target];
      }
    }
  }
}

/**
 * @brief Build the weighted interaction structure of a quantum circuit
 */
[[nodiscard]] QuantumOrderingProblem
buildQuantumOrderingProblem(const qc::QuantumComputation& circuit) {
  QuantumOrderingProblem problem;
  problem.numQubits = circuit.getNqubits();
  problem.controlCount.assign(problem.numQubits, 0U);
  problem.parameterizedGateCount.assign(problem.numQubits, 0U);
  problem.supportCount.assign(problem.numQubits, 0U);

  std::map<std::vector<std::size_t>, double> hyperedgeWeights;
  for (const auto& op : circuit) {
    if (op) {
      addQuantumOperation(problem, hyperedgeWeights, *op);
    }
  }
  problem.hyperedges.reserve(hyperedgeWeights.size());
  problem.hyperedgeWeights.reserve(hyperedgeWeights.size());
  for (auto& [support, weight] : hyperedgeWeights) {
    problem.hyperedges.emplace_back(std::move(support));
    problem.hyperedgeWeights.emplace_back(weight);
  }
  return problem;
}

/**
 * @brief Compute the normalized pairwise interaction graph
 * @details A hyperedge of size k contributes weight w/(k-1) to every incident
 * pair. This keeps one multi-qubit gate from contributing k times as much as
 * a two-qubit gate.
 */
[[nodiscard]] InteractionGraph
buildInteractionGraph(const QuantumOrderingProblem& problem) {
  InteractionGraph graph;
  graph.numQubits = problem.numQubits;
  graph.weight.assign(graph.numQubits,
                      std::vector<double>(graph.numQubits, 0.0));
  graph.degree.assign(graph.numQubits, 0.0);

  for (std::size_t index = 0U; index < problem.hyperedges.size(); ++index) {
    const auto& edge = problem.hyperedges[index];
    if (edge.size() < 2U) {
      continue;
    }
    const auto pairWeight =
        problem.hyperedgeWeights[index] / static_cast<double>(edge.size() - 1U);
    for (std::size_t i = 0U; i < edge.size(); ++i) {
      for (std::size_t j = i + 1U; j < edge.size(); ++j) {
        const auto a = edge[i];
        const auto b = edge[j];
        if (a == b || a >= graph.numQubits || b >= graph.numQubits) {
          continue;
        }
        graph.weight[a][b] += pairWeight;
        graph.weight[b][a] += pairWeight;
        graph.degree[a] += pairWeight;
        graph.degree[b] += pairWeight;
      }
    }
  }
  return graph;
}

/**
 * @brief FORCE variable order
 * @details Every hyperedge pulls its qubits toward the center of gravity of
 * the hyperedge. The qubits are sorted by the mean center of gravity of the
 * hyperedges that contain them, and the process repeats until the order stops
 * changing. The method is the FORCE heuristic of Aloul, Markov, and Sakallah,
 * GLSVLSI 2003, doi:10.1145/764808.764839.
 * @param problem The weighted quantum interaction structure
 * @return The qubit at every level
 */
[[nodiscard]] std::vector<std::size_t>
forceOrder(const QuantumOrderingProblem& problem) {
  const auto numQubits = problem.numQubits;
  std::vector<std::size_t> order(numQubits);
  std::iota(order.begin(), order.end(), 0U);
  if (problem.hyperedges.empty()) {
    return order;
  }

  constexpr std::size_t maxIterations = 50U;
  std::vector<std::size_t> position(numQubits, 0U);
  std::vector<double> center(problem.hyperedges.size(), 0.0);
  for (std::size_t iteration = 0U; iteration < maxIterations; ++iteration) {
    for (std::size_t level = 0U; level < numQubits; ++level) {
      position[order[level]] = level;
    }
    for (std::size_t index = 0U; index < problem.hyperedges.size(); ++index) {
      double sum = 0.0;
      for (const auto qubit : problem.hyperedges[index]) {
        sum += static_cast<double>(position[qubit]);
      }
      center[index] =
          sum / static_cast<double>(problem.hyperedges[index].size());
    }
    std::vector<double> span(numQubits, 0.0);
    std::vector<double> weightSum(numQubits, 0.0);
    for (std::size_t index = 0U; index < problem.hyperedges.size(); ++index) {
      for (const auto qubit : problem.hyperedges[index]) {
        span[qubit] += center[index] * problem.hyperedgeWeights[index];
        weightSum[qubit] += problem.hyperedgeWeights[index];
      }
    }
    for (std::size_t qubit = 0U; qubit < numQubits; ++qubit) {
      if (weightSum[qubit] > 0.0) {
        span[qubit] /= weightSum[qubit];
      } else {
        span[qubit] = static_cast<double>(position[qubit]);
      }
    }
    std::vector<std::size_t> next(numQubits);
    std::iota(next.begin(), next.end(), 0U);
    std::ranges::stable_sort(next, {}, [&](const std::size_t qubit) {
      return std::pair{span[qubit], position[qubit]};
    });
    if (next == order) {
      break;
    }
    order = next;
  }
  return order;
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
 * @brief Order qubits by the quantum gate features of the circuit
 * @details Qubits with more control uses and more nontrivial parameterized
 * gates act near the root. Control counts determine rank scores while a
 * monotone logarithm of the parameterized-gate count adjusts that score.
 */
[[nodiscard]] std::vector<std::size_t>
quantumScoreOrder(const QuantumOrderingProblem& problem) {
  const auto numQubits = problem.numQubits;
  std::vector<std::size_t> order(numQubits);
  std::iota(order.begin(), order.end(), 0U);
  if (problem.hasNonUnitaryOperation) {
    return order;
  }

  std::vector<std::size_t> byControlCount = order;
  std::ranges::stable_sort(byControlCount, {},
                           [&problem](const std::size_t qubit) {
                             return problem.controlCount[qubit];
                           });

  std::vector<double> score(numQubits, 1.0);
  double rankScore = 1.0;
  std::size_t previousCount = problem.controlCount[byControlCount.front()];
  for (const auto qubit : byControlCount) {
    if (problem.controlCount[qubit] != previousCount) {
      rankScore *= 2.0;
      previousCount = problem.controlCount[qubit];
    }
    score[qubit] = rankScore;
  }

  for (const auto qubit : order) {
    const auto parameterizedCount = problem.parameterizedGateCount[qubit];
    if (parameterizedCount > 0U) {
      score[qubit] *= 1.0 + std::log1p(static_cast<double>(parameterizedCount));
    }
  }

  std::ranges::stable_sort(order, {}, [&score](const std::size_t qubit) {
    return std::pair{score[qubit], qubit};
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
 * @brief A bisection of a hypergraph vertex set
 */
struct HyperBisection {
  /// The vertices assigned to the first part
  std::vector<std::size_t> left;
  /// The vertices assigned to the second part
  std::vector<std::size_t> right;
};

/**
 * @brief Bisect a vertex set of a hypergraph
 * @details The initial split follows the Fiedler vector of the pairwise
 * interaction graph. The refinement then moves single vertices between the two
 * parts while the move reduces the number of cut hyperedges and keeps the two
 * parts balanced. A cut hyperedge is a gate whose qubits end up in both parts.
 * This is the partitioning step of MINCE, which keeps every gate as one
 * hyperedge instead of expanding it into pairwise edges.
 * @param graph The pairwise interaction graph, used for the initial split
 * @param edges The hyperedges, one per distinct gate support
 * @param weights The number of gates behind every hyperedge
 * @param vertices The vertex set to bisect
 * @return The two parts
 */
[[nodiscard]] HyperBisection
bisectHypergraph(const InteractionGraph& graph,
                 const std::vector<std::vector<std::size_t>>& edges,
                 const std::vector<double>& weights,
                 const std::vector<std::size_t>& vertices) {
  const auto n = vertices.size();
  std::vector<bool> inSet(graph.numQubits, false);
  for (const auto qubit : vertices) {
    inSet[qubit] = true;
  }

  // Project the hyperedges onto the current vertex set.
  std::vector<std::vector<std::size_t>> localEdges;
  std::vector<double> localWeights;
  for (std::size_t index = 0U; index < edges.size(); ++index) {
    std::vector<std::size_t> projected;
    for (const auto qubit : edges[index]) {
      if (inSet[qubit]) {
        projected.emplace_back(qubit);
      }
    }
    if (projected.size() >= 2U) {
      localEdges.emplace_back(std::move(projected));
      localWeights.emplace_back(weights[index]);
    }
  }
  std::vector<std::vector<std::size_t>> incident(graph.numQubits);
  for (std::size_t index = 0U; index < localEdges.size(); ++index) {
    for (const auto qubit : localEdges[index]) {
      incident[qubit].emplace_back(index);
    }
  }

  // Initial split: the first half of the Fiedler order.
  std::vector<double> fiedler = fiedlerVector(graph, vertices);
  std::vector<std::size_t> sorted = vertices;
  if (isDegenerate(fiedler)) {
    sorted = degreeOrder(graph, vertices);
  } else {
    std::vector<std::pair<double, std::size_t>> keyed;
    keyed.reserve(n);
    for (std::size_t index = 0U; index < n; ++index) {
      keyed.emplace_back(fiedler[index], vertices[index]);
    }
    std::ranges::stable_sort(keyed, {}, &std::pair<double, std::size_t>::first);
    for (std::size_t index = 0U; index < n; ++index) {
      sorted[index] = keyed[index].second;
    }
  }

  std::vector<bool> side(graph.numQubits, false); // true = right part
  for (std::size_t index = 0U; index < sorted.size(); ++index) {
    side[sorted[index]] = index >= sorted.size() / 2U;
  }
  std::size_t leftSize = sorted.size() / 2U;
  std::size_t rightSize = sorted.size() - leftSize;

  std::vector<std::size_t> leftCount(localEdges.size(), 0U);
  for (std::size_t index = 0U; index < localEdges.size(); ++index) {
    for (const auto qubit : localEdges[index]) {
      if (!side[qubit]) {
        ++leftCount[index];
      }
    }
  }

  // Balanced single-vertex moves while the cut decreases.
  for (std::size_t step = 0U; step < n; ++step) {
    std::size_t bestVertex = n;
    double bestDelta = 0.0;
    for (const auto qubit : sorted) {
      const bool fromLeft = !side[qubit];
      const auto newLeft = fromLeft ? leftSize - 1U : leftSize + 1U;
      const auto newRight = fromLeft ? rightSize + 1U : rightSize - 1U;
      const auto newDiff =
          newLeft > newRight ? newLeft - newRight : newRight - newLeft;
      // Never empty a part: the recursion would then repeat the same split.
      if (newLeft == 0U || newRight == 0U || newDiff > 2U) {
        continue;
      }
      double delta = 0.0;
      for (const auto index : incident[qubit]) {
        const auto size = localEdges[index].size();
        const auto beforeLeft = leftCount[index];
        const auto beforeRight = size - beforeLeft;
        const auto afterLeft = fromLeft ? beforeLeft - 1U : beforeLeft + 1U;
        const auto afterRight = size - afterLeft;
        const bool cutBefore = beforeLeft > 0U && beforeRight > 0U;
        const bool cutAfter = afterLeft > 0U && afterRight > 0U;
        delta += localWeights[index] * (static_cast<double>(cutAfter ? 1 : 0) -
                                        static_cast<double>(cutBefore ? 1 : 0));
      }
      if (delta < bestDelta) {
        bestDelta = delta;
        bestVertex = qubit;
      }
    }
    if (bestVertex == n) {
      break;
    }
    const bool fromLeft = !side[bestVertex];
    side[bestVertex] = !side[bestVertex];
    for (const auto index : incident[bestVertex]) {
      if (fromLeft) {
        --leftCount[index];
      } else {
        ++leftCount[index];
      }
    }
    if (fromLeft) {
      --leftSize;
      ++rightSize;
    } else {
      ++leftSize;
      --rightSize;
    }
  }

  HyperBisection result;
  for (const auto qubit : vertices) {
    (side[qubit] ? result.right : result.left).emplace_back(qubit);
  }
  return result;
}

/**
 * @brief Recursively bisect the hypergraph
 */
void minceRecurse(const InteractionGraph& graph,
                  const std::vector<std::vector<std::size_t>>& edges,
                  const std::vector<double>& weights,
                  const std::vector<std::size_t>& vertices,
                  std::vector<std::size_t>& order) {
  if (vertices.size() <= 1U) {
    order.insert(order.end(), vertices.begin(), vertices.end());
    return;
  }
  auto [left, right] = bisectHypergraph(graph, edges, weights, vertices);
  if (left.empty() || right.empty()) {
    // The refinement must never empty a part. Fall back to a plain half split
    // so that the recursion always shrinks the vertex set.
    const auto middle = vertices.size() / 2U;
    left.assign(vertices.begin(),
                vertices.begin() + static_cast<std::ptrdiff_t>(middle));
    right.assign(vertices.begin() + static_cast<std::ptrdiff_t>(middle),
                 vertices.end());
  }
  std::vector<std::size_t> leftOrder;
  std::vector<std::size_t> rightOrder;
  minceRecurse(graph, edges, weights, left, leftOrder);
  minceRecurse(graph, edges, weights, right, rightOrder);
  // The partition fixes the groups but not the direction inside a group. Pick
  // the direction that keeps the boundary edge inside the final order.
  if (!leftOrder.empty() && !rightOrder.empty()) {
    const auto weight = [&](const std::size_t a, const std::size_t b) {
      return graph.weight[a][b];
    };
    const auto lf = leftOrder;
    const auto lr =
        std::vector<std::size_t>(leftOrder.rbegin(), leftOrder.rend());
    const auto rf = rightOrder;
    const auto rr =
        std::vector<std::size_t>(rightOrder.rbegin(), rightOrder.rend());
    const std::array<std::pair<const std::vector<std::size_t>*,
                               const std::vector<std::size_t>*>,
                     4>
        options{{{&lf, &rf}, {&lf, &rr}, {&lr, &rf}, {&lr, &rr}}};
    const auto* bestLeft = &lf;
    const auto* bestRight = &rf;
    auto bestWeight = weight(lf.back(), rf.front());
    for (const auto& [candidateLeft, candidateRight] : options) {
      const auto current =
          weight(candidateLeft->back(), candidateRight->front());
      if (current > bestWeight) {
        bestWeight = current;
        bestLeft = candidateLeft;
        bestRight = candidateRight;
      }
    }
    leftOrder = *bestLeft;
    rightOrder = *bestRight;
  }
  order.insert(order.end(), leftOrder.begin(), leftOrder.end());
  order.insert(order.end(), rightOrder.begin(), rightOrder.end());
}

/**
 * @brief MINCE variable order
 * @details Recursively bisects the hypergraph of multi-qubit gates, minimizing
 * the number of cut gates, and orders the qubits by the depth-first traversal
 * of the partition tree. The method is MINCE, the hypergraph partitioner of
 * Aloul, Markov, and Sakallah, JUCS 10(12):1562-1596, 2004.
 * @param problem The weighted quantum interaction structure
 * @param graph Its pairwise interaction graph, used for the initial split
 * @return The qubit at every level
 */
[[nodiscard]] std::vector<std::size_t>
minceOrder(const QuantumOrderingProblem& problem,
           const InteractionGraph& graph) {
  const auto numQubits = problem.numQubits;
  std::vector<std::size_t> vertices(numQubits);
  std::iota(vertices.begin(), vertices.end(), 0U);
  std::vector<std::size_t> order;
  order.reserve(numQubits);
  minceRecurse(graph, problem.hyperedges, problem.hyperedgeWeights, vertices,
               order);
  return order;
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
  return buildQuantumOrderingProblem(circuit).supportCount;
}

qc::Permutation computeVariableOrder(const qc::QuantumComputation& circuit,
                                     const OrderingStrategy strategy) {
  const auto numQubits = circuit.getNqubits();
  if (numQubits == 0U) {
    return {};
  }

  const auto problem = buildQuantumOrderingProblem(circuit);
  const auto graph = buildInteractionGraph(problem);
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
  case OrderingStrategy::Force: {
    order = forceOrder(problem);
    break;
  }
  case OrderingStrategy::Mince: {
    order = minceOrder(problem, graph);
    break;
  }
  case OrderingStrategy::QuantumScore: {
    order = quantumScoreOrder(problem);
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
  case OrderingStrategy::Force:
    return "force";
  case OrderingStrategy::Mince:
    return "mince";
  case OrderingStrategy::QuantumScore:
    return "quantum-score";
  }
  return "unknown";
}

} // namespace dd
