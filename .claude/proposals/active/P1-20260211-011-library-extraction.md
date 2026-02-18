---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260211-011
title: "Library Extraction: Embeddable Inference Engine"
priority: P1
component: architecture
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-11
updated: 2026-02-18
completed_phases: [1, 2, 3, 4, 5]
tags: [architecture, packaging, library, refactor, api]
completed_date: null
scoped_files:
  - pyproject.toml
  - src/entropi/__init__.py
  - src/entropi/core/base.py
  - src/entropi/core/logging.py
  - src/entropi/core/engine.py
  - src/entropi/inference/orchestrator.py
  - src/entropi/inference/__init__.py
  - src/entropi/inference/adapters/base.py
  - src/entropi/config/schema.py
  - src/entropi/config/loader.py
  - src/entropi/prompts/__init__.py
  - src/entropi/app.py
  - src/entropi/cli.py
  - src/entropi/data/prompts/identity_*.md
  - tests/
depends_on:
  - P1-20260216-018  # In-process tools (resolved barrier #4)
blocks: []
---

# Library Extraction: Embeddable Inference Engine

## Problem Statement

Entropi's inference engine is tightly packaged with its TUI. The engine has standalone
value (headless agents, CI/CD tools, custom applications) but cannot be consumed
independently because:

- All deps are mandatory — `pip install entropi` pulls textual, rich, llama-cpp-python
- Core abstractions are hardcoded — `ModelTier` is a fixed 5-value enum, `ModelsConfig`
  has 5 named slots, routing/handoff rules are class constants
- Classification prompt + grammar hardcoded to 4 entropi-specific tiers
- No public API surface — `__init__.py` only exports `__version__`

Application #1 is a test/validation agent: headless, custom tools, programmatic config.

**Packaging model:** Single `entropi` package with extras for UI layers only.
`pip install entropi` includes llama-cpp-python (inference is core, not optional).
`pip install entropi[tui]` adds textual/rich/click for the TUI experience.
`pip install entropi[voice]` adds torch/audio deps.

**ModelTier:** Becomes an extensible base class (not enum) with required `focus`
metadata. Focus is enforced via YAML frontmatter schema on identity files — Pydantic
validates that every identity file declares `focus` with at least one entry. This
replaces the fragile `_extract_focus_points()` markdown section parsing with
schema-validated structured data. Identity files remain the single source of truth
for both focus (frontmatter) and system prompt content (markdown body).

**Router:** Handled separately from tiers. Not a `ModelTier`. Dedicated config,
always-loaded behavior, classification-only purpose.

## Phase 1: Core API — ModelTier, Dict Config, Auto-Generated Routing

**Goal:** Replace fixed enum and named config slots with extensible, consumer-defined
tiers. Auto-generate classification prompt and grammar from tier definitions.

### 1a. Fix `rich` module-level import (prerequisite)

**File:** `src/entropi/core/logging.py` (lines 11-12)

Move `from rich.console import Console` and `from rich.logging import RichHandler` into
`setup_logging()` body. `get_logger()` is clean (just `logging.getLogger`) — only the
setup function needs rich. This unblocks `import entropi.core.engine` without rich.

### 1b. `ModelTier` base class

**File:** `src/entropi/core/base.py` (move from orchestrator — it's a shared type)

```python
class ModelTier:
    def __init__(self, name: str, *, focus: Sequence[str], examples: Sequence[str] = ()):
        if not focus:
            raise ValueError(f"ModelTier '{name}' requires at least one focus point")
        self._name = name
        self._focus = tuple(focus)
        self._examples = tuple(examples)
```

**Design rationale:**
- `focus` required in constructor — enforces the routing contract at creation time
- `__eq__` supports both `ModelTier` and `str` comparison — interops with config keys
- Hashable by name — usable as dict keys
- Router is NOT a ModelTier — no identity, no focus, no handoff rules

### 1c. Identity file frontmatter schema

**File:** `src/entropi/prompts/__init__.py`

```python
class TierIdentity(BaseModel):
    name: str
    focus: list[str] = Field(min_length=1)
    examples: list[str] = []
```

`load_tier_identity(path)` → `(TierIdentity, body)`. Body is markdown after frontmatter,
used as system prompt identity. `_extract_focus_points()` removed — replaced by schema.

Identity files get YAML frontmatter. Current hardcoded examples in `classification.md`
move into respective identity files' frontmatter `examples` field.

### 1d. Router separated from tiers

Router is a classification model, not a generation tier:
- `ModelsConfig.router` stays as `ModelConfig | None` (separate field)
- Orchestrator tracks `self._router: ModelBackend | None` separately
- Router always loaded if configured (replaces `AUX_TIERS` constant)
- No `ModelTier` instance for router

### 1e. `TierConfig` + dict-based `ModelsConfig`

**File:** `src/entropi/config/schema.py`

```python
class TierConfig(ModelConfig):
    focus: list[str] = []  # Optional — can come from ModelTier or identity files

class ModelsConfig(BaseModel):
    tiers: dict[str, TierConfig] = {}
    router: ModelConfig | None = None
    default: str = "normal"
```

**Focus resolution** (in orchestrator init):
1. Consumer passes `ModelTier` instances → use `tier.focus` directly
2. Config-only → load identity files, parse frontmatter, create `ModelTier`
3. `TierConfig.focus` non-empty → overrides identity file focus

### 1f. `RoutingConfig` with auto-generation

```python
class RoutingConfig(BaseModel):
    enabled: bool = True
    fallback_tier: str = "normal"
    classification_prompt: str | None = None  # None = auto-generate
    tier_map: dict[str, str] = {}             # Empty = auto-derive
    handoff_rules: dict[str, list[str]] = {}  # Empty = all-to-all
    use_grammar: bool = False                 # Opt-in GBNF constraint
```

`build_classification_prompt(tiers, message, history)` auto-generates from tier focus.
`build_classification_grammar(num_tiers)` generates GBNF dynamically when enabled.
Static `classification.gbnf` removed. `classification.md` template replaced by function.

### 1g. Orchestrator migration

**File:** `src/entropi/inference/orchestrator.py` (~80 references)

- Remove `ModelTier` enum, import base class from `core.base`
- Remove `CLASSIFICATION_MAP`, `MAIN_TIERS`, `AUX_TIERS`, `HANDOFF_RULES`
- New `__init__` with optional `tiers: list[ModelTier] | None` param
- `self._tiers: dict[ModelTier, ModelBackend]` — ModelTier keys
- `self._router: ModelBackend | None` — separate from tiers
- Derive tier_map, handoff_rules from config at init
- `_create_backend` return type widens to `ModelBackend`

### 1h. Engine migration

**File:** `src/entropi/core/engine.py` (~8 direct + ~20 indirect references)

- `_refresh_context_limit()`: dict lookup replaces hardcoded tier-to-config
- `_get_max_context_tokens()`: `tiers.get()` replaces `getattr()`
- `_directive_tier_change()`: tier lookup replaces `ModelTier(string)` enum
- `.value` → `.name` or `str()` throughout
- `LoopContext.locked_tier` typed as `ModelTier | None`

### 1i. Adapter identity prompt path

Update `get_identity_prompt()` to use `load_tier_identity()` internally — returns
constitution + body (markdown after frontmatter, no raw YAML).

### 1j. Config loader backward compat

Translate named-slot YAML format to dict format before constructing `ModelsConfig`.

### 1k. CLI migration

Iterate `config.models.tiers.items()` instead of hardcoded slots. Fix `micro` bug.

### 1l. app.py migration

Replace `.value` accessors with `str()` or `.name`.

### 1m. Test migration

| File | Refs | Change |
|------|------|--------|
| `tests/unit/test_orchestrator_loading.py` | ~59 | Enum → ModelTier instances |
| `tests/unit/test_engine.py` | ~6 | ModelTier instances |
| `tests/model/test_routing.py` | ~16 | ModelTier in assertions |
| `tests/conftest.py` | ~2 | Update import path |
| `tests/unit/test_config.py` | ~5 | Dict-based ModelsConfig |

### Phase 1 acceptance criteria

- [x] `ModelTier` is a base class in `core/base.py` with required `focus`
- [x] `ModelTier` is subclassable with custom attributes
- [x] Router is separate from tiers (not a `ModelTier`)
- [x] `ModelsConfig` accepts arbitrary tier names via dict
- [x] `RoutingConfig` accepts consumer-defined tier_map and handoff_rules
- [x] Identity files use YAML frontmatter with `TierIdentity` schema validation
- [x] Classification prompt auto-generated from `ModelTier.focus`
- [x] `_extract_focus_points()` removed — replaced by frontmatter schema
- [x] Grammar auto-generated from tier count
- [x] Orchestrator derives all routing data from config, not constants
- [x] `core/logging.py` rich import is lazy
- [x] Existing YAML config still loads (backward-compatible)
- [x] All unit + model tests pass

## Phase 2: Extension Points

### 2a. Backend factory injection

```python
BackendFactory = Callable[[ModelConfig, str], ModelBackend]

class ModelOrchestrator:
    def __init__(self, config, tiers=None, backend_factory=None):
        self._backend_factory = backend_factory or self._default_backend_factory
```

### 2b. Prompt fallback control

`use_bundled_prompts: bool = True` in `EntropyConfig`. When `False`, raise
`FileNotFoundError` instead of falling back to entropi's bundled content.

### 2c. Programmatic config

`EntropyConfig(...)` must work without files on disk. `ConfigLoader` stays for CLI only.

### Phase 2 acceptance criteria

- [x] Custom `backend_factory` is called when provided
- [x] `use_bundled_prompts=False` raises on missing prompt
- [x] `EntropyConfig(...)` works without config files
- [x] All unit + model tests pass

## Phase 3: Public Surface + Packaging

### 3a. Public API exports

`src/entropi/__init__.py` exports core types, engine, orchestrator, config, MCP.
Add `src/entropi/py.typed` marker.

### 3b. Extras split in `pyproject.toml`

```toml
[project]
dependencies = [
    "llama-cpp-python>=0.2.0",
    "pydantic>=2.0.0", "pydantic-settings>=2.0.0",
    "pyyaml>=6.0.0", "httpx>=0.25.0", "aiosqlite>=0.19.0",
    "mcp>=1.0.0",
]

[project.optional-dependencies]
tui = ["textual>=0.47.0", "rich>=13.0.0", "click>=8.0.0", "pylspclient>=0.0.7"]
app = ["entropi[tui]"]
all = ["entropi[app,voice]"]
```

### 3c. Lazy imports for TUI deps

Guard `textual`, `rich`, `click` imports. Clear errors: "Install entropi[tui]".

### 3d. Docker removal + native install

Remove Docker files. Rewrite `scripts/install.sh` native-only.

### Phase 3 acceptance criteria

- [x] `pip install -e .` → core + inference (no TUI deps)
- [x] `pip install -e ".[app]"` → full TUI
- [x] `import entropi` works without TUI deps
- [x] `py.typed` present
- [x] No Docker files in repo
- [x] All tests pass

## Phase 4: Validation — First Consumer

### 4a. Consumer integration test

Test imports only from `entropi` public API. Custom `ModelTier` subclass, programmatic
config, in-process tools, headless engine, validates classification + inference.

### 4b. Consumer example: `examples/pychess/`

Chess game using entropi as the AI backend. Validates the full library stack end-to-end:
- Custom `ModelTier` for chess-specific roles
- Custom `BaseMCPServer` with chess tools (`get_board`, `make_move`)
- `python-chess` for board management and move validation
- `AgentEngine` running headlessly with custom everything
- CLI game loop: human vs LLM

This is the primary validation that the library extraction actually works — a real
consumer application using real models, custom tools, and the full agentic loop.

### Phase 4 acceptance criteria

- [x] Integration test passes using only public API imports
- [x] Custom `ModelTier` subclass with domain focus points
- [x] No `ConfigLoader`, no bundled prompts, no built-in tier instances
- [x] Custom in-process tools registered and callable (`examples/pychess/`)
- [x] `AgentEngine` works headlessly with consumer-defined tiers + tools
- [x] Playable chess game against LLM using entropi as backend (manual verification)
- [ ] Dead artifacts cleaned up (`classification.gbnf`, `classification.md`)

## Phase 5: Hardening — BaseTool, Schema Validation, Documentation

### 5a. BaseTool abstraction

`BaseTool` ABC unifies tool definition (JSON) + behavior (execute method) into a single
class. `ToolRegistry` replaces dict-dispatch boilerplate. `BaseMCPServer.register_tool()`
gives consumers a clean way to define tools without overriding `get_tools()`/`execute_tool()`.

### 5b. Schema cross-validation

Pydantic validators on `EntropyConfig` catch consumer config errors at load time:
- `routing.fallback_tier` must reference a defined tier
- `routing.tier_map` values must reference defined tiers
- `routing.handoff_rules` keys/values must reference defined tiers
- `compaction.warning_threshold_percent` must be less than `threshold_percent`

### 5c. Tier auto-build from identity frontmatter

`tier.py` eliminated. Orchestrator auto-builds `ModelTier` objects from config tier names +
identity file frontmatter. Consumers define tiers in config YAML and identity `.md` files
only — no Python code needed.

### 5d. Public API completions

- `ChatAdapter`, `get_adapter`, `register_adapter` — adapter ecosystem
- `TierIdentity`, `load_tier_identity` — identity file API
- `BaseTool`, `load_tool_definition` — tool authoring API

### 5e. Library consumer documentation

`docs/library-consumer-guide.md` — end-to-end setup guide with 10-step process,
API reference table, config requirements, and initialization order diagram.

### Phase 5 acceptance criteria

- [x] `BaseTool` ABC with `ToolRegistry` dispatch
- [x] `BaseMCPServer.register_tool()` backward-compatible (override still works)
- [x] Config cross-validation catches invalid tier references at load time
- [x] Tiers auto-built from identity frontmatter (no `tier.py` needed)
- [x] Full public API surface exported from `entropi`
- [x] Consumer guide documented (`docs/library-consumer-guide.md`)
- [x] All unit + model tests pass

## Files Modified (by phase)

| Phase | Files |
|-------|-------|
| 1 | `core/logging.py`, `core/base.py`, `config/schema.py`, `inference/orchestrator.py`, `prompts/__init__.py`, `core/engine.py`, `config/loader.py`, `cli.py`, `app.py`, `inference/__init__.py`, 4 identity `.md` files, 5 test files |
| 2 | `inference/orchestrator.py`, `config/schema.py`, `prompts/__init__.py` |
| 3 | `__init__.py`, `pyproject.toml`, `scripts/install.sh`, delete `docker/`, lazy import guards |
| 4 | New `tests/integration/test_library_consumer.py`, new example |

## Risks & Mitigations

| Risk | Mitigation |
|---|---|
| ModelTier base class vs enum loses exhaustiveness checks | Pydantic validation on config; runtime checks in orchestrator |
| Breaking existing YAML configs | ConfigLoader translates named-slot YAML to dict format |
| Frontmatter schema migration breaks identity files | Validate all 4 files at load time; clear error messages |
| Classification prompt quality degrades with auto-generation | Model tests validate routing accuracy end-to-end |

## References

- P1-018 — In-process tool execution (completed, resolved barrier #4)
- `src/entropi/core/base.py` — `ToolProvider` Protocol, `ModelBackend` ABC
- `src/entropi/ui/headless.py` — `HeadlessPresenter` (library's presenter impl)
- `src/entropi/ui/presenter.py` — `Presenter` ABC (library's UI contract)

## Implementation Log

### 2026-02-17 — Phases 1-3 complete, Phase 4 partial (feature/library-extraction-phase1)

**Commits:**
- `6df095a` — Phase 1: Extensible ModelTier base class replaces fixed enum
- `a1b68d1` — Phase 2: Backend factory injection, use_bundled_prompts control
- `9f1ce04` — Phase 3: Public API surface, extras split, Docker removal
- `5490f50` — Phase 4: Consumer integration test validates public library API

**Phase 1 — Core API (6df095a)**
- `core/logging.py`: Lazy rich import in `setup_logging()` body only
- `core/base.py`: `ModelTier` base class with required `focus`, `__eq__` str compat, hashable
- `prompts/__init__.py`: `TierIdentity` Pydantic schema, `load_tier_identity()`, auto-generated
  classification prompt/grammar from tier focus. `_extract_focus_points()` removed.
- `config/schema.py`: `TierConfig(ModelConfig)`, dict-based `ModelsConfig.tiers`,
  `RoutingConfig` with `tier_map`, `handoff_rules`, `use_grammar`
- `inference/orchestrator.py`: ~80 references migrated. Enum removed, constants removed.
  Router separated from tiers (`self._router`). Auto-numbered tier_map. All-to-all handoff.
- `core/engine.py`: Dict lookup replaces getattr/hardcoded config access. `.value` → `.name`.
- `config/loader.py`: Named-slot YAML → dict translation for backward compat.
- `cli.py`, `app.py`: Migrated `.value` accessors
- 4 identity files: Added YAML frontmatter with focus + examples
- All unit (442) + model (22) tests passing

**Phase 2 — Extension Points (a1b68d1)**
- `BackendFactory = Callable[[ModelConfig, str], ModelBackend]` type alias
- `ModelOrchestrator.__init__` accepts optional `backend_factory` param
- `use_bundled_prompts: bool = True` on `EntropyConfig`, threaded through
  prompts, adapters, and backend chain
- `EntropyConfig(...)` works standalone without `ConfigLoader` or files

**Phase 3 — Public Surface + Packaging (9f1ce04)**
- `__init__.py`: Full public API exports (core types, engine, config, orchestrator, MCP)
- `py.typed` marker for PEP 561
- `pyproject.toml`: Core deps only in `[project]`, TUI deps moved to `[tui]` extra
- Docker files removed (`docker/Dockerfile`, `docker-compose.yaml`)
- `scripts/install.sh` rewritten for native install with CUDA detection

**Phase 4 — Consumer Validation (5490f50)**
- `tests/integration/test_library_consumer.py`: 13 tests using only public API imports
- Custom `EducationTier(ModelTier)` subclass with `grade_level` metadata
- `MockBackend` + `mock_backend_factory` for testing without GPU
- Validates: imports, custom tiers, programmatic config, orchestrator with custom factory

**Notable fixes during implementation:**
- YAML frontmatter parsing: colons in focus strings parsed as dicts (quoted to fix)
- `.value` → `.name` migration caught 3 stragglers in test files (model conftest, model logger)
- `MockEntropyConfig` needed `use_bundled_prompts` attribute added

### 2026-02-17 — Library API gaps + PyChess example (feature/library-extraction-phase1)

**Commits:**
- `37bb21a` — Library API gaps for consumer support (P1-011 Phase 4)
- `TBD` — PyChess example: multi-tier chess consumer app

**API gaps fixed (37bb21a):**
1. `ServerManager.register_server()` — was missing, consumers couldn't add custom servers
2. `LoopConfig` / `InProcessProvider` — not exported from top-level `entropi` package
3. Identity prompt fallback — crashed when consumer had no identity file for a tier
4. Constitution split — `get_identity_prompt()` coupled constitution + identity loading
5. Config tier validation — `ModelsConfig.validate_default_tier` rejected empty tiers dict
6. `ConfigLoader` export — added to top-level `entropi` package

**ConfigLoader parameterization (this commit):**
- `app_dir_name` param replaces hardcoded `.entropi/` — consumers use their own dir name
- `default_config_path` param — consumers seed from their own defaults, not entropi's
- `global_config_dir=None` — disables global `~/.entropi/` config layer for consumer apps
- ENTROPI.md only created for `.entropi` dirs (not consumer app dirs)
- `_seed_project_config` priority: custom default → global config → package default
- Full backward compatibility — TUI behavior unchanged (all new params have defaults)

**PyChess example (examples/pychess/):**
- Independent venv with `entropi` as a pip dependency
- `.pychess/` runtime directory (not `.entropi/`) via parameterized `ConfigLoader`
- Three tiers: suggest → validate → execute with handoff chain
- Routing enabled — router classifies incoming messages
- Custom `BaseMCPServer` subclass with `get_board` / `make_move` tools
- `allowed_tools` per tier restricts tool visibility (suggest/validate: chess.get_board +
  entropi.handoff, execute: chess.get_board + chess.make_move)
- Config seeded from `data/default_config.yaml` on first run (Qwen3-8B defaults)

**Findings (follow-up items):**
1. **EntropiServer always registered** — `_register_builtin_servers()` unconditionally adds
   `EntropiServer` (todo/handoff tools). Mitigated via `allowed_tools` tier filtering.
   Future: add `enable_entropi` flag to `MCPConfig`.
2. **`_compute_model_context_bytes` uses getattr** — Returns `None` for dict-based tiers
   (doesn't find tier on `ModelsConfig`). No impact when filesystem server is disabled.
   Future: fix to use `config.models.tiers.get()`.

### 2026-02-18 — Phase 5: Library hardening (feature/library-extraction-phase1)

**Commits:**
- `a5ec097` — BaseTool abstraction, logging parameterization, engine enforcement
- `6c006cc` — PyChess example app (three-tier chess consumer)
- `a43eeb5` — Constrain generation windows for rapid-fire chess play
- `3c2fdea` — Eliminate tier.py, auto-build tiers from identity frontmatter
- `TBD` — Schema cross-validation, ChatAdapter export, consumer guide

**BaseTool + ToolRegistry (a5ec097):**
- `src/entropi/mcp/tools.py`: `BaseTool` ABC (definition + execute), `ToolRegistry` (dispatch)
- `BaseMCPServer.register_tool()` — non-abstract defaults delegate to registry
- Backward-compatible — servers overriding `get_tools()`/`execute_tool()` still work
- 18 unit tests for BaseTool, ToolRegistry, and server integration

**PyChess example (6c006cc):**
- Three-tier chess: suggest → validate → execute handoff chain
- Custom `BaseMCPServer` + `BaseTool` implementations
- `allowed_tools` per tier for tool visibility control
- Validated against real model run (Qwen3-8B): routing, handoffs, tool filtering all working

**Tier auto-build (3c2fdea):**
- `tier.py` eliminated — orchestrator reads config tier names + identity file frontmatter
- `orchestrator.tier_names` property for consumer access
- `TierIdentity`, `load_tier_identity` exported from public API

**Schema cross-validation (this session):**
- `EntropyConfig.validate_routing_references()` — fallback_tier, tier_map, handoff_rules
  all cross-validated against `models.tiers`
- `CompactionConfig.validate_threshold_ordering()` — warning < compaction threshold
- 14 new unit tests covering valid configs, invalid references, and edge cases

**Public API completions (this session):**
- `ChatAdapter`, `get_adapter`, `register_adapter` exported
- `TierIdentity`, `load_tier_identity` exported
- `BaseTool`, `load_tool_definition` exported

**Documentation (this session):**
- `docs/library-consumer-guide.md` — 10-step setup process, API reference, config schema,
  initialization order diagram

**Design doc resolution:**
All 9 barriers from `library-api-design.md` resolved:
1. ModelTier extensible ✓  2. Dict-based config ✓  3. Auto-generated classification ✓
4. Config-driven routing ✓  5. In-process tools ✓  6. Backend factory injection ✓
7. Prompt fallback control ✓  8. Parameterized config loading ✓  9. Public API surface ✓
