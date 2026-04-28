# Contributing to Entropic

## Prerequisites

- Linux (tested on Ubuntu 24.04)
- cmake 3.21+
- C++20 compiler (gcc 11+ or clang 15+)
- Python 3.10+
- CUDA toolkit (optional, for GPU inference)
- Git with submodule support

## Setup

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/tvanfossen/entropic.git
cd entropic

# Create venv and install dev tools (invoke, pre-commit, gcovr, ruff,
# mypy, pytest — everything the pre-commit gates need).
python3 -m venv .venv
.venv/bin/pip install -e ".[dev]"
.venv/bin/pre-commit install
```

## Building

All builds go through `invoke`. Never run cmake/ctest directly.

```bash
# Full CUDA build (default)
inv build --clean

# CPU-only build (faster, no CUDA required)
inv build --cpu

# Incremental rebuild
inv build --cpu --no-clean
```

### CMake Presets

| Preset | Description | Use case |
|--------|-------------|----------|
| `full` | CUDA shared libs, all servers | TUI developer workstation |
| `dev` | CPU debug, tests | Fast iteration |
| `minimal-static` | CPU static `.a`, minimal servers | Embedded consumer |
| `game` | CUDA, minimal MCP servers | Game engine integration |
| `coverage` | Debug + gcov instrumentation | Coverage analysis |
| `asan-ubsan` | ASan + UBSan | Memory/UB bug hunting |
| `tsan` | ThreadSanitizer | Data race detection |

## Testing

```bash
# CPU unit + regression tests (~750 tests, pre-commit gate)
inv test --cpu --no-build

# Model tests (GPU required, writes results.json)
inv test --model --no-build
```

Unit tests run as a pre-commit gate on every commit. Model tests are
run manually at minor version bumps.

## Code Quality

Pre-commit enforces all quality gates. Always run checks through pre-commit,
never run tools directly:

```bash
# Run all hooks on all files
.venv/bin/pre-commit run --all-files

# Run a specific hook on specific files
.venv/bin/pre-commit run ruff --files src/foo.py
.venv/bin/pre-commit run knots --files src/core/engine.cpp
```

### C/C++ (knots)

| Metric | Threshold |
|--------|-----------|
| Cognitive complexity | 15 |
| McCabe cyclomatic | 15 |
| Nesting depth | 4 |
| SLOC per function | 50 |
| ABC magnitude | 10.0 |
| Max returns | 3 |

### Python (flake8 + ruff)

| Metric | Threshold |
|--------|-----------|
| Cognitive complexity (CCR001) | 15 |
| Max returns (CFQ004) | 3 |
| Max line length | 100 |

### Doxygen

Every C/C++ function requires a Doxygen comment with at minimum `@brief`
and `@version`. The `doxygen-guard` hook enforces this. Functions must also
have one of `@internal`, `@utility`, or `@callback` tags.

## Branch Workflow

```
main          <- Stable releases (user merges from develop)
  └── develop <- Integration branch (merge feature branches here)
        └── feature/vX.Y.Z-description <- Feature work
```

- Create `feature/` branches from `develop`
- Merge completed features into `develop`
- Never push directly to `main`

## Commit Messages

- Concise subject line (imperative mood)
- Reference version in scope: `v2.0.1: Add hello-world C example`
- Mention interface changes explicitly if any `.h` files under
  `include/entropic/interfaces/` were modified

## Proposals

Larger features use the proposal workflow in `.claude/proposals/`:

```
IDEAS/      Rough concepts needing refinement
BACKLOG/    Ready to implement, awaiting capacity
ACTIVE/     Currently being worked on
STAGED/     Complete, awaiting review
COMPLETE/   Finished, archived
```

Each proposal has a unique ID (`P[0-3]-YYYYMMDD-NNN`) and tracks its
implementation progress in an `## Implementation Log` section.

## Architecture

See `docs/architecture-cpp.md` for:
- Library decomposition and dependency graph
- Interface contract patterns (pure C at `.so` boundaries)
- Three-layer class hierarchy (interface, base 80%, impl 20%)
- Plugin architecture for MCP servers
- Build configuration via generated `entropic_config.h`
