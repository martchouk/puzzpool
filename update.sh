#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="build"

echo "[update] sync submodule config"
git submodule sync --recursive

echo "[update] checkout pinned submodule commits"
git submodule update --init --recursive

echo "[update] configure"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

echo "[update] build incrementally"
cmake --build "$BUILD_DIR" -j

echo "[update] install frontend dependencies"
npm ci --prefix frontend

echo "[update] build frontend → public/index.html"
npm run build --prefix frontend
