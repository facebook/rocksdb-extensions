# Dependency Release Automation

## Promotion invariant

RocksDB Extensions advances a dependency only after the candidate passes the
GCC and Clang integration builds. A failed candidate never changes the promoted
revision on `main`.

Promoted revisions live in [`dependencies.lock`](../dependencies.lock) as exact
40-character commits. The RocksDB release version is retained as display
metadata. Normal pull-request CI and container-image validation read that file
instead of duplicating dependency pins in workflow or shell code.

## Weekly workflow

[`latest-dependency-releases.yml`](../.github/workflows/latest-dependency-releases.yml)
runs every Monday at 09:17 UTC and can also be started with
`workflow_dispatch`.

The scheduled path advances RocksDB:

1. Read the currently promoted revisions from `dependencies.lock`.
2. Resolve the highest stable semantic RocksDB release and peel it to an exact
   commit.
3. Build `rocksdb_extensions_nimble_test` with GCC and Clang.
4. If both compiler jobs pass, update a deterministic automation branch and
   open or refresh a pull request.
5. Wait for the pull request's `gcc` and `clang` checks and squash-merge only
   after both pass.

Failed candidates remain visible in the workflow logs. They do not modify the
lock file or create a new pull request. An existing automation pull request is
updated instead of opening duplicates.

RocksDB uses the highest non-draft, non-prerelease semantic GitHub Release. If
the repository has no stable GitHub Release, the resolver falls back to its
highest stable version tag. Scheduled promotion requires the version to move
forward and stores the release's exact commit, so a moving tag cannot silently
change normal CI.

Nimble currently has no explicit release version, and its standalone repository
is frozen while the project moves into Velox. The scheduled workflow therefore
keeps the validated Nimble commit fixed. A manually dispatched run can test a
specific Nimble ref, which is resolved to an exact commit for the build. Nimble
promotion remains a reviewed compatibility change until the Velox dependency
set can be pinned explicitly; the weekly bot never writes a moving Nimble ref.

## Repository setup

The promotion jobs require the following one-time GitHub repository setup:

- Add a `DEPENDENCY_UPDATE_TOKEN` Actions secret. Use a fine-grained personal
  access token or bot token with Actions/checks read access and read/write
  access to repository contents and pull requests. The default `GITHUB_TOKEN`
  is not sufficient because pull requests created with it do not trigger
  `pull_request` workflows.
- Protect `main` and require the `gcc` and `clang` checks from
  `rocksdb_extension.yml`.
- Require branches to be up to date, or use a merge queue, so a dependency
  update is retested if `main` advances while its pull request is open. The
  workflow also checks the named jobs itself before issuing the merge.

The scheduled workflow automatically proposes RocksDB updates. A manually
dispatched run can test explicit RocksDB or Nimble candidates. Its `promote`
input applies only to a forward stable RocksDB release; Nimble changes remain
manual during the migration.

## Local reproduction

Test the promoted revisions:

```bash
./scripts/build-latest-releases.sh --locked
```

Locked mode is authoritative and ignores ambient dependency revision
variables.

Test the latest upstream candidates:

```bash
./scripts/build-latest-releases.sh
```

Test explicit revisions:

```bash
ROCKSDB_REVISION=v11.8.0 \
NIMBLE_REVISION=<nimble-commit> \
  ./scripts/build-latest-releases.sh
```

The build disables Folly's liburing integration by default because Ubuntu
24.04 can provide headers that make Folly enable io_uring without all symbols
needed by the selected Folly source. Set `FOLLY_FORCE_DISABLE_LIBURING=OFF`
only with a compatible liburing header/library pair.

On x86_64, the build also adds `-msse4.2` so Folly F14 and the local extension
agree on SIMD/CRC support. Set `FORCE_X86_SSE42=OFF` only on older hosts that do
not support SSE4.2.

## Nimble and Velox

Nimble is moving into the Velox repository at `velox/dwio/nimble`. The
standalone Nimble repository is being retired, but the public Velox CMake and CI
integration is not yet ready for downstream consumption. See
[Velox pull request #18468](https://github.com/facebookincubator/velox/pull/18468).

For now this repository still fetches the standalone Nimble repository. That
currently locked source build brings the Velox commit pinned by Nimble's
submodule, so Velox is already built transitively; this repository is not
independently tracking the latest Velox revision. Candidate validation fails
instead of falling back to an unpinned Velox checkout if a selected standalone
Nimble revision stops providing those targets.

After the public migration lands, replace the fixed standalone Nimble commit
with the matching Velox release or immutable commit, enable Velox's Nimble CMake
option, port `VeloxWriter`/`VeloxWriterOptions` to `Writer`/`WriterOptions`,
replace the `nimble_velox_writer` target with `nimble_writer`, and key the
dependency cache on the Velox revision. At that point Nimble and Velox become
one promoted dependency set.

## Cache behavior

CI caches FetchContent sources and ccache objects. Source cache keys use the
exact RocksDB and Nimble commits, operating system, architecture, the declared
container image and CMake executable identities, and an explicit source-cache
schema. Bump that schema when source patches, independently pinned FetchContent
inputs, or cached subbuild layout changes.

Object archives are partitioned by those dependency commits, operating system,
architecture, compiler family, and a toolchain fingerprint made from the
declared container image, C and C++ compiler contents, and ccache version. The
primary key adds `github.sha` only to create an immutable rolling cache
generation; the restore prefix omits it so later commits using the same
dependencies can reuse the latest archive. Repository CMake and build-script
hashes are deliberately not part of this outer key. Ccache still validates the
actual compiler content, compile command, source, and included headers for every
object and reports the real hit rate at the end of each job.

The raw FetchContent build tree is intentionally not cached because a local
RelWithDebInfo build is much larger than the reusable source and compiler-object
caches. GitHub cache scope and trust rules still apply: fork pull requests can
restore base-repository caches but cannot publish new cache entries.
