# Copyright (c) Meta Platforms, Inc. and affiliates.
# This source code is licensed under both the GPLv2 (found in the
# COPYING file in the root directory) and Apache 2.0 License
# (found in the LICENSE.Apache file in the root directory).

cmake_minimum_required(VERSION 3.24)

project(rocksdb_extensions LANGUAGES CXX)

option(ROCKSDB_EXTENSIONS_BUILD_TESTS "Build unit tests" ON)
option(ROCKSDB_EXTENSIONS_FETCH_DEPS "Allow CMake FetchContent fallback when package targets are not already available" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/Dependencies.cmake)

if(ROCKSDB_EXTENSIONS_BUILD_TESTS)
  include(CTest)
endif()

add_subdirectory(nimble)
