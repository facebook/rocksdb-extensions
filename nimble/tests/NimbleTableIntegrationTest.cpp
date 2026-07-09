// Copyright (c) Meta Platforms, Inc. and affiliates.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include "rocksdb_extensions/nimble/NimbleTable.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "dwio/nimble/index/IndexConfig.h"
#include "dwio/nimble/serializer/Deserializer.h"
#include "dwio/nimble/tablet/TabletReaderCache.h"
#include "dwio/nimble/velox/FlushPolicy.h"
#include "dwio/nimble/velox/SchemaUtils.h"
#include "dwio/nimble/velox/VeloxReader.h"
#include "dwio/nimble/velox/VeloxWriter.h"
#include "dwio/nimble/velox/VeloxWriterOptions.h"
#include "folly/executors/CPUThreadPoolExecutor.h"
#include "rocksdb/comparator.h"
#include "rocksdb/db.h"
#include "rocksdb/external_table.h"
#include "rocksdb/file_system.h"
#include "rocksdb/multi_scan.h"
#include "rocksdb/options.h"
#include "velox/buffer/Buffer.h"
#include "velox/common/base/Exceptions.h"
#include "velox/common/file/File.h"
#include "velox/common/file/LocalFile.h"
#include "velox/common/io/IoStatistics.h"
#include "velox/common/memory/Memory.h"
#include "velox/dwio/common/ColumnSelector.h"
#include "velox/serializers/KeyEncoder.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/FlatVector.h"

namespace facebook::rocks {
namespace {

class NimbleTableIntegrationTest : public ::testing::Test {
protected:
  static constexpr int64_t kNumRows = 10'000;
  static constexpr int kNumDataColumns = 100;
  static constexpr int64_t kBatchSize = 1'000;

  static void SetUpTestSuite() {
    velox::memory::MemoryManager::initialize({});
    nimble::TabletReaderCache::initialize(nimble::TabletReaderCache::Options{
        .executor = std::make_shared<folly::CPUThreadPoolExecutor>(4)});
  }

  void SetUp() override {
    rootPool_ = velox::memory::memoryManager()->addRootPool(
        "NimbleTableIntegrationTest");
    leafPool_ = rootPool_->addLeafChild("leaf");
    tmpDir_ = std::filesystem::temp_directory_path() /
              ("rocksdb_ext_nimble_" + std::to_string(::getpid()) + "_" +
               std::to_string(counter_++));
    std::filesystem::create_directories(tmpDir_);
  }

  void TearDown() override {
    leafPool_.reset();
    rootPool_.reset();
    std::filesystem::remove_all(tmpDir_);
  }

  velox::RowTypePtr fullType() const {
    std::vector<std::string> names;
    std::vector<velox::TypePtr> types;
    names.reserve(kNumDataColumns + 1);
    types.reserve(kNumDataColumns + 1);
    names.push_back("key");
    types.push_back(velox::BIGINT());
    for (int column = 0; column < kNumDataColumns; ++column) {
      names.push_back(columnName(column));
      types.push_back(velox::BIGINT());
    }
    return velox::ROW(std::move(names), std::move(types));
  }

  velox::RowTypePtr projectedType(const std::vector<int> &columns) const {
    std::vector<std::string> names;
    std::vector<velox::TypePtr> types;
    names.reserve(columns.size());
    types.reserve(columns.size());
    for (auto column : columns) {
      names.push_back(columnName(column));
      types.push_back(velox::BIGINT());
    }
    return velox::ROW(std::move(names), std::move(types));
  }

  std::vector<velox::RowVectorPtr> makeBatches() {
    std::vector<velox::RowVectorPtr> batches;
    for (int64_t base = 0; base < kNumRows; base += kBatchSize) {
      const auto rows = std::min<int64_t>(kBatchSize, kNumRows - base);
      std::vector<velox::VectorPtr> children;
      children.reserve(kNumDataColumns + 1);

      children.push_back(
          makeFlatVector(static_cast<velox::vector_size_t>(rows),
                         [base](auto row) { return base + row; }));

      for (int column = 0; column < kNumDataColumns; ++column) {
        children.push_back(makeFlatVector(
            static_cast<velox::vector_size_t>(rows), [base, column](auto row) {
              const auto absoluteRow = base + row;
              return valueFor(absoluteRow, column);
            }));
      }

      batches.push_back(std::make_shared<velox::RowVector>(
          leafPool_.get(), fullType(), nullptr,
          static_cast<velox::vector_size_t>(rows), std::move(children)));
    }
    return batches;
  }

  std::string
  writeNimbleFileToDisk(const std::vector<velox::RowVectorPtr> &batches) {
    std::string fileData;
    auto writeFile = std::make_unique<velox::InMemoryWriteFile>(&fileData);

    nimble::VeloxWriterOptions options;
    options.enableChunking = true;

    nimble::ClusterIndexConfig indexConfig;
    indexConfig.columns = {"key"};
    indexConfig.sortOrders = {nimble::SortOrder{.ascending = true}};
    indexConfig.enforceKeyOrder = true;
    indexConfig.noDuplicateKey = true;
    options.clusterIndexConfig = std::move(indexConfig);
    options.flushPolicyFactory = [] {
      return std::make_unique<nimble::LambdaFlushPolicy>(
          [](const nimble::StripeProgress &progress) {
            return progress.stripeRawSize >= (256 << 10);
          });
    };

    nimble::VeloxWriter writer(fullType(), std::move(writeFile), *rootPool_,
                               std::move(options));
    for (const auto &batch : batches) {
      writer.write(batch);
    }
    writer.close();

    auto path = tmpDir_ / "projected_scan.nimble";
    std::ofstream out(path, std::ios::binary);
    out.write(fileData.data(), static_cast<std::streamsize>(fileData.size()));
    out.close();
    return path.string();
  }

  std::shared_ptr<NimbleTableFactory> makeFactory() {
    return std::make_shared<NimbleTableFactory>(
        [executor = ioExecutor_](velox::memory::MemoryPool *pool) {
          velox::dwio::common::ReaderOptions readerOptions(pool);
          readerOptions.setDataIoStats(
              std::make_shared<velox::io::IoStatistics>());
          readerOptions.setMetadataIoStats(
              std::make_shared<velox::io::IoStatistics>());
          readerOptions.setIndexIoStats(
              std::make_shared<velox::io::IoStatistics>());
          readerOptions.setLoadClusterIndex(true);
          readerOptions.setFileFormat(velox::dwio::common::FileFormat::NIMBLE);
          readerOptions.setCacheData(false);
          readerOptions.setIOExecutor(executor);
          return readerOptions;
        });
  }

  std::string encodeKey(int64_t key) {
    auto rowType = velox::ROW({"key"}, {velox::BIGINT()});
    std::vector<velox::core::SortOrder> sortOrders = {
        velox::core::SortOrder{true, false}};
    auto keyEncoder = velox::serializer::KeyEncoder::create(
        {"key"}, rowType, sortOrders, leafPool_.get());

    auto keyVector = std::make_shared<velox::RowVector>(
        leafPool_.get(), rowType, nullptr, 1,
        std::vector<velox::VectorPtr>{
            makeFlatVector(1, [key](auto) { return key; })});

    velox::serializer::IndexBounds bounds;
    bounds.indexColumns = {"key"};
    bounds.set(velox::serializer::IndexBound{keyVector, true},
               velox::serializer::IndexBound{keyVector, true});

    auto encoded = keyEncoder->encodeIndexBounds(bounds);
    EXPECT_EQ(encoded.size(), 1);
    return *encoded[0].lowerKey;
  }

  std::vector<std::vector<int64_t>>
  readDirectProjection(const std::string &nimblePath,
                       const std::vector<int> &projection) {
    auto readFile = std::make_shared<velox::LocalReadFile>(nimblePath);
    std::vector<std::string> projectedNames;
    projectedNames.reserve(projection.size());
    for (auto column : projection) {
      projectedNames.push_back(columnName(column));
    }
    auto selector = std::make_shared<velox::dwio::common::ColumnSelector>(
        fullType(), projectedNames);
    nimble::VeloxReader reader(readFile, *leafPool_, std::move(selector));

    std::vector<std::vector<int64_t>> values(projection.size());
    velox::VectorPtr batch;
    while (reader.next(2048, batch)) {
      auto *rowVector = batch->as<velox::RowVector>();
      VELOX_CHECK_NOT_NULL(rowVector);
      for (size_t column = 0; column < projection.size(); ++column) {
        auto *flat =
            rowVector->childAt(column)->as<velox::FlatVector<int64_t>>();
        VELOX_CHECK_NOT_NULL(flat);
        auto &output = values[column];
        output.reserve(output.size() + rowVector->size());
        for (velox::vector_size_t row = 0; row < rowVector->size(); ++row) {
          output.push_back(flat->valueAt(row));
        }
      }
    }
    return values;
  }

  uint64_t compareRocksScanWithDirectReader(
      rocksdb::DB *db, const std::vector<int> &projection,
      const std::vector<std::vector<int64_t>> &directValues,
      const std::vector<std::pair<int64_t, int64_t>> &ranges) {
    std::string projectionCsv;
    for (size_t i = 0; i < projection.size(); ++i) {
      if (i != 0) {
        projectionCsv += ',';
      }
      projectionCsv += columnName(projection[i]);
    }

    std::unordered_map<std::string, std::string> propertyBag{
        {"nimble.columns", projectionCsv},
        {"nimble.max_rows", std::to_string(kNumRows)}};
    rocksdb::MultiScanArgs scanArgs(rocksdb::BytewiseComparator());
    std::vector<std::string> encodedKeys;
    encodedKeys.reserve(ranges.size() * 2);
    std::vector<int64_t> expectedRows;
    for (const auto &[start, limit] : ranges) {
      encodedKeys.push_back(encodeKey(start));
      encodedKeys.push_back(encodeKey(limit));
      scanArgs.insert(
          rocksdb::Slice(encodedKeys[encodedKeys.size() - 2]),
          rocksdb::Slice(encodedKeys[encodedKeys.size() - 1]),
          std::optional<std::unordered_map<std::string, std::string>>(
              propertyBag));
      for (auto row = start; row < limit; ++row) {
        expectedRows.push_back(row);
      }
    }

    rocksdb::ReadOptions readOptions;
    auto scans =
        db->NewMultiScan(readOptions, db->DefaultColumnFamily(), scanArgs);

    auto projectedVeloxType = projectedType(projection);
    auto projectedNimbleSchema =
        nimble::convertToNimbleType(*projectedVeloxType);

    uint64_t rowsSeen = 0;
    for (auto scan : *scans) {
      for (auto kv : scan) {
        const auto *chunk = GetNimbleResultIOBuf(kv.second);
        VELOX_CHECK_NOT_NULL(chunk);
        auto rowRange = readResultRowRange(*chunk);
        auto coalesced = chunk->cloneCoalescedAsValue();

        nimble::DeserializerOptions deserOptions;
        deserOptions.hasHeader = true;
        nimble::Deserializer deserializer(projectedNimbleSchema,
                                          leafPool_.get(), deserOptions);
        velox::VectorPtr decoded;
        deserializer.deserialize(
            std::string_view(reinterpret_cast<const char *>(coalesced.data()),
                             coalesced.length()),
            decoded);
        auto *rowVector = decoded->as<velox::RowVector>();
        VELOX_CHECK_NOT_NULL(rowVector);
        VELOX_CHECK_GE(rowVector->size(), rowRange.endRow);

        for (uint32_t stripeRow = rowRange.startRow;
             stripeRow < rowRange.endRow; ++stripeRow) {
          VELOX_CHECK_LT(rowsSeen, expectedRows.size());
          const auto expectedRow = expectedRows[rowsSeen];
          for (size_t column = 0; column < projection.size(); ++column) {
            auto *flat =
                rowVector->childAt(column)->as<velox::FlatVector<int64_t>>();
            VELOX_CHECK_NOT_NULL(flat);
            EXPECT_EQ(flat->valueAt(stripeRow),
                      directValues[column][expectedRow])
                << "row=" << rowsSeen << " column=" << projection[column];
            EXPECT_EQ(
                flat->valueAt(stripeRow),
                valueFor(static_cast<int64_t>(expectedRow), projection[column]));
          }
          ++rowsSeen;
        }
      }
    }
    return rowsSeen;
  }

  static std::string columnName(int column) {
    return "c" + std::to_string(column);
  }

  static int64_t valueFor(int64_t row, int column) {
    return row * 1'000 + column;
  }

  template <typename ValueAt>
  velox::FlatVectorPtr<int64_t> makeFlatVector(velox::vector_size_t size,
                                               ValueAt valueAt) {
    auto values =
        velox::AlignedBuffer::allocate<int64_t>(size, leafPool_.get());
    auto vector = std::make_shared<velox::FlatVector<int64_t>>(
        leafPool_.get(), velox::BIGINT(), nullptr, size, std::move(values),
        std::vector<velox::BufferPtr>());
    for (velox::vector_size_t row = 0; row < size; ++row) {
      vector->set(row, valueAt(row));
    }
    return vector;
  }

  std::shared_ptr<velox::memory::MemoryPool> rootPool_;
  std::shared_ptr<velox::memory::MemoryPool> leafPool_;
  std::shared_ptr<folly::CPUThreadPoolExecutor> ioExecutor_{
      std::make_shared<folly::CPUThreadPoolExecutor>(4)};
  std::filesystem::path tmpDir_;
  inline static int counter_{0};
};

TEST_F(NimbleTableIntegrationTest,
       IngestAndProjectedScanMatchesDirectNimbleReader) {
  auto batches = makeBatches();
  auto nimblePath = writeNimbleFileToDisk(batches);
  const std::vector<int> projection{0, 3, 7, 11, 25, 50, 73, 99};
  auto directValues = readDirectProjection(nimblePath, projection);
  for (const auto &columnValues : directValues) {
    ASSERT_EQ(columnValues.size(), kNumRows);
  }

  auto factory = makeFactory();
  rocksdb::Options options;
  options.create_if_missing = true;
  options.table_factory = rocksdb::NewExternalTableFactory(std::move(factory));

  auto dbPath = (tmpDir_ / "db").string();
  std::unique_ptr<rocksdb::DB> db;
  auto status = rocksdb::DB::Open(options, dbPath, &db);
  ASSERT_TRUE(status.ok()) << status.ToString();

  rocksdb::IngestExternalFileOptions ingestOptions;
  ingestOptions.allow_db_generated_files = true;
  status = db->IngestExternalFile({nimblePath}, ingestOptions);
  ASSERT_TRUE(status.ok()) << status.ToString();

  EXPECT_EQ(
      compareRocksScanWithDirectReader(
          db.get(), projection, directValues, {{0, kNumRows}}),
      kNumRows);
  EXPECT_EQ(
      compareRocksScanWithDirectReader(
          db.get(), projection, directValues,
          {{0, 1'500}, {3'250, 4'000}, {8'500, kNumRows}}),
      3'750);

  ASSERT_TRUE(db->Close().ok());
}

} // namespace
} // namespace facebook::rocks
