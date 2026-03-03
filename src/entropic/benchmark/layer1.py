"""Layer 1 benchmark orchestration: raw model performance."""

from collections.abc import Callable
from datetime import datetime, timezone
from pathlib import Path

from entropic import __version__
from entropic.benchmark.gpu import get_gpu_info
from entropic.benchmark.runner import BenchmarkRunner
from entropic.benchmark.types import Layer1Results, ModelSpec
from entropic.core.logging import get_logger

logger = get_logger("benchmark.layer1")

# P1-022 target: warm→active swap under this threshold
SWAP_TARGET_MS = 3000.0


async def run_layer1(
    spec: ModelSpec,
    *,
    sweep_step: int = 10,
    on_phase: Callable[[str], None] | None = None,
) -> Layer1Results:
    """Run the full Layer 1 benchmark sequence for one model.

    Measures:
      - Cold load (COLD → ACTIVE)
      - Three-state swap sequence (validates P1-022 targets)
      - Inference tok/s (with model loaded)
      - GPU layer sweep (tok/s + VRAM at each layer count)

    Args:
        spec: Model path and load parameters.
        sweep_step: n_gpu_layers increment for the GPU sweep.
        on_phase: Optional callback invoked with phase description before each phase.

    Returns:
        Layer1Results with all measurements populated.
    """
    logger.info(f"[layer1] Starting benchmark: {spec.path.name}")
    runner = BenchmarkRunner(spec)

    results = Layer1Results(
        model_path=spec.path,
        timestamp=datetime.now(timezone.utc).isoformat(),
        engine_version=__version__,
        gpu_info=get_gpu_info(),
    )

    if on_phase:
        on_phase("Phase 1/4: cold load")
    logger.info("[layer1] Phase 1/4: cold load")
    results.cold_load = await runner.timed_cold_load()

    if on_phase:
        on_phase("Phase 2/4: swap latency")
    logger.info("[layer1] Phase 2/4: swap latency")
    results.swap = await runner.timed_swap()
    _log_swap_verdict(results.swap.activate_ms, results.swap.reactivate_ms)

    if on_phase:
        on_phase("Phase 3/4: inference tok/s")
    logger.info("[layer1] Phase 3/4: inference tok/s")
    results.inference = await runner.timed_inference()

    if on_phase:
        on_phase(f"Phase 4/4: GPU sweep (step={sweep_step})")
    logger.info(f"[layer1] Phase 4/4: GPU sweep (step={sweep_step})")
    results.sweep = await runner.gpu_sweep(step=sweep_step)

    logger.info("[layer1] Benchmark complete")
    return results


def _log_swap_verdict(activate_ms: float, reactivate_ms: float) -> None:
    """Log whether each warm→active transition meets the P1-022 target.

    Both activate and reactivate are checked independently — the target
    applies per-swap, not to their combined sum.
    """
    for label, ms in [("activate", activate_ms), ("reactivate", reactivate_ms)]:
        if ms < SWAP_TARGET_MS:
            logger.info(f"[layer1] {label} target MET: {ms:.0f}ms < {SWAP_TARGET_MS:.0f}ms")
        else:
            logger.warning(f"[layer1] {label} target MISSED: {ms:.0f}ms >= {SWAP_TARGET_MS:.0f}ms")


def save_results(results: Layer1Results, output_dir: Path) -> Path:
    """Save Layer1Results to JSON in output_dir.

    Filename: {model_stem}-{date}.json

    Args:
        results: Completed benchmark results.
        output_dir: Directory to write JSON file.

    Returns:
        Path to the written file.
    """
    import json

    output_dir.mkdir(parents=True, exist_ok=True)
    date_str = results.timestamp[:10]  # YYYY-MM-DD
    filename = f"{results.model_path.stem}-{date_str}.json"
    out_path = output_dir / filename

    data = _results_to_dict(results)
    out_path.write_text(json.dumps(data, indent=2))
    logger.info(f"[layer1] Results saved: {out_path}")
    return out_path


def _results_to_dict(results: Layer1Results) -> dict:
    """Convert Layer1Results to a JSON-serializable dict."""
    data: dict = {
        "model": str(results.model_path),
        "timestamp": results.timestamp,
        "engine_version": results.engine_version,
        "gpu_info": results.gpu_info,
        "layer1": {},
    }

    if results.cold_load:
        data["layer1"]["cold_load"] = {
            "phase": results.cold_load.phase,
            "elapsed_ms": round(results.cold_load.elapsed_ms, 1),
            "vram_used_mb": results.cold_load.vram_used_mb,
        }

    if results.swap:
        swap = results.swap
        data["layer1"]["swap"] = {
            "warm_ms": round(swap.warm_ms, 1),
            "activate_ms": round(swap.activate_ms, 1),
            "deactivate_ms": round(swap.deactivate_ms, 1),
            "reactivate_ms": round(swap.reactivate_ms, 1),
            "activate_target_met": swap.activate_ms < SWAP_TARGET_MS,
            "reactivate_target_met": swap.reactivate_ms < SWAP_TARGET_MS,
            "vram_warm_mb": swap.vram_warm_mb,
            "vram_active_mb": swap.vram_active_mb,
        }

    if results.inference:
        inf = results.inference
        data["layer1"]["inference"] = {
            "tokens": inf.tokens,
            "elapsed_ms": round(inf.elapsed_ms, 1),
            "tok_s": round(inf.tok_s, 1),
            "gpu_layers": inf.gpu_layers,
            "vram_used_mb": inf.vram_used_mb,
        }

    if results.sweep:
        data["layer1"]["gpu_sweep"] = [
            {
                "gpu_layers": p.gpu_layers,
                "load_ms": round(p.load_ms, 1),
                "tok_s": round(p.tok_s, 1),
                "tokens": p.tokens,
                "vram_mb": p.vram_mb,
                "oom": p.oom,
            }
            for p in results.sweep
        ]

    return data
