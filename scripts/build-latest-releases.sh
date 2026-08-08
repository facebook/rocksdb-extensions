#!/usr/bin/env bash

# Copyright (c) Meta Platforms, Inc. and affiliates.
# This source code is licensed under both the GPLv2 (found in the
# COPYING file in the root directory) and Apache 2.0 License
# (found in the LICENSE.Apache file in the root directory).

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly REPOSITORY_ROOT

resolve_latest_revision() {
  local repository="$1"
  local default_branch
  local release_tag
  local stable_tag
  local tags

  if ! command -v gh >/dev/null 2>&1; then
    echo "The GitHub CLI (gh) is required to resolve ${repository}'s latest revision." >&2
    echo "Install gh or set ROCKSDB_REVISION and NIMBLE_REVISION explicitly." >&2
    return 1
  fi

  if release_tag="$(
    gh api "repos/${repository}/releases/latest" --jq '.tag_name' 2>/dev/null
  )" && [[ -n "${release_tag}" ]]; then
    printf '%s\n' "${release_tag}"
    return
  fi

  if tags="$(
    gh api --paginate "repos/${repository}/tags" --jq '.[].name' 2>/dev/null
  )"; then
    stable_tag="$(
      printf '%s\n' "${tags}" |
        { grep -E '^v?[0-9]+(\.[0-9]+){1,2}$' || true; } |
        LC_ALL=C sort -V |
        tail -n 1
    )"
    if [[ -n "${stable_tag}" ]]; then
      echo "${repository} has no GitHub Release; using stable tag ${stable_tag}." >&2
      printf '%s\n' "${stable_tag}"
      return
    fi
  fi

  if default_branch="$(
    gh api "repos/${repository}" --jq '.default_branch' 2>/dev/null
  )" && [[ -n "${default_branch}" ]]; then
    echo "${repository} has no GitHub Release or stable version tag; using default branch ${default_branch}." >&2
    printf '%s\n' "${default_branch}"
    return
  fi

  echo "Could not resolve a release, stable tag, or default branch for ${repository}." >&2
  echo "Set the corresponding *_REVISION variable explicitly." >&2
  return 1
}

rocksdb_revision="${ROCKSDB_REVISION:-${ROCKSDB_RELEASE_TAG:-}}"
if [[ -z "${rocksdb_revision}" ]]; then
  rocksdb_revision="$(resolve_latest_revision facebook/rocksdb)"
fi

nimble_revision="${NIMBLE_REVISION:-${NIMBLE_RELEASE_TAG:-}}"
if [[ -z "${nimble_revision}" ]]; then
  nimble_revision="$(resolve_latest_revision facebookincubator/nimble)"
fi

build_directory="${BUILD_DIRECTORY:-${REPOSITORY_ROOT}/build-latest-releases}"
build_type="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
clean_build_directory="${CLEAN_BUILD_DIRECTORY:-ON}"
cxx_extensions="${CMAKE_CXX_EXTENSIONS:-ON}"
force_gnu_cxx20="${FORCE_GNU_CXX20:-ON}"
force_disable_folly_liburing="${FOLLY_FORCE_DISABLE_LIBURING:-ON}"
force_x86_sse42="${FORCE_X86_SSE42:-ON}"
flatbuffers_include_dir="${FLATBUFFERS_INCLUDE_DIR:-${build_directory}/_deps/flatbuffers-src/include}"
fastfloat_include_dir="${FASTFLOAT_INCLUDE_DIR:-${build_directory}/_deps/fastfloat-src/include}"
nimble_include_dir="${NIMBLE_INCLUDE_DIR:-${build_directory}/_deps/nimble-src}"
# The gflags 2.2 CMake package on CentOS validates shared components using
# unnamespaced target names. RocksDB otherwise enables namespaced targets in
# the shared CMake cache before Velox resolves gflags.
gflags_use_target_namespace="${GFLAGS_USE_TARGET_NAMESPACE:-OFF}"
fetchcontent_fully_disconnected="${FETCHCONTENT_FULLY_DISCONNECTED:-OFF}"
fetchcontent_updates_disconnected="${FETCHCONTENT_UPDATES_DISCONNECTED:-OFF}"

cmake_generator_args=()
if [[ -n "${CMAKE_GENERATOR:-}" ]]; then
  cmake_generator_args=(-G "${CMAKE_GENERATOR}")
elif command -v ninja >/dev/null 2>&1; then
  cmake_generator_args=(-G Ninja)
else
  cmake_generator_args=(-G "Unix Makefiles")
fi

build_jobs="${BUILD_JOBS:-}"
if [[ -z "${build_jobs}" ]]; then
  build_jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2\n')"
fi

cmake_standard_flag_args=()
if [[ "${force_gnu_cxx20}" != "OFF" ]]; then
  cmake_standard_flag_args=(
    -DCMAKE_CXX20_STANDARD_COMPILE_OPTION:STRING="-std=gnu++20"
    -DCMAKE_CXX20_EXTENSION_COMPILE_OPTION:STRING="-std=gnu++20"
  )
fi

cmake_cxx_flags="${CMAKE_CXX_FLAGS:-}"
if [[ -n "${flatbuffers_include_dir}" ]]; then
  cmake_cxx_flags="${cmake_cxx_flags:+${cmake_cxx_flags} }-I${flatbuffers_include_dir}"
fi
if [[ -n "${nimble_include_dir}" ]]; then
  cmake_cxx_flags="${cmake_cxx_flags:+${cmake_cxx_flags} }-I${nimble_include_dir}"
fi
if [[ "${force_disable_folly_liburing}" != "OFF" ]]; then
  cmake_cxx_flags="${cmake_cxx_flags:+${cmake_cxx_flags} }-DFOLLY_FORCE_DISABLE_LIBURING=1"
fi
if [[ "${force_x86_sse42}" != "OFF" ]]; then
  case "$(uname -m)" in
    x86_64 | amd64)
      # Folly F14 has a link-time check that requires libfolly and all
      # consumers to agree on SIMD/CRC support.
      cmake_cxx_flags="${cmake_cxx_flags:+${cmake_cxx_flags} }-msse4.2"
      ;;
  esac
fi

patch_nimble_source() {
  local nimble_source_dir="${build_directory}/_deps/nimble-src"
  local nimble_exception="${nimble_source_dir}/dwio/nimble/common/NimbleException.cpp"
  local nullable_encoding="${nimble_source_dir}/dwio/nimble/encodings/NullableEncoding.h"

  if [[ ! -f "${nimble_exception}" ]]; then
    return
  fi

  if ! grep -q "NIMBLE_HAS_FOLLY_SYMBOLIZER" "${nimble_exception}"; then
    echo "Patching Nimble exception symbolization for Folly builds without libdwarf."
    printf '%s\n' \
      '--- dwio/nimble/common/NimbleException.cpp' \
      '+++ dwio/nimble/common/NimbleException.cpp' \
      '@@ -21,6 +21,12 @@' \
      ' #include <folly/debugging/symbolizer/Symbolizer.h>' \
      ' #include <glog/logging.h>' \
      ' ' \
      '+#if __linux__ && FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF' \
      '+#define NIMBLE_HAS_FOLLY_SYMBOLIZER 1' \
      '+#else' \
      '+#define NIMBLE_HAS_FOLLY_SYMBOLIZER 0' \
      '+#endif' \
      '+' \
      ' namespace facebook::nimble {' \
      ' ' \
      ' namespace {' \
      '@@ -128,6 +134,7 @@ void NimbleException::finalizeMessage() const {' \
      '     finalizedMessage_ += context_;' \
      '   }' \
      ' ' \
      '+#if NIMBLE_HAS_FOLLY_SYMBOLIZER' \
      '   if (LIKELY(!exceptionFrames_.empty())) {' \
      '     std::vector<folly::symbolizer::SymbolizedFrame> symbolizedFrames;' \
      '     symbolizedFrames.resize(exceptionFrames_.size());' \
      '@@ -150,6 +157,7 @@ void NimbleException::finalizeMessage() const {' \
      '     finalizedMessage_ += "\nStack Trace:\n";' \
      '     finalizedMessage_ += printer.str();' \
      '   }' \
      '+#endif' \
      ' }' \
      ' ' \
      ' NimbleUserError::NimbleUserError(' |
      patch -d "${nimble_source_dir}" -p0
  fi

  if [[ -f "${nullable_encoding}" ]] &&
    grep -q "Encoding::serializePrefixSize(rowCount, useVarint)" "${nullable_encoding}"; then
    echo "Patching Nimble nullable encoding protected helper access."
    printf '%s\n' \
      '--- dwio/nimble/encodings/NullableEncoding.h' \
      '+++ dwio/nimble/encodings/NullableEncoding.h' \
      '@@ -388,11 +388,11 @@ std::string_view NullableEncoding<T>::encodeNullable(' \
      '       EncodingIdentifiers::Nullable::Nulls, nulls, scopedBuffer.get(), options);' \
      ' ' \
      '   const uint32_t encodingSize =' \
      '-      Encoding::serializePrefixSize(rowCount, useVarint) + 4 +' \
      '+      NullableEncoding<T>::serializePrefixSize(rowCount, useVarint) + 4 +' \
      '       serializedValues.size() + serializedNulls.size();' \
      '   char* reserved = buffer.reserve(encodingSize);' \
      '   char* pos = reserved;' \
      '-  Encoding::serializePrefix(' \
      '+  NullableEncoding<T>::serializePrefix(' \
      '       EncodingType::Nullable,' \
      '       TypeTraits<T>::dataType,' \
      '       rowCount,' |
      patch -d "${nimble_source_dir}" -p0
  fi
}

patch_folly_source() {
  if [[ "${force_disable_folly_liburing}" == "OFF" ]]; then
    return
  fi

  local folly_source_dir="${build_directory}/_deps/folly-src"
  local liburing_header="${folly_source_dir}/folly/io/async/Liburing.h"

  if [[ ! -f "${liburing_header}" ]]; then
    return
  fi

  if ! grep -q "FOLLY_FORCE_DISABLE_LIBURING" "${liburing_header}"; then
    echo "Patching Folly liburing detection for release validation."
    printf '%s\n' \
      '--- folly/io/async/Liburing.h' \
      '+++ folly/io/async/Liburing.h' \
      '@@ -15,7 +15,8 @@' \
      ' ' \
      ' #pragma once' \
      ' ' \
      '-#if defined(__linux__) && __has_include(<liburing.h>)' \
      '+#if defined(__linux__) && !defined(FOLLY_FORCE_DISABLE_LIBURING) && \' \
      '+    __has_include(<liburing.h>)' \
      ' #define FOLLY_HAS_LIBURING 1' \
      ' #else' \
      ' #define FOLLY_HAS_LIBURING 0' |
      patch -d "${folly_source_dir}" -p0
  fi
}

patch_gnu_cxx20_build_flags() {
  if [[ "${force_gnu_cxx20}" == "OFF" || ! -d "${build_directory}" ]]; then
    return
  fi

  echo "Forcing generated C++20 build flags to GNU C++20 for __int128 support."
  find "${build_directory}" \
    \( -name flags.make -o -name build.ninja -o -name compile_commands.json \) \
    -type f \
    -exec sed -i 's/-std=c++20/-std=gnu++20/g' {} +
}

patch_flatbuffers_include_build_flags() {
  if [[ ! -d "${build_directory}" || -z "${flatbuffers_include_dir}" ]]; then
    return
  fi

  echo "Pinning generated FlatBuffers include path: ${flatbuffers_include_dir}"
  while IFS= read -r -d '' flags_file; do
    if ! grep -Fq -- "${flatbuffers_include_dir}" "${flags_file}"; then
      sed -i "s|^CXX_FLAGS = |CXX_FLAGS = -I${flatbuffers_include_dir} |" "${flags_file}"
    fi
  done < <(find "${build_directory}" -name flags.make -type f -print0)

  while IFS= read -r -d '' ninja_file; do
    if ! grep -Fq -- "${flatbuffers_include_dir}" "${ninja_file}"; then
      sed -i "s|^  FLAGS = |  FLAGS = -I${flatbuffers_include_dir} |" "${ninja_file}"
      sed -i "s|^  INCLUDES = |  INCLUDES = -I${flatbuffers_include_dir} |" "${ninja_file}"
    fi
  done < <(find "${build_directory}" -name build.ninja -type f -print0)
}

echo "Testing RocksDB revision: ${rocksdb_revision}"
echo "Testing Nimble revision:  ${nimble_revision}"
echo "Build directory:          ${build_directory}"
echo "C++ extensions:           ${cxx_extensions}"
echo "Force GNU C++20:          ${force_gnu_cxx20}"
echo "Disable Folly liburing:   ${force_disable_folly_liburing}"
echo "Force x86 SSE4.2:         ${force_x86_sse42}"
echo "FlatBuffers include dir:  ${flatbuffers_include_dir}"
echo "FastFloat include dir:    ${fastfloat_include_dir}"
echo "Nimble include dir:       ${nimble_include_dir}"

if [[ "${clean_build_directory}" != "OFF" ]]; then
  cmake -E rm -rf "${build_directory}"
fi

cmake \
  -UFlatbuffers_DIR \
  -UFolly_DIR \
  -URocksDB_DIR \
  -UROCKSDB_EXTENSIONS_NIMBLE_TARGETS \
  -UROCKSDB_EXTENSIONS_VELOX_TARGETS \
  -Uflatbuffers_DIR \
  -Ufmt_DIR \
  -Ufolly_DIR \
  -Unimble_DIR \
  -Uvelox_DIR \
  -S "${REPOSITORY_ROOT}" \
  -B "${build_directory}" \
  "${cmake_generator_args[@]}" \
  "${cmake_standard_flag_args[@]}" \
  -DCMAKE_BUILD_TYPE="${build_type}" \
  -DCMAKE_CXX_FLAGS:STRING="${cmake_cxx_flags}" \
  -DCMAKE_CXX_EXTENSIONS:BOOL="${cxx_extensions}" \
  -DFETCHCONTENT_FULLY_DISCONNECTED:BOOL="${fetchcontent_fully_disconnected}" \
  -DFETCHCONTENT_UPDATES_DISCONNECTED:BOOL="${fetchcontent_updates_disconnected}" \
  -DFASTFLOAT_INCLUDE_DIR:PATH="${fastfloat_include_dir}" \
  -DGFLAGS_USE_TARGET_NAMESPACE:BOOL="${gflags_use_target_namespace}" \
  -DROCKSDB_EXTENSIONS_BUILD_TESTS=ON \
  -DROCKSDB_EXTENSIONS_FETCH_DEPS=ON \
  -DROCKSDB_EXTENSIONS_ROCKSDB_GIT_TAG="${rocksdb_revision}" \
  -DROCKSDB_EXTENSIONS_NIMBLE_GIT_TAG="${nimble_revision}"

patch_nimble_source
patch_folly_source
patch_gnu_cxx20_build_flags
patch_flatbuffers_include_build_flags

cmake \
  --build "${build_directory}" \
  --target rocksdb_extensions_nimble_test \
  --parallel "${build_jobs}"

ctest \
  --test-dir "${build_directory}" \
  -R '^rocksdb_extensions_nimble_test$' \
  --output-on-failure
