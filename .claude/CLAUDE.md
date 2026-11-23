# Entropic Project Guidelines

Project-specific guidelines. See global `~/.claude/CLAUDE.md` for universal standards.

## Source of Truth

- **Roadmap**: `docs/roadmap.md` — versioned feature plan, version targeting
- **Architecture**: `docs/architecture-cpp.md` — library decomposition, design rules
- **Per-version proposals + implementation logs**: **GitHub issues** (`gh issue list`, `gh issue view N`) — design notes in the issue body, implementation log in issue comments. Roadmap entries reference `gh#N`.
- **Bug reports + feature pulls from consumers**: also GitHub issues — same surface, single source of truth.
- **Legacy proposal files**: `.claude/proposals/` (pre-2026-05 workflow) — historical only, not maintained in parallel with GitHub issues. Do NOT create new files here. `.old/proposals/` (gitignored) is older archive.

## Git Branching

```
main          <- User handles merges from develop (stable releases)
  └── develop <- Claude merges completed feature branches here
        └── feature/xyz <- Claude creates branches for each task
```

- Claude merges to develop; the user owns the release decision (whether/when to
  ship develop → main)
- **Push requires explicit per-push user approval** — NOT never-push. `git push`
  is allow-listed; for a release Claude may merge develop → main, tag, and push,
  but only after the user gives a clear go-ahead for that specific push (a vague
  "go ahead" is not enough — get explicit approval to push to `main`)
- Version bumps per `docs/roadmap.md` — each feature has an assigned version

## Current State (v2.2.4 — C++ engine on `main`)

C++ engine is the live implementation. Python remains only as an
auto-generated wrapper around `entropic.h` (see `python/entropic_native/`)
plus CLI shims. The v1.7.x Python engine is gone; do not reference it.

## C++ Engine Standards

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
| Minor version | Each x.y.0 bump | Full model/benchmark suite (developer-run, GPU; results attached to the GitHub Release as `model-results-vX.Y.Z.json`) |
| Patch version | Each x.y.z bump | Unit tests only |

Model tests and benchmarks are merged into one suite at v1.10.0.
No model tests in pre-commit — too slow, GPU-dependent.

Model-test output lands under `build/test-reports/` (gitignored via the
generic `build/` rule). The release-time model-results JSON is the
audit record; capture it via the manual release workflow in
`docs/releasing.md`.

## Configuration

- Global: `~/.entropic/config.yaml`
- Project: `.entropic/config.local.yaml`
- Context: `.entropic/ENTROPIC.md`
- Model registry: `data/bundled_models.yaml`
- `path:` resolves bundled model keys (e.g., `primary` → IQ3_XXS path)

## Session Protocol (MANDATORY — every session)

### Before writing ANY code:
1. Read `docs/architecture-cpp.md` — full document
2. Read the **GitHub issue** for the version being implemented — issue body + all comments (`gh issue view N --comments`). The issue is the proposal AND the implementation log.
3. Read ALL interface headers under `include/entropic/interfaces/`
4. Read the design decision log at the bottom of `docs/architecture-cpp.md`
5. If building on a prior version's work, read that version's GitHub issue
   AND validate the actual code matches its contract before building on it

### Before closing any session:
1. If any design decision was made not in the architecture doc,
   append it to the design decision log
2. If any interface header was modified, flag it explicitly in the commit message
   — interface changes are design changes, not implementation details
3. Post an implementation-log comment to the relevant GitHub issue
   (`gh issue comment N`) — what was done, what remains, decisions made,
   files changed. Same structure as the legacy Implementation Log section,
   just posted to the issue.

### Interface headers are immutable once written
The `interfaces/i_*.h` files do not change without a new proposal.
If a session discovers an interface needs modification, it stops and
flags it — that is a design change requiring user approval.

## Legacy Cleanup

Pre-C++ artifacts have been removed. If something stale resurfaces,
the user moves it to root-level `.old/` (gitignored). Claude does not
manage `.old/`.

## clew — Repository Knowledge Graph (MCP)

`clew` is an MCP server exposing a queryable knowledge graph over this
repository: symbols, call edges, threads, locks and critical sections,
dispatch, requirement links, file inventory and prose. It is the maintainer's
own project — **dogfood it, and file issues to its repo when it misbehaves.**

**It enhances the default harness; it does not replace it.** Reading source
after the index has named the file is the intended path, not a failure. A
question about exact comment text or a name spelled differently per consumer is
one the index can point at but not settle.

### When to reach for it first

| Question | Tool |
|---|---|
| Anything about a NAMED thing | `dossier(subject=...)` — takes functions, variables, macros, classes, **locks**, **requirement ids**, threads, Kconfig symbols |
| Don't have the name yet | `search(text, corpus=...)` then hand `kind` verbatim to `dossier` |
| Whole-layer inventory | `search(corpus='locks' / 'threads' / 'files' / 'config')` |
| Index administration | `index(action='status' / 'refresh' / 'targets' / 'cull')` |

`dossier` accepts a **list of up to 8 subjects in ONE call** — prefer that over
one call per symbol. `depth=2..6` walks a bounded causal chain across call and
shared-key edges rather than just adjacency.

`search` matching is a **conjunction** — every whitespace-separated token must
appear in one searchable unit, so use the two most distinctive words.

### Why it fits this repo specifically

- **Requirement traceability.** `docs/requirements.yaml` carries 105 REQ ids
  against ~1634 `@req` tags. `dossier` resolves a requirement id to its
  implementers *and* its covering tests — the query the catalog exists to
  answer. The `clew-guard` hook enforces the catalog (dead `@req`, uncovered
  entries, `guard.coverage.min_req_ratio`); `dossier` explains it.
- **Locks and threads are first-class.** The gh#58 multi-instance family keeps
  recurring; teardown ordering and mutex lifetime are exactly what the index
  holds natively.

### Precedence over auto mode's Bash preference

Auto mode instructs: *"do your work through the Bash tool wherever it can
accomplish the job … fall back to a dedicated tool only when Bash genuinely
cannot do the job."* Read precisely, **that condition does not cover clew's core
queries.** grep returns text matches; it never returns call edges,
requirement→test links, or a lock inventory. Bash genuinely cannot do that job,
so there is no conflict for those questions — but the reminder creates a general
pull toward grep that leaks past its own condition, so the boundary is written
down here rather than re-decided per question.

| Question | Tool |
|---|---|
| A NAMED function / var / macro / class / lock / req / thread | `dossier` |
| "What breaks if I change X" — blast radius | `dossier(depth=2..6, direction='reverse')` |
| Which tests cover a requirement | `dossier(<REQ-id>)` — nothing else answers it |
| Literal text: message strings, comment wording, tag spellings | grep |
| Sweep across many files (every `SKIP(` site, every `.gguf` reference) | grep |
| Non-symbol artifacts: CMake, YAML, whether a file exists on disk | Bash |

A `found:false` is a statement about the **index**, not the repository. Enums are
known-missing (clew#6). One miss is not grounds to stop using the tool for the
rest of a session — fall back for that one question and carry on.

### Operating notes

- Default target is DERIVED from `CLAUDE_PROJECT_DIR`; there is no `set_target`.
  Every tool takes an optional `target` (repo path or slug), and passing it on
  one call does not affect the next.
- Build scope is clew's whole-repo default (`index_scope` is deliberately
  undeclared in `.clew.yaml`). `exclude=[...]` is
  the only narrowing and is **recorded and replayed by every later build** —
  `extern/` is the obvious candidate, since llama.cpp is vendored there.
- Every query reply names the target it answered from and carries a staleness
  block only when there is one. Call `index(action='status')` when an answer
  looks wrong, not routinely.
