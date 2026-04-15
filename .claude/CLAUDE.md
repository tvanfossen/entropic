# Entropic Project Guidelines

Project-specific guidelines. See global `~/.claude/CLAUDE.md` for universal standards.

## Source of Truth

- **Roadmap**: `docs/roadmap.md` — versioned feature plan, version targeting
- **Architecture**: `docs/architecture-cpp.md` — library decomposition, design rules
- **Proposals**: `.claude/proposals/ACTIVE/` — per-version implementation plans
- **Archived**: `.old/proposals/` (gitignored) — absorbed into roadmap, local reference only

## Git Branching

```
main          <- User handles merges from develop (stable releases)
  └── develop <- Claude merges completed feature branches here
        └── feature/xyz <- Claude creates branches for each task
```

- Claude merges to develop, user merges develop → main
- Never push — user reviews and pushes
- Version bumps per `docs/roadmap.md` — each feature has an assigned version

## Current State (v1.7.x — Python)

The Python codebase is the behavioral specification for the C++ rewrite.
v1.7.1 on PyPI is the fallback — frozen, tagged, known working.
Python coding standards apply to any Python that remains (wrapper, CLI).

## Target State (2.0.0 — C++)

### Design Rules (from `docs/architecture-cpp.md`)

1. Pure C at all `.so` boundaries — opaque handles, no C++ ABI crossing
2. Three-layer hierarchy: C interface → concrete base (80%) → implementation (20%)
3. Plugin `.so` for MCP servers — `dlopen`, `entropic_create_server()` factory
4. Inference backends as compile-time variants (CUDA, Vulkan, CPU)
5. Doxygen comments ARE documentation — heavy commenting standard
6. Exceptions do not cross `.so` boundaries — error codes + callbacks
7. Auto-generated Python wrapper from `entropic.h`
8. Config via generated `entropic_config.h` — frontend-swappable (CMake → Kconfig)

### C++ Coding Standards

- Concrete base classes with 80%+ logic (same principle as Python ABCs)
- Every public symbol gets a Doxygen block
- `ENTROPIC_EXPORT` macro on all exported symbols
- No third-party headers in interface contracts
- `std::atomic` for state queries, mutex for transitions only

### Pre-commit Quality Gates (LOCKED — do not modify without explicit user permission)

**C/C++ (knots):**
- Cognitive complexity ≤ 15
- McCabe cyclomatic ≤ 15
- Nesting depth ≤ 4
- SLOC ≤ 50
- ABC magnitude ≤ 10.0
- Returns ≤ 3
- Excludes: `extern/`, vendor code

**Python (flake8):**
- Cognitive complexity ≤ 15 (CCR001)
- Max returns ≤ 3 (CFQ004)

### Test Gating

| Gate | When | What runs |
|------|------|-----------|
| Pre-commit | Every commit | Unit tests (fast, CPU, no GPU) |
| Minor version | Each x.y.0 bump | Full model/benchmark suite (developer-run, GPU, results checked in) |
| Patch version | Each x.y.z bump | Unit tests only |

Model tests and benchmarks are merged into one suite at v1.10.0.
No model tests in pre-commit — too slow, GPU-dependent.

## Configuration

- Global: `~/.entropic/config.yaml`
- Project: `.entropic/config.local.yaml`
- Context: `.entropic/ENTROPIC.md`
- Model registry: `python/entropic/data/bundled_models.yaml`
- `path:` resolves bundled model keys (e.g., `primary` → IQ3_XXS path)

## Session Protocol (MANDATORY — every session)

### Before writing ANY code:
1. Read `docs/architecture-cpp.md` — full document
2. Read the proposal for the version being implemented
3. Read ALL interface headers under `include/entropic/interfaces/`
4. Read the design decision log at the bottom of `docs/architecture-cpp.md`
5. If building on a prior version's work, read that version's proposal
   AND validate the actual code matches its contract before building on it

### Before closing any session:
1. If any design decision was made not in the architecture doc,
   append it to the design decision log
2. If any interface header was modified, flag it explicitly in the commit message
   — interface changes are design changes, not implementation details
3. Update the relevant proposal's implementation log

### Interface headers are immutable once written
The `interfaces/i_*.h` files do not change without a new proposal.
If a session discovers an interface needs modification, it stops and
flags it — that is a design change requiring user approval.

## Legacy Cleanup

v1.7.1 on PyPI is the fallback. No legacy code maintained.
User moves stale files to root-level `.old/` (gitignored) as replacements land.
Claude does not manage `.old/`.

Files to be cleaned up (by user, when replaced):
- `docs/` — all except `roadmap.md` and `architecture-cpp.md`
- `install.sh` — Python-specific, replaced by CMake
- `.dockerignore` — no Docker in roadmap
- `vendor/personaplex/` — moves with TUI at v1.7.2
- `test-manual/` — session artifacts
- `scripts/` — Python-specific (except model test scripts while Python engine lives)
- `src/entropic/` — DELETED in v1.9.15
- `benchmark/results/` — re-run on C++ engine
- `examples/` — rewritten for C API at v1.9.15
