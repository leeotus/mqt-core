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
 * @file dump_unitary.cpp
 * @brief Dump the unitary of a circuit as a sparse matrix for every ordering
 * strategy, together with the level order that produced it.
 *
 * The output permutation is set equal to the input layout, so the decision
 * diagram stays in the chosen level order. The companion script
 * verify_unitary.py maps the level indices back to physical qubits with the
 * dumped layout. This avoids dd::buildFunctionality's restoration step, which
 * permutes only one side of a matrix decision diagram and therefore does not
 * restore a permuted layout.
 */

#include "dd/FunctionalityConstruction.hpp"
#include "dd/Package.hpp"
#include "dd/VariableOrdering.hpp"
#include "ir/Permutation.hpp"
#include "ir/QuantumComputation.hpp"
#include "qasm3/Importer.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: dump_unitary circuit.qasm ...\n");
    return 2;
  }

  const std::vector<dd::OrderingStrategy> strategies{
      dd::OrderingStrategy::Identity, dd::OrderingStrategy::Degree,
      dd::OrderingStrategy::Spectral, dd::OrderingStrategy::RecursiveBisection,
      dd::OrderingStrategy::QuantumScore};

  for (int index = 1; index < argc; ++index) {
    const std::string path = argv[index];
    qc::QuantumComputation circuit;
    try {
      circuit = qasm3::Importer::importf(path);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "skip %s: %s\n", path.c_str(), error.what());
      continue;
    }
    circuit.removeFinalMeasurements();
    const auto numQubits = circuit.getNqubits();
    if (numQubits == 0U) {
      continue;
    }

    for (const auto strategy : strategies) {
      auto reordered = circuit;
      reordered.initialLayout = dd::computeVariableOrder(circuit, strategy);
      reordered.outputPermutation = reordered.initialLayout;

      for (const auto& [qubit, level] : reordered.initialLayout) {
        std::printf("%s,%s,layout,%u,%u\n", path.c_str(),
                    dd::toString(strategy), static_cast<unsigned>(qubit),
                    static_cast<unsigned>(level));
      }

      auto package = std::make_unique<dd::Package>(numQubits);
      try {
        const auto unitary = dd::buildFunctionality(reordered, *package);
        const auto sparse = unitary.getSparseMatrix(numQubits, 1e-12);
        for (const auto& [key, value] : sparse) {
          const auto magnitude = std::abs(value);
          if (magnitude > 1e-9) {
            std::printf("%s,%s,%zu,%zu,%.12f\n", path.c_str(),
                        dd::toString(strategy), key.first, key.second,
                        magnitude);
          }
        }
      } catch (const std::exception& error) {
        std::fprintf(stderr, "error %s %s: %s\n", path.c_str(),
                     dd::toString(strategy), error.what());
      }
    }
  }
  return 0;
}
