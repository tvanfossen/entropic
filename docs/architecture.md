# Architecture

> System design and component overview

## High-Level Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              ENTROPIC ENGINE                                │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌─────────────┐  │
│  │  Presenter  │    │   Agentic   │    │    MCP      │    │   Storage   │  │
│  │  (UI/API)   │◄──►│    Loop     │◄──►│   Client    │◄──►│   (SQLite)  │  │
│  └─────────────┘    └──────┬──────┘    └──────┬──────┘    └─────────────┘  │
│                            │                  │                             │
│                    ┌───────┴───────┐          ▼                             │
│                    │  Delegation   │    ┌─────────────┐                     │
│                    │   Manager     │    │    MCP      │                     │
│                    └───────┬───────┘    │   Servers   │                     │
│                            │           └─────────────┘                     │
│                            ▼                                                │
│                     ┌─────────────┐                                        │
│                     │   Model     │                                        │
│                     │Orchestrator │                                        │
│                     └──────┬──────┘                                        │
│                            │                                                │
│                     ┌──────┴──────┐                                        │
│                     │  Identity   │  ◄── 11 bundled roles (lead, eng,      │
│                     │  Prompts    │      qa, arch, ux, ui, analyst,        │
│                     └──────┬──────┘      devops, compactor, scribe,        │
│                            │             benchmark_judge)                   │
│                            ▼                                                │
│                     ┌─────────────┐                                        │
│                     │    Model    │  ◄── Single model, one tier active     │
│                     │   (GGUF)   │      at a time (VRAM constraint)        │
│                     └─────────────┘                                        │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
                    ┌───────────────────────────────┐
                    │       llama-cpp-python        │
                    │     (CUDA / Vulkan / CPU)     │
                    └───────────────────────────────┘
```

## Core Concepts

### Identity-Based Architecture (v1.6.0+)

Entropic uses **identity prompts** (personas) instead of task-type routing. Each
identity defines a role with specific focus areas, tool access, and inference
parameters.

```
                    ┌──────────┐
                    │   lead   │  ◄── Orchestrates, delegates, plans
                    └────┬─────┘
                         │ delegates to
          ┌──────────┬───┴───┬──────────┐
          ▼          ▼       ▼          ▼
      ┌───────┐ ┌───────┐ ┌──────┐ ┌────────┐
      │  eng  │ │  qa   │ │ arch │ │ others │
      └───┬───┘ └───┬───┘ └──────┘ └────────┘
          │         │
     auto_chain  auto_chain
          │         │
          └────┬────┘
               ▼
           back to lead
```

**Role types:**
- **Front office** (lead, eng, qa, arch, ux, ui, analyst, devops): User-facing, `auto_chain: lead`
- **Back office** (compactor, scribe): Engine-invoked, no auto_chain
- **Utility** (benchmark_judge): Assessment, no auto_chain

### Delegation System

Delegation spawns **child inference loops** with isolated context:

1. Lead calls `entropic.delegate(target="eng", task="...")`
2. Engine creates child `LoopContext` locked to target tier
3. Child runs to completion (or `entropic.complete`)
4. Result returns to parent as tool result
5. Lead processes the result

Pipelines chain stages: `entropic.pipeline(stages=["eng", "qa"], task="...")`

### Phase Transitions

Roles can define multiple **phases** with different inference parameters and
bash command access. Example: QA has `design` (no bash, thinking enabled) and
`execute` (bash for test runners, lower temperature).

```yaml
phases:
  default:           # Analysis phase
    bash_commands: null
    enable_thinking: true
  execute:           # Test execution phase
    bash_commands: [pytest, npm test, ...]
    enable_thinking: false
```

Transition via `entropic.phase_change(phase="execute")`.

## Components

### Presenter Interface (`src/entropic/ui/presenter.py`)

Abstract presenter that decouples the engine from display concerns:
- TUI presenter (`tui_presenter.py`) — Textual-based terminal UI
- Headless presenter (`headless.py`) — For programmatic/test use
- Consumers implement their own presenter

### Agentic Loop (`src/entropic/core/engine.py`)

The Plan-Act-Observe cycle with directive-driven control flow:

```
1. Receive user message
2. Lock tier (route or pre-locked for delegation)
3. Generate response (streaming, phase-aware)
4. Parse tool calls from output
5. Execute tools via MCP (with approval, dedup, bash scoping)
6. Process directives from tool results
7. Inject results into context
8. Evaluate: complete, auto_chain, or continue
```

Engine subsystems (extracted for maintainability):
- `response_generator.py` — Model invocation, tier routing, pause/injection
- `tool_executor.py` — Tool execution, approval, dedup, bash scoping
- `context_manager.py` — Compaction, context budget, overflow recovery
- `delegation.py` — Child loop creation and execution
- `worktree.py` — Git worktree isolation for delegated tasks

### Model Orchestrator (`src/entropic/inference/orchestrator.py`)

Manages model lifecycle:
- Single model loaded at a time (VRAM constraint on 16GB)
- Dynamic model swapping with lock (prevents TOCTOU races)
- Identity prompt assembly per tier
- Adapter selection per model family

### Directive System (`src/entropic/core/directives.py`)

Typed directives control engine behavior from tool results:

| Directive | Effect |
|-----------|--------|
| `StopProcessing` | Stop processing remaining tool calls |
| `Delegate` | Spawn child inference loop |
| `Pipeline` | Sequential multi-stage delegation |
| `Complete` | Signal explicit task completion |
| `PhaseChange` | Switch active phase within role |
| `TierChange` | Internal tier switch (auto_chain) |
| `PruneMessages` | Reduce message history |
| `InjectContext` | Add context message |
| `ContextAnchor` | Persistent context block |
| `NotifyPresenter` | Send data to UI layer |
| `ClearSelfTodos` | Clear self-directed todos before delegation |

### MCP Servers (`src/entropic/mcp/servers/`)

Built-in servers providing tool capabilities:

| Server | Tools |
|--------|-------|
| `filesystem` | read_file, write_file, edit_file, list_directory, glob, grep |
| `bash` | execute (scoped by `bash_commands` allowlist) |
| `git` | status, diff, log, add, commit, branch, checkout, reset |
| `entropic` | todo_write, delegate, pipeline, complete, phase_change, prune_context |
| `diagnostics` | status, check_errors |
| `web` | search, fetch |
| `external` | Runtime MCP via `.mcp.json` auto-discovery |

### Storage (`src/entropic/storage/`)

SQLite-based persistence:
- Conversation history with message attribution
- Delegation records (parent/child relationships)
- Session management
- Full-text search

## Key Files

| File | Purpose |
|------|---------|
| `src/entropic/app.py` | Application orchestrator (wires engine + presenter) |
| `src/entropic/cli.py` | CLI entry points |
| `src/entropic/core/engine.py` | Agentic loop orchestration |
| `src/entropic/core/engine_types.py` | LoopContext, LoopConfig, AgentState, callbacks |
| `src/entropic/core/tool_executor.py` | Tool execution, approval, dedup |
| `src/entropic/core/response_generator.py` | Model invocation, streaming, phases |
| `src/entropic/core/context_manager.py` | Compaction, context budget |
| `src/entropic/core/delegation.py` | Child loop delegation |
| `src/entropic/core/directives.py` | Directive types and processor |
| `src/entropic/core/worktree.py` | Git worktree isolation |
| `src/entropic/core/todos.py` | Agentic todo list |
| `src/entropic/inference/orchestrator.py` | Model management and routing |
| `src/entropic/inference/llama_cpp.py` | llama-cpp-python wrapper |
| `src/entropic/inference/adapters/*.py` | Model-specific formatting |
| `src/entropic/prompts/__init__.py` | Frontmatter schemas (Identity, Phase, etc.) |
| `src/entropic/prompts/manager.py` | Prompt loading and assembly |
| `src/entropic/mcp/servers/*.py` | Built-in MCP servers |
| `src/entropic/storage/backend.py` | SQLite storage implementation |
| `src/entropic/ui/tui.py` | Textual TUI application |
| `src/entropic/ui/presenter.py` | Abstract presenter interface |

## Project Structure

```
entropic/
├── src/entropic/
│   ├── app.py              # Application orchestrator
│   ├── cli.py              # CLI entry point
│   ├── cli_download.py     # Model download commands
│   │
│   ├── config/
│   │   ├── schema.py       # Pydantic configuration (ModelConfig, TierConfig, etc.)
│   │   └── loader.py       # Configuration loading + hierarchy
│   │
│   ├── core/
│   │   ├── base.py         # Base classes (Message, ToolCall, ModelTier)
│   │   ├── engine.py       # Agentic loop
│   │   ├── engine_types.py # LoopContext, LoopConfig, AgentState, callbacks
│   │   ├── tool_executor.py # Tool execution subsystem
│   │   ├── response_generator.py # Generation subsystem
│   │   ├── context_manager.py # Compaction subsystem
│   │   ├── delegation.py   # Child loop delegation
│   │   ├── directives.py   # Directive types and processor
│   │   ├── worktree.py     # Git worktree isolation
│   │   ├── todos.py        # Agentic todos
│   │   ├── commands.py     # Slash commands
│   │   ├── compaction.py   # Auto-compaction logic
│   │   ├── context.py      # Context utilities
│   │   ├── parser.py       # Tool call parsing
│   │   └── logging.py      # Logging setup
│   │
│   ├── inference/
│   │   ├── backend.py      # Backend interface
│   │   ├── llama_cpp.py    # llama-cpp-python wrapper
│   │   ├── orchestrator.py # Model management + routing
│   │   └── adapters/
│   │       ├── base.py     # Adapter interface
│   │       ├── qwen3.py    # Qwen3 adapter
│   │       ├── qwen35.py   # Qwen3.5 adapter (35B-A3B MoE)
│   │       ├── qwen2.py    # Qwen2/2.5 adapter
│   │       ├── falcon.py   # Falcon adapter
│   │       ├── smollm3.py  # SmolLM3 adapter
│   │       └── router.py   # Router adapter (classification)
│   │
│   ├── mcp/
│   │   ├── client.py       # MCP client
│   │   ├── manager.py      # Server lifecycle management
│   │   ├── tools.py        # BaseTool + ToolRegistry
│   │   └── servers/
│   │       ├── base.py     # BaseMCPServer
│   │       ├── filesystem.py
│   │       ├── bash.py
│   │       ├── git.py
│   │       ├── entropic.py # Internal tools (delegate, pipeline, etc.)
│   │       ├── diagnostics.py
│   │       ├── web.py
│   │       ├── external.py # Runtime MCP discovery
│   │       └── file_tracker.py
│   │
│   ├── prompts/
│   │   ├── __init__.py     # Frontmatter schemas
│   │   └── manager.py      # Prompt loading + assembly
│   │
│   ├── storage/
│   │   ├── backend.py      # SQLite storage
│   │   ├── database.py     # Database migrations
│   │   ├── models.py       # Data models
│   │   └── session.py      # Session management
│   │
│   ├── ui/
│   │   ├── presenter.py    # Abstract presenter interface
│   │   ├── tui.py          # Textual TUI application
│   │   ├── tui_presenter.py # TUI presenter wrapper
│   │   ├── headless.py     # Headless presenter
│   │   ├── widgets.py      # TUI widgets
│   │   ├── components.py   # UI components
│   │   └── themes.py       # Theme configuration
│   │
│   └── data/
│       ├── prompts/
│       │   ├── constitution.md     # Core behavioral constitution
│       │   ├── tool_usage.md       # Tool calling instructions
│       │   └── identity_*.md       # 11 role identity prompts
│       ├── tools/                  # Tool JSON schemas
│       └── grammars/               # GBNF grammar files
│
├── examples/
│   ├── pychess/            # Chess game consumer example
│   └── hello-world/        # Minimal consumer example
│
├── tests/
│   ├── unit/               # 875+ unit tests
│   ├── integration/        # Integration tests
│   └── model/              # Model inference tests (require GPU)
│
├── docs/                   # Documentation
└── pyproject.toml
```

## Configuration System

Configuration is loaded hierarchically:

```
1. Defaults (code)            ◄── Built-in defaults
2. ~/.entropic/config.yaml    ◄── Global user config
3. .entropic/config.yaml      ◄── Project config
4. .entropic/config.local.yaml◄── Local overrides (gitignored)
5. ENTROPIC_* env vars        ◄── Environment overrides
6. CLI arguments              ◄── Runtime overrides
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

| Adapter | Models | Features |
|---------|--------|----------|
| `qwen35` | Qwen3.5-35B-A3B MoE | `<think>` blocks, `<tool_call>` tags |
| `qwen3` | Qwen3-* | `<think>` blocks, `<tool_call>` tags |
| `qwen2` | Qwen2.5-* | `<tool_call>` tags |
| `falcon` | Falcon-H1R-* | `<think>` blocks, `<tool_call>` tags |
| `smollm3` | SmolLM3-* | `<tool_call>` tags |
| `router` | Classification models | Raw text continuation, no chat template |
| `generic` | Any | Basic ChatML |
