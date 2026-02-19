#!/bin/bash
# Install pre-commit hooks with test-report auto-staging wrapper.
#
# The wrapper stages test-reports/ BEFORE pre-commit's stash mechanism,
# preventing stash conflicts when model tests generate new reports.
#
# Safe to re-run — idempotent via marker check.
set -euo pipefail

HOOK=".git/hooks/pre-commit"
MARKER="# entropi: auto-stage test reports"

# Ensure pre-commit hook exists
if [ ! -f "$HOOK" ]; then
    echo "Installing pre-commit hooks..."
    .venv/bin/pre-commit install
fi

# Idempotency: skip if wrapper already installed
if grep -q "$MARKER" "$HOOK" 2>/dev/null; then
    echo "Hook wrapper already installed."
    exit 0
fi

# Read existing hook content (everything after the shebang)
existing=$(tail -n +2 "$HOOK")

# Write wrapper: staging logic before pre-commit framework
cat > "$HOOK" << 'HOOK_HEADER'
#!/usr/bin/env bash
# entropi: auto-stage test reports
# Stages test-reports/ before pre-commit stashes unstaged changes.
# This prevents stash conflicts when model tests regenerate reports.
# Regenerate with: bash scripts/setup-hooks.sh

git diff --quiet -- test-reports/ 2>/dev/null || \
    git add test-reports/ 2>/dev/null || true
HOOK_HEADER

# Append original hook body (minus shebang)
echo "$existing" >> "$HOOK"
chmod +x "$HOOK"
echo "Hook wrapper installed."
