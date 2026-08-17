# RocksDB Nimble Extension

This extension adapts Nimble columnar files to RocksDB's experimental
`ExternalTable` API. Applications can ingest a Nimble file into a RocksDB
database and scan it through RocksDB with column projection pushed down to
Nimble.

## Build

The extension is intentionally built only with an external CMake flow. No
alternate internal build files are provided in this repository.

```bash
cmake -S . -B build -GNinja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DROCKSDB_EXTENSIONS_BUILD_TESTS=ON \
  -DROCKSDB_EXTENSIONS_FETCH_DEPS=OFF \
  -DCMAKE_PREFIX_PATH=/path/to/dependency/prefix
cmake --build build
ctest --test-dir build --output-on-failure
```

To test the latest available RocksDB and Nimble revisions through the
`FetchContent` dependency path, run:

```bash
./scripts/build-latest-releases.sh
```

The script accepts `ROCKSDB_REVISION` and `NIMBLE_REVISION` environment
variables for reproducible or pre-release compatibility testing. See
[`docs/dependency-release-automation.md`](../docs/dependency-release-automation.md)
for the automated version-promotion plan.

The default and preferred dependency mode is installed CMake package targets.
Install RocksDB, Folly, Velox, Nimble, fmt, and GTest into one or more prefixes,
then pass those prefixes through `CMAKE_PREFIX_PATH`. The build expects those
packages to export the CMake targets used by this extension.

There are three supported dependency modes, in priority order:

- Installed CMake package targets for RocksDB, Folly, Velox, Nimble, fmt, and
  GTest.
- Standalone source checkouts passed through the `*_SOURCE_DIR` cache
  variables below.
- `FetchContent`, enabled explicitly with `-DROCKSDB_EXTENSIONS_FETCH_DEPS=ON`
  when package targets and source-directory overrides are not already available.

When using `FetchContent`, pin every `*_GIT_TAG` to a tested commit or release.
Using `main` is intended only for active compatibility work. Dependency
revisions are cache variables:

```bash
-DROCKSDB_EXTENSIONS_ROCKSDB_GIT_TAG=main
-DROCKSDB_EXTENSIONS_FOLLY_GIT_TAG=main
-DROCKSDB_EXTENSIONS_VELOX_GIT_TAG=main
-DROCKSDB_EXTENSIONS_NIMBLE_GIT_TAG=main
-DROCKSDB_EXTENSIONS_FLATBUFFERS_GIT_TAG=v25.2.10
```

Velox owns its compatible Folly and fmt revisions when Velox is built from
source. The Folly repository and tag variables are only used as a fallback when
the selected Velox package does not provide a Folly target.

You can also point the build at already-cloned standalone dependency source
trees:

```bash
-DROCKSDB_EXTENSIONS_ROCKSDB_SOURCE_DIR=/path/to/rocksdb
-DROCKSDB_EXTENSIONS_FOLLY_SOURCE_DIR=/path/to/folly
-DROCKSDB_EXTENSIONS_VELOX_SOURCE_DIR=/path/to/velox
-DROCKSDB_EXTENSIONS_NIMBLE_SOURCE_DIR=/path/to/nimble
```

Each source directory must contain the dependency's top-level `CMakeLists.txt`
and be usable through plain CMake `add_subdirectory()`. Generated CMake
fragments that require non-standalone macros are not valid `*_SOURCE_DIR`
overrides; provide installed package targets for those builds or use standalone
source checkouts. When Velox is built from source, its Folly dependency takes
precedence over `ROCKSDB_EXTENSIONS_FOLLY_SOURCE_DIR`. The build validates the
concrete CMake targets it expects from Velox and Nimble during configure. If a
dependency revision uses different target names, override `ROCKSDB_EXTENSIONS_VELOX_TARGETS`,
`ROCKSDB_EXTENSIONS_NIMBLE_TARGETS`, or
`ROCKSDB_EXTENSIONS_NIMBLE_TEST_TARGETS`.

The default target lists are intentionally narrow. The extension target lists
only direct CMake targets required by its public headers and implementation; the
integration test list adds the Nimble writer/reader/deserializer and Velox vector
targets used only by `rocksdb_extensions_nimble_test`.

RocksDB must provide `rocksdb/external_table.h`, `rocksdb/multi_scan.h`, and
`NewExternalTableFactory()`. These APIs are experimental, so pin the RocksDB
revision to a commit or release that includes them.

## API

Create a `facebook::rocks::NimbleTableFactory`, wrap it with
`rocksdb::NewExternalTableFactory()`, and configure the column family table
factory:

```cpp
auto factory = std::make_shared<facebook::rocks::NimbleTableFactory>(
    [](velox::memory::MemoryPool* pool) {
      velox::dwio::common::ReaderOptions options(pool);
      options.setFileFormat(velox::dwio::common::FileFormat::NIMBLE);
      options.setLoadClusterIndex(true);
      options.setCacheData(false);
      options.setDataIoStats(std::make_shared<velox::io::IoStatistics>());
      options.setMetadataIoStats(std::make_shared<velox::io::IoStatistics>());
      options.setIndexIoStats(std::make_shared<velox::io::IoStatistics>());
      return options;
    });

rocksdb::Options options;
options.table_factory = rocksdb::NewExternalTableFactory(std::move(factory));
```

Applications must initialize Velox memory management and Nimble's
`TabletReaderCache` before opening or scanning Nimble external tables.

Nimble-specific scan options are passed through `ScanOptions::property_bag`:

| Key | Value |
| --- | --- |
| `nimble.columns` | Comma-separated projected column names or subfields. Required for scans. |
| `nimble.max_rows` | Positive integer soft cap on returned rows. Defaults to `10000`. |
| `nimble.max_result_bytes` | Positive integer soft cap on serialized result bytes. Optional. |

Iterator values are `rocksdb::Slice` objects that alias a `folly::IOBuf`
returned by Nimble. Use `GetNimbleResultIOBuf()` for a borrowed pointer or
`CloneNimbleResultIOBuf()` when the result must survive iterator advancement.

## Test Coverage

`rocksdb_extensions_nimble_test` generates a Nimble file with 10,000 rows and
100 data columns, ingests it into RocksDB, runs a projected scan through
RocksDB `NewMultiScan`, and compares every projected cell against a direct
`nimble::VeloxReader` read of the same file.
