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
pip install entropic          # Core library (inference, engine, tools)
pip install entropic[app]     # TUI application (includes tui + storage deps)
pip install entropic[voice]   # Voice interface (PersonaPlex)
pip install entropic[all]     # Everything
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
the model produces a complete response or hits the iteration limit.

Tools communicate back to the engine via **directives** — structured signals
embedded in tool results that can trigger tier handoffs, context anchoring, and
state management without the model needing to orchestrate these concerns.

## Features

- **Fully Local** — All inference on your hardware via llama-cpp-python. No API keys.
- **Library API** — Embed the engine in your own application with `LibraryConfig`
- **Intelligent Routing** — Sub-second prompt classification routes to the right model tier
- **Single-GPU Orchestration** — Dynamic model swapping with VRAM-aware loading
- **Per-Model Adapters** — Model-specific chat templates, tool parsing, thinking block handling
- **Auto-Compaction** — Context summarization for long conversations
- **MCP Tools** — Filesystem, bash, diagnostics, git, and extensible tool servers
- **Headless Mode** — Full engine without TUI for automation and testing
- **TUI** — Terminal interface built on Textual with streaming, tool approval, voice input

## Requirements

- Linux (tested on Ubuntu 24.04)
- NVIDIA GPU with 16GB+ VRAM
- CUDA 12.4+
- Python 3.11+

## Quick Start

```bash
git clone https://github.com/tvanfossen/entropic.git
cd entropic
./install.sh app
```

The install script creates a virtual environment, detects CUDA, and installs
with the `[app]` extras (TUI + storage dependencies).

```bash
# Place GGUF models in ~/models/gguf/ (or configure paths in .entropic/config.local.yaml)

# Run interactive TUI
.venv/bin/entropic

# Or headless
.venv/bin/entropic --headless
```

## CLI

```bash
entropic                    # Interactive TUI
entropic --headless         # Headless mode (automation/testing)
entropic status             # Show model and system status
entropic ask "question"     # Single-shot question
entropic init               # Initialize .entropic/ in current directory
entropic download <model>   # Download model files
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

```python
from entropic import LibraryConfig, Orchestrator, Engine, ServerManager

config = LibraryConfig(
    config_dir=Path("~/.myapp").expanduser(),
    tiers={"normal": {"path": "model.gguf", "adapter": "qwen3"}},
)

orchestrator = Orchestrator(config.to_app_config())
await orchestrator.initialize()

server_manager = ServerManager(config.to_app_config())
await server_manager.initialize()

engine = Engine(orchestrator=orchestrator, server_manager=server_manager)
async for message in engine.run("Hello"):
    print(message.content)
```

See `examples/hello-world/` and `examples/pychess/` for complete integrations.

## License

Apache-2.0
