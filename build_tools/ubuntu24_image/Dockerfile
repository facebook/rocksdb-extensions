# syntax=docker/dockerfile:1

ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive \
    TZ=Etc/UTC \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN set -eux; \
    apt-get update; \
    apt-get upgrade -y; \
    apt-get install -y --no-install-recommends \
      autoconf \
      automake \
      binutils \
      bison \
      ca-certificates \
      ccache \
      clang \
      clang-tools \
      cmake \
      curl \
      flex \
      g++ \
      gcc \
      gh \
      git \
      gnupg \
      libaio-dev \
      libboost-all-dev \
      libbz2-dev \
      libdwarf-dev \
      libdouble-conversion-dev \
      libevent-dev \
      libfast-float-dev \
      libfl-dev \
      libfmt-dev \
      libgflags-dev \
      libgoogle-glog-dev \
      libicu-dev \
      libjemalloc-dev \
      liblz4-dev \
      liblzma-dev \
      libnuma-dev \
      libre2-dev \
      libsnappy-dev \
      libsodium-dev \
      libssl-dev \
      libtool \
      libunwind-dev \
      liburing-dev \
      libzstd-dev \
      lld \
      make \
      ninja-build \
      patch \
      pkg-config \
      python3 \
      python3-pip \
      software-properties-common \
      tzdata \
      unzip \
      wget \
      xz-utils \
      zlib1g-dev; \
    rm -rf /var/lib/apt/lists/*

WORKDIR /workspace/rocksdb-extensions
