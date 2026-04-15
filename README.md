# Entropic

> Local-first agentic inference engine — your models, your hardware, your control

## What Is Entropic?

Entropic is a **C inference engine** that turns a local GGUF model into a
multi-tier, tool-calling AI system. It runs entirely on your hardware — no
cloud, no API keys, no telemetry. You control the model, the prompts, the
tools, and the data.

The name comes from information theory: every handoff between human intent,
prompt, and model is a lossy translation. Information decays at each boundary.
Entropic is purpose-built to manage that decay — structured context management,
identity-based delegation, grammar-constrained output, and tool-augmented
reasoning minimize what gets lost along the way.

## Why Entropic?

**You want local AI that actually does things**, not just answers questions.

Most local inference tools give you a model and a chat loop. Entropic gives you
an **engine** — the infrastructure between your application and the model that
handles the hard parts:

| Problem | How Entropic Solves It |
|---------|----------------------|
| Model just generates text | Agentic loop: generate → parse tool calls → execute → re-generate |
| One model, one personality | Identity system: same model serves multiple roles with different prompts, tools, and constraints |
| Context gets stale | Auto-compaction: summarizes old context to stay within window |
| Output format unpredictable | Grammar constraints: GBNF grammars force structured output |
| Need tools but no cloud | MCP tool servers: filesystem, bash, git, web — all local, plugin architecture |
| Privacy concerns | Zero network calls. Everything stays on your machine. |

## Who Is It For?

Entropic is an engine, not an application. It's for developers building
AI-powered software that needs to run locally:

- **CLI/TUI tools** that use AI for code generation, analysis, or planning
- **Game engines** that need NPC dialogue or decision-making from a local model
- **Embedded systems** with on-device inference (CPU-only static build available)
- **Education platforms** running student-facing AI without cloud dependencies
- **Privacy-sensitive applications** where data cannot leave the device

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Your Application                                       │
│  C/C++ (direct linkage) · Python (auto-generated wrapper)│
├─────────────────────────────────────────────────────────┤
│  librentropic.so — C API                                │
│                                                         │
│  ┌─────────┐ ┌────────┐ ┌──────┐ ┌───────┐ ┌────────┐ │
│  │ Engine  │ │Inference│ │ MCP  │ │Config │ │Storage │ │
│  │ Loop    │ │Backend │ │Servers│ │Loader │ │Backend │ │
│  │         │ │        │ │      │ │       │ │        │ │
│  │ Context │ │ Prompt │ │ Tool │ │ YAML  │ │ SQLite │ │
│  │ Routing │ │ Cache  │ │ Auth │ │ Layer │ │ Audit  │ │
│  │ Delegate│ │ Grammar│ │Plugin│ │ Merge │ │ Session│ │
│  └─────────┘ └────────┘ └──────┘ └───────┘ └────────┘ │
├─────────────────────────────────────────────────────────┤
│  llama.cpp (CUDA / Vulkan / CPU)                        │
└─────────────────────────────────────────────────────────┘
```

Pure C at all `.so` boundaries. No C++ ABI crossing. Any language that can call
C functions can use the engine.

## Quick Start

### Prerequisites

- Linux (tested on Ubuntu 24.04)
- cmake 3.21+, C++20 compiler
- NVIDIA GPU with 16GB+ VRAM (or CPU-only for smaller models)
- Python 3.10+ (for wrapper and build tools)

### Build

```bash
git clone --recurse-submodules https://github.com/tvanfossen/entropic.git
cd entropic

python3 -m venv .venv
.venv/bin/pip install -e .
.venv/bin/pip install invoke pre-commit
.venv/bin/pre-commit install

# CUDA build (default)
inv build --clean

# CPU-only build
inv build --cpu
```

### Download a Model

```bash
entropic download primary    # Qwen3.5-35B-A3B (13.1 GB, recommended)
entropic download mid        # Qwen3.5-9B (9.5 GB, 12GB+ VRAM)
entropic download lightweight # Qwen3.5-4B (4.5 GB, 8GB+ VRAM)
```

### Run an Example

```bash
inv example -n hello-world          # Minimal streaming chat (C)
inv example -n hello-world --wrapper # Same, Python wrapper
inv example -n pychess              # Multi-tier chess (C++)
inv example -n pychess --wrapper    # Same, Python wrapper
```

## Usage

### C API

```c
#include <entropic/entropic.h>

entropic_handle_t h = NULL;
entropic_create(&h);
entropic_configure_dir(h, ".myapp");  // Layered config resolution

// Streaming generation with full engine pipeline
entropic_run_streaming(h, "What is 2+2?", on_token, NULL, NULL);

// Conversation persists across calls
entropic_run_streaming(h, "Explain your reasoning", on_token, NULL, NULL);

// Manage conversation
size_t count;
entropic_context_count(h, &count);   // Message count
entropic_context_clear(h);           // New session

entropic_destroy(h);
```

### Python Wrapper

```python
from entropic import EntropicEngine

engine = EntropicEngine()
engine.configure_dir(".myapp")

# Streaming
engine.run_streaming(
    "What is 2+2?",
    on_token=lambda tok: print(tok, end="", flush=True),
)

# Multi-turn
engine.run_streaming("Explain your reasoning", on_token=print)

# Context management
print(engine.context_count())
engine.context_clear()

engine.destroy()
```

### Configuration

Configuration loads in layers (highest priority wins):

1. **Compiled defaults** — struct initializers built into the engine
2. **Consumer defaults** (`default_config.yaml` in CWD) — shipped with your app
3. **Global** (`~/.entropic/config.yaml`) — user machine-wide settings
4. **Project local** (`{project_dir}/config.local.yaml`) — per-project overrides
5. **Environment** (`ENTROPIC_*` variables)

Minimal config:

```yaml
models:
  lead:
    path: primary           # Resolves via bundled model registry
    adapter: qwen35
    context_length: 16384
  default: lead

routing:
  enabled: false

mcp:
  enable_entropic: true     # Internal tools (delegate, complete, etc.)
  enable_filesystem: false
  enable_bash: false

permissions:
  auto_approve: true        # Skip tool approval prompts
```

### Session Logging

When using `entropic_configure_dir()`, the engine automatically creates:

| File | Contents |
|------|----------|
| `{project_dir}/session.log` | Engine operations — config, routing, timing, errors |
| `{project_dir}/session_model.log` | Full user/assistant exchanges — streamed in real time |

## Key Concepts

### Identity System

A single model serves multiple roles. Each **identity** defines:
- System prompt (who the model is, how it behaves)
- Allowed tools (what it can do)
- Grammar constraints (structured output format)
- Delegation targets (who it can hand off to)

The engine switches identities during delegation — same model, different behavior.

### Multi-Tier Delegation

Tiers are named identities with their own tools and constraints. A parent tier
can delegate work to a child tier:

```
Lead (analyze request)
  → delegate to Eng (write code)
    → delegate to QA (review code)
  ← results flow back up
```

Each delegation creates a child context with the target tier's identity. The
child runs the full engine loop independently and returns a result summary.

### Grammar Constraints (GBNF)

GBNF grammars force the model to produce structurally valid output. The engine
applies grammars per-tier during generation:

```gbnf
root ::= analysis best-move tool-call
analysis ::= "Analysis: " sentence "\n"
best-move ::= "Best move: " uci "\n"
tool-call ::= "<tool_call>" json "</tool_call>"
```

The model can only produce tokens that match the grammar. No post-processing,
no retries — the output is structurally correct by construction.

### MCP Tool Servers

Tools are provided by MCP (Model Context Protocol) servers. Built-in servers
ship as plugins:

| Server | Tools |
|--------|-------|
| `entropic` | delegate, pipeline, complete, diagnose, inspect |
| `filesystem` | read_file, write_file, edit_file, glob, grep |
| `bash` | execute |
| `git` | status, diff, log, commit, branch, checkout |
| `diagnostics` | diagnostics, check_errors |
| `web` | web_fetch, web_search |

External MCP servers connect at runtime via stdio or SSE transport:

```c
entropic_register_mcp_server(h, "chess",
    "{\"command\":\"python3\",\"args\":[\"chess_server.py\"]}");
```

### Prompt Cache

The engine caches the KV state of the system prompt prefix. On multi-turn
conversations, only new tokens need processing — the system prompt is restored
from cache in milliseconds instead of re-processed.

## Bundled Models

| Key | Model | Size | VRAM |
|-----|-------|------|------|
| `primary` | Qwen3.5-35B-A3B-UD-IQ3_XXS | 13.1 GB | 15+ GB |
| `mid` | Qwen3.5-9B-Q8_0 | 9.5 GB | 12+ GB |
| `lightweight` | Qwen3.5-4B-Q8_0 | 4.5 GB | 8+ GB |
| `router` | Qwen3-0.6B-Q8_0 | 0.6 GB | 1 GB |

Use `path: primary` (or `mid`, `lightweight`) in config — the engine resolves
to the full model path via the bundled registry.

## Build Presets

| Preset | Description | Use Case |
|--------|-------------|----------|
| `full` | CUDA, all servers, tests | Development workstation |
| `dev` | CPU, debug, tests | Fast iteration |
| `minimal-static` | CPU, static `.a`, minimal servers | Embedded consumer |
| `game` | CUDA, minimal MCP servers | Game engine integration |
| `coverage` | CPU, gcov instrumentation | Coverage analysis |

```bash
inv build --clean              # full (CUDA)
inv build --cpu                # dev (CPU)
inv test --cpu --no-build      # 673 unit + regression tests
inv test --model --no-build    # 29 model tests (GPU required)
```

## Examples

| Example | Demonstrates | Language |
|---------|-------------|----------|
| `hello-world/main.c` | Single tier, streaming, app context | C |
| `hello-world/main_wrapper.py` | Same via Python wrapper | Python |
| `pychess/main.cpp` | Multi-tier pipeline, grammar, delegation, external MCP | C++ |
| `pychess/main_wrapper.py` | Same via Python wrapper | Python |

Each example has its own `default_config.yaml` (consumer defaults) and
`.{name}/` directory (session logs, local config).

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

LGPL-3.0 (v2.0.0+; prior releases were Apache-2.0)
