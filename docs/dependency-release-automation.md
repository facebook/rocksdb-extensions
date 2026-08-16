# Dependency Release Automation

## Goal

Keep RocksDB Extensions compatible with stable RocksDB and Nimble releases
without advancing a pinned dependency until the integration build and tests
pass.

The promotion invariant is:

1. Discover a newer stable upstream release.
2. Test it on a branch or pull request.
3. Update the pinned version only after all required checks pass.
4. Leave the current version unchanged when the candidate fails.

## Phase 1: Test the Latest Releases

The first phase is implemented by
`scripts/build-latest-releases.sh` and the manually triggered
`latest-dependency-releases.yml` GitHub Actions workflow.

The script first queries the GitHub Releases API for the latest non-draft,
non-prerelease revision. If a repository does not publish GitHub Releases, it
uses its newest stable version tag, then its default branch as a final fallback.
It supplies those revisions to the existing CMake `FetchContent` dependency path, builds
`rocksdb_extensions_nimble_test`, and runs CTest. It does not edit the CMake
defaults or push a version change.

RocksDB and Nimble revisions can be supplied explicitly to reproduce an earlier
run or test a branch or commit:

```bash
ROCKSDB_REVISION=v11.1.2 \
NIMBLE_REVISION=<nimble-tag-or-commit> \
  ./scripts/build-latest-releases.sh
```

Nimble currently does not publish GitHub Releases, so phase 1 tests its newest
stable version tag when available, or its default branch otherwise. Phase 2
will record an exact commit in the dependency lock before promotion.

Velox continues to use the revision configured in `cmake/Dependencies.cmake`.
Its source build supplies the compatible pinned Folly and fmt revisions. Nimble
compatibility may eventually require the Velox revision to be promoted as part
of the same dependency set.

## Pull Request CI

The `rocksdb_extension.yml` GitHub Actions workflow runs on pull requests and pushes to
`main`. It uses the same source-build path as `scripts/build-latest-releases.sh`
but pins the validated dependency revisions so ordinary PR CI is reproducible:

- RocksDB: `v11.8.0`
- Nimble: `acead744054eb006da753390ba80d3b6a29212ce`

The workflow builds `rocksdb_extensions_nimble_test` and runs the filtered CTest
entry for that binary.

Both compiler jobs use the same `32-core-ubuntu` runner class as RocksDB's
Folly CI jobs and build with up to 64 parallel compile processes. CI caches the
FetchContent source trees and a compressed ccache directory. Cache identities
include the resolved RocksDB and Nimble commits, compiler identity, container
image, architecture, and dependency build configuration. An upstream revision
or relevant build configuration change therefore creates a new cache instead
of reusing incompatible objects.

The raw FetchContent build tree is intentionally not cached because a local
RelWithDebInfo build is roughly 18 GB. Restoring source trees plus compiler
objects provides reuse across RocksDB, Nimble, Velox, Folly, FlatBuffers, and
their transitive dependencies without transferring the full CMake build tree.

`scripts/build-latest-releases.sh` disables Folly's liburing integration by
default for this release-validation path. Ubuntu 24.04 ships `liburing-dev`
headers that are new enough to make Folly enable io_uring, but not new enough
for all symbols used by the current Folly sources. Set
`FOLLY_FORCE_DISABLE_LIBURING=OFF` only when validating with a newer compatible
liburing header/library pair.

On x86_64 hosts, the script also adds `-msse4.2` by default so Folly F14's
SIMD/CRC link-mode check is consistent between source-built Folly and the local
extension/test targets. Set `FORCE_X86_SSE42=OFF` only when validating on an
older x86 host without SSE4.2 support.

## Phase 2: Propose Version Updates

Add a small dependency lock file containing the promoted tags, source archive
URLs, and SHA-256 hashes. A scheduled workflow should:

1. Poll each upstream repository for a newer stable release.
2. Create one candidate branch per dependency update.
3. Update the lock file and run the normal GCC and Clang build matrix.
4. Open an update pull request when the candidate build succeeds.
5. Record the failed candidate without modifying the promoted lock when it
   fails.

Keeping RocksDB and Nimble updates in separate pull requests makes failures
attributable. When a Nimble release requires matching Folly or Velox revisions,
promote that compatible set in one pull request instead.

## Phase 3: Merge Successful Updates

Protect `main`, make the GCC and Clang jobs required checks, and enable
auto-merge for dependency-update pull requests. The bot must never push a new
version directly to `main`; GitHub merges it only after all required checks
pass.

Renovate can manage custom CMake version fields, but a repository workflow is
preferred here because it can update a coordinated dependency set, calculate
archive hashes, and run the integration test before promotion.

## Phase 4: Trigger and Failure Reporting

Start with scheduled polling and `workflow_dispatch`. GitHub `release` events
do not cross repository boundaries. If the upstream repositories cooperate,
their release workflows can send a `repository_dispatch` event to this
repository for immediate testing. That requires a GitHub App or token with
permission to dispatch workflows in this repository.

On a failed candidate, retain the logs and resolved tags as workflow artifacts
and optionally open or update a tracking issue. Do not repeatedly open new pull
requests for the same failing release.
