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
 * @file order_check.cpp
 * @brief Command line checker for dd::computeVariableOrder
 *
 * Usage:
 *   mqt-core-order-check [options] circuit.qasm [circuit2.qasm ...]
 *
 * The tool loads each circuit, tries every ordering strategy, reports the
 * decision diagram size, the number of unique table insertions, and the
 * wall-clock time, and verifies that reordering did not change the operator.
 * Verification compares the reordered result with the identity order through
 * the level mapping, so it also checks that every level carries the right
 * qubit. Circuits that cannot be simulated as a state vector or a unitary, for
 * example circuits with mid-circuit measurements, are reported and skipped.
 */

#include "dd/FunctionalityConstruction.hpp"
#include "dd/Package.hpp"
#include "dd/Reordering.hpp"
#include "dd/Simulation.hpp"
#include "dd/StateGeneration.hpp"
#include "dd/VariableOrdering.hpp"
#include "ir/Permutation.hpp"
#include "ir/QuantumComputation.hpp"
#include "qasm3/Importer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

/// The largest number of qubits for which the tool compares dense results.
constexpr std::size_t VERIFICATION_QUBIT_LIMIT = 16U;

struct Options {
  std::vector<std::string> circuits;
  std::string mode = "state";
  std::string csv;
  std::string style = "table";
  std::string only;
  std::size_t repeats = 1U;
  std::size_t maxQubits = 20U;
  bool verbose = false;
};

struct Measurement {
  std::size_t nodes = 0U;
  std::size_t inserts = 0U;
  double timeMs = 0.0;
  double deviation = 0.0;
  bool verified = false;
  bool verificationSkipped = false;
};

/// A sparse operator, stored as (row, column) to value.
using SparseOperator =
    std::vector<std::pair<std::pair<std::size_t, std::size_t>, std::complex<double>>>;

[[nodiscard]] std::size_t countNodes(const dd::vEdge& edge,
                                     std::unordered_set<const dd::vNode*>& seen) {
  if (edge.isTerminal() || seen.find(edge.p) != seen.end()) {
    return 0U;
  }
  seen.insert(edge.p);
  std::size_t count = 1U;
  for (const auto& child : edge.p->e) {
    count += countNodes(child, seen);
  }
  return count;
}

[[nodiscard]] std::size_t countNodes(const dd::mEdge& edge,
                                     std::unordered_set<const dd::mNode*>& seen) {
  if (edge.isTerminal() || seen.find(edge.p) != seen.end()) {
    return 0U;
  }
  seen.insert(edge.p);
  std::size_t count = 1U;
  for (const auto& child : edge.p->e) {
    count += countNodes(child, seen);
  }
  return count;
}

/// Map a physical basis index to the index of the same basis state in level
/// order.
[[nodiscard]] std::size_t levelIndex(const std::size_t physicalIndex,
                                     const qc::Permutation& layout,
                                     const std::size_t numQubits) {
  std::size_t index = 0U;
  for (qc::Qubit qubit = 0; qubit < numQubits; ++qubit) {
    if ((physicalIndex & (static_cast<std::size_t>(1U) << qubit)) != 0U) {
      index |= static_cast<std::size_t>(1U) << layout.at(qubit);
    }
  }
  return index;
}

[[nodiscard]] qc::Permutation identityPermutation(const std::size_t n) {
  qc::Permutation permutation;
  for (qc::Qubit qubit = 0; qubit < n; ++qubit) {
    permutation[qubit] = qubit;
  }
  return permutation;
}

[[nodiscard]] Options parseOptions(const int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--mode" && i + 1 < argc) {
      options.mode = argv[++i];
    } else if (arg == "--csv" && i + 1 < argc) {
      options.csv = argv[++i];
    } else if (arg == "--style" && i + 1 < argc) {
      options.style = argv[++i];
    } else if (arg == "--only" && i + 1 < argc) {
      options.only = argv[++i];
    } else if (arg == "--repeats" && i + 1 < argc) {
      options.repeats = std::strtoul(argv[++i], nullptr, 10);
    } else if (arg == "--max-qubits" && i + 1 < argc) {
      options.maxQubits = std::strtoul(argv[++i], nullptr, 10);
    } else if (arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "--list" && i + 1 < argc) {
      std::ifstream list(argv[++i]);
      std::string line;
      while (std::getline(list, line)) {
        if (!line.empty() && line.front() != '#') {
          options.circuits.emplace_back(line);
        }
      }
    } else if (arg == "--help" || arg == "-h") {
      options.mode.clear();
    } else {
      options.circuits.emplace_back(arg);
    }
  }
  return options;
}

void printUsage() {
  std::printf(
      "usage: mqt-core-order-check [options] circuit.qasm ...\n"
      "\n"
      "  --mode state|functionality  measure a state vector or a unitary "
      "(default state)\n"
      "  --repeats N                 run every measurement N times and keep "
      "the fastest (default 1)\n"
      "  --max-qubits N              skip circuits with more qubits "
      "(default 20)\n"
      "  --csv FILE                  append one row per strategy to FILE\n"
      "  --style table|igqmdd        output format (default table)\n"
      "  --only NAME                 run a single strategy, for example "
      "spectral\n"
      "  --list FILE                 read circuit paths from FILE, one per "
      "line\n"
      "  --verbose                   print the level order of every "
      "strategy\n"
      "  -h, --help                  print this text\n");
}

/// Build the unitary of a circuit under a level order and return the dense
/// matrix in level order when requested.
[[nodiscard]] Measurement runFunctionality(
    const qc::QuantumComputation& circuit, const qc::Permutation& layout,
    const std::size_t numQubits, const std::size_t repeats,
    SparseOperator* matrix) {
  Measurement measurement;
  double best = std::numeric_limits<double>::max();
  for (std::size_t repeat = 0U; repeat < repeats; ++repeat) {
    auto reordered = circuit;
    reordered.initialLayout = layout;
    reordered.outputPermutation = layout;
    auto package = std::make_unique<dd::Package>(numQubits);
    const auto start = Clock::now();
    const auto unitary = dd::buildFunctionality(reordered, *package);
    const auto stop = Clock::now();
    const std::chrono::duration<double, std::milli> elapsed = stop - start;
    best = std::min(best, elapsed.count());
    std::unordered_set<const dd::mNode*> seen;
    measurement.nodes = countNodes(unitary, seen);
    std::size_t inserts = 0U;
    const auto& table = package->getUniqueTable<dd::mNode>();
    for (std::size_t level = 0U; level < numQubits; ++level) {
      inserts += table.getStats(level).inserts;
    }
    measurement.inserts = inserts;
    if (matrix != nullptr && repeat + 1U == repeats) {
      matrix->clear();
      for (const auto& [key, value] : unitary.getSparseMatrix(numQubits, 1e-12)) {
        matrix->emplace_back(key, value);
      }
    }
  }
  measurement.timeMs = best;
  return measurement;
}

[[nodiscard]] Measurement
runState(const qc::QuantumComputation& circuit, const qc::Permutation& layout,
         const std::size_t numQubits, const std::size_t repeats,
         dd::CVec* vector) {
  Measurement measurement;
  double best = std::numeric_limits<double>::max();
  for (std::size_t repeat = 0U; repeat < repeats; ++repeat) {
    auto reordered = circuit;
    reordered.initialLayout = layout;
    reordered.outputPermutation = layout;
    auto package = std::make_unique<dd::Package>(numQubits);
    const auto start = Clock::now();
    const auto state = dd::simulate(
        reordered, dd::makeZeroState(numQubits, *package), *package);
    const auto stop = Clock::now();
    const std::chrono::duration<double, std::milli> elapsed = stop - start;
    best = std::min(best, elapsed.count());
    std::unordered_set<const dd::vNode*> seen;
    measurement.nodes = countNodes(state, seen);
    std::size_t inserts = 0U;
    const auto& table = package->getUniqueTable<dd::vNode>();
    for (std::size_t level = 0U; level < numQubits; ++level) {
      inserts += table.getStats(level).inserts;
    }
    measurement.inserts = inserts;
    if (vector != nullptr && repeat + 1U == repeats) {
      *vector = state.getVector();
    }
  }
  measurement.timeMs = best;
  return measurement;
}

[[nodiscard]] double compareVectors(const dd::CVec& reference,
                                    const dd::CVec& candidate,
                                    const qc::Permutation& layout,
                                    const std::size_t numQubits) {
  double deviation = 0.0;
  for (std::size_t i = 0U; i < reference.size(); ++i) {
    const auto index = levelIndex(i, layout, numQubits);
    deviation = std::max(deviation, std::abs(candidate[index] - reference[i]));
  }
  return deviation;
}

[[nodiscard]] double compareMatrices(const SparseOperator& reference,
                                     const SparseOperator& candidate,
                                     const qc::Permutation& layout,
                                     const std::size_t numQubits) {
  if (reference.size() != candidate.size()) {
    return std::numeric_limits<double>::infinity();
  }
  std::map<std::pair<std::size_t, std::size_t>, std::complex<double>> byRow;
  for (const auto& [key, value] : candidate) {
    byRow[key] = value;
  }
  double deviation = 0.0;
  for (const auto& [key, value] : reference) {
    const auto row = levelIndex(key.first, layout, numQubits);
    const auto col = levelIndex(key.second, layout, numQubits);
    const auto it = byRow.find({row, col});
    if (it == byRow.end()) {
      deviation = std::numeric_limits<double>::infinity();
      break;
    }
    deviation = std::max(deviation, std::abs(it->second - value));
  }
  return deviation;
}

void printPermutation(const qc::Permutation& layout,
                      const std::size_t numQubits) {
  std::printf("      level order:");
  for (qc::Qubit level = 0; level < numQubits; ++level) {
    for (const auto& [qubit, assigned] : layout) {
      if (assigned == level) {
        std::printf(" q%u", static_cast<unsigned>(qubit));
        break;
      }
    }
  }
  std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
  const auto options = parseOptions(argc, argv);
  if (options.mode.empty() || options.circuits.empty()) {
    printUsage();
    return options.mode.empty() ? 0 : 2;
  }
  if (options.mode != "state" && options.mode != "functionality") {
    std::fprintf(stderr, "unknown mode '%s'\n", options.mode.c_str());
    return 2;
  }

  const std::vector<std::pair<std::string, dd::OrderingStrategy>> strategies{
      {"identity", dd::OrderingStrategy::Identity},
      {"degree", dd::OrderingStrategy::Degree},
      {"spectral", dd::OrderingStrategy::Spectral},
      {"recursive-bisection", dd::OrderingStrategy::RecursiveBisection},
      {"min-fill", dd::OrderingStrategy::MinFill},
      {"gate-dependency", dd::OrderingStrategy::GateDependency}};

  std::ofstream csv;
  if (!options.csv.empty()) {
    csv.open(options.csv, std::ios::app);
    csv << "circuit,qubits,gates,mode,strategy,nodes,inserts,time_ms,"
           "max_deviation,verified\n";
  }

  int failures = 0;
  bool anyVerificationSkipped = false;
  for (const auto& path : options.circuits) {
    qc::QuantumComputation circuit;
    try {
      circuit = qasm3::Importer::importf(path);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "%s: cannot import: %s\n", path.c_str(),
                   error.what());
      failures += 1;
      continue;
    }
    circuit.removeFinalMeasurements();
    const auto nonUnitary =
        std::ranges::any_of(circuit, [](const auto& op) {
          return op != nullptr &&
                 (!op->isUnitary() || op->isIfElseOperation());
        });
    if (nonUnitary) {
      std::fprintf(stderr,
                   "%s: skipped, the circuit contains measurements, resets, "
                   "or classical control\n",
                   path.c_str());
      continue;
    }
    const auto numQubits = circuit.getNqubits();
    if (numQubits == 0U || numQubits > options.maxQubits) {
      std::fprintf(stderr, "%s: skipped, %zu qubits\n", path.c_str(),
                   numQubits);
      continue;
    }

    const auto degrees = dd::interactionDegrees(circuit);
    const bool igqmddStyleEarly = options.style == "igqmdd";
    if (!igqmddStyleEarly) {
      std::printf("== %s\n", path.c_str());
      std::printf("   qubits=%zu gates=%zu mode=%s\n", numQubits,
                  circuit.size(), options.mode.c_str());
      std::printf("   interaction degrees:");
      for (std::size_t qubit = 0U; qubit < degrees.size(); ++qubit) {
        std::printf(" q%zu=%zu", qubit, degrees[qubit]);
      }
      std::printf("\n");
    }

    const auto identity = identityPermutation(numQubits);
    const bool verify = numQubits <= VERIFICATION_QUBIT_LIMIT;
    if (!verify) {
      anyVerificationSkipped = true;
    }

    dd::CVec referenceVector;
    SparseOperator referenceMatrix;
    Measurement baseline;
    if (options.mode == "state") {
      baseline = runState(circuit, identity, numQubits, options.repeats,
                          verify ? &referenceVector : nullptr);
    } else {
      baseline = runFunctionality(circuit, identity, numQubits, options.repeats,
                                  verify ? &referenceMatrix : nullptr);
    }

    const bool igqmddStyle = options.style == "igqmdd";
    if (igqmddStyle) {
      std::printf("%s 初始DD大小: %zu, 构造时间: %.6fs (qubits=%zu, gates=%zu, "
                  "mode=%s)\n",
                  path.c_str(), baseline.nodes, baseline.timeMs / 1000.0,
                  numQubits, circuit.size(), options.mode.c_str());
    } else {
      std::printf("   %-22s %9s %10s %10s %11s   %s\n", "strategy", "nodes",
                  "inserts", "time[ms]", "max_dev", "verdict");
    }
    for (const auto& [name, strategy] : strategies) {
      if (!options.only.empty() && name != options.only) {
        continue;
      }
      const auto layout = dd::computeVariableOrder(circuit, strategy);
      Measurement measurement;
      double deviation = 0.0;
      bool verified = false;
      bool skipped = false;
      if (options.mode == "state") {
        dd::CVec vector;
        measurement = runState(circuit, layout, numQubits, options.repeats,
                               verify ? &vector : nullptr);
        if (verify) {
          deviation = compareVectors(referenceVector, vector, layout, numQubits);
          verified = deviation < 1e-9;
        } else {
          skipped = true;
        }
      } else {
        SparseOperator matrix;
        measurement = runFunctionality(circuit, layout, numQubits,
                                       options.repeats,
                                       verify ? &matrix : nullptr);
        if (verify) {
          deviation =
              compareMatrices(referenceMatrix, matrix, layout, numQubits);
          verified = deviation < 1e-9;
        } else {
          skipped = true;
        }
      }
      const char* verdict = "ok";
      if (skipped) {
        verdict = "not checked";
      } else if (!verified) {
        verdict = "MISMATCH";
        failures += 1;
      }
      if (igqmddStyle) {
        const auto ratio =
            baseline.nodes == 0U
                ? 1.0
                : static_cast<double>(baseline.nodes) /
                      static_cast<double>(measurement.nodes);
        std::printf("%s %s 最终DD大小: %zu, 总时间: %.6fs, 压缩比: %.2fx"
                    ", 插入次数: %zu, 校验: %s\n",
                    path.c_str(), name.c_str(), measurement.nodes,
                    measurement.timeMs / 1000.0, ratio, measurement.inserts,
                    verdict);
      } else {
        std::printf("   %-22s %9zu %10zu %10.3f %11.3e   %s\n",
                    name.c_str(), measurement.nodes, measurement.inserts,
                    measurement.timeMs, deviation, verdict);
      }
      if (options.verbose) {
        printPermutation(layout, numQubits);
      }
      if (csv) {
        csv << path << ',' << numQubits << ',' << circuit.size() << ','
            << options.mode << ',' << name << ',' << measurement.nodes << ','
            << measurement.inserts << ',' << measurement.timeMs << ','
            << deviation << ',' << (verified ? "true" : "false") << '\n';
      }
    }

    // Dynamic sifting, once from the physical order and once from the best
    // static order.
    const auto totalInserts = [&numQubits](dd::Package& package) {
      std::size_t inserts = 0U;
      if constexpr (true) {
        const auto& vectorTable = package.getUniqueTable<dd::vNode>();
        const auto& matrixTable = package.getUniqueTable<dd::mNode>();
        for (std::size_t level = 0U; level < numQubits; ++level) {
          inserts += vectorTable.getStats(level).inserts;
          inserts += matrixTable.getStats(level).inserts;
        }
      }
      return inserts;
    };
    for (const auto& [label, startFromSpectral] :
         std::vector<std::pair<std::string, bool>>{{"sifting", false},
                                                   {"sifting-spectral", true}}) {
      if (!options.only.empty() && label != options.only) {
        continue;
      }
      auto package = std::make_unique<dd::Package>(numQubits);
      auto dynamicLayout =
          startFromSpectral
              ? dd::computeVariableOrder(circuit, dd::OrderingStrategy::Spectral)
              : dd::computeVariableOrder(circuit, dd::OrderingStrategy::Identity);
      auto reordered = circuit;
      reordered.initialLayout = dynamicLayout;
      reordered.outputPermutation = dynamicLayout;

      const auto insertsBefore = totalInserts(*package);
      dd::SiftingStatistics sifting;
      double timeMs = 0.0;
      std::size_t nodes = 0U;
      double dev = 0.0;
      bool ok = false;
      if (options.mode == "state") {
        auto root = dd::simulate(reordered, dd::makeZeroState(numQubits, *package),
                             *package);
        const auto start = Clock::now();
        root = dd::sift(*package, root, dynamicLayout, {}, &sifting);
        const auto stop = Clock::now();
        timeMs =
            std::chrono::duration<double, std::milli>(stop - start).count();
        nodes = root.size();
        if (verify) {
          const auto vector = root.getVector();
          dev = compareVectors(referenceVector, vector, dynamicLayout,
                               numQubits);
          ok = dev < 1e-9;
        }
      } else {
        auto root = dd::buildFunctionality(reordered, *package);
        const auto start = Clock::now();
        root = dd::sift(*package, root, dynamicLayout, {}, &sifting);
        const auto stop = Clock::now();
        timeMs =
            std::chrono::duration<double, std::milli>(stop - start).count();
        nodes = root.size();
        if (verify) {
          SparseOperator matrix;
          for (const auto& [key, value] : root.getSparseMatrix(numQubits, 1e-12)) {
            matrix.emplace_back(key, value);
          }
          dev = compareMatrices(referenceMatrix, matrix, dynamicLayout,
                                numQubits);
          ok = dev < 1e-9;
        }
      }
      const auto inserts = totalInserts(*package) - insertsBefore;
      const char* verdict = "ok";
      if (!verify) {
        verdict = "not checked";
      } else if (!ok) {
        verdict = "MISMATCH";
        failures += 1;
      }

      if (igqmddStyle) {
        const auto ratio =
            baseline.nodes == 0U
                ? 1.0
                : static_cast<double>(baseline.nodes) /
                      static_cast<double>(nodes);
        std::printf("%s %s 最终DD大小: %zu, 总时间: %.6fs, 压缩比: %.2fx"
                    ", 插入次数: %zu, 交换次数: %zu, 校验: %s\n",
                    path.c_str(), label.c_str(), nodes, timeMs / 1000.0, ratio,
                    inserts, sifting.exchanges, verdict);
      } else {
        std::printf("   %-22s %9zu %10zu %10.3f %11.3e   %s (%zu exchanges)\n",
                    label.c_str(), nodes, inserts, timeMs, dev, verdict,
                    sifting.exchanges);
      }
      if (options.verbose) {
        printPermutation(dynamicLayout, numQubits);
      }
      if (csv) {
        csv << path << ',' << numQubits << ',' << circuit.size() << ','
            << options.mode << ',' << label << ',' << nodes << ',' << inserts
            << ',' << timeMs << ',' << dev << ','
            << (ok ? "true" : "false") << '\n';
      }
    }
    std::printf("\n");
  }

  if (anyVerificationSkipped) {
    std::printf(
        "note: results were not compared with the identity order because the "
        "circuit exceeds %zu qubits\n",
        VERIFICATION_QUBIT_LIMIT);
  }
  return failures == 0 ? 0 : 1;
}
