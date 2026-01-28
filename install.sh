#!/bin/bash
# ============================================================================
# Entropi Installation Script
# Supports both Docker and native installation modes
# ============================================================================

set -e

echo "╔════════════════════════════════════════════════════════════════╗"
echo "║                    Installing Entropi                          ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Parse arguments
INSTALL_MODE="docker"
for arg in "$@"; do
    case $arg in
        --native)
            INSTALL_MODE="native"
            shift
            ;;
    esac
done

# Function to install native dependencies
install_native_deps() {
    echo ""
    echo "=== Installing Native Dependencies ==="
    echo ""

    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        # Detect package manager
        if command -v apt-get &> /dev/null; then
            echo "Installing system packages (requires sudo)..."
            sudo apt-get update
            sudo apt-get install -y \
                python3-dev \
                build-essential \
                libportaudio2 \
                portaudio19-dev \
                libasound2-dev
            echo -e "${GREEN}✓${NC} System packages installed"
        elif command -v dnf &> /dev/null; then
            echo "Installing system packages (requires sudo)..."
            sudo dnf install -y \
                python3-devel \
                gcc \
                portaudio \
                portaudio-devel \
                alsa-lib-devel
            echo -e "${GREEN}✓${NC} System packages installed"
        elif command -v pacman &> /dev/null; then
            echo "Installing system packages (requires sudo)..."
            sudo pacman -S --needed \
                python \
                base-devel \
                portaudio \
                alsa-lib
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
}

# Native installation mode
if [ "$INSTALL_MODE" = "native" ]; then
    echo "Installing in NATIVE mode (no Docker)"
    echo ""

    # Install system dependencies
    install_native_deps

    # Check for Python
    if ! command -v python3 &> /dev/null; then
        echo -e "${RED}ERROR: Python 3 not found.${NC}"
        exit 1
    fi
    echo -e "${GREEN}✓${NC} Python 3 found: $(python3 --version)"

    # Get script directory
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

    # Install in editable mode
    echo ""
    echo "Installing Entropi package..."
    pip install -e "$SCRIPT_DIR[voice]"

    echo ""
    echo -e "${GREEN}✓${NC} Entropi installed!"
    echo ""
    echo "Run 'entropi' to start."
    exit 0
fi

# Docker installation mode (default)
echo "Installing in DOCKER mode"
echo "(use --native flag for native installation)"
echo ""

# Check for Docker
if ! command -v docker &> /dev/null; then
    echo -e "${RED}ERROR: Docker not found.${NC}"
    echo "Please install Docker first: https://docs.docker.com/engine/install/"
    echo ""
    echo "Or run with --native flag to install without Docker:"
    echo "  ./install.sh --native"
    exit 1
fi
echo -e "${GREEN}✓${NC} Docker installed"

# Check for NVIDIA container runtime
if docker info 2>/dev/null | grep -q "nvidia\|Runtimes.*nvidia"; then
    echo -e "${GREEN}✓${NC} NVIDIA container runtime detected"
else
    echo -e "${YELLOW}⚠${NC} NVIDIA container runtime not detected"
    echo "  GPU acceleration may not work."
    echo "  Install: https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/install-guide.html"
    echo ""
fi

# Check for GPU
if command -v nvidia-smi &> /dev/null; then
    GPU_NAME=$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1)
    if [ -n "$GPU_NAME" ]; then
        echo -e "${GREEN}✓${NC} Found GPU: $GPU_NAME"
    fi
else
    echo -e "${YELLOW}⚠${NC} nvidia-smi not found. GPU detection skipped."
fi

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Check if we're in the entropi repo (has pyproject.toml)
if [ -f "$SCRIPT_DIR/pyproject.toml" ]; then
    echo ""
    echo "Building from local source: $SCRIPT_DIR"
    BUILD_FROM_SOURCE=true
else
    BUILD_FROM_SOURCE=false
fi

echo ""
echo "=== Step 1: Building/Pulling Docker image ==="
echo ""

if [ "$BUILD_FROM_SOURCE" = true ]; then
    # Build from local source
    echo "Building Entropi image (this may take several minutes on first build)..."
    docker build -f "$SCRIPT_DIR/docker/Dockerfile" -t entropi:latest "$SCRIPT_DIR"

    IMAGE_NAME="entropi:latest"
else
    # Try to pull from registry
    echo "Pulling Entropi Docker image..."
    if docker pull ghcr.io/entropi/entropi:latest 2>/dev/null; then
        IMAGE_NAME="ghcr.io/entropi/entropi:latest"
        echo -e "${GREEN}✓${NC} Image pulled"
    else
        echo -e "${RED}ERROR: Could not pull image and not in source directory.${NC}"
        echo "Please run this script from the entropi repository."
        exit 1
    fi
fi

echo ""
echo "=== Step 2: Installing wrapper script ==="
echo ""

# Determine install location
if [ -w /usr/local/bin ]; then
    INSTALL_DIR="/usr/local/bin"
else
    INSTALL_DIR="$HOME/.local/bin"
    mkdir -p "$INSTALL_DIR"
fi

# Create wrapper script
WRAPPER_PATH="$INSTALL_DIR/entropi"

cat > "$WRAPPER_PATH" << 'WRAPPER_EOF'
#!/bin/bash
# Entropi - Local AI Coding Assistant
# This wrapper invokes the Docker container with appropriate mounts

# Configuration
ENTROPI_IMAGE="${ENTROPI_IMAGE:-entropi:latest}"
ENTROPI_MODELS_DIR="${ENTROPI_MODELS_DIR:-$HOME/models/gguf}"

# Ensure models directory exists
if [ ! -d "$ENTROPI_MODELS_DIR" ]; then
    echo "WARNING: Models directory not found: $ENTROPI_MODELS_DIR"
    echo "Run: entropi download --help"
    mkdir -p "$ENTROPI_MODELS_DIR"
fi

# Ensure global config directory exists
mkdir -p "$HOME/.entropi"

# Determine if we need interactive mode
DOCKER_FLAGS="--rm"
if [ -t 0 ] && [ -t 1 ]; then
    # Both stdin and stdout are TTYs - use interactive mode
    DOCKER_FLAGS="$DOCKER_FLAGS -it"
fi

# Audio device access for voice mode
AUDIO_FLAGS=""
# ALSA devices
if [ -d /dev/snd ]; then
    AUDIO_FLAGS="$AUDIO_FLAGS --device /dev/snd"
fi
# PulseAudio socket (Linux)
PULSE_SOCKET="/run/user/$(id -u)/pulse/native"
if [ -S "$PULSE_SOCKET" ]; then
    AUDIO_FLAGS="$AUDIO_FLAGS -v /run/user/$(id -u)/pulse:/run/user/$(id -u)/pulse"
    AUDIO_FLAGS="$AUDIO_FLAGS -e PULSE_SERVER=unix:$PULSE_SOCKET"
fi

# Get audio group ID for ALSA access
AUDIO_GID=$(getent group audio | cut -d: -f3 2>/dev/null || echo "")
GROUP_ADD=""
if [ -n "$AUDIO_GID" ]; then
    GROUP_ADD="--group-add $AUDIO_GID"
fi

# Run entropi in Docker
exec docker run $DOCKER_FLAGS \
    --gpus all \
    $AUDIO_FLAGS \
    $GROUP_ADD \
    -e HOME=/home/user \
    -e TERM="$TERM" \
    -v "$ENTROPI_MODELS_DIR:/home/user/models/gguf:ro" \
    -v "$HOME/.entropi:/home/user/.entropi" \
    -v "$(pwd):/workspace" \
    -w /workspace \
    --user "$(id -u):$(id -g)" \
    "$ENTROPI_IMAGE" \
    "$@"
WRAPPER_EOF

chmod +x "$WRAPPER_PATH"
echo -e "${GREEN}✓${NC} Installed wrapper to: $WRAPPER_PATH"

# Check if install dir is in PATH
if [[ ":$PATH:" != *":$INSTALL_DIR:"* ]]; then
    echo ""
    echo -e "${YELLOW}NOTE:${NC} $INSTALL_DIR is not in your PATH"
    echo "Add to your ~/.bashrc or ~/.zshrc:"
    echo ""
    echo "  export PATH=\"$INSTALL_DIR:\$PATH\""
    echo ""
fi

echo ""
echo "=== Step 3: Verifying installation ==="
echo ""

# Test the wrapper
if command -v entropi &> /dev/null; then
    echo -e "${GREEN}✓${NC} entropi command available: $(which entropi)"
else
    echo -e "${YELLOW}!${NC} entropi not in PATH yet. Run:"
    echo "  export PATH=\"$INSTALL_DIR:\$PATH\""
fi

echo ""
echo "╔════════════════════════════════════════════════════════════════╗"
echo "║                   Installation Complete!                        ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""
echo "Next steps:"
echo ""
echo "  1. Ensure models are in ~/models/gguf/"
echo "     (or set ENTROPI_MODELS_DIR environment variable)"
echo ""
echo "  2. Navigate to any project and run:"
echo "     cd /path/to/your/project"
echo "     entropi"
echo ""
echo "  3. First run will auto-create .entropi/ config in your project"
echo ""
