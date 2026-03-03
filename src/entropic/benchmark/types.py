"""Data types for benchmark results."""

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class ModelSpec:
    """Specification for a model to benchmark."""

    path: Path
    context_length: int = 4096
    gpu_layers: int = -1  # -1 = all layers; overridden per sweep point


@dataclass
class LoadResult:
    """Timing for a model load operation."""

    phase: str  # "cold_to_active", "warm_only", "activate_only"
    elapsed_ms: float
    vram_used_mb: int = 0
    notes: str = ""


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
    """Timing for the three-state lifecycle transitions (validates P1-022).

    Measures each state transition in isolation:
      warm_ms      — COLD → WARM (disk read + mlock into CPU RAM)
      activate_ms  — WARM → ACTIVE (PCIe transfer to GPU)
      deactivate_ms — ACTIVE → WARM (GPU release, CPU pages retained)
      reactivate_ms — WARM → ACTIVE again (should be ≤ activate_ms)

    Target: activate_ms + deactivate_ms + reactivate_ms < 3000ms for 21GB model.
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
class Layer1Results:
    """Aggregated Layer 1 benchmark results for one model."""

    model_path: Path
    timestamp: str
    engine_version: str
    gpu_info: dict[str, Any] = field(default_factory=dict)
    cold_load: LoadResult | None = None
    swap: SwapResult | None = None
    inference: InferenceResult | None = None
    sweep: list[SweepPoint] = field(default_factory=list)
