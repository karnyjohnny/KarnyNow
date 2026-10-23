#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [[ -f /etc/os-release ]]; then
  echo "==> OS detected: $(grep '^NAME=' /etc/os-release | head -n1)"
fi

echo "==> installing toolchain"
apt-get update
apt-get install -y --no-install-recommends \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  wget \
  ca-certificates \
  git

echo "==> preparing third_party"
mkdir -p third_party
cd third_party

if [[ ! -d mbedtls ]]; then
  git clone --depth 1 --branch v3.6.0 https://github.com/Mbed-TLS/mbedtls.git
fi

cd mbedtls
git submodule update --init --recursive
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_PROGRAMS=OFF \
  -DENABLE_TESTING=OFF \
  -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
  -DUSE_STATIC_MBEDTLS_LIBRARY=ON
cmake --build build

echo "==> sandbox prep complete"
echo "==> mbedTLS tree ready at: $ROOT_DIR/third_party/mbedtls"
echo "==> next step: integrate mbedTLS into CMake and switch TLS backend via KN_TLS_BACKEND=mbedtls"
