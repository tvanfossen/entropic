# Implementation 10: Docker & Distribution

> Docker packaging, CI/CD, and release automation

**Prerequisites:** Implementation 09 complete  
**Estimated Time:** 2-3 hours with Claude Code  
**Checkpoint:** `docker run` works with GPU support

---

## Objectives

1. Create production Dockerfile with CUDA support
2. Build development container for testing
3. Set up GitHub Actions CI/CD
4. Create release automation
5. Implement model download helper

---

## 1. Production Dockerfile

### File: `docker/Dockerfile`

```dockerfile
# ============================================================================
# Entropi Production Dockerfile
# Multi-stage build with CUDA support
# ============================================================================

# Stage 1: Build llama-cpp-python wheel
FROM nvidia/cuda:12.4-devel-ubuntu24.04 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    python3 \
    python3-pip \
    python3-venv \
    python3-dev \
    build-essential \
    cmake \
    git \
    && rm -rf /var/lib/apt/lists/*

# Create virtual environment
RUN python3 -m venv /opt/venv
ENV PATH="/opt/venv/bin:$PATH"

# Upgrade pip
RUN pip install --upgrade pip setuptools wheel

# Build llama-cpp-python with CUDA
# Using sm_89 for Ada architecture, adjust for your GPU
ENV CMAKE_ARGS="-DGGML_CUDA=on -DCMAKE_CUDA_ARCHITECTURES=89"
RUN pip install llama-cpp-python --no-cache-dir

# Install entropi dependencies
COPY pyproject.toml /build/
WORKDIR /build
RUN pip install . --no-cache-dir

# Stage 2: Runtime image
FROM nvidia/cuda:12.4-runtime-ubuntu24.04 AS runtime

# Install Python runtime
RUN apt-get update && apt-get install -y \
    python3 \
    python3-venv \
    git \
    && rm -rf /var/lib/apt/lists/*

# Copy virtual environment from builder
COPY --from=builder /opt/venv /opt/venv
ENV PATH="/opt/venv/bin:$PATH"

# Copy application code
COPY src/ /app/src/
COPY pyproject.toml /app/
WORKDIR /app

# Install application
RUN pip install -e . --no-deps

# Create non-root user
RUN useradd -m -s /bin/bash entropi
USER entropi

# Model volume mount point
VOLUME /models

# Workspace volume mount point
VOLUME /workspace

# Default working directory
WORKDIR /workspace

# Environment
ENV ENTROPI_MODELS_DIR=/models
ENV ENTROPI_CONFIG_DIR=/home/entropi/.entropi

# Create config directory
RUN mkdir -p /home/entropi/.entropi

# Entry point
ENTRYPOINT ["entropi"]
CMD ["--help"]
```

---

## 2. Development Dockerfile

### File: `docker/Dockerfile.dev`

```dockerfile
# ============================================================================
# Entropi Development Dockerfile
# Includes dev tools and test framework
# ============================================================================

FROM nvidia/cuda:12.4-devel-ubuntu24.04

# Install system dependencies
RUN apt-get update && apt-get install -y \
    python3 \
    python3-pip \
    python3-venv \
    python3-dev \
    build-essential \
    cmake \
    git \
    curl \
    vim \
    && rm -rf /var/lib/apt/lists/*

# Create virtual environment
RUN python3 -m venv /opt/venv
ENV PATH="/opt/venv/bin:$PATH"

# Upgrade pip
RUN pip install --upgrade pip setuptools wheel

# Build llama-cpp-python with CUDA
ENV CMAKE_ARGS="-DGGML_CUDA=on -DCMAKE_CUDA_ARCHITECTURES=89"
RUN pip install llama-cpp-python --no-cache-dir

# Install development dependencies
COPY pyproject.toml /app/
WORKDIR /app
RUN pip install -e ".[dev]" --no-cache-dir

# Copy source
COPY . /app/

# Create non-root user for development
RUN useradd -m -s /bin/bash dev
RUN chown -R dev:dev /app
USER dev

# Volumes
VOLUME /models
VOLUME /workspace

WORKDIR /workspace

# Development environment
ENV ENTROPI_LOG_LEVEL=DEBUG
ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1

# Default command runs tests
CMD ["pytest", "tests/", "-v"]
```

---

## 3. Docker Compose

### File: `docker-compose.yaml`

```yaml
version: "3.8"

services:
  entropi:
    build:
      context: .
      dockerfile: docker/Dockerfile
    image: entropi:latest
    runtime: nvidia
    environment:
      - NVIDIA_VISIBLE_DEVICES=all
      - ENTROPI_MODELS_DIR=/models
    volumes:
      - ${HOME}/models/gguf:/models:ro
      - .:/workspace
    stdin_open: true
    tty: true
    deploy:
      resources:
        reservations:
          devices:
            - driver: nvidia
              count: 1
              capabilities: [gpu]

  entropi-dev:
    build:
      context: .
      dockerfile: docker/Dockerfile.dev
    image: entropi:dev
    runtime: nvidia
    environment:
      - NVIDIA_VISIBLE_DEVICES=all
      - ENTROPI_LOG_LEVEL=DEBUG
    volumes:
      - ${HOME}/models/gguf:/models:ro
      - .:/app
      - .:/workspace
    stdin_open: true
    tty: true
    deploy:
      resources:
        reservations:
          devices:
            - driver: nvidia
              count: 1
              capabilities: [gpu]

  test:
    build:
      context: .
      dockerfile: docker/Dockerfile.dev
    image: entropi:dev
    runtime: nvidia
    environment:
      - NVIDIA_VISIBLE_DEVICES=all
    volumes:
      - ${HOME}/models/gguf:/models:ro
      - .:/app
    command: pytest tests/ -v --cov=src/entropi --cov-report=html
    deploy:
      resources:
        reservations:
          devices:
            - driver: nvidia
              count: 1
              capabilities: [gpu]
```

---

## 4. GitHub Actions CI/CD

### File: `.github/workflows/ci.yaml`

```yaml
name: CI

on:
  push:
    branches: [main, develop]
  pull_request:
    branches: [main]

jobs:
  lint:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: "3.12"

      - name: Install dependencies
        run: |
          pip install ruff black mypy
          pip install -e ".[dev]" --no-deps

      - name: Lint with ruff
        run: ruff check src/ tests/

      - name: Format check with black
        run: black --check src/ tests/

      - name: Type check with mypy
        run: mypy src/

  test-cpu:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: "3.12"

      - name: Install dependencies (CPU only)
        run: |
          pip install llama-cpp-python  # CPU version
          pip install -e ".[dev]"

      - name: Run unit tests
        run: pytest tests/unit/ -v --cov=src/entropi

      - name: Upload coverage
        uses: codecov/codecov-action@v4

  test-gpu:
    runs-on: self-hosted
    if: github.ref == 'refs/heads/main'
    steps:
      - uses: actions/checkout@v4

      - name: Build test container
        run: docker build -f docker/Dockerfile.dev -t entropi:test .

      - name: Run GPU tests
        run: |
          docker run --gpus all \
            -v ${{ secrets.MODELS_PATH }}:/models:ro \
            entropi:test \
            pytest tests/ -v

  build:
    needs: [lint, test-cpu]
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Set up Docker Buildx
        uses: docker/setup-buildx-action@v3

      - name: Login to GitHub Container Registry
        uses: docker/login-action@v3
        with:
          registry: ghcr.io
          username: ${{ github.actor }}
          password: ${{ secrets.GITHUB_TOKEN }}

      - name: Build and push
        uses: docker/build-push-action@v5
        with:
          context: .
          file: docker/Dockerfile
          push: ${{ github.ref == 'refs/heads/main' }}
          tags: |
            ghcr.io/${{ github.repository }}:latest
            ghcr.io/${{ github.repository }}:${{ github.sha }}
          cache-from: type=gha
          cache-to: type=gha,mode=max
```

### File: `.github/workflows/release.yaml`

```yaml
name: Release

on:
  push:
    tags:
      - "v*"

jobs:
  release:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: "3.12"

      - name: Install build tools
        run: pip install build twine

      - name: Build package
        run: python -m build

      - name: Create GitHub Release
        uses: softprops/action-gh-release@v1
        with:
          files: dist/*
          generate_release_notes: true

      - name: Build and push Docker image
        uses: docker/build-push-action@v5
        with:
          context: .
          file: docker/Dockerfile
          push: true
          tags: |
            ghcr.io/${{ github.repository }}:${{ github.ref_name }}
            ghcr.io/${{ github.repository }}:latest
```

---

## 5. Model Download Helper

### File: `src/entropi/cli_download.py`

```python
"""
Model download helper.

Provides convenient model downloading and verification.
"""
import hashlib
import os
from pathlib import Path

import click
import httpx
from rich.console import Console
from rich.progress import Progress, SpinnerColumn, TextColumn, BarColumn

console = Console()

# Model registry - Task-specialized architecture
MODELS = {
    "thinking": {
        "name": "qwen3-14b-q4_k_m",
        "url": "https://huggingface.co/Qwen/Qwen3-14B-GGUF/resolve/main/qwen3-14b-q4_k_m.gguf",
        "size_gb": 8.5,
        "description": "Deep reasoning (thinking mode ON)",
        "sha256": None,
    },
    "normal": {
        "name": "qwen3-8b-q4_k_m",
        "url": "https://huggingface.co/Qwen/Qwen3-8B-GGUF/resolve/main/qwen3-8b-q4_k_m.gguf",
        "size_gb": 4.5,
        "description": "Fast reasoning (thinking mode OFF)",
        "sha256": None,
    },
    "code": {
        "name": "Qwen2.5-Coder-7B-Instruct-Q4_K_M",
        "url": "https://huggingface.co/bartowski/Qwen2.5-Coder-7B-Instruct-GGUF/resolve/main/Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf",
        "size_gb": 4.0,
        "description": "Code generation (always used for code)",
        "sha256": None,
    },
    "micro": {
        "name": "qwen2.5-coder-0.5b-instruct-q8_0",
        "url": "https://huggingface.co/Qwen/Qwen2.5-Coder-0.5B-Instruct-GGUF/resolve/main/qwen2.5-coder-0.5b-instruct-q8_0.gguf",
        "size_gb": 0.5,
        "description": "Routing (always loaded)",
        "sha256": None,
    },
}


@click.command()
@click.argument("model", type=click.Choice(list(MODELS.keys()) + ["all"]))
@click.option(
    "--output-dir",
    "-o",
    type=click.Path(path_type=Path),
    default=Path("~/models/gguf").expanduser(),
    help="Output directory for models",
)
@click.option("--force", "-f", is_flag=True, help="Overwrite existing files")
def download(model: str, output_dir: Path, force: bool) -> None:
    """Download Entropi models."""
    output_dir = output_dir.expanduser()
    output_dir.mkdir(parents=True, exist_ok=True)

    models_to_download = MODELS.keys() if model == "all" else [model]

    for model_key in models_to_download:
        model_info = MODELS[model_key]
        filename = f"{model_info['name']}.gguf"
        output_path = output_dir / filename

        if output_path.exists() and not force:
            console.print(f"[yellow]Skipping {model_key} (already exists)[/yellow]")
            continue

        console.print(f"\n[bold]Downloading {model_key}[/bold] ({model_info['size_gb']} GB)")

        try:
            download_file(model_info["url"], output_path)
            console.print(f"[green]✓ Downloaded to {output_path}[/green]")
        except Exception as e:
            console.print(f"[red]✗ Failed to download {model_key}: {e}[/red]")


def download_file(url: str, output_path: Path) -> None:
    """Download file with progress bar."""
    with httpx.stream("GET", url, follow_redirects=True) as response:
        response.raise_for_status()
        total = int(response.headers.get("content-length", 0))

        with Progress(
            SpinnerColumn(),
            TextColumn("[progress.description]{task.description}"),
            BarColumn(),
            TextColumn("[progress.percentage]{task.percentage:>3.0f}%"),
        ) as progress:
            task = progress.add_task("Downloading...", total=total)

            with open(output_path, "wb") as f:
                for chunk in response.iter_bytes(chunk_size=8192):
                    f.write(chunk)
                    progress.update(task, advance=len(chunk))


if __name__ == "__main__":
    download()
```

---

## 6. Installation Script

### File: `scripts/install.sh`

```bash
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
```

---

## 7. Tests

### File: `tests/integration/test_docker.py`

```python
"""Integration tests for Docker deployment."""
import subprocess
import pytest


@pytest.mark.integration
class TestDocker:
    """Tests for Docker image."""

    def test_image_builds(self) -> None:
        """Test Docker image builds successfully."""
        result = subprocess.run(
            ["docker", "build", "-f", "docker/Dockerfile", "-t", "entropi:test", "."],
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0, f"Build failed: {result.stderr}"

    def test_help_command(self) -> None:
        """Test entropi --help works in container."""
        result = subprocess.run(
            ["docker", "run", "--rm", "entropi:test", "--help"],
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0
        assert "Entropi" in result.stdout

    def test_version_command(self) -> None:
        """Test entropi --version works in container."""
        result = subprocess.run(
            ["docker", "run", "--rm", "entropi:test", "--version"],
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0
```

---

## 8. README

### File: `README.md`

```markdown
# Entropi

> Local AI Coding Assistant powered by Qwen models

Entropi is a terminal-based AI coding assistant that runs fully locally using
quantized Qwen models with CUDA acceleration. It's designed as a local alternative
to cloud-based coding assistants.

## Features

- 🚀 **Multi-model architecture** - Intelligent routing between 14B/7B/1.5B/0.5B models
- 🔧 **MCP tools** - Filesystem, bash, and git integration via Model Context Protocol
- ✨ **Quality enforcement** - Code quality checks at generation time
- 💻 **Rich terminal UI** - Beautiful interface with streaming output
- 🐳 **Docker-first** - Easy deployment with GPU support

## Quick Start

```bash
# Install via Docker
curl -fsSL https://raw.githubusercontent.com/user/entropi/main/scripts/install.sh | bash

# Download models
entropi download all

# Start using
cd your-project
entropi
```

## Requirements

- NVIDIA GPU with 16GB+ VRAM (RTX 4000 series recommended)
- Docker with NVIDIA Container Toolkit
- 50GB disk space for models

## Documentation

- [Architecture](docs/architecture.md)
- [Configuration](docs/configuration.md)
- [MCP Tools](docs/tools.md)
- [Commands](docs/commands.md)

## License

Apache 2.0
```

---

## Checkpoint: Verification

```bash
# Build production image
docker build -f docker/Dockerfile -t entropi:latest .

# Build dev image
docker build -f docker/Dockerfile.dev -t entropi:dev .

# Test production image
docker run --rm entropi:latest --version

# Test with GPU
docker run --rm --gpus all \
  -v ~/models/gguf:/models:ro \
  entropi:latest status

# Run tests in container
docker-compose run test
```

**Success Criteria:**
- [ ] Production Docker image builds
- [ ] Dev Docker image builds
- [ ] `entropi --version` works in container
- [ ] GPU access works in container
- [ ] Tests pass in container
- [ ] CI pipeline passes

---

## Final Integration

At this point, all core components are complete:

1. ✅ Foundation (config, CLI)
2. ✅ Inference Engine (model loading, generation)
3. ✅ MCP Client (tool communication)
4. ✅ MCP Servers (filesystem, bash, git)
5. ✅ Agentic Loop (state machine, execution)
6. ✅ Terminal UI (rich interface)
7. ✅ Storage (SQLite, history)
8. ✅ Commands & Context (slash commands, ENTROPI.md)
9. ✅ Quality Enforcement (analyzers, regeneration)
10. ✅ Docker & Distribution (packaging, CI/CD)

## Chess Game Test

To validate the complete system, use Entropi to create a Chess game:

```bash
# Start Entropi in a new directory
mkdir chess-game && cd chess-game
entropi init
entropi

# In the interactive session:
> Create a complete Chess game in Python with the following:
> - Board representation
> - Move validation
> - Check/checkmate detection
> - Simple ASCII display
> - Game loop for two players
```

This exercises:
- Code generation with quality enforcement
- Multi-file project creation
- Tool usage (filesystem, bash for tests)
- Context management over long session
