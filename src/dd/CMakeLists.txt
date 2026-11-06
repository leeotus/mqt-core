# Copyright (c) 2023 - 2026 Chair for Design Automation, TUM
# Copyright (c) 2025 - 2026 Munich Quantum Software Company GmbH
# All rights reserved.
#
# SPDX-License-Identifier: MIT
#
# Licensed under the MIT License

if(NOT TARGET ${MQT_CORE_TARGET_NAME}-dd)
  # collect headers and source files
  file(GLOB_RECURSE DD_HEADERS ${MQT_CORE_INCLUDE_BUILD_DIR}/dd/*.hpp)
  file(GLOB_RECURSE DD_SOURCES **.cpp)

  # create the library target (initially empty)
  add_mqt_core_library(${MQT_CORE_TARGET_NAME}-dd ALIAS_NAME DD)

  # add sources to target
  target_sources(${MQT_CORE_TARGET_NAME}-dd PRIVATE ${DD_SOURCES})

  # add headers using file sets
  target_sources(
    ${MQT_CORE_TARGET_NAME}-dd PUBLIC FILE_SET HEADERS BASE_DIRS ${MQT_CORE_INCLUDE_BUILD_DIR}
                                      FILES ${DD_HEADERS})

  # add link libraries
  target_link_libraries(
    ${MQT_CORE_TARGET_NAME}-dd
    PUBLIC MQT::CoreIR
    PRIVATE $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>)

  # The dense matrix MLE in QST benefits from a fast BLAS and from thread
  # parallelism. Both are optional: without them the code falls back to the
  # scalar loops.
  find_library(MQT_CORE_BLAS_LIBRARY NAMES blas openblas libblas.so.3)
  if(MQT_CORE_BLAS_LIBRARY)
    target_link_libraries(${MQT_CORE_TARGET_NAME}-dd PRIVATE ${MQT_CORE_BLAS_LIBRARY})
    target_compile_definitions(${MQT_CORE_TARGET_NAME}-dd PRIVATE MQT_CORE_QST_USE_BLAS)
    message(STATUS "QST: using BLAS at ${MQT_CORE_BLAS_LIBRARY}")
  endif()

  find_package(OpenMP QUIET)
  if(OpenMP_CXX_FOUND)
    target_link_libraries(${MQT_CORE_TARGET_NAME}-dd PRIVATE OpenMP::OpenMP_CXX)
    target_compile_definitions(${MQT_CORE_TARGET_NAME}-dd PRIVATE MQT_CORE_QST_USE_OPENMP)
    message(STATUS "QST: using OpenMP")
  endif()

  # generate export header
  include(GenerateExportHeader)
  generate_export_header(${MQT_CORE_TARGET_NAME}-dd BASE_NAME mqt_core_dd)
  target_sources(
    ${MQT_CORE_TARGET_NAME}-dd PUBLIC FILE_SET HEADERS BASE_DIRS ${CMAKE_CURRENT_BINARY_DIR}/..
                                      FILES ${CMAKE_CURRENT_BINARY_DIR}/mqt_core_dd_export.h)
  if(NOT BUILD_MQT_CORE_SHARED_LIBS)
    target_compile_definitions(${MQT_CORE_TARGET_NAME}-dd PUBLIC MQT_CORE_DD_STATIC_DEFINE)
  endif()

  # add to list of MQT core targets
  set(MQT_CORE_TARGETS
      ${MQT_CORE_TARGETS} ${MQT_CORE_TARGET_NAME}-dd
      PARENT_SCOPE)
endif()
