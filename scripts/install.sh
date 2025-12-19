#!/bin/bash
# Entropi installation script
set -e

echo "╔════════════════════════════════════════════════════════════════╗"
echo "║                    Installing Entropi                          ║"
echo "╚════════════════════════════════════════════════════════════════╝"

# Detect OS
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    OS="linux"
elif [[ "$OSTYPE" == "darwin"* ]]; then
    OS="macos"
else
    echo "Unsupported OS: $OSTYPE"
    exit 1
fi

# Check for NVIDIA GPU
if command -v nvidia-smi &> /dev/null; then
    HAS_GPU=true
    echo "✓ NVIDIA GPU detected"
else
    HAS_GPU=false
    echo "⚠ No NVIDIA GPU detected (CPU-only mode)"
fi

# Check Docker
if command -v docker &> /dev/null; then
    echo "✓ Docker installed"
else
    echo "✗ Docker not found. Please install Docker first."
    exit 1
fi

# Pull or build image
echo ""
echo "Pulling Entropi Docker image..."
if docker pull ghcr.io/user/entropi:latest 2>/dev/null; then
    echo "✓ Image pulled"
else
    echo "Building from source..."
    docker build -t entropi:latest -f docker/Dockerfile .
fi

# Create wrapper script
INSTALL_DIR="${HOME}/.local/bin"
mkdir -p "$INSTALL_DIR"

cat > "${INSTALL_DIR}/entropi" << 'EOF'
#!/bin/bash
# Entropi wrapper script

MODELS_DIR="${ENTROPI_MODELS_DIR:-$HOME/models/gguf}"
WORKSPACE="${PWD}"

docker run -it --rm \
    --gpus all \
    -v "${MODELS_DIR}:/models:ro" \
    -v "${WORKSPACE}:/workspace" \
    -w /workspace \
    ghcr.io/user/entropi:latest "$@"
EOF

chmod +x "${INSTALL_DIR}/entropi"

echo ""
echo "╔════════════════════════════════════════════════════════════════╗"
echo "║                   Installation Complete!                        ║"
echo "╚════════════════════════════════════════════════════════════════╝"
echo ""
echo "Run 'entropi --help' to get started"
echo ""
echo "To download models:"
echo "  entropi download all"
echo ""
