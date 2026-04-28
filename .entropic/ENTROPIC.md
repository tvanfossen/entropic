# Project Context — entropic

## Overview

entropic is itself a local-first agentic inference engine. It is a
C++ library with a pure C ABI at the `.so` boundary, distributed as
a shared library (`librentropic.so`) plus an `entropic` CLI binary
plus a thin Python wrapper. Consumers embed the engine via
`find_package(entropic)` from C/C++, via `pip install
entropic-engine` + `entropic install-engine` from Python, or via the
MCP bridge (`entropic mcp-bridge`) for tool-protocol clients like
Claude Code.

When the engine runs against this repo (e.g. `inv example -n
explorer`, or any TUI session pointed at the codebase), this file is
the project context the engine loads alongside the constitution and
app context to ground its responses about what entropic *is*.

## Tech Stack

- **Language**: C++20 with a pure C ABI at every `.so` boundary
  (`include/entropic/*.h` + `include/entropic/interfaces/i_*.h`).
- **Inference backends**: llama.cpp (CUDA, Vulkan, CPU compile-time
  variants) under `src/inference/` + `extern/llama.cpp/`.
- **Tool surface**: MCP (Model Context Protocol) servers in
  `src/mcp/servers/` (filesystem, bash, git, web, diagnostics,
  entropic) plus external MCP plugins via `dlopen` factories.
- **JSON / config / logging**: `nlohmann::json`, `ryml`, `spdlog`,
  all absorbed statically into `librentropic.so` so consumers
  link only system libs (libc, libstdc++, libssl, libsqlite3,
  libz, libgomp, libm).
- **Persistence**: SQLite via `src/storage/`.
- **Tests**: Catch2 v3 for unit tests + model tests
  (`tests/unit/`, `tests/model/`), coverage via gcovr,
  pre-commit-driven gates.

## Structure

```
include/entropic/         Public headers (C ABI + selected C++
                            internal-public). interfaces/i_*.h
                            are immutable without a proposal.
src/                      Implementation:
  core/                   Loop, response generator, validator,
                            delegation, identity, compaction.
  facade/                 entropic.cpp — the C ABI shim that
                            translates external_bridge.cpp +
                            engine_handle.h into the public API.
  mcp/                    Tool executor, server manager, transport
                            (stdio, sse), built-in servers.
  inference/              Backend orchestration and adapters
                            (Qwen3.5, Qwen2.5, etc.).
  prompts/                Prompt manager, identity assembly.
  storage/                SQLite backend.
  types/                  Shared types (logging, hooks,
                            tool_result, validation, error).
  config/                 Layered config loader (~/.entropic →
                            project local → ENTROPIC_* env).
  cli/                    `entropic` CLI binary (mcp-bridge,
                            mcp-connect, download, version).

python/src/entropic/      v2.1.0 wrapper — pure-Python ctypes
                            binding + install-engine subcommand.
                            No OOP wrapper class.

examples/                 Consumer reference apps:
  headless/               Pure-C minimal harness.
  pychess/                C++ multi-tier showcase (delegation,
                            grammar, MCP).
  explorer/               Interactive C++ REPL.
  openai-server/          OpenAI-compat HTTP front-end.

tests/                    unit/ (Catch2 C++), model/ (GPU-required),
                            distribution-smoke-consumer/ (release
                            consumer experience smoke).

data/                     Runtime data: prompts/, schemas/,
                            grammars/, tools/, bundled_models.yaml.

docs/                     architecture-cpp.md (design rules),
                            roadmap.md (version targeting),
                            getting-started.md, releasing.md,
                            contributing.md, security.md,
                            cli-install-routes.md.

build/                    Gitignored. CMake builds, Doxygen
                            output, test-reports, coverage.
extern/                   Vendored deps (llama.cpp, etc.).
.claude/                  Proposals, scheduled tasks, session
                            memory.
```

## Conventions

- **Pure C at every `.so` boundary** — opaque handles, no C++ ABI
  crossing, error codes via enum + callback patterns. No exceptions
  cross the boundary.
- **Three-layer hierarchy** — C interface → concrete base class
  carrying ~80% of logic → implementation specialisation.
- **knots quality gates** on every C/C++ function (locked):
  cognitive complexity ≤ 15, McCabe ≤ 15, nesting ≤ 4, SLOC ≤ 50,
  ABC ≤ 10, returns ≤ 3.
- **doxygen-guard** requires every function to carry a doxygen
  block including `@brief`, an exemption tag (`@utility`,
  `@internal`, `@callback`), and a `@version` that bumps on every
  body change.
- **Pre-commit is the source of truth for hooks** — ruff config,
  flake8 thresholds, knots thresholds, coverage thresholds all
  live in `.pre-commit-config.yaml`. `pyproject.toml` carries only
  setuptools build config.
- **`inv` over scripts** — `tasks.py` is the canonical place for
  build / test / release orchestration. Bash scripts and inline
  python are anti-patterns; prefer `inv <task>`.
- **Branch flow**: feature branch → develop (Claude merges) →
  main (user merges). Releases are locally hand-published via
  `gh release create --target <sha>` — see `docs/releasing.md`.
- **Test gating**:
  - pre-commit: unit tests (CPU, no GPU)
  - x.y.0 minor bumps: full model/benchmark suite (GPU,
    developer-run, results attached as a GitHub Release artifact)
  - x.y.z patch bumps: unit tests only
- **Interface headers are immutable** — `include/entropic/interfaces/i_*.h`
  do not change without a new proposal under
  `.claude/proposals/ACTIVE/`.

## Current target

v2.1.0 release in flight on `feature/repo-structure-cleanup`
(merging to develop next). Bundles:
- A1–A5 engine bug fixes (E1, E2, E5+E6, E8, E9)
- A6 UTF-8 facade sanitization (#47)
- A7–A10 demo asks #41–#44 (iteration exposure, validation
  visibility, assembled-prompt inspection, empty/error tool result
  classification)
- B1–B6 release infrastructure (CI rescope, Python wrapper, OpenAI
  example, GETTING_STARTED, version sync, proposal consolidation)
- C1–C6 pre-release cleanup
- Repo structure cleanup (this branch)

See `.claude/proposals/ACTIVE/v2.1.0-formal-release.md` for the
full implementation log.
