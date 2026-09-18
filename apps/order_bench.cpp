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
 * @file order_bench.cpp
 * @brief Measure decision diagram construction with and without reordering
 * @details For every input circuit the program reports the size and the
 * construction time of the functional decision diagram for three cases: the
 * original physical order, several static variable orders, and dynamic
 * reordering with sifting. The program prints one CSV row per circuit.
 */

#include "RevLibReader.hpp"
#include "dd/FunctionalityConstruction.hpp"
#include "dd/Package.hpp"
#include "dd/Reordering.hpp"
#include "dd/Simulation.hpp"
#include "dd/StateGeneration.hpp"
#include "dd/VariableOrdering.hpp"
#include "ir/Permutation.hpp"
#include "ir/QuantumComputation.hpp"
#include "qasm3/Importer.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Measurement {
  std::size_t nodes = 0U;
  double timeMs = 0.0;
  std::size_t exchanges = 0U;
};

struct Options {
  std::vector<std::string> circuits;
  std::string mode = "functionality";
  std::size_t maxQubits = 40U;
  std::size_t dynamicThreshold = 1000U;
  std::size_t siftPasses = 1U;
  std::string dynamicStart = "identity";
  bool skipPostHoc = false;
  bool header = false;
};

[[nodiscard]] Options parseOptions(const int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--header") {
      options.header = true;
    } else if (arg == "--max-qubits" && i + 1 < argc) {
      options.maxQubits = std::strtoul(argv[++i], nullptr, 10);
    } else if (arg == "--dynamic-threshold" && i + 1 < argc) {
      options.dynamicThreshold = std::strtoul(argv[++i], nullptr, 10);
    } else if (arg == "--skip-post-hoc") {
      options.skipPostHoc = true;
    } else if (arg == "--sift-passes" && i + 1 < argc) {
      options.siftPasses = std::strtoul(argv[++i], nullptr, 10);
    } else if (arg == "--dynamic-start" && i + 1 < argc) {
      options.dynamicStart = argv[++i];
    } else if (arg == "--mode" && i + 1 < argc) {
      options.mode = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      options.circuits.clear();
      options.mode.clear();
    } else {
      options.circuits.emplace_back(arg);
    }
  }
  return options;
}

void printUsage() {
  std::printf(
      "usage: mqt-core-order-bench [options] circuit.qasm ...\n"
      "\n"
      "  --header              print the CSV header and exit\n"
      "  --mode functionality  build a unitary (default)\n"
      "  --mode state          build a state vector\n"
      "  --max-qubits N        skip circuits with more qubits (default 40)\n"
      "  --dynamic-threshold N size that triggers construction-time reordering "
      "(default 1000)\n"
      "  --skip-post-hoc       skip the post-hoc sifting measurements\n"
      "  --sift-passes N       sweeps per reordering during construction "
      "(default 1)\n"
      "  --dynamic-start NAME  variable order that the dynamic construction "
      "starts from: identity, degree, spectral, recursive_bisection "
      "(default identity)\n"
      "  -h, --help            print this text\n");
}

void printHeader() {
  std::printf("circuit,qubits,gates,status,"
              "original_nodes,original_time_ms,"
              "static_degree_nodes,static_degree_time_ms,"
              "static_spectral_nodes,static_spectral_time_ms,"
              "static_recursive_bisection_nodes,"
              "static_recursive_bisection_time_ms,"
              "static_best_strategy,static_best_nodes,static_best_time_ms,"
              "dynamic_sifting_nodes,dynamic_sifting_time_ms,"
              "dynamic_sifting_exchanges,"
              "dynamic_sifting_spectral_nodes,dynamic_sifting_spectral_time_ms,"
              "dynamic_sifting_spectral_exchanges,"
              "dynamic_construction_nodes,dynamic_construction_time_ms,"
              "dynamic_construction_reorder_time_ms,"
              "dynamic_construction_reorderings,dynamic_construction_exchanges,"
              "dynamic_construction_peak_size,"
              "peak_rss_mb\n");
}

[[nodiscard]] double peakRssMb() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0.0;
  }
  return static_cast<double>(usage.ru_maxrss) / 1024.0;
}

[[nodiscard]] qc::Permutation identityPermutation(const std::size_t n) {
  qc::Permutation permutation;
  for (qc::Qubit qubit = 0; qubit < n; ++qubit) {
    permutation[qubit] = qubit;
  }
  return permutation;
}

/**
 * @brief Build a decision diagram in a fixed variable order
 */
[[nodiscard]] Measurement measureBuild(const qc::QuantumComputation& circuit,
                                       const qc::Permutation& layout,
                                       const bool buildState) {
  const auto numQubits = circuit.getNqubits();
  auto package = std::make_unique<dd::Package>(numQubits);
  auto reordered = circuit;
  reordered.initialLayout = layout;
  reordered.outputPermutation = layout;

  const auto start = Clock::now();
  const auto nodes = [&]() -> std::size_t {
    if (buildState) {
      return dd::simulate(reordered, dd::makeZeroState(numQubits, *package),
                          *package)
          .size();
    }
    return dd::buildFunctionality(reordered, *package).size();
  }();
  const auto stop = Clock::now();
  const std::chrono::duration<double, std::milli> elapsed = stop - start;
  return {nodes, elapsed.count(), 0U};
}

/**
 * @brief Build a decision diagram and reorder it with sifting
 */
[[nodiscard]] Measurement measureSifting(const qc::QuantumComputation& circuit,
                                         const qc::Permutation& startLayout,
                                         const bool buildState) {
  const auto numQubits = circuit.getNqubits();
  auto package = std::make_unique<dd::Package>(numQubits);
  auto reordered = circuit;
  reordered.initialLayout = startLayout;
  reordered.outputPermutation = startLayout;
  auto layout = startLayout;

  dd::SiftingStatistics statistics;
  const auto start = Clock::now();
  if (buildState) {
    auto root = dd::simulate(reordered, dd::makeZeroState(numQubits, *package),
                             *package);
    root = dd::sift(*package, root, layout, {}, &statistics);
    const auto stop = Clock::now();
    const std::chrono::duration<double, std::milli> elapsed = stop - start;
    return {root.size(), elapsed.count(), statistics.exchanges};
  }
  auto root = dd::buildFunctionality(reordered, *package);
  root = dd::sift(*package, root, layout, {}, &statistics);
  const auto stop = Clock::now();
  const std::chrono::duration<double, std::milli> elapsed = stop - start;
  return {root.size(), elapsed.count(), statistics.exchanges};
}

/**
 * @brief Build a decision diagram with threshold-triggered reordering
 */
[[nodiscard]] Measurement
measureDynamicConstruction(const qc::QuantumComputation& circuit,
                           const bool buildState, const std::size_t threshold,
                           const std::size_t siftPasses,
                           const qc::Permutation& startLayout,
                           dd::ConstructionStatistics* statistics) {
  const auto numQubits = circuit.getNqubits();
  auto package = std::make_unique<dd::Package>(numQubits);
  auto reordered = circuit;
  reordered.initialLayout = startLayout;

  dd::ConstructionOptions options;
  options.dynamic.enabled = true;
  options.dynamic.initialThreshold = threshold;
  options.dynamic.sifting.maxPasses = siftPasses;

  const auto start = Clock::now();
  std::size_t nodes = 0U;
  if (buildState) {
    nodes = dd::simulate(reordered, dd::makeZeroState(numQubits, *package),
                         *package, options, statistics)
                .size();
  } else {
    nodes =
        dd::buildFunctionality(reordered, *package, options, statistics).size();
  }
  const auto stop = Clock::now();
  const std::chrono::duration<double, std::milli> elapsed = stop - start;
  return {nodes, elapsed.count(), statistics->exchanges};
}

[[nodiscard]] std::string trim(const std::string& text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1U);
}

} // namespace

int main(int argc, char** argv) {
  const auto options = parseOptions(argc, argv);
  if (options.header) {
    printHeader();
    return 0;
  }
  if (options.mode.empty() || options.circuits.empty()) {
    printUsage();
    return options.mode.empty() ? 0 : 2;
  }
  const bool buildState = options.mode == "state";

  for (const auto& path : options.circuits) {
    qc::QuantumComputation circuit;
    try {
      if (path.ends_with(".real")) {
        circuit = mqt::apps::readRevLib(path);
      } else {
        circuit = qasm3::Importer::importf(path);
      }
    } catch (const std::exception& error) {
      std::fprintf(stderr, "%s: cannot read: %s\n", path.c_str(), error.what());
      std::printf("%s,,,read-error,,,,,,,,,,,,,,,,,,,,,,\n", path.c_str());
      continue;
    }
    circuit.removeFinalMeasurements();
    const auto nonUnitary = std::ranges::any_of(circuit, [](const auto& op) {
      return op != nullptr && (!op->isUnitary() || op->isIfElseOperation());
    });
    const auto numQubits = circuit.getNqubits();
    const auto gates = circuit.size();

    if (nonUnitary) {
      std::printf("%s,%zu,%zu,non-unitary,,,,,,,,,,,,,,,,,,,,,,\n",
                  path.c_str(), numQubits, gates);
      continue;
    }
    if (numQubits == 0U || numQubits > options.maxQubits) {
      std::printf("%s,%zu,%zu,too-many-qubits,,,,,,,,,,,,,,,,,,,,,,\n",
                  path.c_str(), numQubits, gates);
      continue;
    }

    const auto identity = identityPermutation(numQubits);
    const auto original = measureBuild(circuit, identity, buildState);

    const std::vector<std::pair<const char*, dd::OrderingStrategy>>
        staticStrategies{
            {"degree", dd::OrderingStrategy::Degree},
            {"spectral", dd::OrderingStrategy::Spectral},
            {"recursive_bisection", dd::OrderingStrategy::RecursiveBisection}};

    std::vector<std::pair<std::string, Measurement>> staticResults;
    for (const auto& [name, strategy] : staticStrategies) {
      const auto layout = dd::computeVariableOrder(circuit, strategy);
      staticResults.emplace_back(name,
                                 measureBuild(circuit, layout, buildState));
    }

    const auto* best = &staticResults.front();
    for (const auto& entry : staticResults) {
      if (entry.second.nodes < best->second.nodes) {
        best = &entry;
      }
    }

    Measurement sifting;
    Measurement siftingSpectral;
    if (!options.skipPostHoc) {
      sifting = measureSifting(circuit, identity, buildState);
      const auto spectralLayout =
          dd::computeVariableOrder(circuit, dd::OrderingStrategy::Spectral);
      siftingSpectral = measureSifting(circuit, spectralLayout, buildState);
    }
    // Determine the level order that the dynamic construction starts from.
    qc::Permutation dynamicStart = identity;
    if (options.dynamicStart != "identity") {
      const auto strategy = [&options]() -> dd::OrderingStrategy {
        if (options.dynamicStart == "degree") {
          return dd::OrderingStrategy::Degree;
        }
        if (options.dynamicStart == "spectral") {
          return dd::OrderingStrategy::Spectral;
        }
        if (options.dynamicStart == "recursive_bisection") {
          return dd::OrderingStrategy::RecursiveBisection;
        }
        return dd::OrderingStrategy::Identity;
      }();
      dynamicStart = dd::computeVariableOrder(circuit, strategy);
    }

    dd::ConstructionStatistics dynamicStatistics;
    const auto dynamicConstruction = measureDynamicConstruction(
        circuit, buildState, options.dynamicThreshold, options.siftPasses,
        dynamicStart, &dynamicStatistics);

    std::printf("%s,%zu,%zu,ok,%zu,%.3f", path.c_str(), numQubits, gates,
                original.nodes, original.timeMs);
    for (const auto& entry : staticResults) {
      std::printf(",%zu,%.3f", entry.second.nodes, entry.second.timeMs);
    }
    std::printf(",%s,%zu,%.3f", best->first.c_str(), best->second.nodes,
                best->second.timeMs);
    if (options.skipPostHoc) {
      std::printf(",,,,,,");
    } else {
      std::printf(",%zu,%.3f,%zu", sifting.nodes, sifting.timeMs,
                  sifting.exchanges);
      std::printf(",%zu,%.3f,%zu", siftingSpectral.nodes,
                  siftingSpectral.timeMs, siftingSpectral.exchanges);
    }
    std::printf(",%zu,%.3f,%.3f,%zu,%zu,%zu", dynamicConstruction.nodes,
                dynamicConstruction.timeMs, dynamicStatistics.reorderTimeMs,
                dynamicStatistics.reorderings, dynamicStatistics.exchanges,
                dynamicStatistics.peakSize);
    std::printf(",%.1f\n", peakRssMb());
    std::fflush(stdout);
  }
  return 0;
}
