---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260309-034
title: "Separate TUI into standalone consumer repo"
priority: P2
component: tui
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-03-09
updated: 2026-03-09
tags: [architecture, tui, packaging]
completed_date: null
scoped_files:
  - src/entropic/ui/
  - src/entropic/voice/
  - src/entropic/app.py
  - src/entropic/cli.py
  - src/entropic/data/prompts/app_context_tui.md
depends_on: []
blocks:
  - P1-20260309-033  # native inference layer benefits from lean engine repo
---

# Separate TUI into Standalone Consumer Repo

## Problem Statement

The TUI (`src/entropic/ui/`, `src/entropic/voice/`, `app.py`, `cli.py`) is architecturally
a consumer of the entropic engine — identical in role to `examples/pychess` and
`examples/hello-world`. But it lives inside the engine repo, which creates problems:

1. **Blurred engine boundary** — `VoiceConfig`, `enable_web`, and TUI-specific config
   fields leak into `EntropyConfig` schema. Third-party consumers inherit config options
   that only the TUI uses.

2. **Dependency contamination** — `textual`, `rich`, `click`, `pylspclient`, `aiosqlite`,
   PersonaPlex deps (`numpy`, `safetensors`, `sounddevice`, etc.) exist in `pyproject.toml`
   extras solely for the TUI/voice. The engine's actual deps are just `llama-cpp-python`,
   `mcp`, `pydantic`, `pyyaml`.

3. **Test coupling** — TUI test sessions, manual tests, and voice tests live alongside
   engine unit/model tests. Hard to distinguish "is the engine broken?" from "is the TUI
   broken?"

4. **Blocks native inference migration** — Moving from `llama-cpp-python` to a direct
   C++ wrapper (P1-033) is cleaner when the engine repo has zero UI surface area.

5. **Release coupling** — TUI bugfix forces engine version bump. Engine refactor forces
   TUI rebuild verification. Independent release cadences are better.

## Proposed Solution

Create a new repo (`entropic-tui` or similar) that depends on `entropic-engine` from PyPI.
The TUI repo mirrors the examples pattern: imports the public API, uses bundled identities
with minimal/no config overrides, owns its own tests.

### What moves OUT of `entropic` repo

| Component | Current location | Notes |
|-----------|-----------------|-------|
| TUI screens | `src/entropic/ui/` (~8 files) | Textual app, widgets, components |
| Voice stack | `src/entropic/voice/` (~8 files) | PersonaPlex controller, audio I/O |
| App orchestrator | `src/entropic/app.py` (~400 lines) | Wires engine + MCP + UI |
| CLI entry point | `src/entropic/cli.py` (~150 lines) | Click-based CLI |
| TUI app context | `src/entropic/data/prompts/app_context_tui.md` | Bundled TUI prompt |
| TUI extras | `pyproject.toml` [tui], [app], [voice], [all] | UI/voice deps |
| Voice server script | `entropic-voice-server` entry point | PersonaPlex subprocess |

### What STAYS in `entropic` repo

| Component | Notes |
|-----------|-------|
| `core/` | Engine loop, directives, subsystems |
| `inference/` | Backend ABC, llama_cpp, orchestrator |
| `mcp/` | All MCP servers (including web, entropic) |
| `prompts/` | Identity library, router, prompt manager |
| `config/` | Schema — stripped of TUI-only fields |
| `benchmark/` | Measurement framework + CLI |
| `storage/` | SQLite backend (session persistence is engine-level) |
| `data/` | Bundled identities, grammars, tool definitions, constitution |
| `examples/` | pychess, hello-world stay as reference consumers |

### New repo structure (entropic-tui)

```
entropic-tui/
├── pyproject.toml          # depends on entropic-engine[app-support] or similar
├── src/entropic_tui/
│   ├── __init__.py
│   ├── app.py              # Application orchestrator (from entropic.app)
│   ├── cli.py              # CLI entry point (from entropic.cli)
│   ├── ui/                 # Textual TUI (from entropic.ui)
│   ├── voice/              # PersonaPlex (from entropic.voice)
│   └── data/
│       └── prompts/
│           └── app_context_tui.md
├── tests/
│   ├── unit/               # TUI-specific tests
│   └── manual/             # Manual TUI test sessions
└── .entropic/
    └── config.local.yaml   # TUI default config
```

### Engine-side cleanup

After extraction, remove from `entropic-engine`:

- `VoiceConfig`, `VoiceServerConfig`, `VoicePromptConfig` from `config/schema.py`
- `enable_web` stays (engine-level server, not TUI-specific)
- `[tui]`, `[app]`, `[voice]`, `[all]` extras groups
- `entropic` and `entropic-voice-server` script entry points
- `app_context_tui.md` from bundled data

Add to `entropic-engine`:

- Optional `[storage]` extra for `aiosqlite` (session persistence is engine-level)
- `httpx` promoted to core dep (web server uses it)

### Public API surface for TUI consumption

The TUI currently imports from these engine modules:

```python
from entropic.config.schema import EntropyConfig
from entropic.core.base import Message
from entropic.core.commands import CommandContext, CommandRegistry
from entropic.core.context import ProjectContext
from entropic.core.engine import AgentEngine, EngineCallbacks, LoopConfig
from entropic.core.queue import MessageQueue, MessageSource, QueuedMessage
from entropic.core.session import SessionManager
from entropic.core.tasks import TaskManager
from entropic.inference.orchestrator import ModelOrchestrator, RoutingResult
from entropic.mcp.manager import ServerManager
from entropic.mcp.servers.external import ExternalMCPServer
from entropic.storage.backend import SQLiteStorage
from entropic.ui.presenter import Presenter, StatusInfo
```

All of these except `Presenter` and `StatusInfo` are engine-level. `Presenter` is the
abstraction boundary — it's the callback interface the engine uses to communicate with
any UI. This should remain in the engine (it's the consumer contract), while concrete
TUI implementations move out.

## Acceptance Criteria

- [ ] New repo builds and installs with `pip install entropic-tui`
- [ ] `entropic` CLI command works from the new package
- [ ] `entropic-engine` has zero UI/voice dependencies in core deps
- [ ] `entropic-engine` pyproject.toml has no TUI extras groups
- [ ] `VoiceConfig` removed from `EntropyConfig`
- [ ] All existing TUI functionality preserved (screens, voice, widgets)
- [ ] TUI repo uses bundled identities — no identity overrides
- [ ] TUI repo has its own test suite
- [ ] Engine repo's existing tests pass unchanged
- [ ] Both repos independently releasable

## Implementation Plan

### Phase 1: Stabilize public API boundary

Before extraction, audit and stabilize the imports that `app.py` uses from the engine.
Ensure `Presenter` / `StatusInfo` / `EngineCallbacks` are clean public interfaces, not
leaking engine internals. This work happens in the engine repo.

### Phase 2: Create new repo + copy

Create `entropic-tui` repo. Copy TUI files, set up pyproject.toml with `entropic-engine`
dependency. Verify it builds and runs against the PyPI package.

### Phase 3: Remove TUI from engine repo

Strip TUI code, voice code, TUI-specific config fields, extras groups. Verify engine
tests pass. Bump engine version (minor — removing optional features).

### Phase 4: Independent CI + release

Set up CI for the TUI repo. Ensure it tests against latest `entropic-engine` release.
Publish `entropic-tui` to PyPI (or keep private, user's call).

## Risks & Considerations

- **Session storage** — `SQLiteStorage` is used by the TUI for session persistence but
  is architecturally engine-level (any consumer might want sessions). It should stay in
  the engine, possibly behind an `[storage]` extra.

- **Presenter interface** — `Presenter` and `StatusInfo` in `ui/presenter.py` define the
  engine→UI callback contract. These must stay in the engine. The TUI implements them.
  Current location (`ui/presenter.py`) is misleading — should move to `core/` before
  extraction.

- **app_context_tui.md** — This is TUI-specific and moves with the TUI. The engine's
  default `app_context` remains `None` (disabled). The TUI passes its own bundled prompt
  via `PromptManager`.

- **Voice config on EntropyConfig** — Currently `voice:` section in config YAML is parsed
  by the engine even though only the TUI reads it. After extraction, voice config lives
  entirely in the TUI repo's own config layer.

- **`entropic` CLI name** — The `entropic` command currently comes from this repo's
  `[project.scripts]`. After extraction, it comes from the TUI repo. Engine repo may
  keep `entropic-benchmark` or similar for the benchmark CLI.

- **Tight iteration during early development** — While the engine API is still evolving
  rapidly, a monorepo is easier. This extraction is best timed after the API stabilizes
  (post-P1-033 native inference layer, post-P1-027 work round completion).

## Implementation Log

{Entries added as work progresses}

## References

- P1-20260309-033: Native inference layer (benefits from lean engine repo)
- P1-20260302-027: Master work round plan (engine stabilization)
- Current engine public API: `core/engine.py`, `core/base.py`, `inference/orchestrator.py`
