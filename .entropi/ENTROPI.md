# Entropi

Local AI coding assistant running entirely on local hardware.

## Tech Stack

- **Language:** Python 3.12+
- **Inference:** llama-cpp-python (CUDA), PersonaPlex/moshi (voice)
- **Tools:** MCP (Model Context Protocol)
- **UI:** Textual TUI
- **Config:** Pydantic

## Project Structure

```
src/entropi/
├── config/         # Pydantic schemas, config loader
├── core/           # Engine, context, compaction, todos
├── inference/      # Model orchestrator, adapters (Qwen2, Qwen3)
├── mcp/            # MCP client and servers (filesystem, git, bash, diagnostics)
├── lsp/            # LSP integration (pyright, clangd)
├── ui/             # Textual TUI (tui.py, widgets.py, voice_screen.py)
├── voice/          # Voice mode controller (PersonaPlex/moshi integration)
└── storage/        # SQLite persistence
```

## Key Files

- `src/entropi/ui/tui.py` - Main TUI app, keybindings, screens
- `src/entropi/ui/widgets.py` - Chat widgets, StatusFooter, ContextBar
- `src/entropi/ui/voice_screen.py` - Voice mode screen (F5)
- `src/entropi/voice/controller.py` - PersonaPlex model loading
- `src/entropi/core/engine.py` - Agentic loop, tool execution
- `src/entropi/mcp/` - MCP servers (filesystem, git, bash, external)

## Development

```bash
./install.sh        # Build Docker image and run
entropi             # Interactive mode
```

## Conventions

- Use Textual for all UI (not Rich directly)
- MCP for all tool interactions
- Async throughout (asyncio)
- Type hints required
- Docker-only distribution
