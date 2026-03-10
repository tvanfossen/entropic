# Hello World

Minimal entropic integration — single tier, no routing.

Demonstrates the simplest possible consumer: one model, one identity
(bundled `lead`), streaming output, and app context injection.

## Setup

1. Install entropic: `pip install entropic-engine`
2. Run once to seed config: `python main.py`
3. Edit `.hello-world/config.local.yaml` — set model path to your GGUF
4. Run again: `python main.py`

## What it demonstrates

- `ConfigLoader` with consumer overrides (own app dir, no global config)
- `ModelOrchestrator` with a single tier (no routing, no router model)
- `AgentEngine` with default `ServerManager` (entropic internal tools only)
- Streaming output via `EngineCallbacks`
- App context injection (consumer-specific personality)

## Next steps

- [examples/pychess/](../pychess/) — multi-tier example with custom MCP tools, handoff, and grammars
- [docs/library-consumer-guide.md](../../docs/library-consumer-guide.md) — full API reference
