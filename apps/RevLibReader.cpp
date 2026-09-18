/*
 * Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
 * Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
 * All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Licensed under the MIT License
 */

#include "RevLibReader.hpp"

#include "ir/Definitions.hpp"
#include "ir/QuantumComputation.hpp"
#include "ir/operations/Control.hpp"

#include <cstddef>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mqt::apps {

namespace {

/// A parsed statement of the circuit body.
using Statement = std::vector<std::string>;

/// A parsed macro definition of a `.define` block.
struct Macro {
  std::vector<std::string> parameters;
  std::vector<Statement> body;
};

[[nodiscard]] std::string stripQuotes(const std::string& text) {
  if (text.size() >= 2U && text.front() == '"' && text.back() == '"') {
    return text.substr(1U, text.size() - 2U);
  }
  return text;
}

[[nodiscard]] std::vector<std::string> split(const std::string& line) {
  std::istringstream stream(line);
  std::vector<std::string> tokens;
  std::string token;
  while (stream >> token) {
    tokens.emplace_back(stripQuotes(token));
  }
  return tokens;
}

/// Return whether a gate name is a Toffoli-style gate.
[[nodiscard]] bool isToffoli(const std::string& name) {
  if (name.empty() || name.front() != 't') {
    return false;
  }
  return name.size() == 1U ||
         name.find_first_not_of("0123456789", 1U) == std::string::npos;
}

/**
 * @brief Expand a statement into Toffoli-style statements
 */
void expand(const Statement& statement,
            const std::unordered_map<std::string, Macro>& macros,
            const std::unordered_map<std::string, std::string>& environment,
            std::vector<Statement>& out, const std::size_t depth = 0U) {
  if (depth > 64U) {
    throw std::runtime_error("macro recursion too deep");
  }
  if (statement.empty()) {
    return;
  }
  if (isToffoli(statement.front())) {
    Statement expanded;
    expanded.reserve(statement.size() - 1U);
    for (std::size_t i = 1U; i < statement.size(); ++i) {
      const auto it = environment.find(statement[i]);
      expanded.emplace_back(it == environment.end() ? statement[i]
                                                    : it->second);
    }
    out.emplace_back(std::move(expanded));
    return;
  }

  const auto macro = macros.find(statement.front());
  if (macro == macros.end()) {
    throw std::runtime_error("unsupported gate '" + statement.front() + "'");
  }
  if (macro->second.parameters.size() != statement.size() - 1U) {
    throw std::runtime_error("macro '" + statement.front() +
                             "' called with the wrong number of arguments");
  }
  auto local = environment;
  for (std::size_t i = 0U; i < macro->second.parameters.size(); ++i) {
    const auto& argument = statement[i + 1U];
    const auto it = environment.find(argument);
    local[macro->second.parameters[i]] =
        it == environment.end() ? argument : it->second;
  }
  for (const auto& inner : macro->second.body) {
    expand(inner, macros, local, out, depth + 1U);
  }
}

} // namespace

qc::QuantumComputation readRevLib(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open " + path);
  }

  std::vector<std::string> variables;
  std::vector<std::string> inputs;
  std::string constants;
  std::unordered_map<std::string, Macro> macros;
  std::vector<Statement> body;

  std::string line;
  Macro currentMacro;
  std::string currentMacroName;
  bool inMacro = false;
  bool inBody = false;

  while (std::getline(input, line)) {
    const auto tokens = split(line);
    if (tokens.empty() || line.starts_with('#')) {
      continue;
    }
    const auto& keyword = tokens[0];
    if (keyword == ".variables") {
      variables.assign(tokens.begin() + 1, tokens.end());
    } else if (keyword == ".inputs") {
      inputs.assign(tokens.begin() + 1, tokens.end());
    } else if (keyword == ".constants" && tokens.size() >= 2U) {
      constants = tokens[1];
    } else if (keyword == ".define") {
      inMacro = true;
      currentMacroName = tokens[1];
      currentMacro = Macro{};
      currentMacro.parameters.assign(tokens.begin() + 2, tokens.end());
    } else if (keyword == ".enddefine") {
      macros[currentMacroName] = currentMacro;
      inMacro = false;
    } else if (keyword == ".begin") {
      inBody = true;
    } else if (keyword == ".end") {
      if (inBody) {
        break;
      }
    } else if (inMacro) {
      if (keyword.front() != '.') {
        currentMacro.body.emplace_back(tokens);
      }
    } else if (inBody) {
      if (keyword.front() != '.') {
        body.emplace_back(tokens);
      }
    }
  }

  if (variables.empty()) {
    throw std::runtime_error("no .variables section in " + path);
  }

  std::unordered_map<std::string, qc::Qubit> index;
  for (std::size_t i = 0U; i < variables.size(); ++i) {
    index[variables[i]] = static_cast<qc::Qubit>(i);
  }

  const std::unordered_map<std::string, std::string> empty;
  std::vector<Statement> statements;
  for (const auto& statement : body) {
    expand(statement, macros, empty, statements);
  }

  qc::QuantumComputation circuit(variables.size());
  // Prepare the inputs that the file declares as constant.
  for (std::size_t i = 0U; i < variables.size(); ++i) {
    std::string value = i < inputs.size() ? inputs[i] : std::string{};
    if (value != "0" && value != "1" && i < constants.size()) {
      value = std::string(1U, constants[i]);
    }
    if (value == "1") {
      circuit.x(static_cast<qc::Qubit>(i));
    }
  }

  for (const auto& statement : statements) {
    if (statement.empty()) {
      continue;
    }
    const auto targetIt = index.find(statement.back());
    if (targetIt == index.end()) {
      throw std::runtime_error("unknown variable '" + statement.back() + "'");
    }
    const auto target = targetIt->second;
    if (statement.size() == 1U) {
      circuit.x(target);
      continue;
    }
    qc::Controls controls;
    for (std::size_t i = 0U; i + 1U < statement.size(); ++i) {
      const auto controlIt = index.find(statement[i]);
      if (controlIt == index.end()) {
        throw std::runtime_error("unknown variable '" + statement[i] + "'");
      }
      controls.emplace(controlIt->second);
    }
    if (controls.size() == 1U) {
      circuit.cx(*controls.begin(), target);
    } else {
      circuit.mcx(controls, target);
    }
  }
  return circuit;
}

} // namespace mqt::apps
