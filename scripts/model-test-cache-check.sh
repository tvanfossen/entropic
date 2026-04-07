#!/bin/bash
# Check if model tests need to run based on content hash.
#
# Computes a hash of all C++ source files that could affect model test
# behavior. If nothing changed since the last successful run, writes
# SKIP flag. Otherwise, writes RUN flag.
#
# Cache file: test-reports/model-tests.hash
# Flag file:  test-reports/model-tests.flag
#
# v1.10.2: Updated to hash C++ sources (not deleted Python files).

set -euo pipefail

CACHE_DIR="test-reports"
HASH_FILE="$CACHE_DIR/model-tests.hash"
FLAG_FILE="$CACHE_DIR/model-tests.flag"

mkdir -p "$CACHE_DIR"

# Compute content hash of all files that affect model test behavior.
# Includes: C++ source, headers, test code, CMake config, YAML config.
compute_hash() {
    find src/ include/ tests/model/ CMakeLists.txt \
        .entropic/config.local.yaml \
        -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \
                   -o -name '*.cmake' -o -name 'CMakeLists.txt' \
                   -o -name '*.yaml' \) 2>/dev/null \
        | sort \
        | xargs sha256sum 2>/dev/null \
        | sha256sum \
        | cut -d' ' -f1
}

current_hash=$(compute_hash)

if [ -f "$HASH_FILE" ]; then
    cached_hash=$(cat "$HASH_FILE")
    if [ "$current_hash" = "$cached_hash" ]; then
        echo "SKIP" > "$FLAG_FILE"
        echo "Model tests: no changes since last successful run"
        echo "  To force: rm $HASH_FILE"
        exit 0
    fi
fi

echo "RUN:$current_hash" > "$FLAG_FILE"
echo "Model tests: content changed, tests will run"
exit 0
