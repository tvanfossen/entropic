# Getting Started

> Quick start guide for Entropic

## Prerequisites

- Ubuntu 24.04 (or compatible Linux)
- NVIDIA GPU with 16GB+ VRAM
- CUDA 12.4+ with nvcc
- Python 3.11+

## Installation

### 1. Clone and Install

```bash
git clone https://github.com/user/entropic.git
cd entropic

# Create virtual environment
python3 -m venv .venv
source .venv/bin/activate

# Install with CUDA support
export CUDACXX=/usr/local/cuda/bin/nvcc
CMAKE_ARGS="-DGGML_CUDA=on" pip install -e ".[dev]"
```

### 2. Download Models

Entropic uses a multi-model architecture. Download the required models:

```bash
mkdir -p ~/models/gguf
cd ~/models/gguf

# THINKING model (Qwen3-14B) - Deep reasoning
huggingface-cli download bartowski/Qwen_Qwen3-14B-GGUF \
  --include "Qwen_Qwen3-14B-Q4_K_M.gguf" \
  --local-dir ~/models/gguf

# NORMAL/CODE model (Falcon H1R 7B) - General and code tasks
huggingface-cli download brittlewis12/Falcon-H1R-7B-GGUF \
  --include "Falcon-H1R-7B-Q8_0.gguf" \
  --local-dir ~/models/gguf

# MICRO model (Qwen3-0.6B) - Task classification
huggingface-cli download bartowski/Qwen3-0.6B-GGUF \
  --include "Qwen3-0.6B-Q8_0.gguf" \
  --local-dir ~/models/gguf
```

### 3. Configure

Create your global configuration:

```bash
mkdir -p ~/.entropic
cat > ~/.entropic/config.yaml << 'EOF'
models:
  thinking:
    path: ~/models/gguf/Qwen_Qwen3-14B-Q4_K_M.gguf
    adapter: qwen3
    context_length: 16384
    gpu_layers: -1
  normal:
    path: ~/models/gguf/Falcon-H1R-7B-Q8_0.gguf
    adapter: falcon
    context_length: 32768
    gpu_layers: -1
  code:
    path: ~/models/gguf/Falcon-H1R-7B-Q8_0.gguf
    adapter: falcon
    context_length: 32768
    gpu_layers: -1
  micro:
    path: ~/models/gguf/Qwen3-0.6B-Q8_0.gguf
    adapter: qwen3
    context_length: 4096
    gpu_layers: -1
  default: normal

routing:
  enabled: true
  fallback_model: normal

log_level: INFO
EOF
```

## First Run

### Interactive Mode

```bash
entropic
```

This starts the interactive terminal UI. Type your questions and Entropic will respond.

### Single Query

```bash
entropic ask "Explain what a binary search tree is"
```

### Initialize a Project

```bash
cd /path/to/your/project
entropic init
```

This creates a `.entropic/` directory with project-specific configuration.

## Basic Commands

| Command | Description |
|---------|-------------|
| `/help` | Show available commands |
| `/clear` | Clear conversation history |
| `/status` | Show model and VRAM status |
| `/think on` | Enable deep reasoning mode (uses 14B model) |
| `/think off` | Disable deep reasoning mode |
| `/exit` | Exit Entropic |

## What Entropic Can Do

- **Read and understand code** - Navigate your codebase, explain functions
- **Write and edit code** - Generate new code, refactor existing code
- **Execute commands** - Run bash commands, tests, build scripts
- **Git operations** - Status, diff, log, commit
- **Search files** - Find files by pattern, search content

## Next Steps

- [Configuration](configuration.md) - Customize Entropic for your workflow
- [Commands](commands.md) - Full command reference
- [Models](models.md) - Understand the model tiers
