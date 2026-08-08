# syntax=docker/dockerfile:1

# CentOS Stream 9 matches the environment used to validate
# scripts/build-latest-releases.sh locally. Override this with an internal
# mirror if the public registry is not reachable from the build host.
ARG BASE_IMAGE=quay.io/centos/centos:stream9
FROM ${BASE_IMAGE}

ARG INSTALL_GITHUB_CLI=1
ARG GH_CLI_REPO=https://cli.github.com/packages/rpm/gh-cli.repo

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN set -eux; \
    dnf -y update; \
    dnf -y install dnf-plugins-core epel-release; \
    (dnf config-manager --set-enabled crb || dnf config-manager --set-enabled powertools || true); \
    if [[ "${INSTALL_GITHUB_CLI}" != "0" ]]; then \
      dnf config-manager --add-repo "${GH_CLI_REPO}"; \
    fi; \
    dnf -y install \
      autoconf \
      automake \
      bash \
      binutils \
      bison \
      bzip2 \
      bzip2-devel \
      ca-certificates \
      ccache \
      cmake \
      diffutils \
      double-conversion-devel \
      file \
      findutils \
      flex \
      flex-devel \
      fmt-devel \
      gcc \
      gcc-c++ \
      gdb \
      gflags-devel \
      git \
      glog-devel \
      grep \
      gzip \
      libaio-devel \
      libevent-devel \
      libicu-devel \
      libsodium-devel \
      libtool \
      libunwind-devel \
      liburing-devel \
      libzstd-devel \
      lz4 \
      lz4-devel \
      make \
      ninja-build \
      numactl-devel \
      openssl-devel \
      patch \
      perl \
      pkgconf-pkg-config \
      procps-ng \
      python3 \
      sed \
      snappy-devel \
      tar \
      unzip \
      wget \
      which \
      xz \
      xz-devel \
      zlib-devel \
      zstd; \
    if [[ "${INSTALL_GITHUB_CLI}" != "0" ]]; then \
      dnf -y install gh; \
    fi; \
    dnf clean all; \
    rm -rf /var/cache/dnf

WORKDIR /workspace/rocksdb-extensions
