// Copyright (c) Meta Platforms, Inc. and affiliates.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <folly/Range.h>
#include <folly/io/IOBuf.h>

#include "rocksdb/file_system.h"
#include "velox/common/file/File.h"

namespace facebook::rocks {

class RocksDbReadFile final : public velox::ReadFile {
public:
  RocksDbReadFile(std::string path,
                  std::unique_ptr<rocksdb::FSRandomAccessFile> &&file,
                  uint64_t fileSize, rocksdb::IOOptions ioOptions);

  std::string_view pread(uint64_t offset, uint64_t length, void *buf,
                         const velox::FileIoContext &context) const override;

  uint64_t preadv(uint64_t offset,
                  const std::vector<folly::Range<char *>> &buffers,
                  const velox::FileIoContext &context) const override;

  uint64_t preadv(folly::Range<const velox::common::Region *> regions,
                  folly::Range<folly::IOBuf *> iobufs,
                  const velox::FileIoContext &context) const override;

  uint64_t preadv(folly::Range<const velox::common::Region *> regions,
                  folly::Range<const folly::Range<char *> *> buffers,
                  const velox::FileIoContext &context) const override;

  bool shouldCoalesce() const override;

  uint64_t size() const override;

  uint64_t memoryUsage() const override;

  std::string getName() const override;

  uint64_t getNaturalReadSize() const override;

private:
  rocksdb::Slice readFromFile(uint64_t offset, uint64_t length, char *scratch,
                              uint64_t minExpectedSize) const;

  static std::unique_ptr<char, decltype(&std::free)>
  allocateAligned(size_t alignment, size_t size);

  void readFully(uint64_t offset, uint64_t length, char *buffer,
                 bool countBytes) const;

  const std::string path_;
  const std::unique_ptr<rocksdb::FSRandomAccessFile> file_;
  const uint64_t fileSize_;
  const rocksdb::IOOptions ioOptions_;
};

} // namespace facebook::rocks
