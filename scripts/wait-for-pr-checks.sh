#!/usr/bin/env bash

# Copyright (c) Meta Platforms, Inc. and affiliates.
# This source code is licensed under both the GPLv2 (found in the
# COPYING file in the root directory) and Apache 2.0 License
# (found in the LICENSE.Apache file in the root directory).

set -euo pipefail

if [[ $# -lt 4 ]]; then
  echo "Usage: $0 <pull-request-url> <expected-head-commit> <minimum-check-start-epoch> <required-check> [<required-check> ...]" >&2
  exit 1
fi

pull_request="$1"
expected_head_commit="$2"
minimum_check_start_epoch="$3"
shift 3
required_checks=("$@")
poll_interval_seconds="${CHECK_POLL_INTERVAL_SECONDS:-30}"
timeout_seconds="${CHECK_TIMEOUT_SECONDS:-9000}"

if [[ ! "${expected_head_commit}" =~ ^[0-9a-fA-F]{40}$ ||
  ! "${minimum_check_start_epoch}" =~ ^[0-9]+$ ||
  ! "${poll_interval_seconds}" =~ ^[1-9][0-9]*$ ||
  ! "${timeout_seconds}" =~ ^[1-9][0-9]*$ ]]; then
  echo "Invalid expected commit, check start time, polling interval, or timeout." >&2
  exit 1
fi

start_time="$(date +%s)"
expected_head_seen=0
while true; do
  actual_head_commit="$(
    gh pr view "${pull_request}" --json headRefOid --jq '.headRefOid'
  )"
  if [[ "${actual_head_commit}" != "${expected_head_commit}" ]]; then
    if [[ "${expected_head_seen}" == "1" ]]; then
      echo "Pull-request head changed from ${expected_head_commit} to ${actual_head_commit}." >&2
      exit 1
    fi
    echo "Waiting for pull-request head ${expected_head_commit}; current head is ${actual_head_commit}."
    current_time="$(date +%s)"
    if ((current_time - start_time >= timeout_seconds)); then
      echo "Timed out waiting for the expected pull-request head." >&2
      exit 1
    fi
    sleep "${poll_interval_seconds}"
    continue
  fi
  expected_head_seen=1

  set +e
  check_lines="$(
    gh pr checks "${pull_request}" \
      --json name,bucket,startedAt \
      --jq '.[] | [.name, .bucket, .startedAt] | @tsv' \
      2>/dev/null
  )"
  checks_status=$?
  set -e

  # gh returns 8 while checks are pending and 1 when a check has failed.
  if [[ "${checks_status}" != "0" && "${checks_status}" != "1" &&
    "${checks_status}" != "8" ]]; then
    echo "Could not read pull-request checks (gh exit ${checks_status})." >&2
    exit "${checks_status}"
  fi

  declare -A check_buckets=()
  while IFS=$'\t' read -r check_name check_bucket check_started_at; do
    check_started_epoch=0
    if [[ -n "${check_started_at}" ]]; then
      check_started_epoch="$(date -d "${check_started_at}" +%s)"
    fi
    if [[ -n "${check_name}" &&
      "${check_started_epoch}" -ge "${minimum_check_start_epoch}" ]]; then
      check_buckets["${check_name}"]="${check_bucket}"
    fi
  done <<< "${check_lines}"

  all_passed=1
  for required_check in "${required_checks[@]}"; do
    bucket="${check_buckets[${required_check}]:-missing}"
    echo "${required_check}: ${bucket}"
    case "${bucket}" in
      pass)
        ;;
      fail | cancel | skipping)
        echo "Required check ${required_check} did not pass." >&2
        exit 1
        ;;
      *)
        all_passed=0
        ;;
    esac
  done

  if [[ "${all_passed}" == "1" ]]; then
    exit 0
  fi

  current_time="$(date +%s)"
  if ((current_time - start_time >= timeout_seconds)); then
    echo "Timed out waiting for required pull-request checks." >&2
    exit 1
  fi
  sleep "${poll_interval_seconds}"
done
