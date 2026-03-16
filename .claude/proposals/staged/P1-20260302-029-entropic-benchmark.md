---
version: 1.0.0
type: proposal
schema_version: 1
id: P1-20260302-029
title: "entropic-benchmark: identity-driven model evaluation framework"
priority: P1
component: benchmark
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-03-02
updated: 2026-03-16
tags: [benchmark, models, identities, testing, evaluation]
completed_date: null
scoped_files:
  - "src/entropic/benchmark/**"
  - "examples/moe-vram-benchmark/**"
depends_on: [P1-20260302-024]
blocks: []
---

# entropic-benchmark: Identity-Driven Model Evaluation Framework

## Problem Statement

The current MoE VRAM benchmark (`examples/moe-vram-benchmark/`) is hardcoded around
specific models (Qwen3.5-35B-A3B Q4/Q2, Qwen3-8B). Adding a new model means
editing Python code. Changing identities means rewriting quality prompts. Historical
results are lost between runs.

The engine needs a benchmark tool that:

1. **Accepts a GGUF file as input** and produces a standardized evaluation
2. **Discovers identities from the engine** rather than hardcoding them
3. **Maintains historical records** so previously-tested models don't need re-running
4. **Survives identity changes** — if identities are completely overhauled in 6
   months, the benchmark framework doesn't need rewriting
5. **Interlinks with model tests** (`tests/model/`) as a shared evaluation standard

## Design Principles

### Identity-Agnostic by Design

The benchmark NEVER hardcodes identity names, quality prompts, or expected outputs.
Instead:

- **Identities self-describe** via frontmatter (focus, grammar, tools, temperature)
- **Quality prompts live WITH identities** — each identity ships a `benchmark.yaml`
  defining its evaluation criteria
- **The framework discovers and runs** whatever identities exist at runtime

If identities change, the benchmark changes automatically. The framework is the
runner; identities carry their own test definitions.

### Model as Input, Report as Output

```
entropic benchmark run ~/models/gguf/SomeNewModel-Q4_K_M.gguf
  → loads model
  → discovers identities from engine config
  → runs each identity's benchmark suite against the model
  → produces structured report (JSON + human-readable)
  → stores in historical record
```

No code changes needed to evaluate a new model. No code changes needed when
identities change.

### Two Evaluation Layers

```
Layer 1: Raw llama-cpp-python (no engine)
  - Load time (cold from disk, warm from mlock'd RAM)
  - Swap latency (cold→active, warm→active, identity swap on same model)
  - tok/s at various GPU layer counts (GPU sweep)
  - Context length scaling (tok/s degradation at 2K → 8K → 16K → 32K)
  - VRAM consumption per GPU layer count
  - Pure model performance, no engine overhead

Layer 2: Engine + Identity (through entropic)
  - Per-identity quality evaluation
  - Tool calling accuracy
  - Grammar compliance
  - Pipeline latency (multi-identity chain timing)
  - Think mode behavior
```

Layer 1 answers "how fast is this model?" Layer 2 answers "how well does this
model serve each identity role?"

## Identity Benchmark Contract

Each bundled identity MAY include a `benchmark` section in its frontmatter or
a companion `benchmark.yaml`. The benchmark framework reads these at runtime.

```yaml
# Example: identity_code_writer.md frontmatter or benchmark.yaml
benchmark:
  prompts:
    - prompt: "Write a Python function called is_palindrome that checks if a string is a palindrome."
      checks:
        - type: contains
          value: "def is_palindrome"
        - type: contains
          value: "return"
    - prompt: "Write a function that returns the nth Fibonacci number."
      checks:
        - type: contains
          value: "def "
        - type: regex
          pattern: "fib|fibonacci"
  tool_test:
    prompt: "Read the file at /tmp/test.txt"
    expected_tool: "filesystem.read_file"
  grammar_test:
    prompt: "Write is_even function"
    validates: true  # check output parses against identity's grammar
```

If an identity has no benchmark section, it's skipped. New identities get
benchmarked the moment they add benchmark definitions. Removed identities
stop being benchmarked. Zero framework code changes.

### Check Types

Checks are composable primitives:

| Type | Description |
|------|-------------|
| `contains` | Output contains substring |
| `not_contains` | Output does NOT contain substring |
| `regex` | Output matches regex pattern |
| `tool_call` | Model produced a tool call for expected tool |
| `grammar_valid` | Output parses against identity's GBNF grammar |
| `token_count_max` | Output stays within N tokens |
| `think_block` | Output contains/omits think block as expected |
| `json_schema` | Output validates against JSON schema |

Consumers can define custom check types via plugins.

## Historical Records

```
.entropic/benchmark/
├── results/
│   ├── Qwen3.5-35B-A3B-Q2_K-20260302.json
│   ├── Qwen3.5-35B-A3B-Q4_K_M-20260302.json
│   ├── Qwen3-8B-Q4_K_M-20260302.json
│   └── Falcon-H1R-7B-Q4_K_M-20260315.json
├── comparisons/
│   └── latest.json          # auto-generated cross-model comparison
└── config.yaml              # benchmark-specific settings (timeouts, etc.)
```

### Result Schema

Each result file is self-contained:

```json
{
  "model": {
    "filename": "Qwen3.5-35B-A3B-Q2_K.gguf",
    "architecture": "qwen35moe",
    "params_total": "35B",
    "params_active": "3B",
    "quantization": "Q2_K",
    "file_size_bytes": 13958643712
  },
  "hardware": {
    "gpu": "NVIDIA RTX PRO 4000 Blackwell Laptop",
    "vram_total_mb": 16303,
    "cuda_compute": "12.0"
  },
  "timestamp": "2026-03-02T12:00:00Z",
  "engine_version": "1.0.1",
  "identity_versions": {
    "code_writer": "1.0",
    "conversational": "1.0"
  },
  "layer1": {
    "load_times": {
      "cold_ms": [...],
      "warm_ms": [...]
    },
    "swap_latency": {
      "cold_to_active_ms": [...],
      "warm_to_active_ms": [...],
      "identity_swap_ms": [...]
    },
    "inference": [...],
    "gpu_sweep": [...],
    "context_scaling": [...]
  },
  "layer2": {
    "code_writer": {
      "prompts": [...],
      "tool_calling": {...},
      "grammar_compliance": {...},
      "summary": {"pass": 3, "fail": 0, "skip": 1}
    }
  }
}
```

The `identity_versions` field lets you know when results were generated against
older identity definitions. If an identity changes, its version bumps, and the
comparison tool flags stale results.

### Comparison Tool

```
entropic benchmark compare
  → reads all results in .entropic/benchmark/results/
  → groups by identity
  → produces per-identity leaderboard (best model for each role)
  → flags stale results (identity version mismatch)
```

Output:

```
Identity: code_writer (v1.0)
  Model                         tok/s   Quality   Grammar   VRAM
  Qwen3.5-35B-A3B Q2_K          32.4    3/3       100%     12877 MB
  Qwen3-8B Q4_K_M               62.0    2/3        —        4850 MB
  Falcon-H1R-7B Q4_K_M          45.2    3/3       100%      4600 MB  ← STALE (v0.9)
```

## CLI Interface

```bash
# Evaluate a single model against all identities
entropic benchmark run ~/models/gguf/NewModel.gguf

# Layer 1 only (raw performance, no engine)
entropic benchmark run ~/models/gguf/NewModel.gguf --layer1-only

# Layer 2 only (identity quality, skip raw perf)
entropic benchmark run ~/models/gguf/NewModel.gguf --layer2-only

# Specific identity only
entropic benchmark run ~/models/gguf/NewModel.gguf --identity code_writer

# Compare all historical results
entropic benchmark compare

# Compare specific models
entropic benchmark compare Qwen3.5-35B-A3B-Q2_K Falcon-H1R-7B-Q4_K_M

# List available results
entropic benchmark list

# GPU layer sweep for a model (layer 1 subset)
entropic benchmark sweep ~/models/gguf/NewModel.gguf
```

## Relationship to Model Tests (`tests/model/`)

Model tests and benchmarks serve different purposes but share evaluation logic:

| | Model Tests (`tests/model/`) | Benchmark (`entropic benchmark`) |
|---|---|---|
| **Purpose** | Regression gate (pass/fail) | Model evaluation (quantitative) |
| **When** | Pre-commit, CI | On-demand, new model assessment |
| **Output** | Pass/fail + test report | Structured JSON + comparison |
| **Models** | Currently configured models | Any GGUF file |
| **Speed** | Must be fast (<3min) | Can be thorough (10-30min) |

**Shared evaluation primitives**: The check types (`contains`, `regex`,
`tool_call`, `grammar_valid`) should be a shared library that both model tests
and benchmarks import. This ensures model tests and benchmarks agree on what
"correct" means.

```
src/entropic/benchmark/
├── __init__.py
├── cli.py              # CLI commands (entropic benchmark run/compare/list)
├── runner.py           # Discovers identities, runs evaluations
├── checks.py           # Shared check primitives (also used by tests/model/)
├── layer1.py           # Raw llama-cpp performance tests
├── layer2.py           # Identity-driven quality tests
├── results.py          # Result storage, loading, comparison
└── report.py           # Human-readable output formatting
```

## Migration from `examples/moe-vram-benchmark/`

The current MoE benchmark becomes a historical artifact. Its hardcoded tests
migrate into identity benchmark definitions. The raw performance tests (load
times, GPU sweep, VRAM measurement) become Layer 1 of the new framework.

The `examples/moe-vram-benchmark/` directory can remain as a standalone script
for quick ad-hoc testing, but the canonical benchmark tool is `entropic benchmark`.

## Acceptance Criteria

- [ ] `entropic benchmark run <model.gguf>` produces a structured result file
- [ ] Results stored in `.entropic/benchmark/results/` with consistent schema
- [ ] `entropic benchmark compare` produces per-identity leaderboard
- [ ] Adding a new identity (with benchmark.yaml) automatically includes it
- [ ] Removing an identity automatically excludes it
- [ ] Stale results flagged when identity versions change
- [ ] Layer 1 (raw perf) works without any identities configured
- [ ] Shared check primitives importable by `tests/model/`
- [ ] No hardcoded model names, identity names, or quality prompts in framework

## Implementation Plan

### Phase 1: Core Framework + Layer 1
- `src/entropic/benchmark/` package structure
- Layer 1 runner: load times (cold/warm), swap latency (cold→active,
  warm→active, identity swap), tok/s, VRAM, GPU sweep, context scaling
- Result schema and storage
- CLI: `entropic benchmark run --layer1-only`
- Validates P1-022 (VRAM orchestration) — swap latency is the key metric

### Phase 2: Identity Discovery + Layer 2
- Identity benchmark.yaml schema
- Runtime identity discovery from engine config
- Check primitive library (shared with tests/model/)
- CLI: `entropic benchmark run` (full)

### Phase 3: Comparison + Historical
- Result comparison engine
- Staleness detection (identity version tracking)
- CLI: `entropic benchmark compare`, `entropic benchmark list`
- Human-readable report formatting

### Phase 4: Model Test Integration
- Refactor `tests/model/` to use shared check primitives
- Align model test prompts with identity benchmark definitions
- Single source of truth for "what does correct look like"

## Implementation Log

### 2026-03-03 — Layer 1 complete (P1-027 Track 3)

- [x] Package structure: `src/entropic/benchmark/__init__.py`, `types.py`, `gpu.py`, `runner.py`, `layer1.py`, `report.py`, `cli.py`
- [x] `types.py`: ModelSpec, LoadResult, InferenceResult, SwapResult, SweepPoint, Layer1Results
- [x] `gpu.py`: get_vram_mb() (nvidia-smi), get_gpu_info(), free_model() (GC + VRAM poll), get_block_count()
- [x] `runner.py`: BenchmarkRunner — timed_cold_load(), timed_swap(), timed_inference(), gpu_sweep()
- [x] `layer1.py`: run_layer1() orchestration, save_results(), SWAP_TARGET_MS=3000ms
- [x] `report.py`: Rich tables + plain-text fallback, results_to_json()
- [x] `cli.py`: `entropic benchmark run <model.gguf> --layer1-only`, `entropic benchmark sweep`
- [x] `src/entropic/cli.py`: registered `benchmark` subgroup
- [x] Unit tests: test_benchmark_gpu.py (get_vram_mb, get_gpu_info, free_model), test_benchmark_runner.py (BenchmarkRunner, sweep helpers)
- [x] Pre-commit: all hooks pass (ruff, flake8, pytest unit + model)
- [x] Merged to develop at v1.3.1
- **Decision:** Layer 1 uses LlamaCppBackend directly — no engine, no identities
- **Decision:** JSON saved to ~/.entropic/benchmark/results/ by default
- **Decision:** Patch bump (not minor) — benchmark is a diagnostic tool, not engine API
- **Files changed:** src/entropic/benchmark/ (7 files), src/entropic/cli.py, tests/unit/test_benchmark_gpu.py, tests/unit/test_benchmark_runner.py
- **Remaining (Layer 2):** identity-based evaluation — blocked on P1-024 (bundled identity library)

## References

- P1-20260302-024: Bundled identity library (identity definitions)
- P1-20260226-022: VRAM orchestration (load/swap mechanics)
- `examples/moe-vram-benchmark/`: Current benchmark (migration source)
- `tests/model/`: Current model tests (integration target)
