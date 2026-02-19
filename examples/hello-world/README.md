# Hello World

Two-tier entropi integration with automatic routing.

Demonstrates the core entropi value proposition: a tiny router model
classifies each prompt and routes it to the right tier automatically.
Simple prompts go to the fast 8B model, complex analysis goes to the
14B thinking model. Only one main model is loaded in VRAM at a time.

## Setup

1. Install entropi: `pip install entropi`
2. Run once to seed config: `python main.py`
3. Edit `.hello-world/config.local.yaml` — set model paths to your GGUFs
4. Run again: `python main.py`

## What it demonstrates

- `ConfigLoader` with consumer overrides (own app dir, no global config)
- `ModelOrchestrator` with two tiers + router model
- Automatic prompt classification and tier routing
- `AgentEngine` with default `ServerManager` (entropi internal tools only)
- VRAM-managed model swapping (one main model at a time)
- Streaming output via `EngineCallbacks`

## Try it

```
You: hello                                    → routed to: normal (fast)
You: design a caching strategy for a REST API → routed to: thinking (deep)
```

## Next steps

- [examples/pychess/](../pychess/) — multi-tier example with custom MCP tools
- [docs/library-consumer-guide.md](../../docs/library-consumer-guide.md) — full API reference
