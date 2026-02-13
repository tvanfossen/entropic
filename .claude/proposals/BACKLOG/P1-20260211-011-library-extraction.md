---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260211-011
title: "Library Extraction: Separate Engine from TUI"
priority: P1
component: architecture
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-11
updated: 2026-02-11
tags: [architecture, packaging, library, refactor]
completed_date: null
scoped_files:
  - pyproject.toml
  - install.sh
  - docker/
  - docker-compose.yaml
  - src/entropi/core/
  - src/entropi/inference/
  - src/entropi/mcp/
  - src/entropi/config/
  - src/entropi/prompts/
  - src/entropi/ui/
  - src/entropi/app.py
  - src/entropi/cli.py
depends_on: []
blocks: []
---

# Library Extraction: Separate Engine from TUI

## Problem Statement

Entropi's inference engine (orchestrator, agentic loop, MCP tools, adapters) is tightly
packaged with the TUI application. The engine has value beyond the TUI — e.g., headless
CI/CD agents, custom local-model tools — but cannot be consumed independently today.

The TUI should become one application built on the engine, not the only way to use it.

## Research Summary

Entropi's architecture already has a clean separation between its inference engine
(orchestrator, adapters, agentic loop, MCP tools) and its TUI — the refactoring cost to
extract a library is minimal. The orchestrator's combination of VRAM-constrained
single-GPU model swapping, semantic tier routing via a lightweight classifier model, and
per-model adapter handling is genuinely novel — no existing framework (LangChain, Ollama,
vLLM, smolagents, or llama.cpp's own Dec 2025 router mode) combines these capabilities,
and upstream contribution to llama.cpp is impractical due to architectural mismatch
(multi-process vs in-process) and a severe review bottleneck (701 open PRs, 2 server
reviewers). llama.cpp remains the correct inference backend (best single-GPU efficiency,
lightest dependencies, GGUF is the dominant format), with the main risk being the
solo-maintained Python bindings — mitigated by the existing adapter abstraction. The
recommended path is a phased approach: Phase 1 restructures the current package with
optional dependency extras, and Phase 2 splits into a uv workspace monorepo only when a
second application materializes.

## Competitive Landscape

### Backend Decision: Stay on llama.cpp

| Backend | Verdict | Why |
|---|---|---|
| **llama.cpp** | **Correct choice** | Best single-GPU efficiency, lightest deps, GGUF dominant format, fine-grained VRAM control |
| vLLM | Wrong architecture | Pre-allocates all VRAM, no room for model swapping. GGUF perf poor (93 vs 140+ tok/s) |
| Ollama | Wrong abstraction | Client-server only, can't embed in-process, black-box VRAM management |
| PyTorch+Transformers | Secondary only | Heavy deps, no grammar support, slow without quant. Good for future multimodal |
| ExLlamaV2 | Too risky | Solo maintainer, NVIDIA-only, niche format |
| mistral.rs | Watch list | Rust engine w/ Python bindings, young but architecturally sound |

### Why Not Upstream to llama.cpp?

- Router mode (Dec 2025) is multi-process, not in-process — architectural mismatch
- Nobody has proposed intelligent routing — zero issues/PRs/discussions
- 701 open PRs, 2 server reviewers — review bottleneck is severe
- Our orchestration logic (adapters + handoff rules + tier routing + VRAM mgmt) is deeply integrated

### Entropi's Unique Value (What Nobody Else Does)

1. **VRAM-constrained intelligent orchestration** — single-GPU, router model classifies prompts automatically
2. **Per-tier adapter system** — model-specific chat templates, tool parsing, `<think>` block handling
3. **In-process Python, not HTTP** — direct GPU memory control, suitable for embedding

## Current Architecture (Already Clean)

| Layer | Coupled to TUI? | Refactoring Needed |
|---|---|---|
| `core/engine.py` (agentic loop) | No | No |
| `inference/` (orchestrator, adapters) | No | No |
| `mcp/` (tool servers) | No | Minor (external.py) |
| `config/` (schema, loader) | No | No |
| `prompts/` (template loader) | No | No |
| `storage/` (conversations) | No | No |
| `ui/presenter.py` (abstraction) | Abstracted | No |
| `app.py` (wiring) | Some | Yes (split concerns) |
| `cli.py` (entry point) | CLI tied to TUI | Minor |

HeadlessPresenter already proves the engine runs without a TUI.

## Current Install Flow (Docker-First, Should Be Native-First)

```
./install.sh → builds Docker image → installs shell wrapper to ~/.local/bin/entropi
               wrapper overwrites pip entry point in .venv/bin/entropi
               `entropi` command → docker run → container → entropi.cli:main
```

Docker was serving as build-step documentation, not as a deployment requirement.

## Proposed Approach

### Phase 1: Native-First + Extras Split (Separate Worktree)

**Branch:** `refactor/native-first` on a new worktree

1. **Restructure `pyproject.toml` with optional extras:**
   ```toml
   [project]
   dependencies = [
       "pydantic>=2.0.0",
       "pydantic-settings>=2.0.0",
       "pyyaml>=6.0.0",
       "httpx>=0.25.0",
       "aiosqlite>=0.19.0",
   ]

   [project.optional-dependencies]
   inference = ["llama-cpp-python>=0.2.0"]
   tui = ["textual>=0.47.0", "rich>=13.0.0", "click>=8.0.0"]
   mcp = ["mcp>=1.0.0", "pylspclient>=0.0.7"]
   voice = [...]
   app = ["entropi[inference,tui,mcp]"]
   all = ["entropi[app,voice]"]
   ```

2. **New `install.sh` — native-only:**
   - Installs system deps (captured from Dockerfile knowledge)
   - `pip install -e .[app]` for full TUI experience
   - Restores pip entry point (`entropi.cli:main` directly, no Docker)

3. **Strip Docker files from repo:**
   - Remove `docker/Dockerfile`, `docker-compose.yaml`
   - Preserve system dep knowledge in install script
   - Preserve CUDA build instructions in install script or `BUILDING.md`

4. **Add lazy imports for optional deps:**
   - `llama-cpp-python`, `textual`, `torch` import only when used
   - `pip install entropi` (core only) doesn't pull heavy deps

5. **Verify end-to-end:** `entropi` command works natively, same UX

### Phase 2: uv Workspace Split (When Second App Materializes)

**Trigger:** When the CI/CD agent repo needs to `import entropi_core`.

```
entropi/
├── pyproject.toml              # workspace root
├── uv.lock
├── packages/
│   ├── entropi-core/           # engine, orchestrator, adapters, MCP, config
│   └── entropi/                # TUI application, CLI, UI
```

- Only split when extras approach feels constraining
- Monorepo, not separate repos — avoids cross-repo coordination tax

### Phase 3: CI/CD Agent (Separate Repo, Owns Docker)

```
entropi-ci-agent/
├── Dockerfile                  # CUDA + entropi core
├── pyproject.toml              # depends on entropi (core)
├── src/
│   ├── custom prompts/
│   ├── custom tool servers (ADO, deploy)
│   └── own tier config
└── HeadlessPresenter-based, runs as daemon/service
```

Docker is an application deployment concern, not a library concern.

## Docker's Role Going Forward

| Scenario | Docker Value |
|---|---|
| Dev machine (TUI) | None — native install |
| CI/CD hosted agent | High — reproducible env, CUDA bundled, restart on crash |
| Team distribution | Moderate — avoids llama-cpp-python build issues |

**Decision:** Docker leaves this repo. Future Docker lives in application repos that need it.

## Risks & Mitigations

| Risk | Mitigation |
|---|---|
| llama-cpp-python solo maintainer (bus factor 1) | Adapter abstraction isolates the binding. Ready to fork or swap. |
| llama.cpp router mode adds intelligent routing | Monitor. Our in-process approach is architecturally different. |
| Extras approach becomes constraining | Phase 2 (uv workspace) is the escape hatch. |
| Breaking native install for current users (just me) | Separate worktree, test thoroughly before merging. |

## Success Criteria

- [ ] `pip install -e .` installs core only (no textual, no torch, no llama-cpp-python)
- [ ] `pip install -e ".[app]"` installs full TUI experience
- [ ] `entropi` command works natively without Docker in the path
- [ ] No Docker files in the repo
- [ ] System dep installation captured in `install.sh`
- [ ] All existing tests pass
