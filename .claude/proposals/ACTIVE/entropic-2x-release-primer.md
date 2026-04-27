# Entropic 2.x Release Primer

> Working document for a Claude Code session. Target repo: `tvanfossen/entropic`.
> Audience: Claude Code executing via `gh` CLI and local edits.
> Goal: close repo metadata gaps, fix documentation drift, and reconcile CI config against current reality before tagging v2.0.6.
> Scope: GitHub repository updates only. No engine code changes. No new architectural proposals.

---

## 0. Context

- **Current CMake version**: `2.0.6` (`CMakeLists.txt`)
- **Current pyproject version**: `2.0.5` (drift — fix in 1.2)
- **Latest GitHub release**: `v1.0.0` (Feb 19, 2026 — Python-era)
- **Active engine work**: rc18 hardening as of Apr 24, 2026
- **License**: LGPL-3.0-or-later (v1.x was Apache-2.0, noted in README)
- **Release pipeline**: local build machine produces release tarballs; `gh release upload` attaches them. `release.yaml` is partially aspirational — see 2.1.
- **Platform**: Linux x86_64 only in v2.0.x (`install.sh` platform gate)

### Out of scope for this primer

Called out explicitly because these were ambiguous in earlier drafts:

- PyPI publishing. v2.1.1 wheel work is in the backlog — not touched here. This primer *guards against* accidental early publish (2.7) but does not move toward one.
- OpenAI-compat HTTP surface. v2.0.7 is in the backlog — not touched here.
- macOS / Windows / aarch64 / NPU backend support. Roadmap-level work — not touched here.
- Engine source changes of any kind.
- Rename / rebrand work.


These things should all be in the updated roadmap for a real 2.x.x cut

---

## 1. Phase 1 — Metadata, docs, version sync

Fast, low-risk. Single PR. These are the discoverability and hygiene fixes.

### 1.1 Set GitHub repo metadata

Sidebar currently reads "No description, website, or topics provided." Biggest lever for search discoverability.

```bash
gh repo edit tvanfossen/entropic \
  --description "Local-first agentic inference engine in C/C++. Multi-tier model routing, grammar-constrained output, MCP tool servers. Embeddable via C ABI." \
  --homepage "https://github.com/tvanfossen/entropic"

# Topics — GitHub caps at 20
gh repo edit tvanfossen/entropic \
  --add-topic llm \
  --add-topic local-llm \
  --add-topic inference-engine \
  --add-topic agentic-ai \
  --add-topic agentic-framework \
  --add-topic llama-cpp \
  --add-topic mcp \
  --add-topic tool-calling \
  --add-topic gguf \
  --add-topic gbnf \
  --add-topic grammar-constrained-decoding \
  --add-topic cpp \
  --add-topic cuda \
  --add-topic on-device-ai \
  --add-topic edge-ai \
  --add-topic embedded-ai \
  --add-topic privacy-first \
  --add-topic cpp20
```

Description string is deliberately neutral; maintainer can revise before execution.

Acceptance: `gh repo view tvanfossen/entropic --json description,homepageUrl,repositoryTopics` returns all three non-empty.

### 1.2 Fix version drift in `pyproject.toml`

Bump to match `CMakeLists.txt` and modernize metadata.

```toml
# pyproject.toml
[project]
name = "entropic-engine"
version = "2.0.6"                        # was 2.0.5 — syncs with CMakeLists.txt VERSION
description = "Local-first agentic inference engine with tier-based model routing"
readme = "README.md"
license = "LGPL-3.0-or-later"            # PEP 639 SPDX expression — replaces {text = "LGPL-3.0"}
license-files = ["LICENSE"]
requires-python = ">=3.10"
authors = [
    {name = "Tristan VanFossen", email = "vanfosst@gmail.com"},
]
keywords = ["ai", "agentic", "llm", "local", "inference", "llama-cpp", "edge", "cpp", "mcp"]
classifiers = [
    "Development Status :: 4 - Beta",
    "Environment :: Console",
    "Environment :: GPU :: NVIDIA CUDA",
    "Intended Audience :: Developers",
    "License :: OSI Approved :: GNU Lesser General Public License v3 (LGPLv3)",
    "Operating System :: POSIX :: Linux",
    "Programming Language :: C",
    "Programming Language :: C++",
    "Programming Language :: Python :: 3.10",
    "Programming Language :: Python :: 3.11",
    "Programming Language :: Python :: 3.12",
    "Programming Language :: Python :: 3.13",
    "Topic :: Scientific/Engineering :: Artificial Intelligence",
    "Topic :: Software Development",
    "Topic :: Software Development :: Libraries",
]

[project.urls]
Homepage      = "https://github.com/tvanfossen/entropic"
Repository    = "https://github.com/tvanfossen/entropic"
Issues        = "https://github.com/tvanfossen/entropic/issues"
Documentation = "https://github.com/tvanfossen/entropic/tree/main/docs"
Changelog     = "https://github.com/tvanfossen/entropic/blob/main/CHANGELOG.md"
```

Acceptance: `grep 'version = "2.0.6"' pyproject.toml` matches; `python -m build` produces a wheel whose `METADATA` includes all `[project.urls]` fields.

### 1.3 Create `CHANGELOG.md`

Keep-a-Changelog format. Cover the v1 → v2 pivot as the headline entry; retrofit RC history from `git log`.

Skeleton:

```markdown
# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The C API uses the major version as its compatibility axis;
`find_package(entropic 2.0 REQUIRED)` accepts any 2.x release per
`SameMajorVersion` in the installed `entropic-config-version.cmake`.

## [Unreleased]

## [2.0.6] — TBD

### Changed
- Engine hardening (rc16–rc18): verdict enum, critique tier, ON_COMPLETE
  validation block, identity caps, synthetic-termination plumbing, relay log
  disambiguation.
- Consolidated per-tool-call logging into a single structured entry.
- Async `entropic.ask` with push-notification progress and `ask_status` polling.
- Bridge symbols exported; multi-client bridge support.
- `relay_single_delegate` — skip lead re-synthesis for single-delegate responses.

### Added
- `entropic.context_inspect` MCP tool.
- Per-identity `max_iterations` and `max_tool_calls_per_turn` overrides.
- `ON_COMPLETE` hook point for application-side summary validation.
- `seed` field in `GenerationParams`, wired through to the sampler.
- `last_loop_metrics()` / `sp_get_metrics` surface.

### Fixed
- Zero-tool-call loop was unbounded (P1-6 regression).
- Partial streaming content preserved on mid-stream backend failure (P3-19).

## [2.0.0] — ARCHITECTURAL PIVOT — 2026-xx-xx

### Changed
- Engine ported from Python to C/C++ with a pure C ABI at the `.so` boundary.
- License changed from Apache-2.0 to LGPL-3.0-or-later.
- Primary distribution is now a GitHub Release tarball
  (`entropic-X.Y.Z-linux-x86_64-{cpu,cuda}.tar.gz`), not a PyPI wheel.
  The PyPI `entropic-engine` package is frozen at v1.7.1; a new wheel is
  planned for v2.1.1.
- Build system: CMake 3.21+, C++20, `find_package(entropic)` support.
- Inference backend: direct `llama.cpp` submodule (was `llama-cpp-python`).

### Added
- Tier-based routing with single-GPU VRAM-aware model swap.
- Identity/delegation system (multiple roles from one model).
- GBNF grammar-constrained output as a first-class per-tier feature.
- MCP plugin architecture (filesystem, bash, git, diagnostics, web, entropic).
- Build-option matrix: CUDA / CPU-only / static / shared.

## [1.0.0] — 2026-02-19
Final Python-era release. See `docs/history-v1-to-v2.md` for the v1.x Python
engine history (renamed from the old `docs/roadmap.md`).
```

Acceptance: `CHANGELOG.md` exists at repo root; `[project.urls].Changelog` in `pyproject.toml` resolves to it; the Unreleased section is present.

### 1.4 Fix `SECURITY.md`

Current file says `1.x: Yes, < 1.0: No` and references `llama-cpp-python` and `Textual` — both inaccurate for 2.x.

Replace Supported Versions table and Scope block:

```markdown
## Supported Versions

| Version | Supported |
|---------|-----------|
| 2.x     | Yes (current) |
| 1.x     | Security fixes only through 2026-06-30 |
| < 1.0   | No |

## Reporting a Vulnerability

If you discover a security vulnerability in Entropic, please report it
responsibly. **Do not open a public issue.**

Open a [private security advisory](https://github.com/tvanfossen/entropic/security/advisories/new)
on GitHub. Include a description of the vulnerability, steps to reproduce, and
any relevant logs or artifacts. You will receive an acknowledgment within
72 hours.

## Scope

Entropic is a local inference engine distributed as a C library. The following
are in scope:

- Arbitrary code execution via tool servers (filesystem, bash, diagnostics,
  git, web)
- Privilege escalation through MCP tool approval bypass
- Prompt injection that circumvents tool approval controls
- Path traversal in filesystem tool operations
- Memory safety issues in the C/C++ engine (use-after-free, OOB read/write,
  data races)
- Denial of service through crafted model inputs or configurations

Out of scope:

- Vulnerabilities in upstream dependencies (`llama.cpp`, SQLite, nlohmann/json,
  ryml, cpp-httplib, spdlog) — report these to the respective projects
- Model behavior (bias, hallucination, unsafe outputs) — properties of the
  loaded model, not the engine
- Issues requiring physical access to the host machine
```

Acceptance: no occurrence of `llama-cpp-python` or `Textual` in the file; date on 1.x deprecation window is explicit.

### 1.5 Fix `install.sh` example version

Line ~11 hardcodes `--version v2.0.5` as a usage example. That tag doesn't exist. Replace with a placeholder:

```bash
# Explicit version / backend / prefix:
#   curl -fsSL https://raw.githubusercontent.com/tvanfossen/entropic/main/install.sh \
#       | bash -s -- --version vX.Y.Z --cuda --prefix /opt/entropic --yes
```

Acceptance: `grep v2.0.5 install.sh` returns empty.

### 1.6 Rewrite `docs/roadmap.md`

Current doc is titled "Entropic Roadmap: v1.7.1 → v2.0.0" and treats v1.7.1 as current state. The repo is at 2.0.6 rc18. Two-step restructure:

1. Move the existing content to `docs/history-v1-to-v2.md` unchanged.
2. Create a new `docs/roadmap.md` sourcing forward-looking items from `.claude/proposals/BACKLOG/`:

```markdown
# Entropic Roadmap

This document is forward-looking only. For the v1.x Python-engine history and
the v1 → v2 C/C++ port plan, see `docs/history-v1-to-v2.md`.

## Current state — v2.0.6 (in flight)

- C/C++ engine, llama.cpp submodule
- Linux x86_64, CPU + CUDA backends
- C ABI + in-tree ctypes Python wrapper
- Engine hardening RC cycle: observer universality, multi-client bridge,
  critique tier, ON_COMPLETE validation, verdict enum

## v2.0.7

- OpenAI-compatible example consumer
  (see `.claude/proposals/BACKLOG/v2.0.7-openai-compat-example.md`)

## v2.1.x

- Dynamic MCP registration refresh (v2.1)
- Python wrapper on PyPI with prebuilt wheels, scikit-build-core +
  cibuildwheel pipeline (v2.1.1)

## v2.2.x

- Documentation causal graph (v2.2.0)

## Concurrent

- P2-20260421-001 — Parallel bridge sessions

## Longer-horizon (captured intent, not scheduled)

Items below are gated primarily on platform coverage expanding beyond
Linux x86_64.

- aarch64 / ARM Linux release target (Jetson / Pi class and embedded SoCs
  like RZ/V2L, i.MX 8M)
- Apple Silicon / Metal backend
- NPU backend abstraction (builds on the v1.9.13 `InferenceBackend` interface)
```

Maintainer: review the longer-horizon list and prune/amend before Claude Code writes the file.

Acceptance: `docs/roadmap.md` does not open with "v1.7.1"; `docs/history-v1-to-v2.md` preserves prior content.

### 1.7 Populate `.entropic/ENTROPIC.md`

Current file is the unfilled template with `<!-- placeholder -->` comments. ENTROPIC.md is itself a feature of the engine — shipping it unfilled in the engine's own repo is a poor first impression.

Replace with:

```markdown
# Project Context: Entropic

## Overview

Entropic is a C/C++ inference engine that turns a local GGUF model into a
multi-tier, tool-calling AI system. It runs entirely on user hardware — no
cloud, no API keys, no telemetry. It is distributed as a C library that
consumers link against.

## Tech Stack

- C/C++20, CMake 3.21+
- Inference: llama.cpp (submodule, CUDA / Vulkan / CPU backends)
- Storage: SQLite 3.35+ with FTS5
- Serialization: nlohmann/json, ryml
- HTTP: cpp-httplib
- Logging: spdlog
- Build orchestration: CMake presets + invoke (`tasks.py`)
- Python wrapper: ctypes, auto-generated, in-tree at `python/entropic/`

## Structure

- `src/` — C/C++ implementation, one library per subdirectory
- `include/entropic/` — public C headers (stable ABI; changes require proposal)
- `extern/` — llama.cpp submodule
- `python/entropic/` — ctypes wrapper
- `data/` — bundled prompts, grammars, tool definitions, model registry
- `examples/` — hello-world, pychess, headless, explorer
- `.claude/proposals/` — versioned feature proposals (ACTIVE, BACKLOG, STAGED,
  complete)
- `docs/` — architecture, getting-started, library-consumer-guide, roadmap

## Conventions

- SPDX headers on every source file: `SPDX-License-Identifier: LGPL-3.0-or-later`
- Pre-commit enforces quality gates (knots, flake8, ruff, doxygen-guard,
  build, tests)
- Every C/C++ function requires `@brief` and `@version` Doxygen comments
- Branch workflow: `feature/*` → `develop` → `main`
- Commit subject includes target version: `v2.0.6: Fix zero-tool-call loop`
- Interface headers under `include/entropic/interfaces/` are immutable without
  a new proposal
```

Acceptance: no `<!--` sequences remain in the file.

### 1.8 README badges

Insert at the top, immediately below the H1:

```markdown
[![CI](https://github.com/tvanfossen/entropic/actions/workflows/ci.yml/badge.svg)](https://github.com/tvanfossen/entropic/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/tvanfossen/entropic?display_name=tag&sort=semver)](https://github.com/tvanfossen/entropic/releases)
[![License: LGPL v3](https://img.shields.io/badge/License-LGPLv3-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platform: Linux x86_64](https://img.shields.io/badge/platform-linux--x86__64-lightgrey)](#requirements)
```

Acceptance: badges render on the repo landing page.

### 1.9 Document the full examples set

README's Examples table lists only `hello-world` and `pychess`; `examples/` contains four (`explorer`, `headless`, `hello-world`, `pychess`). Extend the table with two rows describing `explorer` and `headless`. If either is not production-ready, mark the row with a ⚠ Experimental note and a one-line caveat. Do not silently ship them undocumented.

Maintainer: supply the one-line descriptions for `explorer` and `headless` before Claude Code writes the table, or confirm Claude Code should draft from the directory contents.

Acceptance: README examples table row count matches `ls examples/ | wc -l`.

---

## 2. Phase 2 — CI/CD and pre-commit reconciliation

Separate PR. These reconcile workflow config with the current "local build is the release path" reality. **Nothing in this phase attempts to make hosted runners build CUDA artifacts.**

### 2.1 Reconcile `release.yaml` against local-build reality — DECISION REQUIRED

File comments in `release.yaml` (lines 98–105, 131–133) describe a CUDA build on a paid 8-core runner that is not how releases are currently produced. The matrix itself points CUDA at the free `ubuntu-24.04` runner, which cannot realistically build a full 12-arch CUDA binary in the hosted runner's disk/memory envelope anyway.

Current reality per maintainer: CUDA release artifacts are built on a local build machine and attached to the GitHub Release manually. Self-hosted runner migration is on the horizon but not active.

Three options — maintainer picks before Claude Code proceeds:

**Option A — Keep workflow, CPU-only matrix, honest comments.** Remove the `cuda` matrix entry from `build-package`. Rewrite the file header comment to state: "Hosted runners build CPU tarballs only. CUDA tarballs are built on a local machine and attached to the release via `gh release upload`. Self-hosted CUDA runner migration is planned but not active." Keep the tag validator, the CPU build, and the `github-release` job — CPU tarballs ship via the workflow; CUDA tarballs are uploaded post-hoc to the same release.

**Option B — Disable the workflow entirely.** Change `on: push: tags:` → `on: workflow_dispatch:` (or rename to `release.yaml.disabled`). Document the manual release procedure in a new `docs/release-process.md`. Cleanest if all release artifacts — CPU included — are currently being built locally.

**Option C — Keep current shape, mark CUDA matrix as tolerated failure.** Add `continue-on-error: true` to the CUDA matrix entry, rewrite comments to acknowledge CUDA builds will fail on hosted runners. Not recommended — makes the workflow badge misleading and leaves failed job noise in every release run.

Recommendation: **Option A** if CPU builds are genuinely working on hosted runners today; **Option B** if everything is local. These are operationally equivalent in outcome but differ in how automation is reintroduced later.

Acceptance varies by option; common criterion: the comment block in `release.yaml` accurately describes what the file does today.

### 2.2 Document the actual release process

Regardless of 2.1 choice, add `docs/release-process.md` describing the current path end-to-end:

1. Bump `CMakeLists.txt` VERSION and `pyproject.toml` version together on `develop`
2. Update `CHANGELOG.md` Unreleased section → new versioned section
3. Merge `develop` → `main` via PR
4. Tag on `main`: `git tag -a vX.Y.Z -m "..."` + `git push origin vX.Y.Z`
5. What happens automatically (per 2.1 outcome)
6. What the maintainer does manually on the local build machine (CMake preset, artifact paths, sha256 generation)
7. `gh release upload <tag> <artifacts>` command reference
8. Post-release bump of `CMakeLists.txt` + `pyproject.toml` to next patch on `develop`

Acceptance: file exists, covers tag → attached artifacts end-to-end.

### 2.3 Pin `doxygen-guard` pre-commit rev

`.pre-commit-config.yaml` currently has `rev: main` for `tvanfossen/doxygen-guard` — floating branch ref, non-reproducible builds, supply-chain smell. Pin to a tag or SHA:

```yaml
- repo: https://github.com/tvanfossen/doxygen-guard
  rev: v0.1.0        # or the current commit SHA if no tag exists
  hooks:
    - id: doxygen-guard
```

Maintainer note: if `doxygen-guard` has no release tag yet, cut one (even `v0.0.1`) before pinning so downstream users aren't also SHA-pinning.

Acceptance: `grep -E "rev: (main|master|HEAD)" .pre-commit-config.yaml` returns empty.

### 2.4 `pre-commit autoupdate`

Sweep stale hook versions. Current file has `pre-commit-hooks v4.5.0` (latest 5.x), `ruff-pre-commit v0.3.4` (latest 0.8.x+), `flake8 7.0.0` (still 7.x but bumpable).

```bash
.venv/bin/pre-commit autoupdate
.venv/bin/pre-commit run --all-files
```

If any hook breaks after the update, revert that specific hook's bump and open a follow-up issue tracking the breakage. Do not block this PR on a broken third-party hook.

Acceptance: `pre-commit run --all-files` green; pinned revs updated in the config.

### 2.5 Resolve `nightly.yml` reference

`.github/actions/setup-entropic/action.yml` line 9 says the composite action is "Used by ci.yml (PR checks), release.yml (Phase 3), and nightly.yml (Phase 4)." No `nightly.yml` exists.

Two choices — maintainer picks:

- **Create a stub `nightly.yml`** that mirrors `release-validation.yml` (valgrind + abbreviated stress + perf-bench) on a `schedule: cron: "0 6 * * *"` trigger. Easy win if trend data on memory safety / stress is useful.
- **Remove the reference from `action.yml`.** Clean if nightly isn't planned near-term.

Acceptance: either the file exists or the comment doesn't mention it.

### 2.6 Reconcile `release-validation.yml` branch trigger

Workflow fires on `release/**` branches. `CONTRIBUTING.md` documents `feature/*` → `develop` → `main` with no `release/**`. Choose one:

- Add a `release/**` convention to `CONTRIBUTING.md`'s Branch Workflow section describing when such a branch is cut (e.g., during RC stabilization).
- Change the trigger to `workflow_dispatch` + `schedule` on main if the `release/**` pattern is vestigial.

Acceptance: `CONTRIBUTING.md` branch scheme and `release-validation.yml` trigger agree.

### 2.7 Prevent accidental early PyPI publish

This is a **guard**, not a publish step. The v2.1.1 wheel work is still backlog; this primer does not move it forward. Today's `pyproject.toml` is close enough to "publishable" shape that a stray `python -m build && twine upload` would push a source dist that cannot load `librentropic.so`, producing a broken PyPI package that is cumbersome to yank.

Two independent guards, both cheap:

1. **Top-of-file comment in `pyproject.toml`:**

   ```toml
   # ----------------------------------------------------------------------
   # NOT CURRENTLY PUBLISHED TO PyPI.
   #
   # v2.0.x distribution is via GitHub Releases tarball + install.sh only.
   # The PyPI wheel is deferred to v2.1.1 and requires a scikit-build-core
   # rework to bundle librentropic.so correctly.
   # See .claude/proposals/BACKLOG/v2.1.1-python-wrapper.md.
   #
   # Do NOT run `twine upload` against this package until v2.1.1 lands.
   # ----------------------------------------------------------------------
   ```

2. **Classifier guard:**

   ```toml
   classifiers = [
       # ... existing classifiers ...
       "Private :: Do Not Upload",
   ]
   ```

   `twine check` doesn't enforce this, but the classifier is a widely-recognized visible second signal.

Acceptance: `head -20 pyproject.toml` shows the comment block; `grep "Do Not Upload" pyproject.toml` matches.

### 2.8 Add `.github/dependabot.yml`

Minimum — keep GitHub Actions versions current. Not chasing Python deps since the package isn't publishing.

```yaml
version: 2
updates:
  - package-ecosystem: "github-actions"
    directory: "/"
    schedule:
      interval: "weekly"
    labels: ["dependencies", "ci"]
    open-pull-requests-limit: 5
```

Acceptance: Dependabot appears under Insights → Dependency graph → Dependabot.

---

## 3. Phase 3 — Community health files

Lowest urgency, lowest risk. Can ship before, with, or after the v2.0.6 tag. Separate PR.

### 3.1 `CODE_OF_CONDUCT.md`

Contributor Covenant 2.1 verbatim at repo root:

```bash
curl -fsSL https://www.contributor-covenant.org/version/2/1/code_of_conduct/code_of_conduct.md \
  -o CODE_OF_CONDUCT.md
# Edit the contact placeholder at the bottom to point at the Security Advisory page
# or a dedicated email.
```

### 3.2 `.github/ISSUE_TEMPLATE/`

Three YAML issue forms (not legacy markdown):

- `bug_report.yml` — reproduction steps, engine version (`entropic version`), OS, CUDA/CPU backend, model name, log excerpt.
- `feature_request.yml` — use case, proposed API surface if known, whether it touches `include/entropic/interfaces/` (which requires a proposal).
- `config.yml` with `blank_issues_enabled: false` and a `contact_links:` entry routing security reports to the Security Advisory page.

### 3.3 `.github/PULL_REQUEST_TEMPLATE.md`

Brief checklist:

```markdown
## Summary


## Linked proposal ID


## Checklist
- [ ] Pre-commit passes locally
- [ ] Tests added or updated (if behavioral change)
- [ ] `CHANGELOG.md` Unreleased section updated
- [ ] Interface headers (`include/entropic/interfaces/`) not modified, OR a proposal covering the change is linked above
- [ ] Commit subject includes target version tag
```

### 3.4 Social preview image

Upload a 1280×640 PNG via Settings → Social preview. Should visually signal "C/C++ library" (terminal glimpse with a `find_package(entropic ...)` line, or similar) — not a chatbot UI screenshot, because that's the wrong mental model for this project.

Maintainer-produced image, or defer.

### 3.5 Expand `CODEOWNERS`

Add an explicit `/docs/` line so doc-only PRs route identically to code PRs once outside contributors arrive:

```
/docs/   @tvanfossen
```

Not strictly required while the root rule `* @tvanfossen` covers everything, but low-cost future-proofing.

---

## 4. Release cut — tagging v2.0.6

After Phase 1 + Phase 2 have merged to `main`:

```bash
git checkout main
git pull origin main
git tag -a v2.0.6 -m "v2.0.6 — engine hardening, observability, validation hooks"
git push origin v2.0.6
```

What happens next depends on the 2.1 decision:

- **Option A**: `release.yaml` runs, builds CPU tarball, creates the GitHub Release, attaches CPU artifacts. Maintainer then runs the local build machine for CUDA and `gh release upload v2.0.6 entropic-2.0.6-linux-x86_64-cuda.tar.gz*`.
- **Option B**: Maintainer runs the full local release procedure in `docs/release-process.md` and `gh release create v2.0.6 ...` with all tarballs.
- **Option C**: `release.yaml` runs, CPU succeeds, CUDA fails but tolerated; maintainer uploads CUDA artifacts locally.

Post-release checklist:

- [ ] `install.sh --version v2.0.6 --cpu` on a clean Ubuntu 24.04 VM; verify `/usr/local/bin/entropic version` prints 2.0.6
- [ ] `install.sh --version v2.0.6 --cuda` on a GPU box; verify same
- [ ] Downstream C++ consumer builds against the installed `entropic-config.cmake` (re-run `packaging/smoke-consumer` against `/usr/local`)
- [ ] `CHANGELOG.md` Unreleased section reopened, empty
- [ ] On `develop`, bump `CMakeLists.txt` VERSION and `pyproject.toml` version to `2.0.7` so the sync is preserved

---

## 5. Execution order for Claude Code

One PR per numbered item:

1. **`chore: v2.0.6 metadata and documentation`** — all of Phase 1 (1.1–1.9). GitHub metadata via `gh repo edit`, file edits for the rest. No CI changes.
2. **`ci: reconcile workflows against current release process`** — all of Phase 2. Blocked on maintainer decisions (2.1 A/B/C; 2.5 stub vs remove; 2.6 document vs retire) before Claude Code opens the PR.
3. **Tag `v2.0.6`** per Section 4 — only after PRs 1 and 2 merge.
4. **`docs: community health files`** — Phase 3. No sequence dependency; can land any time.

Every PR description should reference the section number(s) from this doc so review comments map cleanly back to acceptance criteria.

---

## 6. Decision points requiring maintainer input

Items Claude Code cannot resolve autonomously:

1. **1.1** — Ship proposed description string, or supply a maintainer-preferred version.
2. **1.6** — Longer-horizon roadmap list: confirm accuracy, prune, or amend.
3. **1.9** — Supply one-line descriptions for `explorer` and `headless` examples, or authorize drafting from directory contents.
4. **2.1** — `release.yaml` Option A / B / C.
5. **2.5** — `nightly.yml`: create stub, or remove reference.
6. **2.6** — `release/**` branch convention: document, or retire the trigger.
7. **3.4** — Social preview image: produce, or defer.

All other tasks have clear defaults and no input required.
