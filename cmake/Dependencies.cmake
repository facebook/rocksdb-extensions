# Copyright (c) Meta Platforms, Inc. and affiliates.
# This source code is licensed under both the GPLv2 (found in the
# COPYING file in the root directory) and Apache 2.0 License
# (found in the LICENSE.Apache file in the root directory).

include_guard(GLOBAL)

include(FetchContent)

set(ROCKSDB_EXTENSIONS_ROCKSDB_GIT_REPOSITORY "https://github.com/facebook/rocksdb.git" CACHE STRING "RocksDB repository")
set(ROCKSDB_EXTENSIONS_ROCKSDB_GIT_TAG "main" CACHE STRING "RocksDB revision with external_table.h support")
set(ROCKSDB_EXTENSIONS_ROCKSDB_SOURCE_DIR "" CACHE PATH "Optional local RocksDB source directory")
set(ROCKSDB_EXTENSIONS_FOLLY_GIT_REPOSITORY "https://github.com/facebook/folly.git" CACHE STRING "Folly repository")
set(ROCKSDB_EXTENSIONS_FOLLY_GIT_TAG "main" CACHE STRING "Folly revision")
set(ROCKSDB_EXTENSIONS_FOLLY_SOURCE_DIR "" CACHE PATH "Optional local Folly source directory")
set(ROCKSDB_EXTENSIONS_VELOX_GIT_REPOSITORY "https://github.com/facebookincubator/velox.git" CACHE STRING "Velox repository")
set(ROCKSDB_EXTENSIONS_VELOX_GIT_TAG "main" CACHE STRING "Velox revision")
set(ROCKSDB_EXTENSIONS_VELOX_SOURCE_DIR "" CACHE PATH "Optional local Velox source directory")
set(ROCKSDB_EXTENSIONS_NIMBLE_GIT_REPOSITORY "https://github.com/facebookincubator/nimble.git" CACHE STRING "Nimble repository")
set(ROCKSDB_EXTENSIONS_NIMBLE_GIT_TAG "main" CACHE STRING "Nimble revision")
set(ROCKSDB_EXTENSIONS_NIMBLE_SOURCE_DIR "" CACHE PATH "Optional local Nimble source directory")
set(ROCKSDB_EXTENSIONS_FLATBUFFERS_GIT_REPOSITORY "https://github.com/google/flatbuffers.git" CACHE STRING "FlatBuffers repository")
set(ROCKSDB_EXTENSIONS_FLATBUFFERS_GIT_TAG "v25.2.10" CACHE STRING "FlatBuffers revision")

function(rocksdb_extensions_fetch_dependency name repository tag)
  if(NOT ROCKSDB_EXTENSIONS_FETCH_DEPS)
    return()
  endif()

  FetchContent_Declare(
    ${name}
    GIT_REPOSITORY ${repository}
    GIT_TAG ${tag}
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(${name})
endfunction()

function(rocksdb_extensions_validate_source_dependency name source_dir)
  if("${source_dir}" STREQUAL "")
    return()
  endif()
  if(NOT EXISTS "${source_dir}/CMakeLists.txt")
    message(FATAL_ERROR
      "The configured ${name} source dir does not contain CMakeLists.txt. "
      "Point the *_SOURCE_DIR cache variable at a standalone checkout root.")
  endif()

  set(cmake_lists "${source_dir}/CMakeLists.txt")
  if("${name}" STREQUAL "folly")
    file(STRINGS "${cmake_lists}" generated_folly_cmake REGEX "folly_add_library")
    if(generated_folly_cmake)
      message(FATAL_ERROR
        "The configured Folly source dir appears to be a generated CMake "
        "fragment that requires non-standalone CMake macros. Provide an installed "
        "Folly package target, use a standalone Folly checkout, or leave "
        "ROCKSDB_EXTENSIONS_FOLLY_SOURCE_DIR unset to use FetchContent.")
    endif()
  elseif("${name}" STREQUAL "velox")
    file(STRINGS "${cmake_lists}" velox_project_line REGEX "project *\\(")
    if(NOT velox_project_line)
      message(FATAL_ERROR
        "The configured Velox source dir does not look like a standalone "
        "checkout root. Provide an installed Velox package target, use a "
        "standalone Velox checkout, or leave "
        "ROCKSDB_EXTENSIONS_VELOX_SOURCE_DIR unset to use FetchContent.")
    endif()
  endif()
endfunction()

function(rocksdb_extensions_add_source_dependency out_var name source_dir)
  set(${out_var} FALSE PARENT_SCOPE)
  if("${source_dir}" STREQUAL "")
    return()
  endif()
  rocksdb_extensions_validate_source_dependency("${name}" "${source_dir}")
  add_subdirectory(
    "${source_dir}"
    "${CMAKE_BINARY_DIR}/_deps/${name}-build"
    EXCLUDE_FROM_ALL)
  set(${out_var} TRUE PARENT_SCOPE)
endfunction()

function(rocksdb_extensions_resolve_target out_var)
  foreach(candidate IN LISTS ARGN)
    if(TARGET ${candidate})
      set(${out_var} ${candidate} PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out_var} "" PARENT_SCOPE)
endfunction()

function(rocksdb_extensions_require_targets group_name)
  set(missing_targets)
  foreach(required_target IN LISTS ARGN)
    if(NOT TARGET ${required_target})
      list(APPEND missing_targets ${required_target})
    endif()
  endforeach()
  if(missing_targets)
    string(REPLACE ";" ", " missing_targets_string "${missing_targets}")
    message(FATAL_ERROR
      "Missing ${group_name} CMake target(s): ${missing_targets_string}. "
      "Use compatible dependency revisions or override the target list cache variables.")
  endif()
endfunction()

find_package(RocksDB CONFIG QUIET)
if(NOT TARGET RocksDB::rocksdb AND NOT TARGET rocksdb AND NOT TARGET rocksdb_static)
  rocksdb_extensions_add_source_dependency(
    ROCKSDB_EXTENSIONS_ROCKSDB_SOURCE_ADDED
    rocksdb
    "${ROCKSDB_EXTENSIONS_ROCKSDB_SOURCE_DIR}")
endif()
if(NOT TARGET RocksDB::rocksdb AND NOT TARGET rocksdb AND NOT TARGET rocksdb_static AND NOT ROCKSDB_EXTENSIONS_ROCKSDB_SOURCE_ADDED)
  set(WITH_TESTS OFF CACHE BOOL "" FORCE)
  set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
  set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
  set(WITH_CORE_TOOLS OFF CACHE BOOL "" FORCE)
  rocksdb_extensions_fetch_dependency(
    rocksdb
    ${ROCKSDB_EXTENSIONS_ROCKSDB_GIT_REPOSITORY}
    ${ROCKSDB_EXTENSIONS_ROCKSDB_GIT_TAG})
endif()

# Nimble requires both the FlatBuffers headers and flatc. Some system packages
# export incompatible package-name capitalization or omit the compiler. The
# find-package override makes Nimble's find_package(flatbuffers) resolve this
# pinned source build consistently.
if(ROCKSDB_EXTENSIONS_FETCH_DEPS)
  set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FLATBUFFERS_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    flatbuffers
    GIT_REPOSITORY ${ROCKSDB_EXTENSIONS_FLATBUFFERS_GIT_REPOSITORY}
    GIT_TAG ${ROCKSDB_EXTENSIONS_FLATBUFFERS_GIT_TAG}
    GIT_SHALLOW TRUE
    OVERRIDE_FIND_PACKAGE)
  FetchContent_MakeAvailable(flatbuffers)
endif()

if(NOT TARGET fmt::fmt)
  find_package(fmt CONFIG QUIET)
endif()
if(NOT TARGET fmt::fmt)
  find_package(fmt QUIET)
endif()

# These options must be in the cache before Nimble adds its pinned Velox
# submodule. They also apply if Velox has to be resolved separately below.
set(Boost_SOURCE BUNDLED CACHE STRING "" FORCE)
set(FastFloat_SOURCE BUNDLED CACHE STRING "" FORCE)
set(VELOX_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(VELOX_BUILD_TEST_UTILS OFF CACHE BOOL "" FORCE)
set(VELOX_BUILD_MINIMAL_WITH_DWIO ON CACHE BOOL "" FORCE)
set(VELOX_BUILD_RUNNER OFF CACHE BOOL "" FORCE)
set(VELOX_ENABLE_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(VELOX_ENABLE_GEO OFF CACHE BOOL "" FORCE)
set(VELOX_ENABLE_PARQUET OFF CACHE BOOL "" FORCE)

find_package(nimble CONFIG QUIET)
if(NOT TARGET nimble_index_projector AND NOT TARGET nimble::nimble)
  set(NIMBLE_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(NIMBLE_ENABLE_BENCHMARKS OFF CACHE BOOL "" FORCE)
  rocksdb_extensions_add_source_dependency(
    ROCKSDB_EXTENSIONS_NIMBLE_SOURCE_ADDED
    nimble
    "${ROCKSDB_EXTENSIONS_NIMBLE_SOURCE_DIR}")
endif()
if(NOT TARGET nimble_index_projector AND NOT TARGET nimble::nimble AND NOT ROCKSDB_EXTENSIONS_NIMBLE_SOURCE_ADDED)
  rocksdb_extensions_fetch_dependency(
    nimble
    ${ROCKSDB_EXTENSIONS_NIMBLE_GIT_REPOSITORY}
    ${ROCKSDB_EXTENSIONS_NIMBLE_GIT_TAG})
endif()

# A source build of Nimble adds the Velox submodule revision that Nimble pins.
# Resolve Velox only after Nimble so CMake does not add an independent Velox
# checkout first and then fail when Nimble creates the same targets again.
find_package(velox CONFIG QUIET)
if(NOT TARGET velox_dwio_common)
  rocksdb_extensions_add_source_dependency(
    ROCKSDB_EXTENSIONS_VELOX_SOURCE_ADDED
    velox
    "${ROCKSDB_EXTENSIONS_VELOX_SOURCE_DIR}")
endif()
if(NOT TARGET velox_dwio_common AND NOT ROCKSDB_EXTENSIONS_VELOX_SOURCE_ADDED)
  rocksdb_extensions_fetch_dependency(
    velox
    ${ROCKSDB_EXTENSIONS_VELOX_GIT_REPOSITORY}
    ${ROCKSDB_EXTENSIONS_VELOX_GIT_TAG})
endif()

# Resolve these after Nimble and Velox so a source build uses their compatible
# pinned revisions. Adding independent source trees first creates duplicate
# Folly::folly and fmt::fmt targets and can mix incompatible versions.
if(NOT TARGET fmt::fmt)
  find_package(fmt CONFIG QUIET)
endif()
if(NOT TARGET fmt::fmt)
  find_package(fmt QUIET)
endif()

if(NOT TARGET Folly::folly AND NOT TARGET folly)
  find_package(Folly CONFIG QUIET)
endif()
if(NOT TARGET Folly::folly AND NOT TARGET folly)
  rocksdb_extensions_add_source_dependency(
    ROCKSDB_EXTENSIONS_FOLLY_SOURCE_ADDED
    folly
    "${ROCKSDB_EXTENSIONS_FOLLY_SOURCE_DIR}")
endif()
if(NOT TARGET Folly::folly AND NOT TARGET folly AND NOT ROCKSDB_EXTENSIONS_FOLLY_SOURCE_ADDED)
  rocksdb_extensions_fetch_dependency(
    folly
    ${ROCKSDB_EXTENSIONS_FOLLY_GIT_REPOSITORY}
    ${ROCKSDB_EXTENSIONS_FOLLY_GIT_TAG})
endif()

rocksdb_extensions_resolve_target(
  ROCKSDB_EXTENSIONS_ROCKSDB_TARGET
  RocksDB::rocksdb
  rocksdb
  rocksdb_static)
if(NOT ROCKSDB_EXTENSIONS_ROCKSDB_TARGET)
  message(FATAL_ERROR
    "Could not find a RocksDB CMake target. Provide RocksDB::rocksdb, "
    "rocksdb, or rocksdb_static through CMAKE_PREFIX_PATH, "
    "ROCKSDB_EXTENSIONS_ROCKSDB_SOURCE_DIR, or ROCKSDB_EXTENSIONS_FETCH_DEPS.")
endif()

rocksdb_extensions_resolve_target(
  ROCKSDB_EXTENSIONS_FOLLY_TARGET
  Folly::folly
  folly)
if(NOT ROCKSDB_EXTENSIONS_FOLLY_TARGET)
  message(FATAL_ERROR
    "Could not find a Folly CMake target. Provide Folly::folly or folly "
    "through CMAKE_PREFIX_PATH, ROCKSDB_EXTENSIONS_FOLLY_SOURCE_DIR, or "
    "ROCKSDB_EXTENSIONS_FETCH_DEPS.")
endif()

if(NOT TARGET fmt::fmt)
  message(FATAL_ERROR
    "Could not find fmt::fmt through CMAKE_PREFIX_PATH or the system "
    "package registry.")
endif()

# Keep these lists to targets the extension's public headers or implementation
# use directly. Transitive dependencies should come from each dependency's own
# CMake targets.
set(ROCKSDB_EXTENSIONS_VELOX_TARGETS
  velox_common_base
  velox_caching
  velox_file
  velox_memory
  velox_type
  velox_dwio_common
  velox_exception
  CACHE STRING "Velox targets required by the Nimble RocksDB extension")

set(ROCKSDB_EXTENSIONS_NIMBLE_TARGETS
  nimble_common
  nimble_deserializer
  nimble_serializer
  nimble_tablet_reader
  nimble_tablet_reader_cache
  nimble_velox_common
  nimble_index_projector
  CACHE STRING "Nimble targets required by the Nimble RocksDB extension")

rocksdb_extensions_require_targets(
  "Velox"
  ${ROCKSDB_EXTENSIONS_VELOX_TARGETS})
rocksdb_extensions_require_targets(
  "Nimble"
  ${ROCKSDB_EXTENSIONS_NIMBLE_TARGETS})
