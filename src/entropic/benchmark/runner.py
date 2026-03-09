"""BenchmarkRunner: measurement primitives using LlamaCppBackend directly.

Uses the three-state lifecycle (warm/activate/deactivate) to measure
swap latency, validating P1-022 targets.
"""

import time
from pathlib import Path

from entropic.benchmark.gpu import free_model, get_block_count, get_vram_mb
from entropic.benchmark.types import (
    InferenceResult,
    LoadResult,
    ModelSpec,
    RawSwapResult,
    SweepPoint,
)
from entropic.config.schema import ModelConfig
from entropic.core.logging import get_logger
from entropic.inference.llama_cpp import LlamaCppBackend

logger = get_logger("benchmark.runner")

# Short prompt for throughput measurement — avoids prompt-length variance
_INFERENCE_PROMPT = "Explain in two sentences why the sky appears blue."


class BenchmarkRunner:
    """Measures raw model performance using LlamaCppBackend directly.

    Does not use AgentEngine, routing, or identities (performance benchmarks only).
    """

    def __init__(self, spec: ModelSpec) -> None:
        """Initialize runner.

        Args:
            spec: Model path and load parameters.
        """
        self._spec = spec

    def _make_backend(self, gpu_layers: int | None = None) -> LlamaCppBackend:
        """Create a fresh COLD backend for the model."""
        effective_layers = gpu_layers if gpu_layers is not None else self._spec.gpu_layers
        config = ModelConfig(
            path=self._spec.path,
            context_length=self._spec.context_length,
            gpu_layers=effective_layers,
        )
        return LlamaCppBackend(config=config, tier="benchmark")

    async def timed_cold_load(self) -> LoadResult:
        """Measure COLD → ACTIVE (full load from disk).

        Returns:
            LoadResult with phase="cold_to_active".
        """
        backend = self._make_backend()
        start = time.perf_counter()
        await backend.load()
        elapsed_ms = (time.perf_counter() - start) * 1000
        vram_used, _ = get_vram_mb()
        await backend.unload()
        if backend._model is not None:
            free_model(backend._model)
        logger.info(f"[bench] cold_to_active: {elapsed_ms:.0f}ms")
        return LoadResult(phase="cold_to_active", elapsed_ms=elapsed_ms, vram_used_mb=vram_used)

    async def timed_swap(self) -> RawSwapResult:
        """Measure the full three-state swap sequence.

        Sequence:
          1. COLD → WARM (warm_ms)
          2. WARM → ACTIVE (activate_ms)
          3. ACTIVE → WARM (deactivate_ms)
          4. WARM → ACTIVE again (reactivate_ms)

        Returns:
            RawSwapResult with timings for each transition.
        """
        backend = self._make_backend()

        t0 = time.perf_counter()
        await backend.warm()
        warm_ms = (time.perf_counter() - t0) * 1000
        vram_warm, _ = get_vram_mb()

        t1 = time.perf_counter()
        await backend.activate(self._spec.gpu_layers)
        activate_ms = (time.perf_counter() - t1) * 1000
        vram_active, _ = get_vram_mb()

        t2 = time.perf_counter()
        await backend.deactivate()
        deactivate_ms = (time.perf_counter() - t2) * 1000

        t3 = time.perf_counter()
        await backend.activate(self._spec.gpu_layers)
        reactivate_ms = (time.perf_counter() - t3) * 1000

        await backend.unload()
        if backend._model is not None:
            free_model(backend._model)

        logger.info(
            f"[bench] swap: warm={warm_ms:.0f}ms activate={activate_ms:.0f}ms "
            f"deactivate={deactivate_ms:.0f}ms reactivate={reactivate_ms:.0f}ms"
        )
        return RawSwapResult(
            warm_ms=warm_ms,
            activate_ms=activate_ms,
            deactivate_ms=deactivate_ms,
            reactivate_ms=reactivate_ms,
            vram_warm_mb=vram_warm,
            vram_active_mb=vram_active,
        )

    async def timed_inference(self, gpu_layers: int | None = None) -> InferenceResult:
        """Measure tok/s with the model loaded.

        Args:
            gpu_layers: GPU layers for this measurement. None = model default.

        Returns:
            InferenceResult with token count, elapsed time, and tok/s.
        """
        backend = self._make_backend(gpu_layers=gpu_layers)
        await backend.load()
        vram_used, _ = get_vram_mb()

        effective_layers = gpu_layers if gpu_layers is not None else self._spec.gpu_layers
        start = time.perf_counter()
        result = await backend.complete(_INFERENCE_PROMPT, max_tokens=128)
        elapsed_ms = (time.perf_counter() - start) * 1000

        await backend.unload()
        if backend._model is not None:
            free_model(backend._model)

        logger.info(
            f"[bench] inference: {result.token_count} tokens "
            f"{elapsed_ms:.0f}ms ({result.token_count / (elapsed_ms / 1000):.1f} tok/s)"
        )
        return InferenceResult(
            tokens=result.token_count,
            elapsed_ms=elapsed_ms,
            gpu_layers=effective_layers,
            vram_used_mb=vram_used,
        )

    async def gpu_sweep(
        self,
        step: int = 10,
        max_layers: int | None = None,
    ) -> list[SweepPoint]:
        """Measure tok/s and VRAM at each GPU layer count.

        Warms the model once (CPU), then cycles through activate/deactivate
        for each layer count to avoid repeated disk reads.

        Args:
            step: Layer count increment between sweep points.
            max_layers: Upper bound (None = model block count).

        Returns:
            List of SweepPoints, one per layer count tested.
        """
        block_count = get_block_count(str(self._spec.path))
        max_n = max_layers if max_layers is not None else block_count
        layers = _build_sweep_layers(step, max_n)

        backend = self._make_backend(gpu_layers=0)
        await backend.warm()

        points: list[SweepPoint] = []
        for n in layers:
            point = await self._sweep_one_layer(backend, n)
            points.append(point)
            if point.oom:
                break

        await backend.unload()
        if backend._model is not None:
            free_model(backend._model)

        return points

    async def _sweep_one_layer(self, backend: LlamaCppBackend, n: int) -> SweepPoint:
        """Activate, measure, and deactivate for a single GPU layer count."""
        load_start = time.perf_counter()
        try:
            await backend.activate(gpu_layers=n)
        except Exception as e:
            logger.warning(f"[bench] sweep n={n}: OOM or error: {e}")
            return SweepPoint(gpu_layers=n, load_ms=0, tok_s=0, tokens=0, vram_mb=0, oom=True)

        load_ms = (time.perf_counter() - load_start) * 1000
        vram_mb, _ = get_vram_mb()

        infer_start = time.perf_counter()
        result = await backend.complete(_INFERENCE_PROMPT, max_tokens=64)
        infer_ms = (time.perf_counter() - infer_start) * 1000

        tok_s = result.token_count / (infer_ms / 1000) if infer_ms > 0 else 0.0

        await backend.deactivate()

        logger.info(f"[bench] sweep n={n}: load={load_ms:.0f}ms tok/s={tok_s:.1f} vram={vram_mb}MB")
        return SweepPoint(
            gpu_layers=n,
            load_ms=load_ms,
            tok_s=tok_s,
            tokens=result.token_count,
            vram_mb=vram_mb,
        )


def _build_sweep_layers(step: int, max_n: int) -> list[int]:
    """Build list of GPU layer counts to test (0 + step increments + max_n)."""
    layers = [0] + list(range(step, max_n, step))
    if max_n not in layers:
        layers.append(max_n)
    return layers


def make_model_spec(path: Path, context_length: int = 4096, gpu_layers: int = -1) -> ModelSpec:
    """Convenience constructor for ModelSpec."""
    return ModelSpec(path=path, context_length=context_length, gpu_layers=gpu_layers)
