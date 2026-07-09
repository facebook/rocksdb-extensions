// Copyright (c) Meta Platforms, Inc. and affiliates.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <folly/io/IOBuf.h>

#include "dwio/nimble/common/Exceptions.h"
#include "dwio/nimble/serializer/DeserializerImpl.h"
#include "dwio/nimble/velox/RowRange.h"
#include "rocksdb/external_table.h"
#include "velox/common/memory/Memory.h"
#include "velox/dwio/common/Options.h"

namespace facebook::rocks {

inline const folly::IOBuf *GetNimbleResultIOBuf(const rocksdb::Slice &value) {
  return reinterpret_cast<const folly::IOBuf *>(value.data());
}

inline folly::IOBuf CloneNimbleResultIOBuf(const rocksdb::Slice &value) {
  return GetNimbleResultIOBuf(value)->cloneAsValue();
}

inline uint32_t readResultChunkRowCount(const folly::IOBuf &chunkSlice) {
  const char *pos = reinterpret_cast<const char *>(chunkSlice.data());
  const char *end = pos + chunkSlice.length();
  return nimble::serde::readTabletChunkHeader(pos, end).rowCount;
}

inline nimble::RowRange readResultRowRange(const folly::IOBuf &chunkSlice) {
  const char *pos = reinterpret_cast<const char *>(chunkSlice.data());
  const char *end = pos + chunkSlice.length();
  return nimble::serde::readTabletChunkHeader(pos, end).rowRange;
}

inline std::optional<std::string>
readResultResumeKey(const folly::IOBuf &chunkSlice) {
  const char *pos = reinterpret_cast<const char *>(chunkSlice.data());
  const char *end = pos + chunkSlice.length();
  return nimble::serde::readTabletChunkHeader(pos, end).resumeKey;
}

class NimbleTableFactory : public rocksdb::ExternalTableFactory {
public:
  using ReaderOptionsBuilder = std::function<velox::dwio::common::ReaderOptions(
      velox::memory::MemoryPool *)>;

  explicit NimbleTableFactory(ReaderOptionsBuilder readerOptionsBuilder);

  ~NimbleTableFactory() override = default;

  const char *Name() const override { return kNimbleTable; }

  rocksdb::Status
  NewTableReader(const rocksdb::ReadOptions &readOptions,
                 const std::string &path,
                 const rocksdb::ExternalTableOptions &tableOptions,
                 std::unique_ptr<rocksdb::ExternalTableReader> *tableReader)
      const override;

  rocksdb::ExternalTableBuilder *
  NewTableBuilder(const rocksdb::ExternalTableBuilderOptions &builderOptions,
                  const std::string &filePath,
                  rocksdb::FSWritableFile *file) const override;

  static constexpr const char *kNimbleTable = "NimbleTable";

private:
  ReaderOptionsBuilder readerOptionsBuilder_;
};

} // namespace facebook::rocks
