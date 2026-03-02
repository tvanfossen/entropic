#!/bin/bash
# ============================================================================
# Entropic Installation Script
#
# Usage:
#   ./install.sh              # Core + inference (no TUI)
#   ./install.sh app          # Full TUI application (recommended)
#   ./install.sh all          # Everything including voice
# ============================================================================

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

EXTRAS="${1:-app}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "╔════════════════════════════════════════════════════════════════╗"
echo "║                    Installing Entropic                          ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""
echo "Extras: $EXTRAS"
echo ""

# --- Step 1: System dependencies (voice extras only) ---

install_voice_deps() {
    echo "=== Installing Voice Dependencies ==="
    echo ""

    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        if command -v apt-get &> /dev/null; then
            echo "Installing system packages (requires sudo)..."
            sudo apt-get update
            sudo apt-get install -y \
                python3-dev build-essential \
                libportaudio2 portaudio19-dev libasound2-dev
            echo -e "${GREEN}✓${NC} System packages installed"
        elif command -v dnf &> /dev/null; then
            echo "Installing system packages (requires sudo)..."
            sudo dnf install -y \
                python3-devel gcc \
                portaudio portaudio-devel alsa-lib-devel
            echo -e "${GREEN}✓${NC} System packages installed"
        elif command -v pacman &> /dev/null; then
            echo "Installing system packages (requires sudo)..."
            sudo pacman -S --needed python base-devel portaudio alsa-lib
            echo -e "${GREEN}✓${NC} System packages installed"
        else
            echo -e "${YELLOW}⚠${NC} Unknown package manager. Please install manually:"
            echo "  - portaudio (libportaudio2 / portaudio19-dev)"
            echo "  - alsa development libraries (libasound2-dev)"
        fi
    elif [[ "$OSTYPE" == "darwin"* ]]; then
        if command -v brew &> /dev/null; then
            echo "Installing system packages with Homebrew..."
            brew install portaudio
            echo -e "${GREEN}✓${NC} System packages installed"
        else
            echo -e "${YELLOW}⚠${NC} Homebrew not found. Please install portaudio manually:"
            echo "  brew install portaudio"
        fi
    fi
    echo ""
}

# Install voice system deps if voice or all extras requested
if [[ "$EXTRAS" == "voice" || "$EXTRAS" == "all" ]]; then
    install_voice_deps
fi

# --- Step 2: Python detection ---

echo "=== Checking Python ==="
echo ""

PYTHON=""
for candidate in python3.12 python3.11 python3; do
    if command -v "$candidate" &> /dev/null; then
        PYTHON="$candidate"
        break
    fi
done

if [ -z "$PYTHON" ]; then
    echo -e "${RED}ERROR: Python 3.10+ required but not found.${NC}"
    exit 1
fi

PY_VERSION=$("$PYTHON" -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')")
echo -e "${GREEN}✓${NC} Python: $PYTHON ($PY_VERSION)"

# --- Step 3: Virtual environment ---

echo ""
echo "=== Setting up Virtual Environment ==="
echo ""

if [ ! -d "$SCRIPT_DIR/.venv" ]; then
    echo "Creating virtual environment..."
    "$PYTHON" -m venv "$SCRIPT_DIR/.venv"
    echo -e "${GREEN}✓${NC} Virtual environment created"
else
    echo -e "${GREEN}✓${NC} Virtual environment exists"
fi

PIP="$SCRIPT_DIR/.venv/bin/pip"

# Warn about old pip — common on Ubuntu 22 and can cause slow dependency resolution
PIP_VERSION=$($PIP --version 2>/dev/null | awk '{print $2}' | cut -d. -f1)
if [ -n "$PIP_VERSION" ] && [ "$PIP_VERSION" -lt 23 ] 2>/dev/null; then
    echo -e "${YELLOW}⚠${NC} pip $($PIP --version | awk '{print $2}') is old and may resolve dependencies slowly"
    echo "  Consider: $SCRIPT_DIR/.venv/bin/python -m pip install --upgrade pip"
    echo ""
fi

# --- Step 4: GPU detection ---

echo ""
echo "=== Checking GPU ==="
echo ""

CMAKE_ARGS=""
if command -v nvidia-smi &> /dev/null; then
    GPU_NAME=$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1)
    if [ -n "$GPU_NAME" ]; then
        echo -e "${GREEN}✓${NC} Found GPU: $GPU_NAME"
        echo "  Building llama-cpp-python with CUDA support"
        CMAKE_ARGS="-DGGML_CUDA=on"
    fi
else
    echo "No NVIDIA GPU detected — CPU-only mode"
fi

# --- Step 5: Clone + build llama-cpp-python ---

echo ""
echo "=== Building llama-cpp-python ==="
echo ""

# Read pinned versions from build_config.py (single source of truth)
BUILD_CONFIG="$SCRIPT_DIR/src/entropic/build_config.py"
LLAMA_CPP_PYTHON_REPO=$(grep 'LLAMA_CPP_PYTHON_REPO' "$BUILD_CONFIG" | head -1 | sed 's/.*= *"//;s/".*//')
LLAMA_CPP_PYTHON_TAG=$(grep 'LLAMA_CPP_PYTHON_TAG' "$BUILD_CONFIG" | head -1 | sed 's/.*= *"//;s/".*//')

echo "llama-cpp-python: $LLAMA_CPP_PYTHON_TAG"
echo "  repo: $LLAMA_CPP_PYTHON_REPO"

BUILD_DIR="$SCRIPT_DIR/.build"
CLONE_DIR="$BUILD_DIR/llama-cpp-python"

if [ -d "$CLONE_DIR" ]; then
    echo -e "${GREEN}✓${NC} Using cached clone: $CLONE_DIR"
else
    echo "Cloning llama-cpp-python $LLAMA_CPP_PYTHON_TAG..."
    mkdir -p "$BUILD_DIR"
    git clone --depth=1 \
        --branch="$LLAMA_CPP_PYTHON_TAG" \
        --recurse-submodules --shallow-submodules \
        "$LLAMA_CPP_PYTHON_REPO" \
        "$CLONE_DIR"
    echo -e "${GREEN}✓${NC} Clone complete"
fi

echo "Building llama-cpp-python..."
if [ -n "$CMAKE_ARGS" ]; then
    echo "  CMAKE_ARGS: $CMAKE_ARGS"
    PIP_CONFIG_FILE=/dev/null CMAKE_ARGS="$CMAKE_ARGS" $PIP install "$CLONE_DIR"
else
    $PIP install "$CLONE_DIR"
fi
echo -e "${GREEN}✓${NC} llama-cpp-python built and installed"

# --- Step 6: Install ---

echo ""
echo "=== Installing Entropic ==="
echo ""

echo "Installing entropic-engine[$EXTRAS]..."
$PIP install -e "$SCRIPT_DIR[$EXTRAS]"

echo ""
echo -e "${GREEN}✓${NC} Entropic installed!"

# --- Step 7: Verify ---

echo ""
echo "=== Verifying Installation ==="
echo ""

ENTROPIC_BIN="$SCRIPT_DIR/.venv/bin/entropic"
if [ -f "$ENTROPIC_BIN" ]; then
    echo -e "${GREEN}✓${NC} entropic command available: $ENTROPIC_BIN"
else
    echo -e "${YELLOW}⚠${NC} entropic command not found in venv"
fi

echo ""
echo "╔════════════════════════════════════════════════════════════════╗"
echo "║                   Installation Complete!                        ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""
echo "Next steps:"
echo ""
echo "  1. Add the entropic command to your PATH (pick one):"
echo ""
echo "     # Option A: Symlink into ~/.local/bin/"
echo "     mkdir -p ~/.local/bin && ln -sf $ENTROPIC_BIN ~/.local/bin/entropic"
echo ""
echo "     # Option B: Add venv bin to PATH in your shell profile"
echo "     echo 'export PATH=\"$SCRIPT_DIR/.venv/bin:\$PATH\"' >> ~/.bashrc"
echo ""
echo "     # Or just use the full path: $ENTROPIC_BIN"
echo ""
echo "  2. Ensure models are in ~/models/gguf/"
echo "     (or configure model paths in .entropic/config.local.yaml)"
echo ""
echo "  3. Navigate to any project and run: entropic"
echo "     First run will auto-create .entropic/ config in your project"
echo ""
