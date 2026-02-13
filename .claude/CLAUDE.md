# Entropi Project Guidelines

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

### Model Orchestrator (`src/entropi/inference/orchestrator.py`)
- Manages multiple LLM tiers (thinking, normal, code, simple, router)
- Only ONE main tier model loaded at a time (VRAM constraint)
- Dynamic model swapping with lock to prevent TOCTOU races

### MCP Servers (`src/entropi/mcp/servers/`)
- Filesystem, bash, diagnostics servers
- All tools require approval unless explicitly in allow list
- Engine handles user prompting; "Always Allow/Deny" persists to config

### Configuration
- Global defaults: `~/.entropi/config.yaml`
- Project (source of truth): `.entropi/config.local.yaml`
- Project context: `.entropi/ENTROPI.md`
