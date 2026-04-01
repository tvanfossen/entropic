# Entropic

> Local-first agentic inference engine with identity-based delegation

Entropic is a **C library** (`librentropic.so`) for local AI inference with an
agentic tool-calling loop, identity-based delegation, and MCP tool servers. A
Python wrapper (auto-generated ctypes bindings) and CLI ship alongside it.

The name is a nod to how this actually works. Every handoff — human intent to
prompt, prompt to model, model to model across tiers — is a lossy translation.
Information decays at each boundary. That's the entropic process this engine
manages: structured routing, context management, and tool-augmented reasoning
to lose as little as possible along the way.

## Architecture

```
+-----------------------------------------------------+
|  Consumers                                           |
|  - CLI (entropic command)                            |
|  - Python apps (via wrapper)                         |
|  - C/C++ apps (direct linkage)                       |
|  - entropic-tui (separate package)                   |
+-----------------------------------------------------+
|  Python Wrapper (auto-generated ctypes)              |
|  - EntropicEngine, GenerationResult, AgentState      |
|  - Error handling (entropic_error_t -> exceptions)   |
+-----------------------------------------------------+
|  librentropic.so (C API)                             |
|  - Engine loop     - Config        - Tools           |
|  - Inference       - Prompts       - MCP             |
|  - Delegation      - Hooks         - Storage         |
|  - Context mgmt    - Grammars      - Audit           |
+-----------------------------------------------------+
|  llama.cpp (direct C linkage, CUDA/Vulkan/CPU)       |
+-----------------------------------------------------+
```

## Quick Start

### Prerequisites

- Linux (tested on Ubuntu 24.04)
- NVIDIA GPU with 16GB+ VRAM (or CPU-only for smaller models)
- Python 3.10+

### Install

```bash
# From PyPI (includes librentropic.so + Python wrapper + CLI)
pip install entropic-engine

# Download the primary model (~13.1 GB)
entropic download primary

# Initialize project config
entropic init

# Single-turn inference
entropic ask "What is 2+2?"
```

### Build from Source

```bash
# C engine
cmake -B build -DENTROPIC_CUDA=ON
cmake --build build

# Python wrapper + CLI
pip install -e .
```

## Library Usage

### Python (via wrapper)

```python
from entropic import EntropicEngine

with EntropicEngine(config_path="config.yaml") as engine:
    result = engine.run("What is 2+2?")
    print(result.response)
```

### Streaming

```python
from entropic import EntropicEngine

engine = EntropicEngine(config_path="config.yaml")
engine.run_streaming(
    "Explain quantum computing",
    on_token=lambda tok: print(tok, end="", flush=True),
)
```

### C API

```c
#include <entropic/entropic.h>

entropic_handle_t handle;
entropic_create(&handle);
entropic_configure_from_file(handle, "config.yaml");

char* result;
entropic_run(handle, "What is 2+2?", &result);
printf("%s\n", result);
entropic_free(result);
entropic_destroy(handle);
```

See `examples/` for complete integrations (`hello-world/`, `pychess/`).

## Identity-Based Architecture

A single model serves all roles. Each role (lead, eng, qa, arch, ux, ui,
analyst) has an identity prompt that defines its behavior, allowed tools, and
inference parameters. The lead identity delegates work to other roles via
pipeline and delegation tools.

## Bundled Models

| Key | Model | Size | Purpose |
|-----|-------|------|---------|
| **primary** | Qwen3.5-35B-A3B-UD-IQ3_XXS | 13.1 GB | All roles (single model) |
| **mid** | Qwen3.5-9B-Q8_0 | 9.5 GB | Alternative (12GB+ VRAM) |
| **lightweight** | Qwen3.5-4B-Q8_0 | 4.5 GB | Alternative (8GB+ VRAM) |
| **router** | Qwen3-0.6B-Q8_0 | 0.6 GB | Prompt classification |

## Features

- **Fully Local** — All inference on your hardware via llama.cpp. No API keys.
- **C Library** — `librentropic.so` with pure C API, usable from any language
- **Python Wrapper** — Auto-generated ctypes bindings from `entropic.h`
- **Identity System** — 11 bundled identities with role-specific tools and behavior
- **Delegation** — Lead delegates to specialized roles via pipeline or direct delegation
- **MCP Tool Servers** — Filesystem, bash, diagnostics, git, web as plugin `.so` files
- **Runtime MCP** — External MCP servers via stdio/SSE transport
- **Plugin Architecture** — Custom MCP servers as standalone `.so` plugins
- **CUDA/Vulkan/CPU** — Compile-time inference backend selection
- **Auto-Compaction** — Context summarization for long conversations
- **Streaming** — First-class streaming API with cancel token

## CLI

```bash
entropic ask "question"     # Single-shot question (streaming)
entropic status             # Show engine version and model status
entropic init               # Initialize .entropic/ in current directory
entropic download <model>   # Download model (primary, mid, lightweight, router, all)
entropic setup-cuda         # Build llama-cpp-python with CUDA
entropic benchmark run      # Run inference benchmarks
```

## Configuration

Configuration loads in priority order (highest wins):

1. Compiled defaults
2. Global config (`~/.entropic/config.yaml`)
3. Project config (`.entropic/config.local.yaml`)
4. Environment variables (`ENTROPIC_*`)

Project context is provided via `.entropic/ENTROPIC.md` — a markdown file
describing the project that gets included in the system prompt.

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
