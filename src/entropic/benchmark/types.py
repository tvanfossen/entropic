"""Data types for benchmark results."""

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class ModelSpec:
    """Specification for a model to benchmark (used by runner/sweep)."""

    path: Path
    context_length: int = 4096
    gpu_layers: int = -1  # -1 = all layers; overridden per sweep point


@dataclass
class LoadResult:
    """Timing for a model load operation."""

    phase: str  # "cold_start"
    elapsed_ms: float  # total wall clock (load + inference)
    swap_ms: float = 0.0  # time spent in model swap/load
    vram_used_mb: int = 0


@dataclass
class InferenceResult:
    """Throughput for an inference run."""

    tokens: int
    elapsed_ms: float
    gpu_layers: int = -1
    vram_used_mb: int = 0

    @property
    def tok_s(self) -> float:
        """Tokens per second."""
        return self.tokens / (self.elapsed_ms / 1000) if self.elapsed_ms > 0 else 0.0


@dataclass
class SwapResult:
    """Timing for model swap measured via generate().

    swap_away_ms — time to swap from home model to alternate model
    swap_back_ms — time to swap back from alternate to home model

    These measure the orchestrator's _get_model() path which handles
    unload/load internally.
    """

    swap_away_ms: float
    swap_back_ms: float
    vram_before_mb: int = 0
    vram_after_mb: int = 0


@dataclass
class RawSwapResult:
    """Raw backend lifecycle transition timings (used by runner/sweep).

    Measures each state transition in isolation via direct backend calls:
      warm_ms      — COLD → WARM (disk read + mlock into CPU RAM)
      activate_ms  — WARM → ACTIVE (PCIe transfer to GPU)
      deactivate_ms — ACTIVE → WARM (GPU release, CPU pages retained)
      reactivate_ms — WARM → ACTIVE again (should be ≤ activate_ms)
    """

    warm_ms: float
    activate_ms: float
    deactivate_ms: float
    reactivate_ms: float
    vram_warm_mb: int = 0
    vram_active_mb: int = 0


@dataclass
class SweepPoint:
    """Single data point in a GPU layer sweep."""

    gpu_layers: int
    load_ms: float
    tok_s: float
    tokens: int
    vram_mb: int
    oom: bool = False
    notes: str = ""


@dataclass
class PerfResults:
    """Aggregated performance benchmark results for one model."""

    model_path: Path
    timestamp: str
    engine_version: str
    gpu_info: dict[str, Any] = field(default_factory=dict)
    cold_load: LoadResult | None = None
    inference: InferenceResult | None = None
    swap: SwapResult | None = None
    sweep: list[SweepPoint] = field(default_factory=list)
