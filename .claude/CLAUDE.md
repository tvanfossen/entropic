# Entropic Project Guidelines

Project-specific guidelines. See global `~/.claude/CLAUDE.md` for universal standards.

## Git Branching Strategy

```
main          <- User handles merges from develop (stable releases)
  └── develop <- Claude merges completed feature branches here
        └── feature/xyz <- Claude creates branches for each task
```

### Workflow
1. **Create feature branch**: `git checkout develop && git checkout -b feature/name`
2. **Commit with hooks**: Pre-commit runs full test suite
3. **Merge to develop**: `git checkout develop && git merge feature/name`
4. **Delete feature branch**: `git branch -d feature/name`
5. **Never push**: User reviews and pushes

### Versioning (`pyproject.toml`)

Bump version in `pyproject.toml` once per merge to develop — not on feature branch commits.

| Change type | Bump | Example |
|-------------|------|---------|
| Bug fix, refactor, docs | **Patch** | 1.0.0 → 1.0.1 |
| New feature, new config field, new API | **Minor** | 1.0.1 → 1.1.0 |
| Breaking change (user explicitly says so) | **Major** | 1.1.0 → 2.0.0 |

Claude bumps patch or minor based on the change. Major is user-initiated only.
Consumer venvs (e.g. `examples/pychess/.venv`) need reinstall after version bumps
unless using editable installs (`pip install -e`).

## Test Structure

```
tests/
├── unit/           # Mocked dependencies, fast, isolated
├── integration/    # Real components, may need setup
└── model/          # Real model inference tests (require GPU)
```

### Model Tests
- Located in `tests/model/`
- Test actual model loading, routing, and inference
- Run as part of pre-commit verification
- Require models to be downloaded and configured

## Project Architecture

### Model Orchestrator (`src/entropic/inference/orchestrator.py`)
- Manages multiple LLM tiers (thinking, normal, code, simple, router)
- Only ONE main tier model loaded at a time (VRAM constraint)
- Dynamic model swapping with lock to prevent TOCTOU races

### MCP Servers (`src/entropic/mcp/servers/`)
- Filesystem, bash, diagnostics servers
- All tools require approval unless explicitly in allow list
- Engine handles user prompting; "Always Allow/Deny" persists to config

### Configuration
- Global defaults: `~/.entropic/config.yaml`
- Project (source of truth): `.entropic/config.local.yaml`
- Project context: `.entropic/ENTROPIC.md`
