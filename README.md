# Entropic

> Local-first agentic coding engine

Entropic is an agentic inference engine that runs fully local language models with
intelligent tier-based routing. No API costs, no data leaving your machine.

The name reflects the architecture: **entropic decay** — the natural information loss as
intent passes from human brain → model → model → model across tiers. The engine's job
is to minimize that decay through structured routing, context management, and
tool-augmented reasoning.

## Architecture

Entropic is designed as a **library first, application second**. The inference engine
(orchestrator, agentic loop, adapters, tool providers) is separable from any specific
UI. The bundled TUI is one consumer; headless automation, CI/CD agents, and embedded
use cases are equally supported.

```
┌─────────────────────────────────────────────────────┐
│  Application Layer (TUI / Headless / Custom)        │
├─────────────────────────────────────────────────────┤
│  Engine          │  Orchestrator    │  Tools         │
│  - Agentic loop  │  - Tier routing  │  - Filesystem  │
│  - Directives    │  - Model swap    │  - Bash        │
│  - Compaction    │  - VRAM mgmt     │  - Diagnostics │
│  - Context mgmt  │  - Adapters      │  - Todo/State  │
├─────────────────────────────────────────────────────┤
│  Inference Backend (llama-cpp-python)                │
│  - GGUF models, single-GPU, in-process              │
└─────────────────────────────────────────────────────┘
```

### Tier-Based Routing

A lightweight router model classifies each prompt and routes it to the appropriate tier.
Only one main model is loaded at a time (VRAM constraint) — the orchestrator handles
dynamic swapping with lock-protected state transitions.

| Tier | Purpose | Typical Model |
|------|---------|---------------|
| **Thinking** | Complex reasoning, architecture, multi-step analysis | Qwen3-14B Q4_K_M |
| **Normal** | General conversation and tasks | Falcon-H1R-7B Q8_0 |
| **Code** | Code generation, editing, refactoring | Falcon-H1R-7B Q8_0 |
| **Simple** | Greetings, acknowledgments, short responses | (shares normal model) |
| **Router** | Prompt classification only | Qwen3-0.6B Q8_0 |

### Agentic Loop

The engine runs an autonomous tool-calling loop: generate → parse tool calls → execute
tools → feed results back → generate again. The loop continues until the model produces
a complete response or hits the iteration limit.

Tools communicate back to the engine via **directives** — structured signals embedded in
tool results that can trigger tier handoffs, context anchoring, and state management
without the model needing to orchestrate these concerns.

## Features

- **Fully Local** — All inference on your hardware via llama-cpp-python. No API keys.
- **Intelligent Routing** — Sub-second prompt classification routes to the right model tier
- **Single-GPU Orchestration** — Dynamic model swapping with VRAM-aware loading
- **Per-Model Adapters** — Model-specific chat templates, tool parsing, thinking block handling
- **Auto-Compaction** — Context summarization for long conversations
- **MCP Tools** — Filesystem, bash, diagnostics, git, and extensible tool servers
- **Session Management** — Persistent conversations with save/restore
- **Headless Mode** — Full engine without TUI for automation and testing
- **TUI** — Terminal interface built on Textual with streaming, tool approval, voice input

## Requirements

- Linux (tested on Ubuntu 24.04)
- NVIDIA GPU with 16GB+ VRAM
- CUDA 12.4+
- Python 3.11+

## Quick Start

```bash
git clone https://github.com/user/entropi.git
cd entropi

python3 -m venv .venv
source .venv/bin/activate

# Install with CUDA support
export CUDACXX=/usr/local/cuda/bin/nvcc
CMAKE_ARGS="-DGGML_CUDA=on" pip install -e ".[dev]"

# Download models to ~/.entropi/models/
entropi download all

# Run interactive TUI
entropi

# Or headless
entropi --headless
```

## CLI

```bash
entropi                    # Interactive TUI
entropi --headless         # Headless mode (automation/testing)
entropi status             # Show model and system status
entropi ask "question"     # Single-shot question
entropi init               # Initialize .entropi/ in current directory
entropi download <model>   # Download model files
```

## Configuration

Configuration loads in priority order (highest wins):

1. Built-in defaults
2. Global config (`~/.entropi/config.yaml`)
3. Project config (`.entropi/config.local.yaml`)
4. CLI arguments

Project context is provided via `.entropi/ENTROPI.md` — a markdown file describing the
project that gets included in the system prompt.

## License

Apache-2.0
