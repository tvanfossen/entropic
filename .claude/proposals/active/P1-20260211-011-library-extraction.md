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
updated: 2026-02-17
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

- [ ] `ModelTier` is a base class in `core/base.py` with required `focus`
- [ ] `ModelTier` is subclassable with custom attributes
- [ ] Router is separate from tiers (not a `ModelTier`)
- [ ] `ModelsConfig` accepts arbitrary tier names via dict
- [ ] `RoutingConfig` accepts consumer-defined tier_map and handoff_rules
- [ ] Identity files use YAML frontmatter with `TierIdentity` schema validation
- [ ] Classification prompt auto-generated from `ModelTier.focus`
- [ ] `_extract_focus_points()` removed — replaced by frontmatter schema
- [ ] Grammar auto-generated from tier count
- [ ] Orchestrator derives all routing data from config, not constants
- [ ] `core/logging.py` rich import is lazy
- [ ] Existing YAML config still loads (backward-compatible)
- [ ] All unit + model tests pass

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

- [ ] Custom `backend_factory` is called when provided
- [ ] `use_bundled_prompts=False` raises on missing prompt
- [ ] `EntropyConfig(...)` works without config files
- [ ] All unit + model tests pass

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

- [ ] `pip install -e .` → core + inference (no TUI deps)
- [ ] `pip install -e ".[app]"` → full TUI
- [ ] `import entropi` works without TUI deps
- [ ] `py.typed` present
- [ ] No Docker files in repo
- [ ] All tests pass

## Phase 4: Validation — First Consumer

### 4a. Consumer integration test

Test imports only from `entropi` public API. Custom `ModelTier` subclass, programmatic
config, in-process tools, headless engine, validates classification + inference.

### 4b. Consumer example

Minimal documented example showing custom tiers, programmatic config, custom tools,
headless execution.

### Phase 4 acceptance criteria

- [ ] Integration test passes using only public API imports
- [ ] Custom `ModelTier` subclass with domain focus points
- [ ] No `ConfigLoader`, no bundled prompts, no built-in tier instances
- [ ] Custom in-process tools registered and callable
- [ ] Router classifies to custom tiers
- [ ] Inference output validated from real model

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

### 2026-02-17 — Phase 1 started (feature/library-extraction-phase1)

**1a. Lazy rich import** — COMPLETE
- Moved `from rich.console import Console` and `from rich.logging import RichHandler`
  from module level into `setup_logging()` body in `core/logging.py`
- `get_logger()` / `get_model_logger()` unchanged (pure stdlib)
- Unblocks `import entropi.core.engine` without rich installed

**1b. ModelTier base class** — IN PROGRESS
- Added `ModelTier` class to `core/base.py` with required `focus: Sequence[str]`,
  optional `examples: Sequence[str]`, name/focus/examples properties
- `__eq__` supports both `ModelTier` and `str` comparison
- Hashable by name, `__str__` returns name
