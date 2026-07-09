// Copyright (c) Meta Platforms, Inc. and affiliates.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include "rocksdb_extensions/nimble/NimbleTable.h"

#include "NimbleTableImpl.h"

#include <atomic>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/core.h>
#include <folly/Portability.h>
#include <folly/Synchronized.h>
#include <folly/ThreadLocal.h>

#include "dwio/nimble/index/ClusterIndex.h"
#include "dwio/nimble/tablet/TabletReader.h"
#include "dwio/nimble/tablet/TabletReaderCache.h"
#include "dwio/nimble/velox/index/NimbleIndexProjector.h"
#include "rocksdb/file_system.h"
#include "rocksdb_extensions/nimble/RocksDbReadFile.h"
#include "velox/common/caching/FileIds.h"
#include "velox/common/file/File.h"
#include "velox/common/io/IoStatistics.h"
#include "velox/type/Subfield.h"

namespace facebook::rocks {
namespace {

constexpr const char *kNimbleColumns = "nimble.columns";
constexpr const char *kNimbleMaxRows = "nimble.max_rows";
constexpr const char *kNimbleMaxResultBytes = "nimble.max_result_bytes";
constexpr uint64_t kDefaultMaxRows = 10000;

void validateReaderOptions(
    const velox::dwio::common::ReaderOptions &readerOptions) {
  VELOX_CHECK_NOT_NULL(
      readerOptions.dataIoStats(),
      "NimbleTableReader requires ReaderOptions::dataIoStats to be set");
  VELOX_CHECK_NOT_NULL(
      readerOptions.metadataIoStats(),
      "NimbleTableReader requires ReaderOptions::metadataIoStats to be set");
  VELOX_CHECK_NOT_NULL(
      readerOptions.indexIoStats(),
      "NimbleTableReader requires ReaderOptions::indexIoStats to be set");
}

void checkResultChunkManaged(const folly::IOBuf &chunk) {
  const folly::IOBuf *node = &chunk;
  size_t index = 0;
  do {
    if (FOLLY_UNLIKELY(!node->isManagedOne())) {
      VELOX_FAIL("NimbleIndexProjector returned an unmanaged result IOBuf node "
                 "(index {} in the chunk chain)",
                 index);
    }
    node = node->next();
    ++index;
  } while (node != &chunk);
}

void debugCheckFreshIoStats(
    const std::shared_ptr<velox::io::IoStatistics> &ioStats) {
  VELOX_DCHECK_NOT_NULL(ioStats);
  VELOX_DCHECK_EQ(ioStats->rawBytesRead(), 0);
  VELOX_DCHECK_EQ(ioStats->rawOverreadBytes(), 0);
  VELOX_DCHECK_EQ(ioStats->read().sum(), 0);
  VELOX_DCHECK_EQ(ioStats->read().count(), 0);
  VELOX_DCHECK_EQ(ioStats->ramHit().sum(), 0);
  VELOX_DCHECK_EQ(ioStats->ramHit().count(), 0);
  VELOX_DCHECK_EQ(ioStats->storageReadLatencyUs().sum(), 0);
}

void debugCheckFreshReaderOptions(
    const velox::dwio::common::ReaderOptions &readerOptions) {
  debugCheckFreshIoStats(readerOptions.dataIoStats());
  debugCheckFreshIoStats(readerOptions.metadataIoStats());
  debugCheckFreshIoStats(readerOptions.indexIoStats());
}

class ThreadLocalMemoryPoolProvider {
public:
  velox::memory::MemoryPool *getPool() {
    auto &threadPoolPtr = *threadPool_;
    if (threadPoolPtr == nullptr) {
      auto rootPool = velox::memory::memoryManager()->addRootPool(
          fmt::format("nimble_table_thread[{}]", nextPoolId_.fetch_add(1)));
      auto leafPool = rootPool->addLeafChild("data");
      threadPoolPtr = leafPool.get();
      poolHolders_.wlock()->push_back(std::move(leafPool));
    }
    return threadPoolPtr;
  }

private:
  std::atomic_uint64_t nextPoolId_{0};
  folly::ThreadLocal<velox::memory::MemoryPool *> threadPool_;
  folly::Synchronized<std::vector<std::shared_ptr<velox::memory::MemoryPool>>>
      poolHolders_;
};

velox::memory::MemoryPool *threadMemoryPool() {
  static ThreadLocalMemoryPoolProvider provider;
  return provider.getPool();
}

std::vector<velox::common::Subfield> parseColumns(std::string_view columns) {
  std::vector<velox::common::Subfield> subfields;
  size_t start = 0;
  while (start < columns.size()) {
    auto end = columns.find(',', start);
    if (end == std::string_view::npos) {
      end = columns.size();
    }
    auto column = columns.substr(start, end - start);
    if (!column.empty()) {
      subfields.emplace_back(std::string(column));
    }
    start = end + 1;
  }
  return subfields;
}

rocksdb::Status parsePositiveUint64(
    const std::unordered_map<std::string, std::string> &propertyBag,
    const char *key, uint64_t defaultValue, uint64_t &result) {
  result = defaultValue;
  auto it = propertyBag.find(key);
  if (it == propertyBag.end()) {
    return rocksdb::Status::OK();
  }
  const auto &value = it->second;
  if (value.empty()) {
    return rocksdb::Status::InvalidArgument(std::string(key) +
                                            " must be a positive integer");
  }

  uint64_t parsed{0};
  const auto *begin = value.data();
  const auto *end = value.data() + value.size();
  auto [ptr, ec] = std::from_chars(begin, end, parsed);
  if (ec != std::errc() || ptr != end || parsed == 0) {
    return rocksdb::Status::InvalidArgument(std::string(key) +
                                            " must be a positive integer");
  }

  result = parsed;
  return rocksdb::Status::OK();
}

} // namespace

NimbleTableIterator::NimbleTableIterator(
    const rocksdb::ReadOptions &readOptions, NimbleTableReader *reader)
    : reader_(reader), readOptions_(readOptions) {}

void NimbleTableIterator::SeekToFirst() {
  mode_ = Mode::kIngestion;
  key_ = reader_->minKey_;
  valid_ = !key_.empty();
  boundCheckResult_ = valid_ ? rocksdb::IterBoundCheck::kInbound
                             : rocksdb::IterBoundCheck::kUnknown;
  status_ = rocksdb::Status::OK();
}

void NimbleTableIterator::SeekToLast() {
  mode_ = Mode::kIngestion;
  key_ = reader_->maxKey_;
  valid_ = !key_.empty();
  boundCheckResult_ = valid_ ? rocksdb::IterBoundCheck::kInbound
                             : rocksdb::IterBoundCheck::kUnknown;
  status_ = rocksdb::Status::OK();
}

void NimbleTableIterator::Seek(const rocksdb::Slice &target) {
  if (mode_ != Mode::kScan) {
    status_ = rocksdb::Status::NotSupported(
        "Seek requires Prepare() to be called first");
    valid_ = false;
    boundCheckResult_ = rocksdb::IterBoundCheck::kUnknown;
    return;
  }

  if (!scanExecuted_) {
    executeScan();
    if (!status_.ok()) {
      valid_ = false;
      boundCheckResult_ = rocksdb::IterBoundCheck::kUnknown;
      return;
    }
  }

  ++currentScanIdx_;
  if (currentScanIdx_ >= static_cast<int>(scanResults_.size())) {
    valid_ = false;
    boundCheckResult_ = rocksdb::IterBoundCheck::kUnknown;
    return;
  }

  auto &scanResult = scanResults_[currentScanIdx_];
  if (scanResult.slices.empty()) {
    valid_ = false;
    boundCheckResult_ = rocksdb::IterBoundCheck::kOutOfBound;
    return;
  }

  scanResult.currentChunkIdx = 0;
  currentChunk_ = scanResult.slices.data();
  seekKey_ = target.ToString();
  updateKey(0);
  valid_ = true;
  boundCheckResult_ = rocksdb::IterBoundCheck::kInbound;
  status_ = rocksdb::Status::OK();
}

void NimbleTableIterator::SeekForPrev(const rocksdb::Slice &) {
  status_ = rocksdb::Status::NotSupported("SeekForPrev not supported");
  valid_ = false;
  boundCheckResult_ = rocksdb::IterBoundCheck::kUnknown;
}

void NimbleTableIterator::Next() {
  if (mode_ == Mode::kIngestion) {
    valid_ = false;
    boundCheckResult_ = rocksdb::IterBoundCheck::kUnknown;
    return;
  }

  if (mode_ != Mode::kScan || currentScanIdx_ < 0 ||
      currentScanIdx_ >= static_cast<int>(scanResults_.size())) {
    valid_ = false;
    boundCheckResult_ = rocksdb::IterBoundCheck::kUnknown;
    return;
  }

  auto &scanResult = scanResults_[currentScanIdx_];
  ++scanResult.currentChunkIdx;
  if (scanResult.currentChunkIdx >=
      static_cast<int>(scanResult.slices.size())) {
    valid_ = false;
    boundCheckResult_ = rocksdb::IterBoundCheck::kOutOfBound;
    return;
  }

  currentChunk_ = &scanResult.slices[scanResult.currentChunkIdx];
  updateKey(scanResult.currentChunkIdx);
  valid_ = true;
  boundCheckResult_ = rocksdb::IterBoundCheck::kInbound;
}

void NimbleTableIterator::updateKey(int chunkIdx) {
  assert(chunkIdx >= 0);
  key_ = seekKey_;
  auto be = static_cast<uint32_t>(chunkIdx);
  char buf[sizeof(uint32_t)];
  buf[0] = static_cast<char>((be >> 24) & 0xFF);
  buf[1] = static_cast<char>((be >> 16) & 0xFF);
  buf[2] = static_cast<char>((be >> 8) & 0xFF);
  buf[3] = static_cast<char>(be & 0xFF);
  key_.append(buf, sizeof(buf));
}

void NimbleTableIterator::Prev() {
  status_ = rocksdb::Status::NotSupported("Prev not supported");
  valid_ = false;
  boundCheckResult_ = rocksdb::IterBoundCheck::kUnknown;
}

bool NimbleTableIterator::Valid() const { return valid_; }

rocksdb::Slice NimbleTableIterator::key() const {
  return valid_ ? rocksdb::Slice(key_) : rocksdb::Slice();
}

rocksdb::Slice NimbleTableIterator::value() const {
  if (!valid_ || mode_ == Mode::kIngestion) {
    return rocksdb::Slice();
  }
  return rocksdb::Slice(reinterpret_cast<const char *>(currentChunk_),
                        sizeof(folly::IOBuf));
}

rocksdb::Status NimbleTableIterator::status() const { return status_; }

bool NimbleTableIterator::PrepareValue() { return true; }

bool NimbleTableIterator::NextAndGetResult(rocksdb::IterateResult *result) {
  Next();
  if (Valid()) {
    result->key = key();
  } else {
    result->key = rocksdb::Slice();
  }
  result->bound_check_result = UpperBoundCheckResult();
  result->value_prepared = Valid();
  return Valid();
}

rocksdb::IterBoundCheck NimbleTableIterator::UpperBoundCheckResult() {
  return valid_ ? rocksdb::IterBoundCheck::kInbound : boundCheckResult_;
}

void NimbleTableIterator::Prepare(const rocksdb::ScanOptions scanOpts[],
                                  size_t numOpts) {
  mode_ = Mode::kScan;
  scanOpts_ = scanOpts;
  numScanOpts_ = numOpts;
  currentScanIdx_ = -1;
  scanExecuted_ = false;
  scanResults_.clear();
  currentChunk_ = nullptr;
  valid_ = false;
  boundCheckResult_ = rocksdb::IterBoundCheck::kUnknown;
  status_ = rocksdb::Status::OK();
}

void NimbleTableIterator::executeScan() {
  scanExecuted_ = true;
  scanResults_.resize(numScanOpts_);
  VELOX_DCHECK_NULL(currentChunk_);

  try {
    std::map<std::tuple<std::string, uint64_t, uint64_t>, std::vector<size_t>>
        projectionGroups;

    for (size_t i = 0; i < numScanOpts_; ++i) {
      const auto &opt = scanOpts_[i];
      if (!opt.property_bag.has_value()) {
        status_ = rocksdb::Status::InvalidArgument(
            "nimble.columns must be specified in property_bag");
        return;
      }
      auto it = opt.property_bag->find(kNimbleColumns);
      if (it == opt.property_bag->end() || it->second.empty()) {
        status_ = rocksdb::Status::InvalidArgument(
            "nimble.columns must be specified in property_bag");
        return;
      }

      uint64_t maxRows{0};
      status_ = parsePositiveUint64(*opt.property_bag, kNimbleMaxRows,
                                    kDefaultMaxRows, maxRows);
      if (!status_.ok()) {
        return;
      }

      uint64_t maxResultBytes{0};
      status_ = parsePositiveUint64(*opt.property_bag, kNimbleMaxResultBytes, 0,
                                    maxResultBytes);
      if (!status_.ok()) {
        return;
      }

      projectionGroups[{it->second, maxRows, maxResultBytes}].push_back(i);
    }

    numProjectionGroups_ = projectionGroups.size();

    for (auto &[groupKey, scanIndices] : projectionGroups) {
      const auto &[columnsStr, maxRows, maxResultBytes] = groupKey;
      auto subfields = parseColumns(columnsStr);

      auto readerOptions = reader_->readerOptionsBuilder_(threadMemoryPool());
      readerOptions.setCacheData(false);
      if (readerOptions.ioExecutor() == nullptr) {
        readerOptions.setIOExecutor(reader_->ioExecutor());
      }
      debugCheckFreshReaderOptions(readerOptions);

      auto projector = nimble::NimbleIndexProjector::create(
          nimble::TabletReaderCache::getInstance(), reader_->fileHandle_,
          subfields, readerOptions);

      nimble::NimbleIndexProjector::Request request;
      request.keyBounds.reserve(scanIndices.size());

      for (auto scanIdx : scanIndices) {
        const auto &opt = scanOpts_[scanIdx];
        velox::serializer::EncodedKeyBounds keyBounds;
        if (opt.range.start.has_value()) {
          keyBounds.lowerKey = opt.range.start->ToString();
        }
        if (opt.range.limit.has_value()) {
          keyBounds.upperKey = opt.range.limit->ToString();
        }
        request.keyBounds.push_back(std::move(keyBounds));
      }

      nimble::NimbleIndexProjector::Options projOptions;
      projOptions.maxRows = maxRows;
      projOptions.maxBytes = maxResultBytes;

      auto result = projector->project(request, projOptions);

      for (size_t j = 0; j < scanIndices.size(); ++j) {
        if (j >= result.responses.size()) {
          break;
        }
        auto &response = result.responses[j];
        if (response.slices.empty()) {
          continue;
        }

        auto &scanResult = scanResults_[scanIndices[j]];
        for (auto &chunkSlice : response.slices) {
          checkResultChunkManaged(chunkSlice);
          scanResult.slices.push_back(std::move(chunkSlice));
        }
      }
    }
  } catch (const std::exception &e) {
    status_ = rocksdb::Status::IOError("Nimble scan failed: " +
                                       std::string(e.what()));
  }
}

NimbleTableReader::NimbleTableReader(
    std::shared_ptr<velox::ReadFile> readFile,
    NimbleTableFactory::ReaderOptionsBuilder readerOptionsBuilder)
    : fileHandle_{[&] {
        const auto name = readFile->getName();
        return velox::FileHandle{std::move(readFile),
                                 velox::StringIdLease(velox::fileIds(), name),
                                 velox::StringIdLease(velox::fileIds(), name)};
      }()},
      readerOptionsBuilder_{std::move(readerOptionsBuilder)}, tablet_{[&] {
        auto options = readerOptionsBuilder_(threadMemoryPool());
        validateReaderOptions(options);
        return nimble::TabletReaderCache::getInstance()
            .get(fileHandle_.file,
                 nimble::TabletReader::configureOptions(options))
            .tablet;
      }()},
      minKey_{[&] {
        const auto *idx = tablet_->clusterIndex();
        return idx != nullptr ? std::string(idx->minKey()) : std::string();
      }()},
      maxKey_{[&] {
        const auto *idx = tablet_->clusterIndex();
        return idx != nullptr ? std::string(idx->maxKey()) : std::string();
      }()},
      numRows_{tablet_->tabletRowCount()}, fileSize_{fileHandle_.file->size()} {
}

rocksdb::ExternalTableIterator *
NimbleTableReader::NewIterator(const rocksdb::ReadOptions &readOptions,
                               const rocksdb::SliceTransform *) {
  return new NimbleTableIterator(readOptions, this);
}

rocksdb::Status NimbleTableReader::Get(const rocksdb::ReadOptions &,
                                       const rocksdb::Slice &,
                                       const rocksdb::SliceTransform *,
                                       rocksdb::PinnableSlice *) {
  return rocksdb::Status::NotSupported("Get() not supported");
}

void NimbleTableReader::MultiGet(const rocksdb::ReadOptions &,
                                 const std::vector<rocksdb::Slice> &keys,
                                 const rocksdb::SliceTransform *,
                                 std::vector<rocksdb::PinnableSlice> *,
                                 std::vector<rocksdb::Status> *statuses) {
  statuses->resize(keys.size());
  for (size_t i = 0; i < keys.size(); ++i) {
    (*statuses)[i] = rocksdb::Status::NotSupported("MultiGet not supported");
  }
}

std::shared_ptr<const rocksdb::TableProperties>
NimbleTableReader::GetTableProperties() const {
  auto props = std::make_shared<rocksdb::TableProperties>();
  props->comparator_name = "leveldb.BytewiseComparator";
  props->num_entries = numRows_;
  props->raw_key_size = numRows_ * (minKey_.empty() ? 1 : minKey_.size());
  props->raw_value_size = fileSize_;
  return props;
}

void NimbleTableBuilder::Add(const rocksdb::Slice &, const rocksdb::Slice &) {}

rocksdb::Status NimbleTableBuilder::Finish() {
  return rocksdb::Status::NotSupported(
      "NimbleTableBuilder: Nimble files are produced externally");
}

void NimbleTableBuilder::Abandon() {}

uint64_t NimbleTableBuilder::FileSize() const { return 0; }

rocksdb::TableProperties NimbleTableBuilder::GetTableProperties() const {
  return rocksdb::TableProperties{};
}

rocksdb::Status NimbleTableBuilder::status() const {
  return rocksdb::Status::NotSupported(
      "NimbleTableBuilder: Nimble files are produced externally");
}

NimbleTableFactory::NimbleTableFactory(
    ReaderOptionsBuilder readerOptionsBuilder)
    : readerOptionsBuilder_(std::move(readerOptionsBuilder)) {
  NIMBLE_CHECK_NOT_NULL(readerOptionsBuilder_,
                        "readerOptionsBuilder must not be empty");
}

rocksdb::Status NimbleTableFactory::NewTableReader(
    const rocksdb::ReadOptions &, const std::string &path,
    const rocksdb::ExternalTableOptions &tableOptions,
    std::unique_ptr<rocksdb::ExternalTableReader> *tableReader) const {
  try {
    std::unique_ptr<rocksdb::FSRandomAccessFile> file;
    auto openStatus = tableOptions.fs->NewRandomAccessFile(
        path, tableOptions.file_options, &file, nullptr);
    if (!openStatus.ok()) {
      return rocksdb::Status::IOError("Failed to open Nimble file: " +
                                      openStatus.ToString());
    }

    uint64_t fileSize = 0;
    auto sizeStatus = file->GetFileSize(&fileSize);
    if (!sizeStatus.ok()) {
      sizeStatus = tableOptions.fs->GetFileSize(
          path, tableOptions.file_options.io_options, &fileSize, nullptr);
    }
    if (!sizeStatus.ok()) {
      return rocksdb::Status::IOError("Failed to get Nimble file size: " +
                                      sizeStatus.ToString());
    }

    auto readFile = std::make_shared<RocksDbReadFile>(
        path, std::move(file), fileSize, tableOptions.file_options.io_options);
    *tableReader = std::make_unique<NimbleTableReader>(std::move(readFile),
                                                       readerOptionsBuilder_);
    return rocksdb::Status::OK();
  } catch (const std::exception &e) {
    return rocksdb::Status::IOError("Failed to open Nimble file: " +
                                    std::string(e.what()));
  }
}

rocksdb::ExternalTableBuilder *NimbleTableFactory::NewTableBuilder(
    const rocksdb::ExternalTableBuilderOptions &, const std::string &,
    rocksdb::FSWritableFile *) const {
  return new NimbleTableBuilder();
}

} // namespace facebook::rocks
