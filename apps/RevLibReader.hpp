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
 * @file RevLibReader.hpp
 * @brief Read RevLib .real files into an MQT Core circuit
 */

#pragma once

#include "ir/QuantumComputation.hpp"

#include <string>

namespace mqt::apps {

/**
 * @brief Read a RevLib .real file
 * @details The reader expands the `.define` macros of the MCT library and maps
 * a `t` gate with any number of operands to a multi-controlled NOT gate whose
 * last operand is the target. Inputs that the file declares as constant are
 * prepared with NOT gates. Gate names that the reader does not know, for
 * example the non-classical V gate, raise a `std::runtime_error`.
 * @param path The path of the .real file
 * @return The circuit
 */
[[nodiscard]] qc::QuantumComputation readRevLib(const std::string& path);

} // namespace mqt::apps
