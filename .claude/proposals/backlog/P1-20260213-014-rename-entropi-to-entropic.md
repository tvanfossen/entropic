---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260213-014
title: "Rename entropi to entropic"
priority: P1
component: project-wide
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-13
updated: 2026-02-13
tags: [rename, refactor, branding]
completed_date: null
scoped_files:
  - src/entropi/
  - tests/
  - pyproject.toml
  - docker/Dockerfile
  - docker-compose.yaml
  - install.sh
  - .entropi/
  - .pre-commit-config.yaml
  - README.md
depends_on: []
blocks: []
---

# Rename entropi to entropic

## Problem Statement

Project is named "entropi" but should be "entropic". This is a branding/naming change
that touches the entire codebase. Needs to be done as a single atomic operation to avoid
a half-renamed state.

## Blast Radius

| Category | Count | Files |
|----------|-------|-------|
| Python imports (`from entropi` / `import entropi`) | 379+ | 77 src + 37 test |
| Directory renames | 2 | `src/entropi/` → `src/entropic/`, `data/tools/entropi/` |
| File renames | 1 | `mcp/servers/entropi.py` → `entropic.py` |
| Config files | 4+ | pyproject.toml, YAML configs |
| Logger names | 2 | `entropi` → `entropic`, `entropi.model_output` → `entropic.model_output` |
| CLI entry points | 2 | `entropi` cmd, `entropi-voice-server` cmd |
| Docker/scripts | 15+ | Dockerfile, install.sh, docker-compose.yaml |
| MCP tool name refs | 3+ | `entropi.todo_write`, `entropi.handoff`, `entropi.prune_context` |
| Identity prompts | 4 | All tier identity .md files reference `entropi.*` tools |
| Documentation | 10+ | README, INTEGRATION, docs/, CLAUDE.md |
| Config directories | 2 | `~/.entropi/` → `~/.entropic/`, `.entropi/` → `.entropic/` |

## Approach

### Phase 1: Code Rename (Single Branch)

1. `git mv src/entropi src/entropic` — preserves git history
2. `git mv src/entropic/mcp/servers/entropi.py src/entropic/mcp/servers/entropic.py`
3. `git mv src/entropic/data/tools/entropi src/entropic/data/tools/entropic`
4. Global find-replace `from entropi` → `from entropic` across all .py files
5. Global find-replace `import entropi` → `import entropic` across all .py files
6. Update `pyproject.toml`: package name, entry points, tool config
7. Update logger names in `core/logging.py`
8. Update MCP tool references in identity prompts and configs
9. Update `.pre-commit-config.yaml` paths

### Phase 2: Config Directory Rename

- `.entropi/` → `.entropic/` (project-local)
- `~/.entropi/` → `~/.entropic/` (global config)
- Update all references in code that construct these paths
- Update `.gitignore` entries

### Phase 3: Docker/Scripts/Docs

- Update Dockerfile, install.sh, docker-compose.yaml
- Update README.md, ENTROPI_INTEGRATION.md, docs/
- Update CLI command names if desired (`entropi` → `entropic`)

## Risks

| Risk | Mitigation |
|------|------------|
| Broken imports missed by find-replace | Pre-commit runs full test suite; will catch immediately |
| Config directory rename breaks existing installs | Migration script or backwards-compat symlink |
| Git history fragmented | Use `git mv` for directory moves; single commit per phase |
| MCP tool names change breaks running sessions | Coordinate with config updates; update allow-lists |

## Decision Points

- **CLI command name:** Keep `entropi` for backwards compat, or rename to `entropic`?
- **Config directory migration:** Auto-migrate `~/.entropi/` → `~/.entropic/`, or manual?
- **Timing:** This should be done before any public release. Ideally before library
  extraction (P1-20260211-011) to avoid renaming twice.

## Execution Notes

- Must be done on a **separate worktree or clean branch** — the rename touches every file
- Should be the ONLY change in the branch — no feature work mixed in
- Run full pre-commit (including model tests) after rename to validate
- Single commit per phase, or single monolithic commit if phases aren't independently useful
