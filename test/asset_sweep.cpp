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
 * @file asset_sweep.cpp
 * @brief Measurement tool for the variable ordering experiments
 *
 * The tool reads one RevLib `.real` circuit, builds its unitary as a matrix
 * decision diagram, and reports one CSV row with the size and the time of
 *
 *   1. the initial construction,
 *   2. every static ordering strategy,
 *   3. dynamic sifting, once from the physical order and once from the best
 *      static order.
 *
 * The tool is experiment scaffolding. It is not part of the MQT Core library.
 */

#include "dd/FunctionalityConstruction.hpp"
#include "dd/Package.hpp"
#include "dd/Reordering.hpp"
#include "dd/VariableOrdering.hpp"
#include "ir/Definitions.hpp"
#include "ir/Permutation.hpp"
#include "ir/QuantumComputation.hpp"
#include "ir/operations/Control.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Macro {
  std::vector<std::string> parameters;
  std::vector<std::vector<std::string>> body;
};

struct RealCircuit {
  std::size_t numVars = 0U;
  std::vector<std::string> variables;
  std::vector<std::string> inputs;
  std::vector<std::vector<std::string>> body;
  std::map<std::string, Macro> macros;
};

[[nodiscard]] std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> tokens;
  std::string current;
  for (const auto character : line) {
    if (character == ' ' || character == '\t' || character == ',') {
      if (!current.empty()) {
        tokens.emplace_back(current);
        current.clear();
      }
    } else {
      current.push_back(character);
    }
  }
  if (!current.empty()) {
    tokens.emplace_back(current);
  }
  return tokens;
}

[[nodiscard]] std::optional<RealCircuit> parseReal(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    return std::nullopt;
  }
  RealCircuit circuit;
  std::string line;
  std::optional<Macro> currentMacro;
  std::string currentName;
  bool inBody = false;
  while (std::getline(input, line)) {
    const auto start = line.find_first_not_of(" \t\r");
    if (start == std::string::npos || line[start] == '#') {
      continue;
    }
    const std::string stripped = line.substr(start);
    if (stripped.starts_with(".numvars")) {
      circuit.numVars = std::stoul(split(stripped).at(1));
    } else if (stripped.starts_with(".variables")) {
      const auto tokens = split(stripped);
      circuit.variables.assign(tokens.begin() + 1, tokens.end());
    } else if (stripped.starts_with(".inputs")) {
      const auto tokens = split(stripped);
      circuit.inputs.assign(tokens.begin() + 1, tokens.end());
    } else if (stripped.starts_with(".define")) {
      const auto tokens = split(stripped);
      currentName = tokens.at(1);
      currentMacro = Macro{};
      currentMacro->parameters.assign(tokens.begin() + 2, tokens.end());
    } else if (stripped.starts_with(".enddefine")) {
      if (currentMacro.has_value()) {
        circuit.macros[currentName] = std::move(*currentMacro);
        currentMacro.reset();
      }
    } else if (stripped.starts_with(".begin")) {
      inBody = true;
    } else if (stripped.starts_with(".end")) {
      if (inBody) {
        break;
      }
    } else if (stripped.starts_with(".")) {
      continue;
    } else if (currentMacro.has_value()) {
      currentMacro->body.emplace_back(split(stripped));
    } else if (inBody) {
      circuit.body.emplace_back(split(stripped));
    }
  }
  if (circuit.variables.size() != circuit.numVars ||
      circuit.inputs.size() != circuit.numVars) {
    return std::nullopt;
  }
  return circuit;
}

/**
 * @brief Expand a statement into primitive gates
 * @return The operand list of every primitive gate, or an empty optional when
 * the statement uses a gate that the tool does not know
 */
[[nodiscard]] std::optional<std::vector<std::string>>
expand(const std::vector<std::string>& statement,
       const std::map<std::string, Macro>& macros,
       const std::map<std::string, std::string>& environment,
       std::vector<std::vector<std::string>>& gates, const int depth) {
  if (depth > 64 || statement.empty()) {
    return std::nullopt;
  }
  const auto& name = statement.front();
  const bool isPrimitive =
      name == "t" || (name.size() > 1U && name.front() == 't' &&
                      std::all_of(name.begin() + 1, name.end(),
                                  [](char c) { return c >= '0' && c <= '9'; }));
  if (isPrimitive) {
    std::vector<std::string> operands;
    for (std::size_t i = 1U; i < statement.size(); ++i) {
      if (const auto it = environment.find(statement[i]);
          it != environment.end()) {
        operands.emplace_back(it->second);
      } else {
        operands.emplace_back(statement[i]);
      }
    }
    gates.emplace_back(operands);
    return operands;
  }
  const auto macro = macros.find(name);
  if (macro == macros.end() ||
      macro->second.parameters.size() + 1U != statement.size()) {
    return std::nullopt;
  }
  auto local = environment;
  for (std::size_t i = 0U; i < macro->second.parameters.size(); ++i) {
    const auto& argument = statement[i + 1U];
    if (const auto it = environment.find(argument); it != environment.end()) {
      local[macro->second.parameters[i]] = it->second;
    } else {
      local[macro->second.parameters[i]] = argument;
    }
  }
  for (const auto& child : macro->second.body) {
    if (!expand(child, macros, local, gates, depth + 1).has_value()) {
      return std::nullopt;
    }
  }
  return std::vector<std::string>{};
}

[[nodiscard]] std::optional<qc::QuantumComputation>
buildCircuit(const RealCircuit& real) {
  std::map<std::string, qc::Qubit> index;
  for (std::size_t i = 0U; i < real.variables.size(); ++i) {
    index[real.variables[i]] = static_cast<qc::Qubit>(i);
  }

  std::vector<std::vector<std::string>> gates;
  for (const auto& statement : real.body) {
    if (!expand(statement, real.macros, {}, gates, 0).has_value()) {
      return std::nullopt;
    }
  }

  qc::QuantumComputation circuit(real.numVars);
  for (std::size_t i = 0U; i < real.inputs.size(); ++i) {
    if (real.inputs[i] == "1") {
      circuit.x(static_cast<qc::Qubit>(i));
    }
  }

  for (const auto& operands : gates) {
    std::vector<qc::Qubit> qubits;
    qubits.reserve(operands.size());
    for (const auto& operand : operands) {
      const auto it = index.find(operand);
      if (it == index.end()) {
        return std::nullopt;
      }
      qubits.emplace_back(it->second);
    }
    if (qubits.size() == 1U) {
      circuit.x(qubits[0]);
    } else if (qubits.size() == 2U) {
      circuit.cx(qubits[0], qubits[1]);
    } else if (qubits.size() == 3U) {
      circuit.mcx(qc::Controls{qc::Control{qubits[0]}, qc::Control{qubits[1]}},
                  qubits[2]);
    } else {
      qc::Controls controls;
      for (std::size_t i = 0U; i + 1U < qubits.size(); ++i) {
        controls.emplace(qubits[i]);
      }
      circuit.mcx(controls, qubits.back());
    }
  }
  return circuit;
}

struct Measurement {
  std::size_t nodes = 0U;
  double timeMs = 0.0;
};

[[nodiscard]] Measurement measure(qc::QuantumComputation circuit,
                                  const qc::Permutation& layout) {
  const auto numQubits = circuit.getNqubits();
  if (!layout.empty()) {
    circuit.initialLayout = layout;
    circuit.outputPermutation = layout;
  }
  auto package = std::make_unique<dd::Package>(numQubits);
  const auto start = Clock::now();
  const auto root = dd::buildFunctionality(circuit, *package);
  const auto stop = Clock::now();
  const std::chrono::duration<double, std::milli> elapsed = stop - start;
  return {root.size(), elapsed.count()};
}

struct DynamicMeasurement {
  std::size_t nodes = 0U;
  double timeMs = 0.0;
  std::size_t exchanges = 0U;
};

[[nodiscard]] DynamicMeasurement
measureDynamic(const qc::QuantumComputation& circuit,
               const qc::Permutation& start, const std::size_t startMs) {
  const auto numQubits = circuit.getNqubits();
  auto package = std::make_unique<dd::Package>(numQubits);
  auto reordered = circuit;
  auto layout = start;
  reordered.initialLayout = layout;
  reordered.outputPermutation = layout;
  const auto root = dd::buildFunctionality(reordered, *package);
  dd::SiftingStatistics statistics;
  const auto begin = Clock::now();
  const auto sifted = dd::sift(*package, root, layout, {}, &statistics);
  const auto end = Clock::now();
  const std::chrono::duration<double, std::milli> elapsed = end - begin;
  return {sifted.size(), startMs + elapsed.count(), statistics.exchanges};
}

[[nodiscard]] std::size_t peakRssMb() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.starts_with("VmHWM:")) {
      const auto tokens = split(line);
      return std::stoul(tokens.at(1)) / 1024U;
    }
  }
  return 0U;
}

} // namespace

namespace {

void printRow(const std::vector<std::string>& fields) {
  for (std::size_t i = 0U; i < fields.size(); ++i) {
    std::printf("%s%s", i == 0U ? "" : ",", fields[i].c_str());
  }
  std::printf("\n");
}

[[nodiscard]] std::string number(const std::size_t value) {
  return std::to_string(value);
}

[[nodiscard]] std::string number(const double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.3f", value);
  return buffer;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: asset_sweep circuit.real\n");
    return 2;
  }
  const std::string path = argv[1];
  const auto real = parseReal(path);
  if (!real.has_value()) {
    printRow({path, "", "", "parse_error"});
    return 1;
  }
  const auto circuit = buildCircuit(*real);
  if (!circuit.has_value()) {
    printRow({path, number(real->numVars), "", "unsupported_gate"});
    return 1;
  }
  const auto numQubits = circuit->getNqubits();

  const auto initial = measure(*circuit, {});

  const std::vector<std::pair<std::string, dd::OrderingStrategy>> strategies{
      {"degree", dd::OrderingStrategy::Degree},
      {"spectral", dd::OrderingStrategy::Spectral},
      {"recursive-bisection", dd::OrderingStrategy::RecursiveBisection}};
  std::size_t spectralNodes = 0U;
  double spectralMs = 0.0;
  std::size_t bestNodes = initial.nodes;
  double bestMs = initial.timeMs;
  std::string bestName = "identity";
  qc::Permutation bestLayout =
      dd::computeVariableOrder(*circuit, dd::OrderingStrategy::Identity);
  for (const auto& [name, strategy] : strategies) {
    const auto layout = dd::computeVariableOrder(*circuit, strategy);
    const auto result = measure(*circuit, layout);
    if (name == "spectral") {
      spectralNodes = result.nodes;
      spectralMs = result.timeMs;
    }
    if (result.nodes < bestNodes) {
      bestNodes = result.nodes;
      bestMs = result.timeMs;
      bestName = name;
      bestLayout = layout;
    }
  }

  const auto dynamic = measureDynamic(
      *circuit,
      dd::computeVariableOrder(*circuit, dd::OrderingStrategy::Identity), 0.0);
  const auto hybrid = measureDynamic(*circuit, bestLayout, bestMs);

  printRow({path, number(numQubits), number(circuit->size()), "ok",
            number(initial.nodes), number(initial.timeMs), number(bestNodes),
            number(bestMs), bestName, number(spectralNodes), number(spectralMs),
            number(dynamic.nodes), number(dynamic.timeMs),
            number(dynamic.exchanges), number(hybrid.nodes),
            number(hybrid.timeMs), number(hybrid.exchanges),
            number(peakRssMb())});
  return 0;
}
