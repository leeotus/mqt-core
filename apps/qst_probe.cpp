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
 * @file qst_probe.cpp
 * @brief Command line probe for the maximum-likelihood QST module
 * @details Builds a known pure state, samples Pauli measurements, runs the
 * reconstruction, and prints one CSV line.
 *
 * Usage:
 *   mqt-core-qst-probe [--state zero|ghz|random] [--circuit FILE.qasm]
 *                      [--qubits N] [--shots S] [--iterations I]
 *                      [--random-bases K] [--seed SEED]
 *
 * With --circuit the state comes from the given OpenQASM file; final
 * measurements are removed because the density matrix is built from the
 * unitary part only.
 */

#include "dd/Package.hpp"
#include "dd/QST.hpp"
#include "ir/QuantumComputation.hpp"
#include "qasm3/Importer.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>

namespace {

struct Options {
  std::string state = "ghz";
  std::string circuitPath;
  std::size_t qubits = 2U;
  std::size_t shots = 2000U;
  std::size_t iterations = 200U;
  std::size_t randomBases = 0U;
  std::size_t seed = 42U;
};

qc::QuantumComputation prepareCircuit(const Options& options) {
  const auto width = static_cast<qc::Qubit>(options.qubits);
  qc::QuantumComputation circuit(width);
  if (options.state == "zero") {
    return circuit;
  }
  if (options.state == "ghz") {
    circuit.h(0);
    for (std::size_t qubit = 1U; qubit < options.qubits; ++qubit) {
      circuit.cx(0, static_cast<qc::Qubit>(qubit));
    }
    return circuit;
  }

  std::mt19937 rng(static_cast<std::mt19937::result_type>(options.seed));
  std::uniform_real_distribution<double> angle(0.0, 2.0 * M_PI);
  for (std::size_t qubit = 0U; qubit < options.qubits; ++qubit) {
    const auto target = static_cast<qc::Qubit>(qubit);
    circuit.ry(angle(rng), target);
    circuit.rz(angle(rng), target);
    circuit.ry(angle(rng), target);
  }
  for (std::size_t qubit = 0U; qubit + 1U < options.qubits; ++qubit) {
    circuit.cx(static_cast<qc::Qubit>(qubit),
               static_cast<qc::Qubit>(qubit + 1U));
  }
  return circuit;
}

} // namespace

int main(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--state" && i + 1 < argc) {
      options.state = argv[++i];
    } else if (argument == "--qubits" && i + 1 < argc) {
      options.qubits = std::stoul(argv[++i]);
    } else if (argument == "--shots" && i + 1 < argc) {
      options.shots = std::stoul(argv[++i]);
    } else if (argument == "--iterations" && i + 1 < argc) {
      options.iterations = std::stoul(argv[++i]);
    } else if (argument == "--random-bases" && i + 1 < argc) {
      options.randomBases = std::stoul(argv[++i]);
    } else if (argument == "--circuit" && i + 1 < argc) {
      options.circuitPath = argv[++i];
    } else if (argument == "--seed" && i + 1 < argc) {
      options.seed = std::stoul(argv[++i]);
    } else if (argument == "-h" || argument == "--help") {
      std::printf("usage: %s [--state zero|ghz|random] [--circuit FILE.qasm] "
                  "[--qubits N] [--shots S] [--iterations I] "
                  "[--random-bases K] [--seed SEED]\n",
                  argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
      return 2;
    }
  }

  qc::QuantumComputation circuit;
  if (!options.circuitPath.empty()) {
    try {
      circuit = qasm3::Importer::importf(options.circuitPath);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "cannot read %s: %s\n", options.circuitPath.c_str(),
                   error.what());
      return 2;
    }
    circuit.removeFinalMeasurements();
    options.qubits = circuit.getNqubits();
    options.state = "qasm";
  } else {
    circuit = prepareCircuit(options);
  }

  // The dense path needs a full 4^n matrix, so the width is bounded by memory.
  if (options.qubits == 0U || options.qubits > 15U) {
    std::fprintf(stderr, "QST supports 1 to 15 qubits, got %zu\n",
                 options.qubits);
    return 2;
  }

  dd::Package dd(options.qubits);
  auto rhoTrue = dd::QST::densityMatrix(dd, circuit);

  const auto bases =
      options.randomBases == 0U
          ? dd::QST::allPauliBases(options.qubits)
          : dd::QST::randomPauliBases(options.qubits, options.randomBases,
                                      options.seed);
  const auto records =
      dd::QST::sample(dd, rhoTrue, bases, options.shots, options.seed);

  dd::QST::Config config;
  config.shotsPerBasis = options.shots;
  config.maxIterations = options.iterations;
  config.seed = options.seed;
  auto result = dd::QST::reconstruct(dd, records, bases, config);

  double fidelity = 0.0;
  double purity = 0.0;
  if (!result.rhoMatrix.empty()) {
    const auto trueMatrix = rhoTrue.getMatrix(options.qubits);
    for (std::size_t row = 0U; row < trueMatrix.size(); ++row) {
      for (std::size_t col = 0U; col < trueMatrix.size(); ++col) {
        fidelity += (trueMatrix[row][col] * result.rhoMatrix[col][row]).real();
        purity +=
            (result.rhoMatrix[row][col] * result.rhoMatrix[col][row]).real();
      }
    }
  } else {
    fidelity = dd::QST::overlap(dd, rhoTrue, result.rho, options.qubits);
    purity = dd::QST::purity(dd, result.rho, options.qubits);
  }

  std::fprintf(stderr, "true_purity=%.6f\n",
               dd::QST::purity(dd, rhoTrue, options.qubits));
  std::printf("state,n,bases,shots,iterations,fidelity,purity,dd_size,time_ms,"
              "converged\n");
  std::printf("%s,%zu,%zu,%zu,%zu,%.6f,%.6f,%zu,%.3f,%d\n",
              options.state.c_str(), options.qubits, bases.size(),
              options.shots, result.iterations, fidelity, purity, result.ddSize,
              result.elapsedMs, result.converged ? 1 : 0);
  return 0;
}
