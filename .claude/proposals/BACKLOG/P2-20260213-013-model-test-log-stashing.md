---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260213-013
title: "Model Test Log Stashing as Training Data"
priority: P2
component: testing
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-13
updated: 2026-02-13
tags: [testing, logging, fine-tuning, training-data, model-tests]
completed_date: null
scoped_files:
  - tests/model/conftest.py
  - tests/conftest.py
  - src/entropi/core/logging.py
  - scripts/run-model-tests.sh
  - .test-reports/
depends_on: []
blocks:
  - P1-20260213-012  # Fine-tuning pipeline needs training data
---

# Model Test Log Stashing as Training Data

## Problem Statement

Model tests run headless through `Application` with real model inference, producing the
richest source of prompt→thinking→action data in the project. But this data is lost:

1. **Model logger not initialized during tests** — `setup_model_logger()` is called in
   `cli.py`, not `Application`. During tests, `get_model_logger()` returns a handler-less
   logger that silently drops all output.
2. **`tmp_project_dir` is a tempdir** — even if logging were wired up, the `.entropi/`
   directory (and its logs) is cleaned up after each test fixture teardown.
3. **Session log (`session.log`) same problem** — `setup_logging()` also only called in CLI.

The existing `.test-reports/` infrastructure captures high-level data (PlantUML diagrams,
text summaries, tool calls), but not the raw model I/O needed for fine-tuning training data.

## Connection to Fine-Tuning (P1-20260213-012)

The fine-tuning proposal identifies `session_model.log` as the primary training data source.
Model tests are the ideal data generator:

- **Controlled inputs** — each test sends a known prompt with expected behavior
- **Labeled outcomes** — test pass/fail labels correct vs. incorrect model behavior
- **Tier coverage** — test files cover simple, reasoning, code, completeness, and errors
- **Reproducible** — same tests produce consistent data across model versions

Stashed logs from model tests become the seed dataset for fine-tuning without manual
annotation effort.

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

## Proposed Approach

### 1. Wire Up Logging in Test Fixtures

In `tests/model/conftest.py`, add logging setup to `headless_app` fixture:

```python
@pytest.fixture
async def headless_app(config, shared_orchestrator, headless_presenter, tmp_project_dir):
    setup_logging(config, project_dir=tmp_project_dir)
    setup_model_logger(project_dir=tmp_project_dir)
    # ... existing Application creation ...
```

Both loggers use `mode='w'` — each test starts fresh (function-scoped fixture).

### 2. Stash Logs Per-Test in Report Hook

Extend `pytest_runtest_makereport` to copy log files after each test:

```
.test-reports/
├── logs/
│   ├── test_hello_produces_response/
│   │   ├── session.log
│   │   └── session_model.log
│   ├── test_thanks_produces_acknowledgment/
│   │   └── ...
│   └── ...
├── test_hello_produces_response.puml
├── model-tests-latest.txt
└── model-tests.hash
```

Copy from `tmp_project_dir/.entropi/session*.log` → `.test-reports/logs/<test_name>/`.
Must happen in the `call` phase of `pytest_runtest_makereport` (before fixture teardown
cleans up tmp_project_dir).

### 3. Add Pass/Fail Metadata

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

### 4. Training Data Export Script

`scripts/extract_training_data.py` (from P1-20260213-012) reads `.test-reports/logs/`
to produce structured training examples. The stashed logs are the raw material; the
script transforms them into the format fine-tuning expects.

## Design Decisions

### Why per-test directories (not one big log)?

- Each test = one interaction = one training example. Clean 1:1 mapping.
- Failed tests are easily excluded or used as negative examples.
- No need to parse test boundaries out of a combined log.

### Why `.test-reports/logs/` (not a separate directory)?

- Already gitignored, already created by existing infrastructure.
- Keeps all test artifacts in one place.
- `run-model-tests.sh` already manages this directory.

### Why not capture logs at the Application level?

`Application.__init__` could call `setup_model_logger()`, but that would change
production behavior (logging setup is currently the CLI's responsibility, not the app's).
Test fixtures are the right place for test-specific wiring.

## Risks

| Risk | Mitigation |
|------|------------|
| Log files add disk usage | Model tests run ~6 tests; ~1-2MB total per run |
| Logger state leaks between tests | `setup_model_logger` clears handlers; function-scoped fixture |
| `session.log` RichHandler pollutes test output | `setup_logging` console handler is WARNING-only; test output unaffected |
| tmp_project_dir cleaned before hook runs | `call` phase report runs before fixture teardown — verified |

## Success Criteria

- [ ] `session_model.log` generated during every model test
- [ ] Logs persisted in `.test-reports/logs/<test_name>/` after test run
- [ ] `metadata.json` includes pass/fail, tier, prompt, timing
- [ ] Existing PlantUML reports unaffected
- [ ] `scripts/extract_training_data.py` can parse stashed logs (P1-20260213-012)
