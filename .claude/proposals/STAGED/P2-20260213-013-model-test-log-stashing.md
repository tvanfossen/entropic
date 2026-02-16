---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260213-013
title: "Model Test Reports — Committed Log Stashing"
priority: P2
component: testing
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-13
updated: 2026-02-16
tags: [testing, logging, fine-tuning, training-data, model-tests]
completed_date: 2026-02-16
scoped_files:
  - tests/model/conftest.py
  - tests/conftest.py
  - src/entropi/core/logging.py
  - scripts/run-model-tests.sh
  - scripts/model-test-cache-check.sh
  - .pre-commit-config.yaml
  - test-reports/
  - .gitignore
depends_on: []
blocks:
  - P1-20260213-012  # Fine-tuning pipeline needs training data
related:
  - P1-20260213-015  # In-process tools resolves file leak (#82)
---

# Model Test Reports — Committed Log Stashing

## Problem Statement

Model tests run headless through `Application` with real model inference, producing the
richest source of prompt->thinking->action data in the project. But this data is lost:

1. **Model logger not initialized during tests** — `setup_model_logger()` is called in
   `cli.py`, not `Application`. During tests, `get_model_logger()` returns a handler-less
   logger that silently drops all output.
2. **`tmp_project_dir` is a tempdir** — even if logging were wired up, the `.entropi/`
   directory (and its logs) is cleaned up after each test fixture teardown.
3. **Session log (`session.log`) same problem** — `setup_logging()` also only called in CLI.

The existing `.test-reports/` infrastructure captures high-level data (PlantUML diagrams,
text summaries, tool calls), but not the raw model I/O needed for fine-tuning training data.

### File Leak (#82)

Model tests also leak files (e.g. `is_even.py`, `sum_list.py`) into the repo root. Root
cause: MCP servers launch as subprocesses defaulting to `Path.cwd()` (repo root) instead
of the test's `tmp_project_dir`. The `MCPManager` doesn't pass `project_dir` to subprocess
args. This is resolved architecturally by P1-20260213-015 (in-process tools), which
eliminates subprocesses for internal tools entirely.

## Connection to Fine-Tuning (P1-20260213-012)

The fine-tuning proposal identifies `session_model.log` as the primary training data source.
Model tests are the ideal data generator:

- **Controlled inputs** — each test sends a known prompt with expected behavior
- **Labeled outcomes** — test pass/fail labels correct vs. incorrect model behavior
- **Tier coverage** — test files cover simple, reasoning, code, completeness, and errors
- **Reproducible** — same tests produce consistent data across model versions

Stashed logs from model tests become the seed dataset for fine-tuning without manual
annotation effort. Training data extraction is deferred to P1-20260213-012.

## Current Architecture

### What's Already There

| Component | Status | Notes |
|-----------|--------|-------|
| `.test-reports/` directory | Exists, gitignored | PlantUML + text summaries |
| `pytest_runtest_makereport` hook | Exists | Captures per-test interaction data |
| `pytest_sessionfinish` hook | Exists | Generates diagrams + summary |
| `TestInteraction` dataclass | Exists | Captures prompt, response, tier, tool calls |
| `run-model-tests.sh` | Exists | Runs pytest, generates report header |
| `HeadlessPresenter` | Exists | Captures stream content, messages, tool calls in memory |

### What's Missing

| Gap | Impact |
|-----|--------|
| `setup_model_logger()` not called in test fixtures | No `session_model.log` generated |
| `setup_logging()` not called in test fixtures | No `session.log` generated |
| No per-test log file copying | Logs (if they existed) would be overwritten per test |
| No persistent storage of raw model I/O | Training data lost after each run |
| Reports gitignored | Cannot track model behavior changes across commits |

## Proposed Approach

### 1. Migrate `.test-reports/` → `test-reports/` (Committed)

Rename the directory (no dot prefix) and remove from `.gitignore`. Only the latest run
is maintained — each test run overwrites the previous reports. This makes model behavior
visible in the repo and trackable across commits.

```
test-reports/
├── logs/
│   ├── test_hello_produces_response/
│   │   ├── session.log
│   │   ├── session_model.log
│   │   └── metadata.json
│   ├── test_thanks_produces_acknowledgment/
│   │   └── ...
│   └── ...
├── test_hello_produces_response.puml
├── model-tests-latest.txt
└── model-tests.hash
```

Update references in:
- `tests/model/conftest.py` (`REPORT_DIR`)
- `scripts/run-model-tests.sh`
- `scripts/model-test-cache-check.sh`
- `.gitignore` (remove `.test-reports/` entry)

### 2. Wire Up Logging in Test Fixtures

In `tests/model/conftest.py`, add logging setup to `headless_app` fixture:

```python
@pytest.fixture
async def headless_app(config, shared_orchestrator, headless_presenter, tmp_project_dir):
    setup_logging(config, project_dir=tmp_project_dir)
    setup_model_logger(project_dir=tmp_project_dir)
    # ... existing Application creation ...
```

Both loggers use `mode='w'` — each test starts fresh (function-scoped fixture).

### 3. Stash Logs Per-Test in Report Hook

Extend `pytest_runtest_makereport` to copy log files after each test:

Copy from `tmp_project_dir/.entropi/session*.log` → `test-reports/logs/<test_name>/`.
Must happen in the `call` phase of `pytest_runtest_makereport` (before fixture teardown
cleans up tmp_project_dir).

### 4. Add Pass/Fail Metadata

Include a `metadata.json` alongside each test's logs:

```json
{
  "test_name": "test_hello_produces_response",
  "passed": true,
  "tier": "simple",
  "duration_s": 3.2,
  "prompt": "Hello",
  "timestamp": "2026-02-13T14:27:22"
}
```

This metadata enables automated filtering: passed tests = positive training examples,
failed tests = negative examples for DPO preference pairs.

### 5. Clean Report Directory on Each Run

`pytest_sessionstart` (or `run-model-tests.sh`) clears `test-reports/logs/` before each
run. Only the latest results are committed. This keeps the directory a snapshot, not an
accumulator.

## Design Decisions

### Why committed (not gitignored)?

- Model behavior is a first-class artifact — changes to model output should be visible
  in diffs, reviewable in PRs, and trackable across commits.
- Training data provenance: committed logs have a git history linking them to the code
  and config that produced them.
- Latest-only keeps repo size bounded (~1-2MB per run, ~22 tests).

### Why per-test directories (not one big log)?

- Each test = one interaction = one training example. Clean 1:1 mapping.
- Failed tests are easily excluded or used as negative examples.
- No need to parse test boundaries out of a combined log.

### Why not capture logs at the Application level?

`Application.__init__` could call `setup_model_logger()`, but that would change
production behavior (logging setup is currently the CLI's responsibility, not the app's).
Test fixtures are the right place for test-specific wiring.

## Risks

| Risk | Mitigation |
|------|------------|
| Log files add repo size | ~1-2MB total per run; latest-only; text compresses well in git |
| Logger state leaks between tests | `setup_model_logger` clears handlers; function-scoped fixture |
| `session.log` RichHandler pollutes test output | `setup_logging` console handler is WARNING-only; test output unaffected |
| tmp_project_dir cleaned before hook runs | `call` phase report runs before fixture teardown — verified |
| Noisy diffs when model output changes | Expected — that's the point. Review like any other test output. |

## Success Criteria

- [x] `session_model.log` generated during every model test
- [x] Logs persisted in `test-reports/logs/<test_name>/` after test run
- [x] `metadata.json` includes pass/fail, tier, prompt, timing
- [x] Existing PlantUML reports migrated to `test-reports/` and unaffected
- [x] `test-reports/` committed (not gitignored), latest run only
- [x] File leak (#82) documented as deferred to P1-015

## Implementation Notes

### Pre-commit Hook Exclusion

Model output naturally contains trailing whitespace and inconsistent newlines.
The `trailing-whitespace` and `end-of-file-fixer` hooks must exclude `test-reports/`
to avoid an infinite modify-restage-commit loop. Added `exclude: ^test-reports/`
to both hooks in `.pre-commit-config.yaml`.

### Gitignore Layering

Three rules interact for `test-reports/`:
- `*.log` (line 57) — ignores all log files globally
- `logs/` (line 58) — ignores any directory named `logs`
- `!test-reports/logs/` + `!test-reports/logs/**/*.log` — negation overrides

The `logs/` directory rule required its own negation (`!test-reports/logs/`) because
git will not traverse into an ignored directory to check file-level negations.

### Log Coverage

11 of 22 tests produce logs (all headless app tests). The 11 routing tests use
the `orchestrator` fixture directly without `headless_app`/`tmp_project_dir`, so
no session logs are generated. Routing tests still produce PlantUML diagrams and
appear in the text summary.

### Commit Workflow

First commit after implementation required a two-pass: model tests regenerated
fresh reports during the pre-commit hook, modifying staged files. Second commit
succeeded because the hash was cached. This is a one-time bootstrap cost; subsequent
commits with no source changes skip model tests entirely via the cache.
