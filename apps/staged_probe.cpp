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
 * @file staged_probe.cpp
 * @brief Probe whether a two-stage variable order beats a single global order
 * @details The tool builds a circuit once with a single global order and once
 * in two stages: the first half of the gates uses the global order, then the
 * diagram is reordered to the order computed from the second half, and the
 * remaining gates are applied in that order.
 */

#include "dd/FunctionalityConstruction.hpp"
#include "dd/Operations.hpp"
#include "dd/Package.hpp"
#include "dd/Reordering.hpp"
#include "dd/VariableOrdering.hpp"
#include "ir/QuantumComputation.hpp"
#include "qasm3/Importer.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Measurement {
  std::size_t nodes = 0U;
  std::size_t peak = 0U;
  double ms = 0.0;
};

dd::OrderingStrategy parseStrategy(const std::string& name) {
  if (name == "degree") {
    return dd::OrderingStrategy::Degree;
  }
  if (name == "recursive_bisection") {
    return dd::OrderingStrategy::RecursiveBisection;
  }
  if (name == "rank_bisection") {
    return dd::OrderingStrategy::RankBisection;
  }
  return dd::OrderingStrategy::Spectral;
}

std::string circuitName(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1U);
}

/// Apply the operations in [begin, end) to an existing matrix diagram.
Measurement buildRange(const qc::QuantumComputation& circuit, dd::Package& dd,
                       dd::MatrixDD& root, qc::Permutation& layout,
                       const std::size_t begin, const std::size_t end) {
  Measurement result;
  result.peak = root.size();
  const auto start = Clock::now();
  auto it = std::next(circuit.begin(), static_cast<std::ptrdiff_t>(begin));
  const auto last =
      std::next(circuit.begin(), static_cast<std::ptrdiff_t>(end));
  for (; it != last; ++it) {
    const auto& op = *it;
    if (op->getType() == qc::OpType::SWAP && !op->isControlled()) {
      const auto& targets = op->getTargets();
      std::swap(layout.at(targets[0U]), layout.at(targets[1U]));
      continue;
    }
    root = dd::applyUnitaryOperation(*op, root, dd, layout);
    result.peak = std::max(result.peak, root.size());
  }
  result.nodes = root.size();
  const auto stop = Clock::now();
  result.ms = std::chrono::duration<double, std::milli>(stop - start).count();
  return result;
}

/// Build the suffix circuit that contains the gates in [begin, end).
qc::QuantumComputation suffixCircuit(const qc::QuantumComputation& circuit,
                                     const std::size_t begin) {
  qc::QuantumComputation suffix(circuit.getNqubits());
  for (auto it = std::next(circuit.begin(), static_cast<std::ptrdiff_t>(begin));
       it != circuit.end(); ++it) {
    suffix.emplace_back((*it)->clone());
  }
  return suffix;
}

} // namespace

int main(int argc, char** argv) {
  std::string strategy = "spectral";
  double fraction = 0.5;
  std::vector<std::string> circuits;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--strategy" && i + 1 < argc) {
      strategy = argv[++i];
    } else if (arg == "--fraction" && i + 1 < argc) {
      fraction = std::atof(argv[++i]);
    } else if (arg == "-h" || arg == "--help") {
      std::printf(
          "usage: %s [--strategy NAME] [--fraction F] circuit.qasm ...\n",
          argv[0]);
      return 0;
    } else {
      circuits.emplace_back(arg);
    }
  }
  if (circuits.empty()) {
    std::fprintf(stderr, "no circuits given\n");
    return 2;
  }

  std::printf("circuit,n,gates,half,global_nodes,global_peak,global_ms,"
              "staged_nodes,staged_peak,staged_ms,reorder_ms,layout_changed\n");

  for (const auto& path : circuits) {
    qc::QuantumComputation circuit;
    try {
      circuit = qasm3::Importer::importf(path);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "%s: cannot read: %s\n", path.c_str(), error.what());
      continue;
    }
    circuit.removeFinalMeasurements();
    const auto numQubits = circuit.getNqubits();
    const auto gates = circuit.size();
    if (numQubits == 0U || gates < 2U) {
      continue;
    }
    const auto half =
        static_cast<std::size_t>(fraction * static_cast<double>(gates));
    if (half == 0U || half >= gates) {
      continue;
    }

    const auto globalLayout =
        dd::computeVariableOrder(circuit, parseStrategy(strategy));
    const auto suffix = suffixCircuit(circuit, half);
    const auto suffixLayout =
        dd::computeVariableOrder(suffix, parseStrategy(strategy));

    Measurement global;
    {
      auto package = std::make_unique<dd::Package>(numQubits);
      auto root = package->createInitialMatrix(circuit.getAncillary());
      auto layout = globalLayout;
      global = buildRange(circuit, *package, root, layout, 0U, gates);
      root = package->reduceAncillae(root, circuit.getAncillary());
      root = package->reduceGarbage(root, circuit.getGarbage());
      global.nodes = root.size();
    }

    Measurement staged;
    double reorderMs = 0.0;
    bool layoutChanged = false;
    {
      auto package = std::make_unique<dd::Package>(numQubits);
      auto root = package->createInitialMatrix(circuit.getAncillary());
      auto layout = globalLayout;
      const auto first = buildRange(circuit, *package, root, layout, 0U, half);

      auto target = suffixLayout;
      const auto reorderStart = Clock::now();
      dd::changePermutation(root, layout, target, *package);
      const auto reorderStop = Clock::now();
      reorderMs =
          std::chrono::duration<double, std::milli>(reorderStop - reorderStart)
              .count();
      layoutChanged = target != globalLayout;

      const auto second =
          buildRange(circuit, *package, root, layout, half, gates);
      root = package->reduceAncillae(root, circuit.getAncillary());
      root = package->reduceGarbage(root, circuit.getGarbage());
      staged.nodes = root.size();
      staged.peak = std::max({first.peak, second.peak, root.size()});
      staged.ms = first.ms + second.ms + reorderMs;
    }

    std::printf("%s,%zu,%zu,%zu,%zu,%zu,%.3f,%zu,%zu,%.3f,%.3f,%d\n",
                circuitName(path).c_str(), numQubits, gates, half, global.nodes,
                global.peak, global.ms, staged.nodes, staged.peak, staged.ms,
                reorderMs, layoutChanged ? 1 : 0);
    std::fflush(stdout);
  }
  return 0;
}
