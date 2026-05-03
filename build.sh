#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="build"

echo "[update] sync submodule config"
git submodule sync --recursive

echo "[update] checkout pinned submodule commits"
git submodule update --init --recursive

echo "[build] configure"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

echo "[build] build incrementally"
cmake --build "$BUILD_DIR" -j

echo "[build] install frontend dependencies"
npm ci --prefix frontend

echo "[build] build frontend → public/index.html"
npm run build --prefix frontend
