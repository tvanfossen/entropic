#!/bin/bash
# Entropi native installation script
#
# Installs entropi with CUDA-accelerated llama-cpp-python if GPU detected.
# Usage:
#   ./scripts/install.sh          # Core + inference (no TUI)
#   ./scripts/install.sh app      # Full TUI application
#   ./scripts/install.sh all      # Everything including voice
set -e

EXTRAS="${1:-}"

echo "Installing Entropi"
echo "=================="

# Detect Python
PYTHON=""
for candidate in python3.12 python3.11 python3; do
    if command -v "$candidate" &> /dev/null; then
        PYTHON="$candidate"
        break
    fi
done

if [ -z "$PYTHON" ]; then
    echo "Error: Python 3.11+ required but not found"
    exit 1
fi

PY_VERSION=$("$PYTHON" -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')")
echo "Python: $PYTHON ($PY_VERSION)"

# Create venv if needed
if [ ! -d ".venv" ]; then
    echo "Creating virtual environment..."
    "$PYTHON" -m venv .venv
fi

PIP=".venv/bin/pip"

# Check for NVIDIA GPU and set CUDA build flags for llama-cpp-python
if command -v nvidia-smi &> /dev/null; then
    echo "NVIDIA GPU detected — building llama-cpp-python with CUDA"
    export CMAKE_ARGS="-DGGML_CUDA=on"
else
    echo "No NVIDIA GPU — CPU-only mode"
fi

# Install entropi
if [ -n "$EXTRAS" ]; then
    echo "Installing entropi[$EXTRAS]..."
    $PIP install -e ".[$EXTRAS]"
else
    echo "Installing entropi (core)..."
    $PIP install -e .
fi

echo ""
echo "Installation complete."
echo "Run: .venv/bin/entropi --help"
