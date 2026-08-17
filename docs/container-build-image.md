# Container Build Image

This repository has Ubuntu 24.04 and CentOS Stream 9 image definitions for the
OS packages needed by `scripts/build-latest-releases.sh`. The images install the
compiler, CMake/Ninja, GitHub CLI, and system development libraries used by the
integration build. RocksDB, Nimble, Folly, Velox, and their source-built
transitive dependencies are still resolved by CMake `FetchContent` into the
build tree.

Ubuntu 24.04 is the default image because GitHub PR jobs use the
`32-core-ubuntu` runner class with an Ubuntu 24.04 job container, so it is the
closest match for CI. The CentOS Stream 9 image is kept because the local build
was debugged and validated on a CentOS Stream 9 dev host, which makes it useful
for devvm parity and reproducing local package behavior.

`scripts/build-container-image.sh` is adapted from
`/home/xbw/bin/rocksdb-docker-build.sh`. The original script builds upstream
RocksDB Ubuntu images for `ghcr.io/facebook/rocksdb_ubuntu`; this repository's
copy builds `rocksdb-extensions` images and can optionally validate the repo
build inside the image.

## Build Locally

```bash
./scripts/build-container-image.sh 24
```

The helper uses `podman` when available, otherwise `docker`. It uses the base
image's normal apt sources. If the host shell has `http_proxy` or `https_proxy`
set to Meta `fwdproxy`, the helper automatically starts a host-side apt proxy
so package downloads do not depend on the build container reaching `fwdproxy`
directly. These proxy settings apply only while building the image and are not
stored in the resulting image. Use `--devvm-proxy` to force this path, or
`--no-proxy` to disable it.

Build all supported images:

```bash
./scripts/build-container-image.sh all
```

Build and run the repository integration test inside the image:

```bash
./scripts/build-container-image.sh 24 --validate
```

On Meta devvms, validation uses host networking so Git/CMake dependency
downloads can reach the host's configured proxy, such as `fwdproxy`.
Validation also inherits `scripts/build-latest-releases.sh`'s default
`FOLLY_FORCE_DISABLE_LIBURING=ON` behavior. That keeps the build independent of
Ubuntu 24.04's older liburing headers while still allowing the image to carry
`liburing-dev` for RocksDB and other consumers. On x86_64 validation hosts, the
script also adds `-msse4.2` by default so source-built Folly and the extension
test agree on Folly F14's SIMD/CRC link mode.

Build with the host-side devvm apt proxy:

```bash
./scripts/build-container-image.sh 24 --devvm-proxy
```

Override tags or base images with environment variables:

```bash
IMAGE_REGISTRY=ghcr.io/facebook \
IMAGE_PREFIX=rocksdb-extensions \
UBUNTU_BASE_IMAGE=<internal-mirror>/ubuntu:24.04 \
CENTOS_BASE_IMAGE=<internal-mirror>/centos/centos:stream9 \
  ./scripts/build-container-image.sh all
```

The default bases are `ubuntu:24.04` and `quay.io/centos/centos:stream9`. They
are configurable because some environments block public registry pulls.

## Validate The Repository Build

```bash
podman run --rm -it \
  -v "$PWD:/workspace/rocksdb-extensions:Z" \
  -w /workspace/rocksdb-extensions \
  ghcr.io/facebook/rocksdb-extensions_ubuntu:24.0 \
  ./scripts/build-latest-releases.sh --locked
```

Pass explicit revisions to test a different dependency pair:

```bash
podman run --rm -it \
  -v "$PWD:/workspace/rocksdb-extensions:Z" \
  -w /workspace/rocksdb-extensions \
  -e ROCKSDB_REVISION=v11.8.0 \
  -e NIMBLE_REVISION=<nimble-commit> \
  ghcr.io/facebook/rocksdb-extensions_ubuntu:24.0 \
  ./scripts/build-latest-releases.sh
```

## Push

```bash
podman login ghcr.io -u <github-user>
./scripts/build-container-image.sh 24 --push
```

Keep the `rocksdb-extensions_ubuntu` package public. The workflows grant
`packages: read` because GitHub automatically authenticates GHCR job-container
pulls with `GITHUB_TOKEN`, even without an explicit `container.credentials`
block.
