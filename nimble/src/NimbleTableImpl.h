// Copyright (c) Meta Platforms, Inc. and affiliates.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "dwio/nimble/tablet/TabletReaderCache.h"
#include "folly/executors/CPUThreadPoolExecutor.h"
#include "rocksdb/external_table.h"
#include "rocksdb_extensions/nimble/NimbleTable.h"
#include "velox/common/caching/FileHandle.h"
#include "velox/common/file/File.h"
#include "velox/common/memory/Memory.h"
#include "velox/dwio/common/Options.h"

namespace facebook::rocks {

class NimbleTableReader;

class NimbleTableIterator : public rocksdb::ExternalTableIterator {
public:
  NimbleTableIterator(const rocksdb::ReadOptions &readOptions,
                      NimbleTableReader *reader);

  ~NimbleTableIterator() override = default;

  void SeekToFirst() override;
  void SeekToLast() override;
  void Seek(const rocksdb::Slice &target) override;
  void SeekForPrev(const rocksdb::Slice &target) override;
  void Next() override;
  void Prev() override;

  bool Valid() const override;
  rocksdb::Slice key() const override;
  rocksdb::Slice value() const override;
  rocksdb::Status status() const override;

  bool PrepareValue() override;
  bool NextAndGetResult(rocksdb::IterateResult *result) override;
  rocksdb::IterBoundCheck UpperBoundCheckResult() override;

  void Prepare(const rocksdb::ScanOptions scanOpts[], size_t numOpts) override;

  size_t numProjectionGroups() const { return numProjectionGroups_; }

private:
  enum class Mode { kNone, kIngestion, kScan };

  void executeScan();
  void updateKey(int chunkIdx);

  NimbleTableReader *reader_;
  rocksdb::ReadOptions readOptions_;
  std::string seekKey_;
  std::string key_;
  bool valid_{false};
  rocksdb::Status status_;
  Mode mode_{Mode::kNone};
  const rocksdb::ScanOptions *scanOpts_{nullptr};
  size_t numScanOpts_{0};

  struct ScanResult {
    std::vector<folly::IOBuf> slices;
    int currentChunkIdx{-1};
  };

  std::vector<ScanResult> scanResults_;
  int currentScanIdx_{-1};
  bool scanExecuted_{false};
  const folly::IOBuf *currentChunk_{nullptr};
  rocksdb::IterBoundCheck boundCheckResult_{rocksdb::IterBoundCheck::kUnknown};
  size_t numProjectionGroups_{0};
};

class NimbleTableReader : public rocksdb::ExternalTableReader {
public:
  NimbleTableReader(
      std::shared_ptr<velox::ReadFile> readFile,
      NimbleTableFactory::ReaderOptionsBuilder readerOptionsBuilder);

  ~NimbleTableReader() override = default;

  rocksdb::ExternalTableIterator *
  NewIterator(const rocksdb::ReadOptions &readOptions,
              const rocksdb::SliceTransform *prefixExtractor) override;

  rocksdb::Status Get(const rocksdb::ReadOptions &readOptions,
                      const rocksdb::Slice &key,
                      const rocksdb::SliceTransform *prefixExtractor,
                      rocksdb::PinnableSlice *value) override;

  void MultiGet(const rocksdb::ReadOptions &readOptions,
                const std::vector<rocksdb::Slice> &keys,
                const rocksdb::SliceTransform *prefixExtractor,
                std::vector<rocksdb::PinnableSlice> *values,
                std::vector<rocksdb::Status> *statuses) override;

  std::shared_ptr<const rocksdb::TableProperties>
  GetTableProperties() const override;

  const std::shared_ptr<folly::Executor> &ioExecutor() const {
    return ioExecutor_;
  }

private:
  const velox::FileHandle fileHandle_;
  const NimbleTableFactory::ReaderOptionsBuilder readerOptionsBuilder_;
  const std::shared_ptr<nimble::TabletReader> tablet_;
  const std::shared_ptr<folly::Executor> ioExecutor_{
      std::make_shared<folly::CPUThreadPoolExecutor>(4)};
  const std::string minKey_;
  const std::string maxKey_;
  const uint64_t numRows_;
  const uint64_t fileSize_;

  friend class NimbleTableIterator;
};

class NimbleTableBuilder : public rocksdb::ExternalTableBuilder {
public:
  void Add(const rocksdb::Slice &key, const rocksdb::Slice &value) override;
  rocksdb::Status Finish() override;
  void Abandon() override;
  uint64_t FileSize() const override;
  rocksdb::TableProperties GetTableProperties() const override;
  rocksdb::Status status() const override;
};

} // namespace facebook::rocks
