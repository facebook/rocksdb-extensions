# Copyright (c) Meta Platforms, Inc. and affiliates.
# This source code is licensed under both the GPLv2 (found in the
# COPYING file in the root directory) and Apache 2.0 License
# (found in the LICENSE.Apache file in the root directory).

add_library(
  rocksdb_extensions_nimble
  src/NimbleTable.cpp
  src/RocksDbReadFile.cpp)

target_include_directories(
  rocksdb_extensions_nimble
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(
  rocksdb_extensions_nimble
  PUBLIC
    ${ROCKSDB_EXTENSIONS_ROCKSDB_TARGET}
    ${ROCKSDB_EXTENSIONS_FOLLY_TARGET}
    ${ROCKSDB_EXTENSIONS_VELOX_TARGETS}
    ${ROCKSDB_EXTENSIONS_NIMBLE_TARGETS}
  PRIVATE
    fmt::fmt)

if(ROCKSDB_EXTENSIONS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
