#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v2.0.5 distribution smoke test.
#
# Builds entropic (shared, CPU-only, no tests) to a fresh build dir,
# installs it to a temporary prefix, and builds+runs
# packaging/smoke-consumer against it using find_package(entropic).
#
# Exits non-zero on any failure. Prints a short status line at the end.
#
# Expected runtime: ~30-60s on a warm FetchContent cache.
#
# Usage:
#   scripts/smoke_distribution.sh [--prefix /tmp/entropic-smoke]
#                                 [--build-dir build/smoke]

set -euo pipefail

PREFIX=${PREFIX:-/tmp/entropic-smoke}
BUILD_DIR=${BUILD_DIR:-build/smoke}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)    PREFIX="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

echo "== entropic: configure + build (shared CPU) =="
rm -rf "$BUILD_DIR" "$PREFIX"
cmake -B "$BUILD_DIR" -S . \
    -DENTROPIC_SHARED=ON \
    -DENTROPIC_STATIC=OFF \
    -DENTROPIC_CPU_ONLY=ON \
    -DENTROPIC_BUILD_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null

cmake --build "$BUILD_DIR" --parallel >/dev/null

echo "== entropic: install → $PREFIX =="
cmake --install "$BUILD_DIR" >/dev/null

echo "== consumer: configure + build against find_package(entropic) =="
CONSUMER_BUILD="$BUILD_DIR/consumer"
rm -rf "$CONSUMER_BUILD"
cmake -B "$CONSUMER_BUILD" -S packaging/smoke-consumer \
    -Dentropic_DIR="$PREFIX/lib/cmake/entropic" >/dev/null
cmake --build "$CONSUMER_BUILD" >/dev/null

echo "== consumer: run =="
"$CONSUMER_BUILD/entropic-smoke"

echo "== CLI: launch from install prefix (RPATH check) =="
"$PREFIX/bin/entropic" version

echo "OK — distribution smoke passed (prefix=$PREFIX)"
