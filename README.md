# Entropic

> Local-first agentic inference engine with identity-based delegation

This started as "I want to build a local-first Claude Code" — which turned out
to be quite the undertaking. The initial build was a tightly coupled TUI, but it
became clear pretty quickly that I was duplicating the same core inference engine
across other local projects wrapping llama-cpp-python. So it evolved into a
library: the inference engine, model orchestration, agentic loop, and tool
framework are all importable and reusable without dragging in a UI. The TUI ships
alongside it as one consumer, and doubles as a testbed for new ideas.

The name is a nod to how this actually works. Every handoff — human intent to
prompt, prompt to model, model to model across tiers — is a lossy translation.
Information decays at each boundary. That's the entropic process this engine tries
to manage: structured routing, context management, and tool-augmented reasoning to
lose as little as possible along the way.

## Quick Start

### Prerequisites

- Linux (tested on Ubuntu 24.04)
- NVIDIA GPU with 16GB+ VRAM
- Python 3.11+

### NVIDIA Driver & CUDA Toolkit

```bash
# Driver (if not already installed)
sudo apt update
sudo apt install -y nvidia-driver-580
sudo reboot

# CUDA Toolkit 12.8
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt update
sudo apt install -y cuda-toolkit-12-8
echo 'export PATH=/usr/local/cuda/bin:$PATH' >> ~/.bashrc
echo 'export LD_LIBRARY_PATH=/usr/local/cuda/lib64:$LD_LIBRARY_PATH' >> ~/.bashrc
source ~/.bashrc

# Verify
nvidia-smi        # GPU visible, driver 580+
nvcc --version    # CUDA 12.8
```

### System Dependencies

```bash
sudo apt install -y python3-venv python3-pip git cmake build-essential
```

### Install & Run

```bash
# Create project
mkdir ~/Projects/my-project && cd ~/Projects/my-project
python3 -m venv .venv
git init

# Install from PyPI (use PIP_CONFIG_FILE=/dev/null if corporate pip config interferes)
PIP_CONFIG_FILE=/dev/null .venv/bin/pip install entropic-engine[app]

# Build llama.cpp with CUDA support
.venv/bin/entropic setup-cuda

# Download the primary model (~13.1 GB)
.venv/bin/entropic download primary

# Initialize project config
.venv/bin/entropic init

# Run
.venv/bin/entropic
```

### Install Extras

```bash
pip install entropic-engine          # Core library (inference, engine, tools)
pip install entropic-engine[app]     # TUI application (includes tui + storage deps)
pip install entropic-engine[voice]   # Voice interface (PersonaPlex)
pip install entropic-engine[all]     # Everything
```

## Architecture

Entropic is a **library first, application second**. The inference engine
(orchestrator, agentic loop, adapters, tool providers) is fully separable from
any UI.

```
+-----------------------------------------------------+
|  Application Layer (TUI / Headless / Custom)        |
+-----------------------------------------------------+
|  Engine          |  Orchestrator    |  Tools         |
|  - Agentic loop  |  - Tier routing  |  - Filesystem  |
|  - Directives    |  - Model swap    |  - Bash        |
|  - Delegation    |  - VRAM mgmt     |  - Diagnostics |
|  - Context mgmt  |  - Adapters      |  - Git / Web   |
+-----------------------------------------------------+
|  Inference Backend (llama-cpp-python)                |
|  - GGUF models, single-GPU, in-process              |
+-----------------------------------------------------+
```

### Identity-Based Architecture

A single model serves all roles. Each role (lead, eng, qa, arch, ux, ui, analyst)
has an identity prompt that defines its behavior, allowed tools, and inference
parameters. The lead identity delegates work to other roles via pipeline and
delegation tools.

### Bundled Models

Models are defined in `bundled_models.yaml` and downloaded via the CLI.
Default configuration resolves model keys automatically.

| Key | Model | Size | Purpose |
|-----|-------|------|---------|
| **primary** | Qwen3.5-35B-A3B-UD-IQ3_XXS | 13.1 GB | All roles (single model) |
| **mid** | Qwen3.5-9B-Q8_0 | 9.5 GB | Alternative (12GB+ VRAM) |
| **lightweight** | Qwen3.5-4B-Q8_0 | 4.5 GB | Alternative (8GB+ VRAM) |
| **router** | Qwen3-0.6B-Q8_0 | 0.6 GB | Prompt classification |

### Agentic Loop

The engine runs an autonomous tool-calling loop: generate → parse tool calls →
execute tools → feed results back → generate again. The lead identity delegates
implementation tasks to specialized roles (eng, qa, arch) via pipeline or
single-role delegation.

Tools communicate back to the engine via **directives** — structured signals
embedded in tool results that trigger delegation, context anchoring, and
state management.

## Features

- **Fully Local** — All inference on your hardware via llama-cpp-python. No API keys.
- **Library API** — Embed the engine in your own application with `LibraryConfig`
- **Identity System** — 11 bundled identities with role-specific tools and behavior
- **Delegation** — Lead delegates to specialized roles via pipeline or direct delegation
- **Single-GPU Orchestration** — Dynamic model swapping with VRAM-aware loading
- **VRAM Lifecycle** — Three-state model lifecycle (COLD→WARM→ACTIVE)
- **Per-Model Adapters** — Model-specific chat templates, tool parsing, thinking block handling
- **Auto-Compaction** — Context summarization for long conversations
- **MCP Tools** — Filesystem, bash, diagnostics, git, web, and extensible tool servers
- **Runtime MCP** — `.mcp.json` auto-discovered at startup
- **Benchmark CLI** — Performance and quality benchmarks via `entropic benchmark run`
- **Headless Mode** — Full engine without TUI for automation and testing
- **TUI** — Terminal interface built on Textual with streaming and tool approval

## CLI

```bash
entropic                # Interactive TUI
entropic --headless         # Headless mode (automation/testing)
entropic status             # Show model and system status
entropic ask "question"     # Single-shot question
entropic init               # Initialize .entropic/ in current directory
entropic download <model>   # Download model (primary, mid, lightweight, router, all)
entropic setup-cuda         # Build llama-cpp-python with CUDA
entropic benchmark run      # Run performance + quality benchmarks
entropic benchmark list     # List identities with benchmark definitions
entropic mcp-bridge         # Stdio→socket bridge for Claude Code integration
```

## Configuration

Configuration loads in priority order (highest wins):

1. Built-in defaults (references `bundled_models.yaml` keys)
2. Global config (`~/.entropic/config.yaml`)
3. Project config (`.entropic/config.local.yaml`)
4. CLI arguments

The `path` field in tier config accepts either a `bundled_models.yaml` key
(e.g., `primary`) or a direct path to a GGUF file.

Project context is provided via `.entropic/ENTROPIC.md` — a markdown file
describing the project that gets included in the system prompt.

## Library Usage

See `examples/` for complete integrations (`hello-world/`, `pychess/`).

## Privacy

Entropic runs entirely on your local hardware. No data is sent to external
servers. No telemetry is collected. Your prompts, conversations, and model
outputs never leave your machine.

## Disclaimer

Entropic runs AI models locally on your hardware. AI-generated outputs may be
inaccurate, biased, or inappropriate. Users are solely responsible for
evaluating and using any generated content. This software does not provide
professional, legal, medical, or financial advice.

## License

Apache-2.0
