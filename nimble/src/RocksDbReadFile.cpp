// Copyright (c) Meta Platforms, Inc. and affiliates.
// This source code is licensed under both the GPLv2 (found in the
// COPYING file in the root directory) and Apache 2.0 License
// (found in the LICENSE.Apache file in the root directory).

#include "rocksdb_extensions/nimble/RocksDbReadFile.h"

#include <algorithm>
#include <cstring>

#include "dwio/nimble/common/Exceptions.h"
#include "velox/common/base/Exceptions.h"

namespace facebook::rocks {
namespace {

void freeOwnedBuffer(void *buffer, void *) { std::free(buffer); }

bool isAligned(uint64_t value, size_t alignment) {
  return value % alignment == 0;
}

bool isAligned(const void *value, size_t alignment) {
  return reinterpret_cast<uintptr_t>(value) % alignment == 0;
}

void copyBytes(void *destination, size_t destinationSize, const char *source,
               size_t size) {
  VELOX_CHECK_LE(size, destinationSize);
  std::memcpy(destination, source, size);
}

} // namespace

RocksDbReadFile::RocksDbReadFile(
    std::string path, std::unique_ptr<rocksdb::FSRandomAccessFile> &&file,
    uint64_t fileSize, rocksdb::IOOptions ioOptions)
    : path_(std::move(path)), file_(std::move(file)), fileSize_(fileSize),
      ioOptions_(std::move(ioOptions)) {
  NIMBLE_CHECK_NOT_NULL(file_, "RocksDbReadFile requires a non-null file");
}

std::string_view RocksDbReadFile::pread(uint64_t offset, uint64_t length,
                                        void *buf,
                                        const velox::FileIoContext &) const {
  readFully(offset, length, static_cast<char *>(buf), true);
  return {static_cast<char *>(buf), length};
}

uint64_t
RocksDbReadFile::preadv(uint64_t offset,
                        const std::vector<folly::Range<char *>> &buffers,
                        const velox::FileIoContext &context) const {
  auto fileSize = size();
  uint64_t totalBytesRead{0};
  if (offset >= fileSize) {
    return 0;
  }

  for (const auto &range : buffers) {
    const auto bytesToRead = std::min<size_t>(range.size(), fileSize - offset);
    if (range.data() != nullptr) {
      pread(offset, bytesToRead, range.data(), context);
    }
    offset += bytesToRead;
    totalBytesRead += bytesToRead;
    if (offset >= fileSize) {
      break;
    }
  }

  return totalBytesRead;
}

uint64_t
RocksDbReadFile::preadv(folly::Range<const velox::common::Region *> regions,
                        folly::Range<folly::IOBuf *> iobufs,
                        const velox::FileIoContext &) const {
  VELOX_CHECK_EQ(regions.size(), iobufs.size());

  const auto directIo = file_->use_direct_io();
  const auto alignment =
      directIo ? std::max<size_t>(1, file_->GetRequiredBufferAlignment())
               : size_t{0};

  std::vector<rocksdb::FSReadRequest> requests;
  requests.reserve(regions.size());
  std::vector<std::unique_ptr<char, decltype(&std::free)>> ownedBuffers;
  ownedBuffers.reserve(regions.size());

  uint64_t totalBytesRead{0};
  for (size_t index = 0; index < regions.size(); ++index) {
    const auto &region = regions[index];
    auto &output = iobufs[index];
    totalBytesRead += region.length;

    if (region.length == 0) {
      output = folly::IOBuf();
      continue;
    }

    rocksdb::FSReadRequest request;
    std::unique_ptr<char, decltype(&std::free)> scratch{nullptr, &std::free};
    if (!directIo) {
      output = folly::IOBuf(folly::IOBuf::CREATE, region.length);
      request.offset = region.offset;
      request.len = region.length;
      request.scratch = reinterpret_cast<char *>(output.writableTail());
    } else if (!output.isChained() && isAligned(region.offset, alignment) &&
               isAligned(region.length, alignment) &&
               output.tailroom() >= region.length &&
               isAligned(output.writableTail(), alignment)) {
      request.offset = region.offset;
      request.len = region.length;
      request.scratch = reinterpret_cast<char *>(output.writableTail());
    } else {
      const auto alignedOffset = region.offset / alignment * alignment;
      const auto offsetDelta = region.offset - alignedOffset;
      const auto alignedLength =
          ((offsetDelta + region.length + alignment - 1) / alignment) *
          alignment;
      scratch = allocateAligned(alignment, alignedLength);
      request.offset = alignedOffset;
      request.len = alignedLength;
      request.scratch = scratch.get();
      output = folly::IOBuf();
    }

    requests.push_back(std::move(request));
    ownedBuffers.push_back(std::move(scratch));
  }

  if (requests.empty()) {
    return 0;
  }

  bytesRead_ += totalBytesRead;
  auto status =
      file_->MultiRead(requests.data(), requests.size(), ioOptions_, nullptr);
  VELOX_CHECK(status.ok(), "RocksDB MultiRead failed: path {}, status {}",
              path_, status.ToString());

  size_t requestIndex{0};
  for (size_t regionIndex = 0; regionIndex < regions.size(); ++regionIndex) {
    const auto &region = regions[regionIndex];
    auto &output = iobufs[regionIndex];
    if (region.length == 0) {
      continue;
    }

    const auto offsetDelta = directIo ? region.offset % alignment : uint64_t{0};
    const auto expectedSize =
        directIo ? offsetDelta + region.length : region.length;
    const auto &request = requests[requestIndex];
    auto &scratch = ownedBuffers[requestIndex];

    VELOX_CHECK(request.status.ok(),
                "RocksDB MultiRead request failed: path {}, offset {}, length "
                "{}, status {}",
                path_, region.offset, region.length, request.status.ToString());
    VELOX_CHECK_GE(request.result.size(), expectedSize);

    if (scratch == nullptr) {
      if (request.result.data() !=
          reinterpret_cast<const char *>(output.writableTail())) {
        copyBytes(output.writableTail(), region.length, request.result.data(),
                  region.length);
      }
      output.append(region.length);
    } else if (request.result.data() == scratch.get()) {
      auto ownedOutput = folly::IOBuf::takeOwnership(
          scratch.release(), request.len, static_cast<size_t>(offsetDelta),
          region.length, freeOwnedBuffer);
      output = std::move(*ownedOutput);
    } else {
      const auto *resultData = request.result.data() + offsetDelta;
      output = folly::IOBuf(folly::IOBuf::CREATE, region.length);
      copyBytes(output.writableData(), region.length, resultData,
                region.length);
      output.append(region.length);
    }
    ++requestIndex;
  }

  return totalBytesRead;
}

uint64_t
RocksDbReadFile::preadv(folly::Range<const velox::common::Region *> regions,
                        folly::Range<const folly::Range<char *> *> buffers,
                        const velox::FileIoContext &) const {
  VELOX_CHECK_EQ(regions.size(), buffers.size());

  const auto directIo = file_->use_direct_io();
  const auto alignment =
      directIo ? std::max<size_t>(1, file_->GetRequiredBufferAlignment())
               : size_t{0};

  struct Destination {
    char *data;
    uint64_t length;
    uint64_t offsetDelta;
  };

  std::vector<rocksdb::FSReadRequest> requests;
  requests.reserve(regions.size());
  std::vector<Destination> destinations;
  destinations.reserve(regions.size());
  std::vector<std::unique_ptr<char, decltype(&std::free)>> ownedBuffers;

  uint64_t totalBytesRead{0};
  for (size_t index = 0; index < regions.size(); ++index) {
    const auto &region = regions[index];
    const auto &buffer = buffers[index];
    totalBytesRead += region.length;

    if (region.length == 0) {
      continue;
    }

    VELOX_CHECK_NOT_NULL(buffer.data());
    VELOX_CHECK_EQ(buffer.size(), region.length);

    rocksdb::FSReadRequest request;
    if (!directIo || (isAligned(region.offset, alignment) &&
                      isAligned(region.length, alignment) &&
                      isAligned(buffer.data(), alignment))) {
      request.offset = region.offset;
      request.len = region.length;
      request.scratch = buffer.data();
      destinations.push_back({buffer.data(), region.length, 0});
    } else {
      const auto alignedOffset = region.offset / alignment * alignment;
      const auto offsetDelta = region.offset - alignedOffset;
      const auto alignedLength =
          ((offsetDelta + region.length + alignment - 1) / alignment) *
          alignment;
      auto scratch = allocateAligned(alignment, alignedLength);
      request.offset = alignedOffset;
      request.len = alignedLength;
      request.scratch = scratch.get();
      destinations.push_back({buffer.data(), region.length, offsetDelta});
      ownedBuffers.push_back(std::move(scratch));
    }

    requests.push_back(std::move(request));
  }

  if (requests.empty()) {
    return 0;
  }

  bytesRead_ += totalBytesRead;
  auto status =
      file_->MultiRead(requests.data(), requests.size(), ioOptions_, nullptr);
  VELOX_CHECK(status.ok(), "RocksDB MultiRead failed: path {}, status {}",
              path_, status.ToString());

  for (size_t i = 0; i < requests.size(); ++i) {
    const auto &request = requests[i];
    const auto &destination = destinations[i];
    VELOX_CHECK(request.status.ok(),
                "RocksDB MultiRead request failed: path {}, status {}", path_,
                request.status.ToString());
    VELOX_CHECK_GE(request.result.size(),
                   destination.offsetDelta + destination.length);
    const auto *source = request.result.data() + destination.offsetDelta;
    if (source != destination.data) {
      copyBytes(destination.data, destination.length, source,
                destination.length);
    }
  }

  return totalBytesRead;
}

bool RocksDbReadFile::shouldCoalesce() const { return false; }

uint64_t RocksDbReadFile::size() const { return fileSize_; }

uint64_t RocksDbReadFile::memoryUsage() const {
  return sizeof(*this) + path_.capacity();
}

std::string RocksDbReadFile::getName() const {
  return path_.empty() ? "<RocksDbReadFile>" : path_;
}

uint64_t RocksDbReadFile::getNaturalReadSize() const { return 10 << 20; }

rocksdb::Slice RocksDbReadFile::readFromFile(uint64_t offset, uint64_t length,
                                             char *scratch,
                                             uint64_t minExpectedSize) const {
  rocksdb::Slice result;
  auto status =
      file_->Read(offset, length, ioOptions_, &result, scratch, nullptr);
  VELOX_CHECK(status.ok(),
              "RocksDB read failed: path {}, offset {}, length {}, status {}",
              path_, offset, length, status.ToString());
  VELOX_CHECK_GE(result.size(), minExpectedSize);
  return result;
}

std::unique_ptr<char, decltype(&std::free)>
RocksDbReadFile::allocateAligned(size_t alignment, size_t size) {
  void *data = nullptr;
  auto rc = posix_memalign(&data, alignment, size);
  VELOX_CHECK_EQ(
      rc, 0,
      "Failed to allocate aligned buffer, alignment: {}, size: {}, errno: {}",
      alignment, size, rc);
  return {static_cast<char *>(data), &std::free};
}

void RocksDbReadFile::readFully(uint64_t offset, uint64_t length, char *buffer,
                                bool countBytes) const {
  if (length == 0) {
    return;
  }
  if (countBytes) {
    bytesRead_ += length;
  }

  const auto destinationSize = static_cast<size_t>(length);
  if (file_->use_direct_io()) {
    const auto alignment =
        std::max<size_t>(1, file_->GetRequiredBufferAlignment());
    if (isAligned(offset, alignment) && isAligned(length, alignment) &&
        isAligned(buffer, alignment)) {
      auto result = readFromFile(offset, length, buffer, length);
      if (result.data() != buffer) {
        copyBytes(buffer, destinationSize, result.data(), destinationSize);
      }
      return;
    }

    const auto alignedOffset = offset / alignment * alignment;
    const auto offsetDelta = offset - alignedOffset;
    const auto alignedLength =
        ((offsetDelta + length + alignment - 1) / alignment) * alignment;
    auto scratch = allocateAligned(alignment, alignedLength);

    auto result = readFromFile(alignedOffset, alignedLength, scratch.get(),
                               offsetDelta + length);
    copyBytes(buffer, destinationSize, result.data() + offsetDelta,
              destinationSize);
    return;
  }

  auto result = readFromFile(offset, length, buffer, length);
  if (result.data() != buffer) {
    copyBytes(buffer, destinationSize, result.data(), destinationSize);
  }
}

} // namespace facebook::rocks
