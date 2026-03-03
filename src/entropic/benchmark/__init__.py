"""Benchmark package for entropic-engine.

Layer 1: Raw llama-cpp-python performance (load times, tok/s, swap latency,
         GPU layer sweep, VRAM consumption). No engine or identity system needed.

Layer 2 (future): Identity-driven quality evaluation via benchmark.yaml
                  definitions shipped with each identity.
"""

from entropic.benchmark.types import (
    InferenceResult,
    Layer1Results,
    LoadResult,
    ModelSpec,
    SwapResult,
    SweepPoint,
)

__all__ = [
    "InferenceResult",
    "Layer1Results",
    "LoadResult",
    "ModelSpec",
    "SwapResult",
    "SweepPoint",
]
