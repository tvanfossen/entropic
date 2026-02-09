#!/bin/bash
# Run model tests with content-hash caching.
#
# Computes a hash of all files that could affect model test behavior.
# If nothing changed since the last successful run, skip the tests.
#
# Cache file: .test-reports/model-tests.hash
# Report file: .test-reports/model-tests-latest.txt

set -euo pipefail

CACHE_DIR=".test-reports"
HASH_FILE="$CACHE_DIR/model-tests.hash"
REPORT_FILE="$CACHE_DIR/model-tests-latest.txt"

mkdir -p "$CACHE_DIR"

# Compute content hash of all files that affect model test behavior.
# Includes: source code, test code, config schema, test fixtures.
compute_hash() {
    find src/ tests/model/ tests/conftest.py .entropi/config.yaml -type f \
        -name '*.py' -o -name '*.yaml' 2>/dev/null \
        | sort \
        | xargs sha256sum 2>/dev/null \
        | sha256sum \
        | cut -d' ' -f1
}

current_hash=$(compute_hash)

# Check if hash matches last successful run
if [ -f "$HASH_FILE" ]; then
    cached_hash=$(cat "$HASH_FILE")
    if [ "$current_hash" = "$cached_hash" ]; then
        echo "Model tests: SKIPPED (no changes since last successful run)"
        echo "  Cache: $HASH_FILE"
        echo "  To force re-run: rm $HASH_FILE"
        exit 0
    fi
fi

echo "Model tests: running (content changed since last run)..."

# Run model tests, capturing output for report
COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "uncommitted")
BRANCH=$(git branch --show-current 2>/dev/null || echo "unknown")
DATE=$(date -Iseconds)

# Run tests and tee to report
{
    echo "=== Model Test Report ==="
    echo "Commit: $COMMIT"
    echo "Branch: $BRANCH"
    echo "Date:   $DATE"
    echo "Hash:   $current_hash"
    echo "========================="
    echo ""
} > "$REPORT_FILE"

# Run pytest, appending output to report
if .venv/bin/pytest tests/model/ -v --tb=short 2>&1 | tee -a "$REPORT_FILE"; then
    # Tests passed - save hash for cache
    echo "$current_hash" > "$HASH_FILE"
    echo "" >> "$REPORT_FILE"
    echo "Result: PASSED" >> "$REPORT_FILE"
    echo ""
    echo "Model tests: PASSED (hash cached for next run)"
    exit 0
else
    # Tests failed - don't cache
    echo "" >> "$REPORT_FILE"
    echo "Result: FAILED" >> "$REPORT_FILE"
    echo ""
    echo "Model tests: FAILED (hash NOT cached)"
    exit 1
fi
