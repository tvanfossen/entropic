# Entropi

> Local AI Coding Assistant powered by Qwen models and llama-cpp-python

Entropi is a terminal-based AI coding assistant that runs fully locally. No API costs, no data leaving your machine.

## Features

- **Fully Local** — All inference runs on your hardware
- **Multi-Model Architecture** — Intelligent routing between 14B/7B/1.5B/0.5B models
- **Code Quality Enforcement** — Cognitive complexity, typing, docstrings enforced at generation time
- **MCP-First** — All tools via standard Model Context Protocol
- **Self-Maintaining** — Can maintain its own repository

## Requirements

- Ubuntu 24.04 (or compatible Linux)
- NVIDIA GPU with 16GB+ VRAM (RTX PRO 4000 or similar)
- CUDA 12.4+
- Python 3.11+

## Installation

```bash
# Clone repository
git clone https://github.com/user/entropi.git
cd entropi

# Create virtual environment
python3 -m venv .venv
source .venv/bin/activate

# Install with CUDA support
CMAKE_ARGS="-DGGML_CUDA=on" pip install -e ".[dev]"

# Install pre-commit hooks
pre-commit install
```

## Usage

```bash
# Start interactive mode
entropi

# Single question
entropi ask "How do I implement a binary search?"

# Show status
entropi status

# Initialize in a project
entropi init
```

## Configuration

Configuration hierarchy (lowest to highest priority):
1. Defaults (built into application)
2. Global config (`~/.entropi/config.yaml`)
3. Project config (`.entropi/config.yaml`)
4. Local config (`.entropi/config.local.yaml`)
5. Environment variables (`ENTROPI_*`)
6. CLI arguments

## License

Apache-2.0
