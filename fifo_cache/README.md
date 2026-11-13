# RocksDB FIFOCache plugin

`FIFOCache` is a RocksDB block cache (`rocksdb::Cache`) that evicts with
S3-FIFO, from [*FIFO Queues are All You Need for Cache
Eviction*](https://jasony.me/publication/sosp23-s3fifo.pdf) (SOSP '23):

- New entries enter a small probation queue. Entries read only once, and
  scans, leave through it without disturbing the hot set.
- Probation entries that were hit again are promoted to the resident queue,
  which evicts with a second-chance clock.
- Keys recently evicted from probation are remembered (keys only, no
  values); re-inserting one goes straight to resident.
- Hits take no lock: a lookup bumps a per-entry counter and moves nothing.
  Inserts, erases and eviction take a per-shard mutex.

`Cache::Priority` is ignored: placement follows access frequency.
[`fifo_cache_options.h`](fifo_cache_options.h) documents the options, memory
accounting and how to choose `num_shard_bits`. `GetFIFOCacheRoleCounts`, in
the same header, returns per-role counts of promotions, ghost returns and
promotions that were never hit, for exporting as metrics. The API is
experimental and may change.

This directory is a [RocksDB
plugin](https://github.com/facebook/rocksdb/blob/main/plugin/README.md): it is
compiled into RocksDB itself, so it uses RocksDB's sharded-cache framework and
utilities directly. It is built and tested against the RocksDB release pinned
in [`dependencies.lock`](../dependencies.lock) (v11.8.1).

## Quick start

From a checkout of this repository,
[`scripts/fifo-cache-quick-start.sh`](../scripts/fifo-cache-quick-start.sh)
builds RocksDB with the plugin, installs it, and runs a short `db_bench`
comparison of FIFOCache, LRUCache and HyperClockCache:

```bash
git clone https://github.com/facebook/rocksdb-extensions.git
cd rocksdb-extensions
./scripts/fifo-cache-quick-start.sh /path/to/new-dir
```

It fetches the RocksDB release pinned in `dependencies.lock`, builds it with
CMake (`Release`, `ROCKSDB_PLUGINS=fifo_cache`) and installs it to
`/path/to/new-dir/prefix`. It then loads a 1M-key DB and runs the same skewed
`readrandom` with each cache (64 MiB, 8 threads, 10 seconds each), printing
ops/sec and the data-block miss ratio per cache. `RUN_BENCHMARK=0` only builds;
`NUM_KEYS`, `CACHE_SIZE`, `THREADS` and `DURATION` change the benchmark. Short
runs on a busy machine are noisy, so use an otherwise idle host and repeat
before drawing conclusions. The sections below do the same steps by hand.

## Build RocksDB with the plugin

Put this directory into a RocksDB source tree as `plugin/fifo_cache` and
build RocksDB with `ROCKSDB_PLUGINS=fifo_cache`. The resulting `librocksdb`
contains FIFOCache and registers it at startup, and so do the tools built in
that tree, such as `db_bench`. The plugin is compiled with RocksDB's own
flags, warnings as errors included.

With CMake:

```bash
git clone --depth 1 --branch v11.8.1 https://github.com/facebook/rocksdb.git
cd rocksdb
ln -s /path/to/rocksdb-extensions/fifo_cache plugin/fifo_cache
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DROCKSDB_PLUGINS=fifo_cache
cmake --build build -j"$(nproc)"
cmake --install build --prefix /path/to/prefix
```

With make, copy the directory instead of linking it, since make writes its
objects next to the sources:

```bash
git clone --depth 1 --branch v11.8.1 https://github.com/facebook/rocksdb.git
cd rocksdb
cp -R /path/to/rocksdb-extensions/fifo_cache plugin/fifo_cache
make -j"$(nproc)" ROCKSDB_PLUGINS=fifo_cache static_lib db_bench
make ROCKSDB_PLUGINS=fifo_cache PREFIX=/path/to/prefix install-static
```

(In v11.8.1, an optimized make build such as `static_lib` without Snappy
installed stops on an unused-parameter warning in RocksDB's
`util/compression.cc`. Install Snappy, or use CMake, which does not enable that
warning.)

Installing adds the public header as
`<prefix>/include/rocksdb/plugin/fifo_cache/fifo_cache_options.h`.

## Use

### Without changing application code

A RocksDB built with the plugin accepts `fifo_cache://` followed by
`name=value` pairs separated by `;` where RocksDB creates a block cache from
a string:

- `db_bench --cache_uri`, and any program that passes the string to
  `Cache::CreateFromString` with an empty result:

  ```bash
  ./db_bench --benchmarks=fillseq,readrandom --compression_type=none \
    --cache_uri='fifo_cache://capacity=1073741824;num_shard_bits=6'
  ```

  The DB's `LOG` then shows `block_cache_name: FIFOCache`.

- Option strings and OPTIONS files, for programs that take their options
  from `GetOptionsFromString`, `GetBlockBasedTableOptionsFromString`,
  `LoadOptionsFromFile` and the like. There the URI is the `id` of the
  `block_cache` value and carries a single option:

  ```
  block_based_table_factory={block_cache={id=fifo_cache://capacity=1G}}
  ```

  In an OPTIONS file, put `block_cache={id=fifo_cache://capacity=1G}` in the
  `[TableOptions/BlockBasedTable "default"]` section. For example, with an
  OPTIONS file a DB wrote and that line added (`--cache_size=0` keeps
  `db_bench` from replacing the file's block cache with its own):

  ```bash
  ./db_bench --options_file=OPTIONS-fifo --cache_size=0 \
    --benchmarks=fillseq,readrandom --compression_type=none
  ```

The string options are `capacity` (required, non-zero), `num_shard_bits`
(below 20; by default chosen from the capacity), `strict_capacity_limit`,
`metadata_charge_policy` (`kFullChargeCacheMetadata` or
`kDontChargeCacheMetadata`), `hash_seed`, `use_adaptive_mutex` and
`max_capacity`. A bad or unknown option fails with `InvalidArgument`.

### From C++

```cpp
#include "rocksdb/plugin/fifo_cache/fifo_cache_options.h"
#include "rocksdb/table.h"

rocksdb::FIFOCacheOptions cache_opts;
cache_opts.capacity = size_t{1} << 30;
rocksdb::BlockBasedTableOptions table_options;
table_options.block_cache = cache_opts.MakeSharedCache();
options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));
```

Build against the installed RocksDB:

```bash
c++ -std=c++20 -I/path/to/prefix/include app.cc \
  /path/to/prefix/lib/librocksdb.a -lpthread -ldl
```

and add the libraries that RocksDB build found (compression libraries,
liburing and so on): make lists them under `Libs.private` in
`<prefix>/lib/pkgconfig/rocksdb.pc`, CMake as `THIRDPARTY_LIBS` in its
configure output. On Debian and Ubuntu, CMake installs the library under
`<prefix>/lib/x86_64-linux-gnu`.

`FIFOCacheOptions` derives from `ShardedCacheOptions`, so `memory_allocator`
and `secondary_cache` can be set here as well.

## Limitations

- RocksDB has to be built from source with the plugin; there is no
  separately built library to load into an existing RocksDB.
- Strings reach the plugin only through RocksDB's object registry, which
  matches them from their first character. So:
  - In an option string the URI must be the `id` of `block_cache` and can
    carry only one option (normally `capacity`); a second option fails with
    `InvalidArgument`. Set the others with `--cache_uri`,
    `Cache::CreateFromString` or C++.
  - `block_cache={fifo_cache://...}` without `id=`, a URI with leading
    spaces or braces, and a `fifo_cache://...` string given to
    `Cache::CreateFromString` with a non-empty result never reach the
    plugin. RocksDB then returns OK and keeps the existing (or default)
    cache, because `ConfigOptions::ignore_unsupported_options` is true by
    default; set it to false to get an error instead, or check the DB's
    `LOG` for `block_cache_name: FIFOCache`.
- `memory_allocator` and `secondary_cache` can only be set from C++.
- `cache_bench` and `db_stress` choose caches by type name, so they cannot
  use FIFOCache, and RocksDB's generic cache test suites do not run against
  it. `db_bench` can, through `--cache_uri`.
- RocksDB's CMake builds plugin tests only in Debug builds; see below for an
  optimized test build.
- Tested on Linux with GCC and Clang.

## Tests

[`fifo_cache_test.cc`](fifo_cache_test.cc) covers the eviction policy, the
lock-free lookup path and its memory reclamation, capacity and table
accounting, pinning, the option strings and thread exit. Tests that steer
interleavings with sync points run only in builds without `NDEBUG`. From the
RocksDB tree with the plugin in place:

```bash
# CMake Debug build
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DROCKSDB_PLUGINS=fifo_cache
cmake --build build-debug -j"$(nproc)" --target fifo_cache_test
./build-debug/fifo_cache_test

# Optimized make build (-O2, NDEBUG). RocksDB's full test library does not
# compile with NDEBUG, so link only its test harness; the test needs RTTI.
# make stops if the tree holds objects built with other flags (such as the
# static_lib build above): run make clean first.
make clean
make -j"$(nproc)" DEBUG_LEVEL=0 USE_RTTI=1 ROCKSDB_PLUGINS=fifo_cache \
  'TEST_LIBRARY=$(TESTHARNESS)' fifo_cache_test
./fifo_cache_test
```

[`scripts/build-fifo-cache-plugin.sh`](../scripts/build-fifo-cache-plugin.sh)
runs what CI runs: it fetches the pinned RocksDB, builds and runs the tests in
both of these builds, and checks that `db_bench --cache_uri` uses FIFOCache.
