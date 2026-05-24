#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="build"

# ── Parallelism ───────────────────────────────────────────────────────────────
# Default to 1 to keep peak RAM low on memory-constrained hosts.
# Each C++ translation unit (especially Crow/Boost headers) can spike to
# 300–500 MB. On a server with other services running, set this to 1 unless
# you know you have headroom.
# Override: BUILD_JOBS=4 ./build.sh
BUILD_JOBS="${BUILD_JOBS:-1}"

# ── Tests ─────────────────────────────────────────────────────────────────────
# Skip building the test suite on production; CI enables it explicitly.
# Building 9 test binaries + Catch2 adds significant compilation time and RAM
# with no benefit on a deployment host.
# Override: PUZZPOOL_BUILD_TESTS=ON ./build.sh
PUZZPOOL_BUILD_TESTS="${PUZZPOOL_BUILD_TESTS:-OFF}"

# ── SQLiteCpp ────────────────────────────────────────────────────────────────
# If the system has libsqlite3-dev installed, using PUZZPOOL_USE_SYSTEM_SQLITECPP=ON
# skips compiling SQLiteCpp from source (saves ~50 MB peak RAM and time).
# The system package is already present on Ubuntu/Debian when you install
# libsqlite3-dev. Default OFF to stay safe on hosts without the package.
# Override: PUZZPOOL_USE_SYSTEM_SQLITECPP=ON ./build.sh
PUZZPOOL_USE_SYSTEM_SQLITECPP="${PUZZPOOL_USE_SYSTEM_SQLITECPP:-OFF}"

echo "[update] sync submodule config"
git submodule sync --recursive

echo "[update] checkout pinned submodule commits"
git submodule update --init --recursive

echo "[build] configure"
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DPUZZPOOL_BUILD_TESTS="${PUZZPOOL_BUILD_TESTS}" \
    -DPUZZPOOL_USE_SYSTEM_SQLITECPP="${PUZZPOOL_USE_SYSTEM_SQLITECPP}" \
    -DPUZZPOOL_LOW_MEMORY_BUILD=ON

echo "[build] build incrementally (jobs=${BUILD_JOBS})"
cmake --build "$BUILD_DIR" -j"${BUILD_JOBS}"

echo "[build] check Node.js release age (supply-chain guard)"
bash "$(dirname "$0")/scripts/check-node-version-age.sh"

echo "[build] install frontend dependencies"
npm ci --prefix frontend

echo "[build] build frontend → public/index.html"
npm run build --prefix frontend
