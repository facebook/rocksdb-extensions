//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

// FIFOCache's public API. Installed with RocksDB as
// rocksdb/plugin/fifo_cache/fifo_cache_options.h.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "rocksdb/cache.h"

namespace ROCKSDB_NAMESPACE {

// EXPERIMENTAL: the API and the fifo_cache:// option string are subject to
// change.
// FIFOCache - A cache using segmented FIFO eviction to stay at or below a
// set capacity. The cache is sharded to 2^num_shard_bits shards, by hash of
// the key. The total capacity is divided and evenly assigned to each shard,
// and each shard has two FIFO queues for evictions: new entries enter
// probation, and probation victims with enough access frequency are promoted
// to resident (second-chance) instead of being evicted. Keys (not values)
// recently evicted from probation are remembered, and re-inserting one goes
// directly to resident. Placement is inferred
// from access frequency: Cache::Priority is ignored. Lookups take no lock;
// inserts, erases and eviction take a per-shard mutex. Pinned entries are
// never evicted; eviction sets long-pinned entries aside instead of
// rescanning them, and a fully-pinned shard evicts nothing.
// The memory that remembers recently evicted keys (a few bytes plus one
// hash-map entry per remembered key, at most one remembered key per
// cached entry, or more for a while after entries are erased) is not
// charged against capacity; it stays at its peak size after SetCapacity
// lowers capacity.
// Under kFullChargeCacheMetadata (the default) the slot table also counts
// against capacity: 64 bytes per slot the table has grown to, 256 bytes
// per shard at construction, growing with the entries, and shrinking back
// once the shard is emptied (by EraseUnRefEntries, or by a capacity cut that
// evicts every entry). A shard needs well over that much capacity to hold any
// entry. Each shard reserves address space for max(2048, shard capacity / 64)
// slots of 64 bytes up front, using max_capacity instead of capacity when it is
// larger, and touches it as the table grows. On Linux the reservation does not
// count against the commit limit unless vm.overcommit_memory=2; on Windows it
// is committed up front. If it cannot be reserved, construction aborts, as for
// HyperClockCache. The table cannot grow past its reservation, which holds
// about one entry per 107 bytes of reserved capacity: once SetCapacity raises
// capacity beyond that (about 38x for 4 KiB entries, with the default
// max_capacity), new keys are also evicted by occupancy. Each shard also keeps
// one cache line per thread that has used it (allocated 64 threads at a time,
// for up to 8192 threads), which is not charged against capacity.
//
// Choosing num_shard_bits: hits take no lock, so more shards only spread
// inserts (misses) over more locks, while fewer, larger shards share space
// more evenly. In db_bench and cache_bench runs, the default (up to 64
// shards) was best or tied from 256 MiB up; fewer shards were faster only
// for a cache much smaller than its hot set, and very small shards were
// slower. So keep the default unless the cache is much smaller than its hot
// set.
//
// This is an opt-in implementation and not the default block cache. Besides
// MakeSharedCache, a RocksDB built with this plugin creates one from
// "fifo_cache://capacity=<n>;..." with the scalar fields below: as the whole
// value given to Cache::CreateFromString with an empty result (e.g. db_bench
// --cache_uri), or as the id of a block_cache option value (e.g.
// block_cache={id={fifo_cache://capacity=1G;num_shard_bits=6}}).
// memory_allocator and secondary_cache can only be set in C++.
struct FIFOCacheOptions : public ShardedCacheOptions {
  // Whether to use adaptive mutexes for cache shards. Note that adaptive
  // mutexes need to be supported by the platform in order for this to have any
  // effect. The default value is true if RocksDB is compiled with
  // -DROCKSDB_DEFAULT_TO_ADAPTIVE_MUTEX, false otherwise.
  bool use_adaptive_mutex = kDefaultToAdaptiveMutex;

  // Capacity the slot table is reserved for, when larger than capacity (see
  // above). Set it to the largest capacity SetCapacity may raise this cache
  // to; 0 (the default) reserves for capacity. On Linux a larger
  // reservation costs only address space until the table grows into it; on
  // Windows it is committed up front.
  size_t max_capacity = 0;

  FIFOCacheOptions() {}
  // Every argument is required, so a positional call written for another
  // cache's options does not compile.
  FIFOCacheOptions(size_t _capacity, int _num_shard_bits,
                   bool _strict_capacity_limit,
                   std::shared_ptr<MemoryAllocator> _memory_allocator,
                   bool _use_adaptive_mutex,
                   CacheMetadataChargePolicy _metadata_charge_policy)
      : ShardedCacheOptions(_capacity, _num_shard_bits, _strict_capacity_limit,
                            std::move(_memory_allocator),
                            _metadata_charge_policy),
        use_adaptive_mutex(_use_adaptive_mutex) {}

  // Construct an instance of FIFOCache using these options
  std::shared_ptr<Cache> MakeSharedCache() const;
};

// Counts of FIFOCache's queue decisions for the entries of one
// CacheEntryRole, since the cache was created. Ghost returns say promotion
// is too strict for the role, wasted promotions and wasted fast-tracks that
// entry to the resident queue is too lenient. Only the resident queue's own
// evictions (including those a capacity cut makes) count as waste: an entry
// erased, overwritten, removed by EraseUnRefEntries or dropped by its last
// Release while the shard is over capacity is not counted either way.
struct FIFOCacheRoleCounts {
  // Probation entries moved to the resident queue at the probation head.
  uint64_t promotions = 0;
  // Inserts of a key recently evicted from probation and still remembered.
  uint64_t ghost_returns = 0;
  // Ghost returns inserted straight into the resident queue (today, every
  // one).
  uint64_t ghost_fast_tracks = 0;
  // Promoted entries evicted from the resident queue with no hit since
  // their promotion.
  uint64_t wasted_promotions = 0;
  // Fast-tracked entries evicted from the resident queue with no hit since
  // their insert.
  uint64_t wasted_fast_tracks = 0;
};

// Sets *counts, indexed by CacheEntryRole, to the counts of the FIFOCache
// that cache is, or that MakeSharedCache wrapped for a secondary cache,
// summed over its shards. Each shard is read under its own lock, so the sum
// is not taken at one instant. Returns false and leaves *counts unchanged
// for any other cache.
bool GetFIFOCacheRoleCounts(
    const Cache& cache,
    std::array<FIFOCacheRoleCounts, kNumCacheEntryRoles>* counts);

}  // namespace ROCKSDB_NAMESPACE
