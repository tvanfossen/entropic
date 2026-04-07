# Test Layer Architecture

Three test layers validate the entropic engine at different levels.
Each layer answers a different question.

## Unit Tests (`tests/unit/`)

**Question:** Does each component work correctly in isolation?

- Mock inference via `mock_inference.h`
- Fast, CPU-only, no model required
- Run in pre-commit on every commit
- 668+ tests as of v1.10.1

## Regression Tests (`tests/regression/`)

**Question:** Does the engine wiring work correctly?

- Mock inference with scripted multi-turn responses
- Validates generate-parse-execute-generate cycle
- Fast, CPU-only, run in pre-commit
- 6 test files covering: basic response, multi-turn context,
  tool calling loop, delegation pipeline, router classification,
  identity behavior

## Model Tests (`tests/model/`)

**Question:** Does the engine produce correct behavior with a real model?

- Live model on GPU (Qwen3.5-35B-A3B-UD-IQ3_XXS)
- BDD format (SCENARIO/GIVEN/WHEN/THEN)
- NOT a pre-commit hook -- manual developer run
- Results committed to `test-reports/model/results.json`
- CI validates freshness (version + git_sha match)

### Subsystem Tests (10) — `entropic-subsystem-tests`

Individual subsystem validation with a live model. Calls
`ModelOrchestrator` directly, bypassing the engine loop.

| # | Test | Validates |
|---|------|-----------|
| 1 | Basic generation | Model loading, prompt assembly, streaming |
| 2 | Tool call parsing | Tool schema injection, adapter parsing |
| 3 | Tier routing | Router classification, digit→tier mapping |
| 4 | Delegation | Delegation prompt, response generation |
| 5 | Grammar constraint | GBNF registry, JSON constraint enforcement |
| 6 | Context retention | Multi-turn context across generations |
| 7 | Logprob evaluation | evaluate_logprobs(), perplexity calculation |
| 8 | Dynamic identity | Identity CRUD, router dirty flag |
| 9 | GPU profile | Generation under constrained resources |
| 10 | MCP authorization | Per-identity access level enforcement |

### Engine-Loop Tests (9) — `entropic-engine-tests`

Full `AgentEngine::run()` with a live model. Exercises the
coordination layer: state machine, tool execution cycle, routing,
compaction, delegation infrastructure, MCP denial.

| # | Test | Validates |
|---|------|-----------|
| E0 | True end-to-end | Real config-loaded identity, zero bypass |
| E1 | Single-turn engine loop | Basic generate-stop cycle |
| E2 | Tool call cycle | generate→parse→execute→regenerate |
| E3 | Routing callback | on_tier_selected fires with tier name |
| E4 | Multi-turn context | Two engine.run() calls retain context |
| E5 | Compaction trigger | on_compaction fires with padded context |
| E6 | State transitions | PLANNING→EXECUTING→COMPLETE sequence |
| E7 | Delegation | Delegation infrastructure wiring |
| E8 | MCP auth denial | Tool executor denial propagation |

### Running model tests

```bash
cmake --preset dev -DENTROPIC_BUILD_MODEL_TESTS=ON
cmake --build build/dev --target entropic-subsystem-tests
cmake --build build/dev --target entropic-engine-tests
ctest --test-dir build/dev -L model
```

### Results

Results are written to `test-reports/model/results.json` and must
be committed. CI fails if results are stale or missing for the
current version.

## Boundary

| What | Unit | Regression | Model (Subsystem) | Model (Engine) |
|------|------|------------|-------------------|----------------|
| Inference | Mock | Mock (scripted) | Real (GPU) | Real (GPU) |
| Engine loop | No | Yes (mock) | No | Yes (real) |
| Speed | Fast (<1s) | Fast (<5s) | Slow (<2min) | Slow (<3min) |
| Gate | Pre-commit | Pre-commit | Manual + CI freshness | Manual + CI freshness |
| Purpose | Component logic | Engine wiring | Subsystem correctness | Coordination correctness |
