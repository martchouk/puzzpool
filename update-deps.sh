#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="build"

git submodule sync --recursive
git submodule update --init --recursive --remote

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j

echo "[update-deps] install frontend dependencies"
npm ci --prefix frontend

echo "[update-deps] build frontend → public/index.html"
npm run build --prefix frontend
