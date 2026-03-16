# Getting Started

> Quick start guide for Entropic

## Prerequisites

- Ubuntu 24.04 (or compatible Linux)
- NVIDIA GPU with 16GB+ VRAM (or CPU-only with reduced models)
- CUDA 12.4+ with nvcc
- Python 3.11+

## Installation

### From PyPI

```bash
# TUI application
pipx install entropic-engine[app]

# CUDA support (after install)
PIP_CONFIG_FILE=/dev/null CMAKE_ARGS="-DGGML_CUDA=on" \
  pipx runpip entropic-engine install llama-cpp-python \
  --force-reinstall --no-cache-dir
```

### From Source (Development)

```bash
git clone https://github.com/user/entropic.git
cd entropic

python3 -m venv .venv
source .venv/bin/activate

export CUDACXX=/usr/local/cuda/bin/nvcc
CMAKE_ARGS="-DGGML_CUDA=on" pip install -e ".[dev]"
```

## Download Models

### Recommended: Qwen3.5-35B-A3B (MoE)

Single model for all roles. MoE architecture keeps only 3B parameters active,
fitting comfortably in 16GB VRAM.

```bash
mkdir -p ~/models/gguf
cd ~/models/gguf

# Primary model — all identity roles use this
huggingface-cli download unsloth/Qwen3.5-35B-A3B-GGUF \
  --include "Qwen3.5-35B-A3B-Q4_K_M.gguf" \
  --local-dir ~/models/gguf
```

### Optional: Router Model

Only needed if you enable automatic routing (`routing.enabled: true`):

```bash
huggingface-cli download bartowski/Qwen3-0.6B-GGUF \
  --include "Qwen3-0.6B-Q8_0.gguf" \
  --local-dir ~/models/gguf
```

## Configure

The bundled default config lives at
[`src/entropic/data/default_config.yaml`](../src/entropic/data/default_config.yaml)
and is loaded automatically. To customize, copy it to your global config:

```bash
mkdir -p ~/.entropic
cp src/entropic/data/default_config.yaml ~/.entropic/config.yaml
# Edit paths to match your model download locations
```

Or create a minimal override (only set what differs from the default):

```bash
mkdir -p ~/.entropic
cat > ~/.entropic/config.yaml << 'EOF'
models:
  lead:
    path: ~/models/gguf/Qwen3.5-35B-A3B-Q4_K_M.gguf
EOF
```

See [Configuration](configuration.md) for full options and hierarchy.

## First Run

### Interactive Mode

```bash
entropic
```

Starts the TUI. Type your questions and Entropic responds with the lead identity.

### Single Query

```bash
entropic ask "Explain what a binary search tree is"
```

### Initialize a Project

```bash
cd /path/to/your/project
entropic init
```

Creates `.entropic/` with project-specific configuration.

## Basic Commands

| Command | Description |
|---------|-------------|
| `/help` | Show available commands |
| `/clear` | Clear conversation history |
| `/status` | Show model and VRAM status |
| `/think on` | Enable thinking mode (`<think>` blocks) |
| `/think off` | Disable thinking mode |
| `/exit` | Exit Entropic |

## What Entropic Can Do

- **Read and understand code** — Navigate codebases, explain functions
- **Write and edit code** — Generate, refactor, debug
- **Execute commands** — Run tests, build scripts (scoped by role)
- **Git operations** — Status, diff, log, commit
- **Delegate work** — Lead orchestrates eng, qa, arch roles
- **Multi-stage pipelines** — Chain roles (eng → qa) for implementation + review
- **Search files** — Find files by pattern, search content

## Next Steps

- [Configuration](configuration.md) — Customize for your workflow
- [Commands](commands.md) — Full command reference
- [Models](models.md) — Identity system and model tiers
- [Architecture](architecture.md) — System design overview
- [Library Consumer Guide](library-consumer-guide.md) — Embed Entropic as a library
