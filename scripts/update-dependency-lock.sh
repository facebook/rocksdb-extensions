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

rocksdb_revision=""
rocksdb_version=""
nimble_revision=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rocksdb-revision)
      if [[ $# -lt 2 ]]; then
        echo "--rocksdb-revision requires a value." >&2
        exit 1
      fi
      rocksdb_revision="${2:-}"
      shift
      ;;
    --rocksdb-version)
      if [[ $# -lt 2 ]]; then
        echo "--rocksdb-version requires a value." >&2
        exit 1
      fi
      rocksdb_version="${2:-}"
      shift
      ;;
    --nimble-revision)
      if [[ $# -lt 2 ]]; then
        echo "--nimble-revision requires a value." >&2
        exit 1
      fi
      nimble_revision="${2:-}"
      shift
      ;;
    *)
      echo "Usage: $0 [--rocksdb-version <version> --rocksdb-revision <commit>] [--nimble-revision <commit>]" >&2
      exit 1
      ;;
  esac
  shift
done

validate_commit() {
  local name="$1"
  local revision="$2"

  if [[ ! "${revision}" =~ ^[0-9a-fA-F]{40}$ ]]; then
    echo "${name} revision must be an exact 40-character commit: ${revision}" >&2
    return 1
  fi
}

if [[ -z "${rocksdb_version}" && -z "${rocksdb_revision}" && -z "${nimble_revision}" ]]; then
  echo "At least one dependency revision must be supplied." >&2
  exit 1
fi

if [[ -n "${rocksdb_version}" || -n "${rocksdb_revision}" ]]; then
  if [[ -z "${rocksdb_version}" || -z "${rocksdb_revision}" ]]; then
    echo "RocksDB updates require both --rocksdb-version and --rocksdb-revision." >&2
    exit 1
  fi
  if [[ ! "${rocksdb_version}" =~ ^v?[0-9]+(\.[0-9]+){1,2}$ ]]; then
    echo "RocksDB version must be a stable semantic version: ${rocksdb_version}" >&2
    exit 1
  fi
  validate_commit RocksDB "${rocksdb_revision}"
fi
if [[ -n "${nimble_revision}" ]]; then
  validate_commit Nimble "${nimble_revision}"
fi

lock_file="${DEPENDENCY_LOCK_FILE:-${REPOSITORY_ROOT}/dependencies.lock}"
if [[ ! -f "${lock_file}" ]]; then
  echo "Dependency lock file not found: ${lock_file}" >&2
  exit 1
fi

sed_args=()
if [[ -n "${rocksdb_revision}" ]]; then
  sed_args+=(
    -e "s|^LOCKED_ROCKSDB_VERSION=.*$|LOCKED_ROCKSDB_VERSION=${rocksdb_version}|"
    -e "s|^LOCKED_ROCKSDB_REVISION=.*$|LOCKED_ROCKSDB_REVISION=${rocksdb_revision}|"
  )
fi
if [[ -n "${nimble_revision}" ]]; then
  sed_args+=(
    -e "s|^LOCKED_NIMBLE_REVISION=.*$|LOCKED_NIMBLE_REVISION=${nimble_revision}|"
  )
fi
sed -i "${sed_args[@]}" "${lock_file}"

if [[ -n "${rocksdb_revision}" ]] &&
  { ! grep -Fxq "LOCKED_ROCKSDB_VERSION=${rocksdb_version}" "${lock_file}" ||
    ! grep -Fxq "LOCKED_ROCKSDB_REVISION=${rocksdb_revision}" "${lock_file}"; }; then
  echo "Failed to update RocksDB in ${lock_file}." >&2
  exit 1
fi
if [[ -n "${nimble_revision}" ]] &&
  ! grep -Fxq "LOCKED_NIMBLE_REVISION=${nimble_revision}" "${lock_file}"; then
  echo "Failed to update dependency revisions in ${lock_file}." >&2
  exit 1
fi
