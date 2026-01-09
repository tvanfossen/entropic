# Entropi

> Local AI Coding Assistant

Entropi is a terminal-based AI coding assistant that runs fully locally. No API costs, no data leaving your machine.

## Features

- **Fully Local** - All inference runs on your hardware via llama-cpp-python
- **Multi-Model Architecture** - Intelligent routing between 14B/7B/0.6B models
- **Task-Specialized Routing** - MICRO model classifies tasks with GBNF grammar constraints
- **MCP-First** - All tools via standard Model Context Protocol
- **Session Management** - Save, restore, and switch between conversations
- **Auto-Compaction** - Automatic context summarization for long conversations

## Current Model Stack

| Tier | Model | Purpose |
|------|-------|---------|
| THINKING | Qwen3-14B Q4_K_M | Complex reasoning, architecture |
| NORMAL | Falcon-H1R-7B Q8_0 | General tasks |
| CODE | Falcon-H1R-7B Q8_0 | Code generation |
| MICRO | Qwen3-0.6B Q8_0 | Task classification |

## Requirements

- Ubuntu 24.04 (or compatible Linux)
- NVIDIA GPU with 16GB+ VRAM
- CUDA 12.4+
- Python 3.11+

## Quick Start

```bash
# Clone repository
git clone https://github.com/user/entropi.git
cd entropi

# Create virtual environment
python3 -m venv .venv
source .venv/bin/activate

# Install with CUDA support
export CUDACXX=/usr/local/cuda/bin/nvcc
CMAKE_ARGS="-DGGML_CUDA=on" pip install -e ".[dev]"

# Download models (see docs/getting-started.md for details)
entropi download --all

# Run
entropi
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

## Commands

| Command | Description |
|---------|-------------|
| `/help` | Show available commands |
| `/clear` | Clear conversation |
| `/status` | Show model status |
| `/think on/off` | Toggle deep reasoning mode |
| `/sessions` | List sessions |
| `/new [name]` | Create new session |

## Configuration

Configuration hierarchy (lowest to highest priority):

1. Defaults (built into application)
2. Global config (`~/.entropi/config.yaml`)
3. Project config (`.entropi/config.yaml`)
4. Local config (`.entropi/config.local.yaml`)
5. Environment variables (`ENTROPI_*`)
6. CLI arguments

See [docs/configuration.md](docs/configuration.md) for details.

## Documentation

- [Getting Started](docs/getting-started.md)
- [Configuration](docs/configuration.md)
- [Commands](docs/commands.md)
- [Models](docs/models.md)
- [Architecture](docs/architecture.md)
- [MCP Tools](docs/mcp-tools.md)

## License

Apache-2.0
