# RocksDB Extensions

`rocksdb-extensions` is a companion repository to
[RocksDB](https://github.com/facebook/rocksdb/) — a persistent key-value store
for fast storage environments. It hosts extensions, plugins, and integrations
that build on top of RocksDB's public APIs but live outside of the core RocksDB
codebase.

Keeping these components here lets them evolve on their own release cadence
while staying decoupled from the core database engine.

## What belongs here

- **Plugins** that hook into RocksDB extension points (e.g. custom
  `FileSystem`, `Env`, `MergeOperator`, `CompactionFilter`,
  `TableFactory`, or `SecondaryCache` implementations).
- **Integrations** with other systems and storage backends.
- **Tools and utilities** that complement RocksDB but are not part of the core
  library.

## Requirements

These extensions are built and tested against a recent release of RocksDB. See
each subdirectory's own `README` for the specific RocksDB version it targets and
for build instructions.

## Getting started

Clone the repository:

```bash
git clone https://github.com/facebook/rocksdb-extensions.git
cd rocksdb-extensions
```

Each extension is self-contained in its own directory with its own build and
usage documentation.

Current extensions:

- [Nimble](nimble/README.md): RocksDB `ExternalTable` integration for scanning
  Nimble columnar files with projection pushdown.

## Contributing

We welcome contributions! Please read [CONTRIBUTING.md](CONTRIBUTING.md) to learn
about our development process, how to propose bug fixes and improvements, and how
to build and test your changes. By participating you are expected to uphold our
[Code of Conduct](CODE_OF_CONDUCT.md).

## License

RocksDB Extensions is dual-licensed, mirroring RocksDB itself. You may use this
software under either:

- the [Apache License, Version 2.0](LICENSE.Apache), or
- the [GNU General Public License, Version 2](COPYING).

See the license files in the root of this repository for the full text.
