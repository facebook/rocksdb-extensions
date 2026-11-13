//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

// Registers "fifo_cache://<options>" with RocksDB's object registry. RocksDB
// built with ROCKSDB_PLUGINS=fifo_cache calls fifo_cache_register (named by
// fifo_cache_FUNC in fifo_cache.mk) for every ObjectRegistry, so
// Cache::CreateFromString reaches this factory for such a value when it has
// no existing cache to configure, and for the id of a block_cache option
// value.

#include <cstring>
#include <memory>
#include <string>

#include "plugin/fifo_cache/fifo_cache.h"
#include "rocksdb/convenience.h"
#include "rocksdb/utilities/object_registry.h"
#include "rocksdb/utilities/options_type.h"

namespace ROCKSDB_NAMESPACE {
namespace {

constexpr char kFIFOCacheScheme[] = "fifo_cache://";

Cache* NewFIFOCacheFromUri(const std::string& uri,
                           std::unique_ptr<Cache>* guard, std::string* errmsg) {
  FIFOCacheOptions cache_opts;
  Status status = OptionTypeInfo::ParseStruct(
      ConfigOptions(), "fifo_cache", &FIFOCacheOptionsTypeInfo(), "fifo_cache",
      uri.substr(std::strlen(kFIFOCacheScheme)), &cache_opts);
  if (status.ok() && cache_opts.capacity == 0) {
    status = Status::InvalidArgument("fifo_cache.capacity",
                                     "must be set and non-zero");
  } else if (status.ok() && cache_opts.num_shard_bits >= 20) {
    status = Status::InvalidArgument("fifo_cache.num_shard_bits",
                                     "must be less than 20");
  }
  if (!status.ok()) {
    *errmsg = status.ToString();
    return nullptr;
  }
  // FIFOCacheOptions::MakeSharedCache without a secondary cache, which the
  // string form cannot set; the registry needs a uniquely owned object.
  if (cache_opts.num_shard_bits < 0) {
    cache_opts.num_shard_bits =
        FIFOCache::DefaultShardBits(cache_opts.capacity);
  }
  guard->reset(new FIFOCache(cache_opts));
  return guard->get();
}

}  // namespace
}  // namespace ROCKSDB_NAMESPACE

extern "C" int fifo_cache_register(ROCKSDB_NAMESPACE::ObjectLibrary& library,
                                   const std::string& /*arg*/) {
  using ROCKSDB_NAMESPACE::Cache;
  using ROCKSDB_NAMESPACE::ObjectLibrary;
  library.AddFactory<Cache>(
      ObjectLibrary::PatternEntry("fifo_cache", /*optional*/ false)
          .AddSeparator("://", /*at_least_one*/ false),
      ROCKSDB_NAMESPACE::NewFIFOCacheFromUri);
  return 1;
}
