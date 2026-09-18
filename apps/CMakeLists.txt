# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License

# Developer applications for the decision diagram variable ordering work.
# They are not installed and they are not part of the Python package.

add_executable(mqt-core-order-bench order_bench.cpp RevLibReader.cpp)
target_link_libraries(mqt-core-order-bench PRIVATE MQT::CoreDD MQT::CoreQASM)

add_executable(mqt-core-order-check order_check.cpp)
target_link_libraries(mqt-core-order-check PRIVATE MQT::CoreDD MQT::CoreQASM)

add_executable(mqt-core-dump-unitary dump_unitary.cpp)
target_link_libraries(mqt-core-dump-unitary PRIVATE MQT::CoreDD MQT::CoreQASM)

# Keep the binaries in a predictable place for the experiment scripts.
set_target_properties(
  mqt-core-order-bench mqt-core-order-check mqt-core-dump-unitary
  PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
