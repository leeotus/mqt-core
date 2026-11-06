/*
 * Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
 * Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Licensed under the MIT License
 */

#pragma once

#include "dd/DDDefinitions.hpp"
#include "dd/Node.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qc {
class QuantumComputation;
} // namespace qc

namespace dd {

class Package;

/**
 * @brief Maximum-likelihood quantum state tomography on density-matrix DDs.
 * @details The density matrix is an n-qubit matrix decision diagram. Every
 * measurement record carries the index of its basis, so records from
 * different bases never mix. The reconstruction is the dilute MLE iteration
 *
 *     R = sum_{b,k} (f_{b,k} / p_{b,k}) Pi_{b,k}
 *     rho <- R rho R / Tr(R rho R)
 *
 * with p_{b,k} = Tr(rho Pi_{b,k}) and f_{b,k} the observed frequency of
 * outcome k in basis b.
 */
class QST {
public:
  /// A single-qubit Pauli measurement basis.
  enum class Basis : std::uint8_t { Z = 0, X = 1, Y = 2 };

  /// One measurement setting: one basis per qubit.
  struct MeasurementBasis {
    std::vector<Basis> bases;

    bool operator==(const MeasurementBasis& other) const {
      return bases == other.bases;
    }
    bool operator<(const MeasurementBasis& other) const {
      return bases < other.bases;
    }
  };

  /// One measured outcome together with the basis that produced it.
  struct MeasurementRecord {
    std::size_t basisIndex = 0U;
    std::vector<int> bits;
    std::size_t count = 0U;
  };

  /// Configuration of the sampling and the reconstruction.
  struct Config {
    /// Number of shots per measurement basis.
    std::size_t shotsPerBasis = 1000U;
    /// Maximum number of MLE iterations.
    std::size_t maxIterations = 100U;
    /// Relative log-likelihood change that stops the iteration.
    double tolerance = 1e-8;
    /// Number of random bases; 0 selects the full 3^n Pauli basis.
    std::size_t randomBases = 0U;
    /// Seed for sampling and random bases.
    std::size_t seed = 42U;
  };

  /// Result of a reconstruction.
  struct Result {
    mEdge rho{};
    CMat rhoMatrix;
    double logLikelihood = 0.0;
    std::size_t iterations = 0U;
    bool converged = false;
    std::size_t ddSize = 0U;
    double elapsedMs = 0.0;
  };

  /// Return the full Pauli basis with 3^n measurement settings.
  [[nodiscard]] static std::vector<MeasurementBasis>
  allPauliBases(std::size_t nQubits);

  /// Return a fixed number of distinct random Pauli settings.
  [[nodiscard]] static std::vector<MeasurementBasis>
  randomPauliBases(std::size_t nQubits, std::size_t count, std::size_t seed);

  /**
   * @brief Sample measurement outcomes from a density matrix.
   * @param dd The DD package
   * @param rho The density matrix
   * @param bases The measurement settings
   * @param shotsPerBasis The number of shots per setting
   * @param seed The sampling seed
   * @return One record per observed (basis, outcome) pair
   */
  [[nodiscard]] static std::vector<MeasurementRecord>
  sample(Package& dd, const mEdge& rho,
         const std::vector<MeasurementBasis>& bases, std::size_t shotsPerBasis,
         std::size_t seed);

  /**
   * @brief Return Tr(rho * Pi), the probability of one outcome.
   */
  [[nodiscard]] static double probability(Package& dd, const mEdge& rho,
                                          const MeasurementBasis& basis,
                                          const std::vector<int>& bits);

  /**
   * @brief Build the projector of one (basis, outcome) pair.
   */
  [[nodiscard]] static mEdge projector(Package& dd, std::size_t nQubits,
                                       const MeasurementBasis& basis,
                                       const std::vector<int>& bits);

  /**
   * @brief Build the density matrix of the state prepared by a circuit.
   * @details Computes rho = U |0..0><0..0| U^dagger with U the functionality
   * of @p circuit.
   */
  [[nodiscard]] static mEdge densityMatrix(Package& dd,
                                           qc::QuantumComputation& circuit);

  /**
   * @brief Run dilute MLE on the measurement records.
   */
  [[nodiscard]] static Result
  reconstruct(Package& dd, const std::vector<MeasurementRecord>& records,
              const std::vector<MeasurementBasis>& bases, const Config& config);

  /**
   * @brief Return Tr(first * second).
   * @details For density matrices this is the Hilbert-Schmidt overlap. It
   * equals the fidelity when at least one argument is a pure state.
   */
  [[nodiscard]] static double overlap(Package& dd, const mEdge& first,
                                      const mEdge& second, std::size_t nQubits);

  /// Return Tr(rho * rho), the purity of a density matrix.
  [[nodiscard]] static double purity(Package& dd, const mEdge& rho,
                                     std::size_t nQubits);

  /// Return the maximally mixed state I / 2^n.
  [[nodiscard]] static mEdge maximallyMixedState(Package& dd,
                                                 std::size_t nQubits);

private:
  /// Build the projector of one qubit and embed it into the full space.
  [[nodiscard]] static mEdge singleQubitProjector(Package& dd,
                                                  std::size_t qubit,
                                                  Basis basis, int outcome);

  /// Return the single-qubit basis-change matrix for one measurement basis.
  [[nodiscard]] static mEdge basisChange(Package& dd, std::size_t qubit,
                                         Basis basis);

  /// Return the outcome probabilities of all 2^n results in one basis.
  [[nodiscard]] static std::vector<double>
  basisProbabilities(Package& dd, const mEdge& rho,
                     const MeasurementBasis& basis, std::size_t nQubits);

  /// Run the matrix-based MLE for small qubit counts.
  [[nodiscard]] static Result
  reconstructMatrix(Package& dd, const std::vector<MeasurementRecord>& records,
                    const std::vector<MeasurementBasis>& bases,
                    const Config& config);
};

} // namespace dd
