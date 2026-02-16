#!/bin/bash
# Run model tests, reading the flag from cache-check step.
#
# Expects model-test-cache-check.sh to have run first and written
# a flag file indicating whether tests should run.
#
# Flag file:  test-reports/model-tests.flag
# Cache file: test-reports/model-tests.hash
# Report file: test-reports/model-tests-latest.txt

set -euo pipefail

CACHE_DIR="test-reports"
FLAG_FILE="$CACHE_DIR/model-tests.flag"
HASH_FILE="$CACHE_DIR/model-tests.hash"
REPORT_FILE="$CACHE_DIR/model-tests-latest.txt"

# Read flag from cache-check step
if [ ! -f "$FLAG_FILE" ]; then
    echo "No flag file found — running cache check inline"
    bash scripts/model-test-cache-check.sh
fi

flag=$(cat "$FLAG_FILE")

if [ "$flag" = "SKIP" ]; then
    echo "Model tests: SKIPPED (cached)"
    exit 0
fi

# Extract hash from flag (format: "RUN:<hash>")
current_hash="${flag#RUN:}"

echo "Running model tests..."

# Build report header
COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "uncommitted")
BRANCH=$(git branch --show-current 2>/dev/null || echo "unknown")
DATE=$(date -Iseconds)

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
    echo "$current_hash" > "$HASH_FILE"
    echo "" >> "$REPORT_FILE"
    echo "Result: PASSED" >> "$REPORT_FILE"
    echo ""
    echo "Model tests: PASSED (hash cached for next run)"
    exit 0
else
    echo "" >> "$REPORT_FILE"
    echo "Result: FAILED" >> "$REPORT_FILE"
    echo ""
    echo "Model tests: FAILED (hash NOT cached)"
    exit 1
fi
