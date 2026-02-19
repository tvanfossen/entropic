# Architecture

> System design and component overview

## High-Level Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              ENTROPIC                                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌─────────────┐  │
│  │   Terminal  │    │   Agentic   │    │    MCP      │    │   Storage   │  │
│  │     UI      │◄──►│    Loop     │◄──►│   Client    │◄──►│   (SQLite)  │  │
│  └─────────────┘    └──────┬──────┘    └──────┬──────┘    └─────────────┘  │
│                            │                  │                             │
│                            ▼                  ▼                             │
│                     ┌─────────────┐    ┌─────────────┐                     │
│                     │   Model     │    │    MCP      │                     │
│                     │Orchestrator │    │   Servers   │                     │
│                     └──────┬──────┘    └─────────────┘                     │
│                            │                                                │
│           ┌────────────────┼────────────────┐                              │
│           ▼                ▼                ▼                              │
│    ┌───────────┐    ┌───────────┐    ┌───────────┐                        │
│    │ THINKING  │    │  NORMAL   │    │   MICRO   │                        │
│    │Qwen3-14B  │    │Falcon-7B  │    │Qwen3-0.6B │                        │
│    │(Reasoning)│    │(General)  │    │ (Router)  │                        │
│    └───────────┘    └───────────┘    └───────────┘                        │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
                    ┌───────────────────────────────┐
                    │       llama-cpp-python        │
                    │         (CUDA Backend)        │
                    └───────────────────────────────┘
```

## Components

### Terminal UI (`src/entropic/ui/`)

Rich terminal interface with:
- Streaming output display
- Thinking indicator for `<think>` blocks
- Status bar showing model and VRAM usage
- Prompt history and completion

### Agentic Loop (`src/entropic/core/engine.py`)

The Plan-Act-Observe cycle:

```
1. Receive user message
2. Classify task type (via MICRO model)
3. Route to appropriate model
4. Generate response (streaming)
5. Parse tool calls from output
6. Execute tools via MCP
7. Inject results into context
8. Repeat until complete
```

### Model Orchestrator (`src/entropic/inference/orchestrator.py`)

Manages multiple models:
- Loads/unloads models based on VRAM budget
- Routes tasks to appropriate model tier
- Handles model swapping transparently

### MCP Client (`src/entropic/mcp/`)

Model Context Protocol integration:
- Discovers tools from MCP servers
- Executes tool calls
- Manages server lifecycle

### Storage (`src/entropic/storage/`)

SQLite-based persistence:
- Conversation history
- Session management
- Full-text search

## Data Flow

### Tool Execution

```
Model Output: <tool_call>{"name": "filesystem.read_file", ...}</tool_call>
     │
     ▼
┌─────────────┐
│   Adapter   │  ◄── Parses tool calls from model output
│parse_tool_  │
│  calls()    │
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ MCP Client  │  ◄── Routes to appropriate server
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ MCP Server  │  ◄── Executes tool (filesystem, bash, git)
│(filesystem) │
└──────┬──────┘
       │
       ▼
Tool Result ──► Injected as user message ──► Model continues
```

### Task Routing

```
User Message
     │
     ▼
┌─────────────────┐
│  MICRO Model    │  ◄── Classifies: CODE | REASONING | SIMPLE | COMPLEX
│  (0.6B, GBNF)   │
└────────┬────────┘
         │
    ┌────┴────┐
    │         │
    ▼         ▼
 NORMAL    THINKING     (based on classification and thinking mode)
```

## Key Files

| File | Purpose |
|------|---------|
| `src/entropic/app.py` | Application orchestrator |
| `src/entropic/cli.py` | CLI entry points |
| `src/entropic/core/engine.py` | Agentic loop |
| `src/entropic/inference/orchestrator.py` | Model management |
| `src/entropic/inference/llama_cpp.py` | llama-cpp-python wrapper |
| `src/entropic/inference/adapters/*.py` | Model-specific formatting |
| `src/entropic/mcp/client.py` | MCP client |
| `src/entropic/mcp/servers/*.py` | Built-in MCP servers |
| `src/entropic/storage/__init__.py` | SQLite storage |
| `src/entropic/ui/terminal.py` | Terminal interface |

## Project Structure

```
entropic/
├── src/entropic/
│   ├── __init__.py
│   ├── __main__.py
│   ├── app.py              # Application orchestrator
│   ├── cli.py              # CLI entry point
│   ├── cli_download.py     # Model download commands
│   │
│   ├── config/
│   │   ├── __init__.py
│   │   ├── schema.py       # Pydantic configuration
│   │   └── loader.py       # Configuration loading
│   │
│   ├── core/
│   │   ├── __init__.py
│   │   ├── base.py         # Base classes (Message, ToolCall)
│   │   ├── engine.py       # Agentic loop
│   │   ├── context.py      # Context management
│   │   ├── commands.py     # Slash commands
│   │   ├── compaction.py   # Auto-compaction
│   │   ├── todos.py        # Agentic todos
│   │   └── logging.py      # Logging setup
│   │
│   ├── inference/
│   │   ├── __init__.py
│   │   ├── backend.py      # Backend interface
│   │   ├── llama_cpp.py    # llama-cpp-python wrapper
│   │   ├── orchestrator.py # Multi-model orchestration
│   │   └── adapters/
│   │       ├── __init__.py
│   │       ├── base.py     # Adapter interface
│   │       ├── qwen3.py    # Qwen3 adapter
│   │       └── falcon.py   # Falcon adapter
│   │
│   ├── mcp/
│   │   ├── __init__.py
│   │   ├── client.py       # MCP client
│   │   ├── manager.py      # Server management
│   │   └── servers/
│   │       ├── __init__.py
│   │       ├── filesystem.py
│   │       ├── bash.py
│   │       └── git.py
│   │
│   ├── storage/
│   │   ├── __init__.py     # SQLite storage
│   │   └── session.py      # Session management
│   │
│   ├── ui/
│   │   ├── __init__.py
│   │   ├── terminal.py     # Terminal UI
│   │   └── components.py   # UI components
│   │
│   ├── prompts/
│   │   └── __init__.py     # Prompt loading
│   │
│   └── data/prompts/
│       ├── identity.md     # Core identity prompt
│       └── tool_usage.md   # Tool calling instructions
│
├── docs/                   # Documentation
├── docker/                 # Docker configuration
├── pyproject.toml
└── README.md
```

## Configuration System

Configuration is loaded hierarchically:

```
1. Defaults (code)           ◄── Built-in defaults
2. ~/.entropic/config.yaml    ◄── Global user config
3. .entropic/config.yaml      ◄── Project config
4. .entropic/config.local.yaml◄── Local overrides (gitignored)
5. ENTROPIC_* env vars        ◄── Environment overrides
6. CLI arguments             ◄── Runtime overrides
```

See [Configuration](configuration.md) for details.

## Adapters

Adapters handle model-specific differences:

| Method | Purpose |
|--------|---------|
| `format_system_prompt()` | Build system prompt with tools |
| `parse_tool_calls()` | Extract tool calls from output |
| `format_tool_result()` | Format tool results for context |
| `is_response_complete()` | Detect when response is finished |

All adapters use `<tool_call>` tags as defined in `tool_usage.md` (single source of truth).
