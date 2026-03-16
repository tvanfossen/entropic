"""Benchmark package for entropic-engine.

Performance: Engine-level speed (cold start, tok/s, swap latency) measured
             through the orchestrator's generate() path — same as production.
             GPU layer sweep uses raw backend for hardware planning.

Quality: Identity-driven output evaluation. Each identity ships benchmark
         definitions (prompts + checks) in its frontmatter. Shared check
         primitives in ``checks.py`` are used by both benchmark CLI and
         ``tests/model/``.
"""

from entropic.benchmark.checks import CheckResult, run_check, run_checks
from entropic.benchmark.types import (
    InferenceResult,
    LoadResult,
    ModelSpec,
    PerfResults,
    RawSwapResult,
    SwapResult,
    SweepPoint,
)

__all__ = [
    "CheckResult",
    "InferenceResult",
    "LoadResult",
    "ModelSpec",
    "PerfResults",
    "RawSwapResult",
    "SwapResult",
    "SweepPoint",
    "run_check",
    "run_checks",
]
