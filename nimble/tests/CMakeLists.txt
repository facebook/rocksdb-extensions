# Copyright (c) Meta Platforms, Inc. and affiliates.
# This source code is licensed under both the GPLv2 (found in the
# COPYING file in the root directory) and Apache 2.0 License
# (found in the LICENSE.Apache file in the root directory).

find_package(GTest CONFIG QUIET)
if(NOT TARGET GTest::gtest_main AND ROCKSDB_EXTENSIONS_FETCH_DEPS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
if(NOT TARGET GTest::gtest_main)
  message(FATAL_ERROR
    "Could not find GTest::gtest_main. Provide a GTest package or enable "
    "ROCKSDB_EXTENSIONS_FETCH_DEPS.")
endif()

# Keep this list to targets used only by the integration test.
set(ROCKSDB_EXTENSIONS_NIMBLE_TEST_TARGETS
  nimble_velox_writer
  nimble_velox_reader
  nimble_deserializer
  velox_key_encoder
  velox_vector
  CACHE STRING "Additional targets needed by Nimble RocksDB integration tests")

rocksdb_extensions_require_targets(
  "Nimble integration test"
  ${ROCKSDB_EXTENSIONS_NIMBLE_TEST_TARGETS})

add_executable(
  rocksdb_extensions_nimble_test
  NimbleTableIntegrationTest.cpp)

target_link_libraries(
  rocksdb_extensions_nimble_test
  PRIVATE
    rocksdb_extensions_nimble
    ${ROCKSDB_EXTENSIONS_NIMBLE_TEST_TARGETS}
    GTest::gtest_main)

add_test(NAME rocksdb_extensions_nimble_test COMMAND rocksdb_extensions_nimble_test)
