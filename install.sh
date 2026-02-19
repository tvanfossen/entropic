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
    echo -e "${RED}ERROR: Python 3.11+ required but not found.${NC}"
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

# --- Step 4: GPU detection ---

echo ""
echo "=== Checking GPU ==="
echo ""

if command -v nvidia-smi &> /dev/null; then
    GPU_NAME=$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1)
    if [ -n "$GPU_NAME" ]; then
        echo -e "${GREEN}✓${NC} Found GPU: $GPU_NAME"
        echo "  Building llama-cpp-python with CUDA support"
        export CMAKE_ARGS="-DGGML_CUDA=on"
    fi
else
    echo "No NVIDIA GPU detected — CPU-only mode"
fi

# --- Step 5: Install ---

echo ""
echo "=== Installing Entropic ==="
echo ""

echo "Installing entropic[$EXTRAS]..."
$PIP install -e "$SCRIPT_DIR[$EXTRAS]"

echo ""
echo -e "${GREEN}✓${NC} Entropic installed!"

# --- Step 6: Verify ---

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
echo "  1. Ensure models are in ~/models/gguf/"
echo "     (or configure model paths in .entropic/config.local.yaml)"
echo ""
echo "  2. Navigate to any project and run:"
echo "     cd /path/to/your/project"
echo "     $ENTROPIC_BIN"
echo ""
echo "  3. First run will auto-create .entropic/ config in your project"
echo ""
