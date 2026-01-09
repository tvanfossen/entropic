# Entropi

Local AI coding assistant powered by Qwen models, running entirely on local hardware.

## Overview

Entropi is a terminal-based coding assistant modeled after Claude Code. It runs fully locally using quantized Qwen models via llama-cpp-python with CUDA acceleration. No API costs, no data leaving your machine.

## Tech Stack

- **Language:** Python 3.12+
- **Inference:** llama-cpp-python (CUDA)
- **Tools:** MCP (Model Context Protocol)
- **UI:** Rich + Prompt Toolkit
- **Storage:** SQLite (aiosqlite)
- **Config:** Pydantic

## Models

Stored in `~/models/gguf/`:

| Slot | Model | Use Case |
|------|-------|----------|
| thinking | Qwen3-14B | Complex reasoning with extended thinking |
| normal | Qwen3-8B | General coding tasks |
| code | Qwen2.5-Coder-7B | Code-focused tasks |
| micro | Qwen2.5-0.5B | Task routing |

## Project Structure

```
src/entropi/
├── config/         # Configuration (Pydantic schemas, loader)
├── core/           # Engine, context, compaction, todos
├── inference/      # Model orchestrator, adapters (Qwen2, Qwen3)
├── mcp/            # MCP client and built-in servers
├── lsp/            # LSP integration (pyright, clangd)
├── storage/        # SQLite persistence
├── ui/             # Terminal UI (Rich components)
└── quality/        # Code quality enforcement
```

## Development

```bash
# Docker (recommended)
./install.sh        # Build and run

# Local development
pip install -e ".[dev]"
entropi             # Interactive mode
entropi ask "..."   # Single query
```

## Key Features

- **Agentic Loop:** Plan → Act → Observe → Repeat cycle
- **Tool Calling:** MCP servers for filesystem, git, bash, diagnostics
- **Auto-Compaction:** Summarizes old context to stay within limits
- **Sessions:** Persistent conversation history per project
- **Todos:** Agentic task tracking displayed in UI

## Architecture Principles

1. Local-first — No external API calls
2. MCP-native — All tools via Model Context Protocol
3. Multi-model — Route tasks to appropriate model size
4. Docker-only distribution — Consistent CUDA environment
