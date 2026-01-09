#!/bin/bash
# ============================================================================
# Entropi Installation Script
# Builds Docker image and installs wrapper script for native CLI experience
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

# Check for Docker
if ! command -v docker &> /dev/null; then
    echo -e "${RED}ERROR: Docker not found.${NC}"
    echo "Please install Docker first: https://docs.docker.com/engine/install/"
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

# Run entropi in Docker
exec docker run $DOCKER_FLAGS \
    --gpus all \
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
