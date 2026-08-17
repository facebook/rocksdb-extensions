#!/usr/bin/env bash

# Copyright (c) Meta Platforms, Inc. and affiliates.
# This source code is licensed under both the GPLv2 (found in the
# COPYING file in the root directory) and Apache 2.0 License
# (found in the LICENSE.Apache file in the root directory).

# RocksDB Extensions container image builder for Meta devvms and local Linux
# hosts. Adapted from /home/xbw/bin/rocksdb-docker-build.sh, but scoped to this
# repository's dependency images.
#
# Usage:
#   scripts/build-container-image.sh [24|centos9|all] [--push] [--validate] [--devvm-proxy]
#
# Examples:
#   scripts/build-container-image.sh 24
#   scripts/build-container-image.sh 24 --push
#   scripts/build-container-image.sh all --validate
#
# Useful overrides:
#   IMAGE_REGISTRY=ghcr.io/facebook
#   IMAGE_PREFIX=rocksdb-extensions
#   UBUNTU_BASE_IMAGE=ubuntu:24.04
#   CENTOS_BASE_IMAGE=quay.io/centos/centos:stream9
#   USE_DEVVM_APT_PROXY=0
#   USE_SUDO=1

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
readonly REPOSITORY_ROOT

CONTAINER_ENGINE="${CONTAINER_ENGINE:-}"
IMAGE_REGISTRY="${IMAGE_REGISTRY:-ghcr.io/facebook}"
IMAGE_PREFIX="${IMAGE_PREFIX:-rocksdb-extensions}"
UBUNTU_BASE_IMAGE="${UBUNTU_BASE_IMAGE:-ubuntu:24.04}"
CENTOS_BASE_IMAGE="${CENTOS_BASE_IMAGE:-quay.io/centos/centos:stream9}"
if [[ -z "${USE_DEVVM_APT_PROXY+x}" ]]; then
  if [[ "${http_proxy:-}${https_proxy:-}" == *fwdproxy* ]]; then
    USE_DEVVM_APT_PROXY=1
  else
    USE_DEVVM_APT_PROXY=0
  fi
fi
USE_SUDO="${USE_SUDO:-0}"
PROXY_PORT="${PROXY_PORT:-28888}"
PROXY_PID_FILE="${PROXY_PID_FILE:-/tmp/rocksdb-extensions-docker-proxy.pid}"
PROXY_LOG="${PROXY_LOG:-/tmp/rocksdb-extensions-docker-proxy.log}"
PROXY_SCRIPT="${PROXY_SCRIPT:-/tmp/rocksdb-extensions-docker-proxy.py}"

TARGET="${1:-24}"
PUSH=0
VALIDATE=0
for arg in "${@:2}"; do
  case "${arg}" in
    --push)
      PUSH=1
      ;;
    --validate)
      VALIDATE=1
      ;;
    --devvm-proxy|--proxy)
      USE_DEVVM_APT_PROXY=1
      ;;
    --no-proxy)
      USE_DEVVM_APT_PROXY=0
      ;;
    *)
      echo "Unknown argument: ${arg}" >&2
      echo "Usage: $0 [24|centos9|all] [--push] [--validate] [--devvm-proxy]" >&2
      exit 1
      ;;
  esac
done

if [[ -z "${CONTAINER_ENGINE}" ]]; then
  if command -v podman >/dev/null 2>&1; then
    CONTAINER_ENGINE=podman
  elif command -v docker >/dev/null 2>&1; then
    CONTAINER_ENGINE=docker
  else
    echo "Install podman or docker, or set CONTAINER_ENGINE explicitly." >&2
    exit 1
  fi
fi

engine=("${CONTAINER_ENGINE}")
if [[ "${USE_SUDO}" != "0" ]]; then
  engine=(sudo "${CONTAINER_ENGINE}")
fi

ubuntu_tag="${IMAGE_REGISTRY}/${IMAGE_PREFIX}_ubuntu:24.0"
centos_tag="${IMAGE_REGISTRY}/${IMAGE_PREFIX}_centos:9.0"

write_proxy_script() {
  cat >"${PROXY_SCRIPT}" <<'PYEOF'
#!/usr/bin/env python3
"""Threaded HTTP/HTTPS streaming proxy for devvm container builds.

HTTP and HTTPS requests are forwarded through fwdproxy from the host namespace.
This avoids relying on build containers being able to reach fwdproxy directly.
"""
import http.server
import socket
import socketserver
import threading
import urllib.error
import urllib.request

FWDPROXY = "http://fwdproxy:8080"
PORT = PROXY_PORT_PLACEHOLDER


def fetch(url):
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({"http": FWDPROXY, "https": FWDPROXY})
    )
    return opener.open(url, timeout=120)


class ForwardProxy(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        url = self.path
        if not url.startswith(("http://", "https://")):
            host = self.headers.get("Host")
            if not host:
                self.send_error(400, "Missing Host header")
                return
            url = f"http://{host}{self.path}"
        try:
            resp = fetch(url)
            self.send_response(resp.status)
            for key, value in resp.headers.items():
                if key.lower() not in ("transfer-encoding", "connection"):
                    self.send_header(key, value)
            self.end_headers()
            while True:
                chunk = resp.read(65536)
                if not chunk:
                    break
                self.wfile.write(chunk)
                self.wfile.flush()
        except urllib.error.HTTPError as exc:
            self.send_error(exc.code, str(exc))
        except Exception as exc:
            self.send_error(502, str(exc))

    def do_CONNECT(self):
        host, _, port_text = self.path.partition(":")
        port = int(port_text) if port_text else 443
        try:
            proxy_sock = socket.create_connection(("fwdproxy", 8080), timeout=30)
            proxy_sock.sendall(
                f"CONNECT {host}:{port} HTTP/1.1\r\n"
                f"Host: {host}:{port}\r\n\r\n".encode()
            )
            resp = b""
            while b"\r\n\r\n" not in resp:
                chunk = proxy_sock.recv(4096)
                if not chunk:
                    break
                resp += chunk
            if b"200" not in resp.split(b"\r\n")[0]:
                self.send_error(502, f"CONNECT failed: {resp[:100]}")
                proxy_sock.close()
                return
            self.send_response(200, "Connection established")
            self.end_headers()

            client_sock = self.connection

            def relay(src, dst):
                try:
                    while True:
                        data = src.recv(65536)
                        if not data:
                            break
                        dst.sendall(data)
                except Exception:
                    pass
                finally:
                    try:
                        dst.shutdown(socket.SHUT_WR)
                    except Exception:
                        pass

            thread = threading.Thread(
                target=relay, args=(proxy_sock, client_sock), daemon=True
            )
            thread.start()
            relay(client_sock, proxy_sock)
            thread.join(timeout=120)
            proxy_sock.close()
        except Exception as exc:
            self.send_error(502, str(exc))

    def log_message(self, fmt, *args):
        pass


class ThreadedIPv6Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    address_family = socket.AF_INET6
    allow_reuse_address = True
    daemon_threads = True


server = ThreadedIPv6Server(("::", PORT), ForwardProxy)
print(f"Proxy on :::{PORT}", flush=True)
server.serve_forever()
PYEOF

  sed -i "s/PROXY_PORT_PLACEHOLDER/${PROXY_PORT}/" "${PROXY_SCRIPT}"
}

start_proxy() {
  if [[ "${USE_DEVVM_APT_PROXY}" == "0" ]]; then
    return
  fi

  echo "Starting apt proxy on port ${PROXY_PORT}..."
  write_proxy_script
  if [[ -f "${PROXY_PID_FILE}" ]]; then
    kill "$(cat "${PROXY_PID_FILE}")" 2>/dev/null || true
    rm -f "${PROXY_PID_FILE}"
  fi
  if command -v lsof >/dev/null 2>&1; then
    lsof -t -i ":${PROXY_PORT}" 2>/dev/null | xargs -r kill 2>/dev/null || true
  fi
  sh -c "echo \$\$ > '${PROXY_PID_FILE}'; exec python3 '${PROXY_SCRIPT}' 2>&1" \
    | tee -a "${PROXY_LOG}" >/dev/null &
  sleep 2
  if command -v ss >/dev/null 2>&1 && ! ss -tln | grep -q ":${PROXY_PORT}"; then
    echo "ERROR: proxy failed to start. Check ${PROXY_LOG}" >&2
    cat "${PROXY_LOG}" >&2 || true
    exit 1
  fi
  echo "Proxy started with PID $(cat "${PROXY_PID_FILE}")"
}

stop_proxy() {
  if [[ -f "${PROXY_PID_FILE}" ]]; then
    kill "$(cat "${PROXY_PID_FILE}")" 2>/dev/null || true
    rm -f "${PROXY_PID_FILE}"
  fi
}

ubuntu_proxy_preamble() {
  if [[ "${USE_DEVVM_APT_PROXY}" == "0" ]]; then
    return
  fi

  # Build args are visible to RUN instructions without becoming image ENV.
  cat <<EOF
ARG https_proxy=http://[::1]:${PROXY_PORT}
ARG http_proxy=http://[::1]:${PROXY_PORT}
ARG no_proxy=localhost,127.0.0.1
ARG NO_PROXY=localhost,127.0.0.1
EOF
}

patch_ubuntu_dockerfile() {
  local src="$1"
  local dst="$2"
  local preamble
  preamble="$(ubuntu_proxy_preamble)"

  python3 - "$src" "$dst" "$preamble" <<'PYEOF'
import sys

src, dst, preamble = sys.argv[1], sys.argv[2], sys.argv[3]
content = open(src).read()
if preamble:
    content = content.replace("FROM ${BASE_IMAGE}\n", "FROM ${BASE_IMAGE}\n" + preamble + "\n")
open(dst, "w").write(content)
print(f"Patched {dst}")
PYEOF
}

build_image() {
  local target="$1"
  local tag="$2"
  local dockerfile
  local context_dir
  local log_file
  local build_args=()

  case "${target}" in
    24)
      dockerfile="/tmp/Dockerfile.rocksdb-extensions-ubuntu24"
      patch_ubuntu_dockerfile \
        "${REPOSITORY_ROOT}/build_tools/ubuntu24_image/Dockerfile" \
        "${dockerfile}"
      context_dir="${REPOSITORY_ROOT}/build_tools/ubuntu24_image"
      log_file="/tmp/rocksdb-extensions-build-ubuntu24.log"
      build_args=(--build-arg "BASE_IMAGE=${UBUNTU_BASE_IMAGE}")
      ;;
    centos9)
      dockerfile="${REPOSITORY_ROOT}/build_tools/centos9_image/Dockerfile"
      context_dir="${REPOSITORY_ROOT}/build_tools/centos9_image"
      log_file="/tmp/rocksdb-extensions-build-centos9.log"
      build_args=(--build-arg "BASE_IMAGE=${CENTOS_BASE_IMAGE}")
      ;;
    *)
      echo "Unknown image target: ${target}" >&2
      exit 1
      ;;
  esac

  echo ""
  echo "=== Building ${target} -> ${tag} ==="
  echo "Dockerfile: ${dockerfile}"
  echo "Log:        ${log_file}"

  if "${engine[@]}" build --network=host \
      "${build_args[@]}" \
      -f "${dockerfile}" \
      -t "${tag}" \
      "${context_dir}" 2>&1 | tee "${log_file}"; then
    echo "Built ${tag}"
  else
    echo "Build failed. See ${log_file}" >&2
    return 1
  fi
}

validate_image() {
  local tag="$1"
  local run_args=()

  if [[ "${USE_DEVVM_APT_PROXY}" != "0" ]]; then
    local validation_http_proxy="${http_proxy:-${HTTP_PROXY:-http://fwdproxy:8080}}"
    local validation_https_proxy="${https_proxy:-${HTTPS_PROXY:-${validation_http_proxy}}}"
    local validation_no_proxy="${no_proxy:-${NO_PROXY:-localhost,127.0.0.1,::1}}"

    run_args+=(
      --network=host
      -e "http_proxy=${validation_http_proxy}"
      -e "https_proxy=${validation_https_proxy}"
      -e "HTTP_PROXY=${validation_http_proxy}"
      -e "HTTPS_PROXY=${validation_https_proxy}"
      -e "no_proxy=${validation_no_proxy}"
      -e "NO_PROXY=${validation_no_proxy}"
    )
  fi

  echo ""
  echo "=== Validating ${tag} ==="
  "${engine[@]}" run --rm \
    "${run_args[@]}" \
    -v "${REPOSITORY_ROOT}:/workspace/rocksdb-extensions:Z" \
    -w /workspace/rocksdb-extensions \
    -e ROCKSDB_REVISION="${ROCKSDB_REVISION:-v11.8.0}" \
    -e NIMBLE_REVISION="${NIMBLE_REVISION:-acead744054eb006da753390ba80d3b6a29212ce}" \
    -e CLEAN_BUILD_DIRECTORY=ON \
    "${tag}" \
    ./scripts/build-latest-releases.sh
}

push_image() {
  local tag="$1"
  echo "Pushing ${tag}..."
  "${engine[@]}" push "${tag}"
  echo "Pushed ${tag}"
}

run_target() {
  local target="$1"
  local tag
  case "${target}" in
    24)
      tag="${ubuntu_tag}"
      ;;
    centos9)
      tag="${centos_tag}"
      ;;
    *)
      echo "Unknown image target: ${target}" >&2
      exit 1
      ;;
  esac

  build_image "${target}" "${tag}"
  if [[ "${VALIDATE}" != "0" ]]; then
    validate_image "${tag}"
  fi
  if [[ "${PUSH}" != "0" ]]; then
    push_image "${tag}"
  fi
}

if [[ "${BASH_SOURCE[0]}" != "${0}" ]]; then
  return 0
fi

case "${TARGET}" in
  24|centos9|all)
    ;;
  *)
    echo "Usage: $0 [24|centos9|all] [--push] [--validate] [--devvm-proxy]" >&2
    exit 1
    ;;
esac

if [[ "${USE_DEVVM_APT_PROXY}" != "0" && "${TARGET}" != "centos9" ]]; then
  trap stop_proxy EXIT
  start_proxy
fi

case "${TARGET}" in
  24|centos9)
    run_target "${TARGET}"
    ;;
  all)
    run_target 24
    run_target centos9
    ;;
esac

echo ""
echo "Done. Log in before pushing, for example:"
echo "  ${CONTAINER_ENGINE} login ghcr.io -u <github-username>"
