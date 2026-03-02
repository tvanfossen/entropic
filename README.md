# Entropic

> Local-first agentic inference engine with tier-based model routing

This started as "I want to build a local-first Claude Code" — which turned out
to be quite the undertaking. The initial build was a tightly coupled TUI, but it
became clear pretty quickly that I was duplicating the same core inference engine
across other local projects wrapping llama-cpp-python. So it evolved into a
library: the inference engine, model orchestration, agentic loop, and tool
framework are all importable and reusable without dragging in a UI. The TUI ships
alongside it as one consumer, and doubles as a testbed for new ideas. There's also
a very broken voice interface via PersonaPlex that I'll get to eventually.

The name is a nod to how this actually works. Every handoff — human intent to
prompt, prompt to model, model to model across tiers — is a lossy translation.
Information decays at each boundary. That's the entropic process this engine tries
to manage: structured routing, context management, and tool-augmented reasoning to
lose as little as possible along the way. A bit of a nihilistic naming convention,
but the tier routing and model management do earn their keep in practice. There's
optimization work ahead, but the foundation is solid and I'm always open to new
directions.

## Architecture

Entropic is a **library first, application second**. The inference engine
(orchestrator, agentic loop, adapters, tool providers) is fully separable from
any UI. The bundled TUI is one consumer; headless automation, CI/CD agents, and
custom applications are equally supported.

```
pip install entropic-engine          # Core library (inference, engine, tools)
pip install entropic-engine[app]     # TUI application (includes tui + storage deps)
pip install entropic-engine[voice]   # Voice interface (PersonaPlex)
pip install entropic-engine[all]     # Everything
```

```
+-----------------------------------------------------+
|  Application Layer (TUI / Headless / Custom)        |
+-----------------------------------------------------+
|  Engine          |  Orchestrator    |  Tools         |
|  - Agentic loop  |  - Tier routing  |  - Filesystem  |
|  - Directives    |  - Model swap    |  - Bash        |
|  - Compaction    |  - VRAM mgmt     |  - Diagnostics |
|  - Context mgmt  |  - Adapters      |  - Git / Todo  |
+-----------------------------------------------------+
|  Inference Backend (llama-cpp-python)                |
|  - GGUF models, single-GPU, in-process              |
+-----------------------------------------------------+
```

### Tier-Based Routing

A lightweight router model classifies each prompt and routes to the appropriate
tier. Only one main model is loaded at a time (VRAM constraint) — the
orchestrator handles dynamic swapping with lock-protected state transitions.

| Tier | Purpose | Typical Model |
|------|---------|---------------|
| **Thinking** | Complex reasoning, architecture, multi-step analysis | Qwen3-14B Q4_K_M |
| **Normal** | General conversation and tasks | Falcon-H1R-7B Q8_0 |
| **Code** | Code generation, editing, refactoring | Falcon-H1R-7B Q8_0 |
| **Simple** | Greetings, acknowledgments, short responses | (shares normal model) |
| **Router** | Prompt classification only | Qwen3-0.6B Q8_0 |

### Agentic Loop

The engine runs an autonomous tool-calling loop: generate -> parse tool calls ->
execute tools -> feed results back -> generate again. The loop continues until
the model produces a complete response or hits the iteration limit. Tiers can
auto-chain — when a tier exhausts its token budget without acting, the engine
hands off to the next tier via configurable handoff rules.

Tools communicate back to the engine via **directives** — structured signals
embedded in tool results that can trigger tier handoffs, context anchoring, and
state management without the model needing to orchestrate these concerns.

## Features

- **Fully Local** — All inference on your hardware via llama-cpp-python. No API keys.
- **Library API** — Embed the engine in your own application with `LibraryConfig`
- **Intelligent Routing** — Sub-second prompt classification routes to the right model tier
- **Auto-Chain** — Automatic tier handoff on token exhaustion or grammar completion
- **GBNF Grammar** — Per-tier output constraints via GBNF grammars (streaming and non-streaming)
- **Single-GPU Orchestration** — Dynamic model swapping with VRAM-aware loading
- **Per-Model Adapters** — Model-specific chat templates, tool parsing, thinking block handling
- **Auto-Compaction** — Context summarization for long conversations
- **MCP Tools** — Filesystem, bash, diagnostics, git, and extensible tool servers
- **Headless Mode** — Full engine without TUI for automation and testing
- **TUI** — Terminal interface built on Textual with streaming, tool approval, voice input

## Requirements

- Linux (tested on Ubuntu 22.04, 24.04)
- NVIDIA GPU with 16GB+ VRAM
- CUDA 12.4+
- Python 3.10+

## Installation

### From source (recommended for GPU users)

```bash
git clone https://github.com/tvanfossen/entropic.git
cd entropic
./install.sh          # auto-detects GPU, builds CUDA support
```

The install script creates a virtual environment, clones and builds
llama-cpp-python with CUDA support (if a GPU is detected), and installs
entropic with the `[app]` extras.

```bash
# Place GGUF models in ~/models/gguf/ (or configure paths in .entropic/config.local.yaml)

# Run interactive TUI
.venv/bin/entropic

# Or headless
.venv/bin/entropic --headless
```

### From PyPI

```bash
pip install entropic-engine
entropic setup-cuda   # build llama-cpp-python with CUDA + latest model support
```

### What `setup-cuda` does

- Clones llama-cpp-python v0.3.25 (JamePeng fork — upstream is abandoned)
- Includes llama.cpp with Qwen3.5-MoE and other recent architectures
- Builds with CUDA support (requires nvidia-smi, cmake, CUDA toolkit)
- Installs into the current Python environment
- Cached at ~/.entropic/.build/ — re-run is fast, use `--force` to rebuild

### CPU-only (no GPU)

```bash
pip install entropic-engine
```

Models will run on CPU. Significantly slower but functional.

## CLI

```bash
entropic                    # Interactive TUI
entropic --headless         # Headless mode (automation/testing)
entropic status             # Show model and system status
entropic ask "question"     # Single-shot question
entropic init               # Initialize .entropic/ in current directory
entropic download <model>   # Download model files
entropic setup-cuda         # Build llama-cpp-python with CUDA
```

## Configuration

Configuration loads in priority order (highest wins):

1. Built-in defaults
2. Global config (`~/.entropic/config.yaml`)
3. Project config (`.entropic/config.local.yaml`)
4. CLI arguments

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
