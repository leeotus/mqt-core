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
 *                      [--random-bases K] [--max-outcomes K]
 *                      [--backend qmdd|dense] [--seed SEED]
 *
 * With --circuit the state comes from the given OpenQASM file; final
 * measurements are removed because the density matrix is built from the
 * unitary part only.
 */

#include "dd/Package.hpp"
#include "dd/QST.hpp"
#include "ir/QuantumComputation.hpp"
#include "qasm3/Importer.hpp"

#include <algorithm>
#include <chrono>
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
  dd::QST::Backend backend = dd::QST::Backend::QMDD;
  std::size_t qubits = 2U;
  std::size_t shots = 2000U;
  std::size_t iterations = 200U;
  std::size_t randomBases = 0U;
  std::size_t maxOutcomes = 0U;
  std::size_t seed = 42U;
};

std::string backendName(const dd::QST::Backend backend) {
  return backend == dd::QST::Backend::QMDD ? "qmdd" : "dense";
}

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
  const auto totalStart = std::chrono::steady_clock::now();
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
    } else if (argument == "--max-outcomes" && i + 1 < argc) {
      options.maxOutcomes = std::stoul(argv[++i]);
    } else if (argument == "--backend" && i + 1 < argc) {
      const std::string backend = argv[++i];
      if (backend == "qmdd") {
        options.backend = dd::QST::Backend::QMDD;
      } else if (backend == "dense") {
        options.backend = dd::QST::Backend::Dense;
      } else {
        std::fprintf(stderr, "unknown backend: %s\n", backend.c_str());
        return 2;
      }
    } else if (argument == "--circuit" && i + 1 < argc) {
      options.circuitPath = argv[++i];
    } else if (argument == "--seed" && i + 1 < argc) {
      options.seed = std::stoul(argv[++i]);
    } else if (argument == "-h" || argument == "--help") {
      std::printf("usage: %s [--state zero|ghz|random] [--circuit FILE.qasm] "
                  "[--qubits N] [--shots S] [--iterations I] "
                  "[--random-bases K] [--max-outcomes K] "
                  "[--backend qmdd|dense] [--seed SEED]\n",
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

  // The sampling path enumerates all 2^n outcomes and keeps one counter and
  // one probability per outcome. The cap keeps that bookkeeping near 256 MB.
  // The dense backend has a much lower limit of its own.
  constexpr std::size_t MAX_QUBITS = 24U;
  if (options.qubits == 0U || options.qubits > MAX_QUBITS) {
    std::fprintf(stderr, "QST supports 1 to %zu qubits, got %zu\n", MAX_QUBITS,
                 options.qubits);
    return 2;
  }

  dd::Package dd(options.qubits);
  const auto densityStart = std::chrono::steady_clock::now();
  auto rhoTrue = dd::QST::densityMatrix(dd, circuit);
  const auto densityMs = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - densityStart)
                             .count();

  const auto basisStart = std::chrono::steady_clock::now();
  const auto bases =
      options.randomBases == 0U
          ? dd::QST::allPauliBases(options.qubits)
          : dd::QST::randomPauliBases(options.qubits, options.randomBases,
                                      options.seed);
  const auto basisMs = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - basisStart)
                           .count();
  const auto samplingStart = std::chrono::steady_clock::now();
  const auto records =
      dd::QST::sample(dd, rhoTrue, bases, options.shots, options.seed);
  const auto samplingMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - samplingStart)
                              .count();

  dd::QST::Config config;
  config.backend = options.backend;
  config.shotsPerBasis = options.shots;
  config.maxIterations = options.iterations;
  config.maxOutcomesPerBasis = options.maxOutcomes;
  config.seed = options.seed;
  auto result = dd::QST::reconstruct(dd, records, bases, config);
  if (result.rho.p == nullptr && result.rhoMatrix.empty()) {
    std::fprintf(stderr, "QST did not produce a reconstruction\n");
    return 1;
  }

  const auto metricsStart = std::chrono::steady_clock::now();
  double fidelity = 0.0;
  double purity = 0.0;
  double trace = 0.0;
  if (!result.rhoMatrix.empty()) {
    const auto trueMatrix = rhoTrue.getMatrix(options.qubits);
    for (std::size_t row = 0U; row < trueMatrix.size(); ++row) {
      for (std::size_t col = 0U; col < trueMatrix.size(); ++col) {
        fidelity += (trueMatrix[row][col] * result.rhoMatrix[col][row]).real();
        purity +=
            (result.rhoMatrix[row][col] * result.rhoMatrix[col][row]).real();
      }
      trace += result.rhoMatrix[row][row].real();
    }
  } else {
    fidelity = dd::QST::overlap(dd, rhoTrue, result.rho, options.qubits);
    purity = dd::QST::purity(dd, result.rho, options.qubits);
    auto identity = dd.createInitialMatrix({});
    dd.incRef(identity);
    trace = dd::QST::overlap(dd, result.rho, identity, options.qubits);
    dd.decRef(identity);
  }

  const auto truePurity = dd::QST::purity(dd, rhoTrue, options.qubits);
  fidelity = std::clamp(fidelity, 0.0, 1.0);
  const auto traceDistance = std::sqrt(std::max(0.0, 1.0 - fidelity));
  const auto traceError = std::abs(trace - 1.0);
  const auto metricsMs = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - metricsStart)
                             .count();
  const auto totalMs = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - totalStart)
                           .count();

  std::fprintf(stderr, "true_purity=%.6f\n", truePurity);
  std::printf(
      "state,backend,n,bases,shots,iterations,fidelity,purity,dd_size,time_ms,"
      "converged,max_iterations,max_outcomes,true_purity,trace_distance,trace,"
      "trace_error,log_likelihood,observed_outcomes,retained_outcomes,"
      "initial_dd_size,peak_dd_size,peak_r_dd_size,density_ms,basis_ms,"
      "sampling_ms,reconstruct_init_ms,reconstruct_iter_ms,metrics_ms,"
      "total_ms\n");
  std::printf(
      "%s,%s,%zu,%zu,%zu,%zu,%.6f,%.6f,%zu,%.3f,%d,%zu,%zu,%.6f,%.6f,%.6f,"
      "%.6f,%.6f,%zu,%zu,%zu,%zu,%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
      options.state.c_str(), backendName(result.backend).c_str(),
      options.qubits, bases.size(), options.shots, result.iterations, fidelity,
      purity, result.ddSize, result.elapsedMs, result.converged ? 1 : 0,
      options.iterations, options.maxOutcomes, truePurity, traceDistance, trace,
      traceError, result.logLikelihood, result.observedOutcomes,
      result.retainedOutcomes, result.initialDdSize, result.peakDdSize,
      result.peakRSize, densityMs, basisMs, samplingMs, result.initializationMs,
      result.iterationMs, metricsMs, totalMs);
  return 0;
}
