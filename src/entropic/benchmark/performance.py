"""Performance benchmark: engine-level speed and VRAM measurement.

Measures cold start, steady-state inference tok/s, and model swap latency
through the engine's orchestrator — same code path as production. Timing
is read from ``GenerationResult`` fields populated by the orchestrator.
"""

from collections.abc import Callable
from datetime import datetime, timezone
from pathlib import Path

from entropic import __version__
from entropic.benchmark.gpu import get_gpu_info, get_vram_mb
from entropic.benchmark.types import InferenceResult, LoadResult, PerfResults, SwapResult
from entropic.config.loader import ConfigLoader
from entropic.config.schema import LibraryConfig
from entropic.core.base import Message, ModelTier
from entropic.core.logging import get_logger
from entropic.inference.orchestrator import ModelOrchestrator

logger = get_logger("benchmark.perf")

# P1-022 target: swap under this threshold
SWAP_TARGET_MS = 3000.0

# Standard prompt for throughput measurement
_INFERENCE_PROMPT = "Explain in two sentences why the sky appears blue."


async def run_performance(
    *,
    config: LibraryConfig | None = None,
    candidate_model: Path | None = None,
    tier_name: str | None = None,
    on_phase: Callable[[str], None] | None = None,
) -> PerfResults:
    """Run performance benchmark through the engine orchestrator.

    Phases:
      1. Cold start: generate() against an unloaded model (total_ms = load + inference)
      2. Inference tok/s: generate() again (model already active, swap_ms ≈ 0)
      3. Swap away: generate() against a tier on a different model file
      4. Swap back: generate() back to the original tier

    All timing comes from ``GenerationResult`` fields populated by the
    orchestrator during its normal ``generate()`` flow.
    """
    if config is None:
        config = ConfigLoader().load()

    target_tier = tier_name or config.models.default

    if candidate_model:
        tc = config.models.tiers.get(target_tier)
        if tc:
            tc.path = candidate_model.expanduser().resolve()

    model_path = Path(config.models.tiers[target_tier].path).expanduser()
    tc = config.models.tiers.get(target_tier)
    gpu_layers = tc.gpu_layers if tc else -1

    results = PerfResults(
        model_path=model_path,
        timestamp=datetime.now(timezone.utc).isoformat(),
        engine_version=__version__,
        gpu_info=get_gpu_info(),
    )

    orchestrator = ModelOrchestrator(config)
    await orchestrator.initialize()

    tier = orchestrator._find_tier(target_tier)
    if tier is None:
        logger.warning(
            f"[perf] Tier '{target_tier}' not found in orchestrator "
            f"(non-routable identities share models with routable tiers — "
            f"use the routable tier name for perf benchmarks)"
        )
        await orchestrator.shutdown()
        return results

    # Unload everything so we can measure cold start cleanly.
    for pool_backend in orchestrator._model_pool.values():
        if pool_backend.is_loaded:
            await pool_backend.unload()
    orchestrator._loaded_main_tier = None

    messages = [Message(role="user", content=_INFERENCE_PROMPT)]

    # Phase 1: Cold start (model unloaded → generate includes full load)
    if on_phase:
        on_phase("Phase 1/4: cold start")
    logger.info("[perf] Phase 1/4: cold start")

    gen = await orchestrator.generate(messages, tier=tier)
    vram_cold, _ = get_vram_mb()

    results.cold_load = LoadResult(
        phase="cold_start",
        elapsed_ms=gen.total_ms,
        swap_ms=gen.swap_ms,
        vram_used_mb=vram_cold,
    )
    logger.info(
        f"[perf] Cold start: {gen.total_ms:.0f}ms total "
        f"(swap/load={gen.swap_ms:.0f}ms, inference={gen.generation_time_ms}ms)"
    )

    # Phase 2: Steady-state inference (model already active)
    if on_phase:
        on_phase("Phase 2/4: inference tok/s")
    logger.info("[perf] Phase 2/4: inference tok/s")

    gen = await orchestrator.generate(messages, tier=tier)
    vram_active, _ = get_vram_mb()

    results.inference = InferenceResult(
        tokens=gen.token_count,
        elapsed_ms=gen.generation_time_ms,
        gpu_layers=gpu_layers,
        vram_used_mb=vram_active,
    )
    logger.info(
        f"[perf] Inference: {gen.token_count} tokens, "
        f"{gen.generation_time_ms}ms ({results.inference.tok_s:.1f} tok/s)"
    )

    # Phase 3 & 4: Swap latency
    if on_phase:
        on_phase("Phase 3/4: swap latency")
    logger.info("[perf] Phase 3/4: swap latency")

    swap_result = await _measure_swap(orchestrator, config, target_tier, messages)
    results.swap = swap_result
    if swap_result:
        _log_swap_result(swap_result)

    if on_phase:
        on_phase("Phase 4/4: complete")

    await orchestrator.shutdown()
    logger.info("[perf] Benchmark complete")
    return results


async def _measure_swap(
    orchestrator: ModelOrchestrator,
    config: LibraryConfig,
    home_tier_name: str,
    messages: list[Message],
) -> SwapResult | None:
    """Measure swap latency using generate() against different model tiers.

    Uses the orchestrator's normal swap path: generate() triggers model
    unload/load when the requested tier needs a different model file.
    """
    tiers = _resolve_swap_tiers(orchestrator, config, home_tier_name)
    if tiers is None:
        return None
    home_tier, other_tier = tiers

    vram_before, _ = get_vram_mb()

    # Swap away: home → other (triggers unload home model, load other model)
    gen_away = await orchestrator.generate(messages, tier=other_tier)

    # Swap back: other → home (triggers unload other model, load home model)
    gen_back = await orchestrator.generate(messages, tier=home_tier)
    vram_after, _ = get_vram_mb()

    logger.info(f"[perf] Swap: away={gen_away.swap_ms:.0f}ms back={gen_back.swap_ms:.0f}ms")

    return SwapResult(
        swap_away_ms=gen_away.swap_ms,
        swap_back_ms=gen_back.swap_ms,
        vram_before_mb=vram_before,
        vram_after_mb=vram_after,
    )


def _resolve_swap_tiers(
    orchestrator: ModelOrchestrator,
    config: LibraryConfig,
    home_tier_name: str,
) -> tuple[ModelTier, ModelTier] | None:
    """Find home and alternate tiers for swap measurement. Returns None if unavailable."""
    home_tier = orchestrator._find_tier(home_tier_name)
    if home_tier is None:
        return None

    home_path = config.models.tiers[home_tier_name].path
    other_name = _find_alternate_tier(orchestrator, config, home_tier_name, home_path)
    if other_name is None:
        logger.info("[perf] No alternate-model tier found, skipping swap measurement")
        return None

    other_tier = orchestrator._find_tier(other_name)
    return (home_tier, other_tier) if other_tier is not None else None


def _find_alternate_tier(
    orchestrator: ModelOrchestrator,
    config: LibraryConfig,
    home_name: str,
    home_path: Path,
) -> str | None:
    """Find a tier using a different model file for swap measurement."""
    for name, tc in config.models.tiers.items():
        if tc.path != home_path and name != home_name:
            if orchestrator._find_tier(name) is not None:
                return name
    return None


def _log_swap_result(swap: SwapResult) -> None:
    """Log whether swap transitions meet the P1-022 target."""
    for label, ms in [("swap-away", swap.swap_away_ms), ("swap-back", swap.swap_back_ms)]:
        if ms < SWAP_TARGET_MS:
            logger.info(f"[perf] {label} target MET: {ms:.0f}ms < {SWAP_TARGET_MS:.0f}ms")
        else:
            logger.warning(f"[perf] {label} target MISSED: {ms:.0f}ms >= {SWAP_TARGET_MS:.0f}ms")


def save_results(results: PerfResults, output_dir: Path) -> Path:
    """Save PerfResults to JSON, accumulating runs and computing averages.

    Layout: ``output_dir/perf/{model-stem}.json``

    Each file contains:
    - ``model``, ``gpu_info``: stable metadata
    - ``runs``: list of individual run results (appended each time)
    - ``average``: rolling averages across all runs
    """
    import json

    perf_dir = output_dir / "perf"
    perf_dir.mkdir(parents=True, exist_ok=True)
    out_path = perf_dir / f"{results.model_path.stem}.json"

    existing = _load_existing(out_path)
    run = _results_to_run(results)
    date_key = results.timestamp[:10]
    existing["runs"].setdefault(date_key, []).append(run)
    existing["model"] = str(results.model_path)
    existing["gpu_info"] = results.gpu_info
    existing["average"] = _compute_perf_average(existing["runs"])

    out_path.write_text(json.dumps(existing, indent=2))
    n = sum(len(v) for v in existing["runs"].values())
    logger.info(f"[perf] Saved: {out_path} ({n} run{'s' if n != 1 else ''})")
    return out_path


def _load_existing(path: Path) -> dict:
    """Load existing results file or return empty structure."""
    import json

    if path.exists():
        try:
            data = json.loads(path.read_text())
            if "runs" in data:
                return data
        except (json.JSONDecodeError, KeyError):
            logger.warning(f"[perf] Corrupt results file, starting fresh: {path}")
    return {"model": "", "gpu_info": {}, "runs": {}, "average": {}}


def _results_to_run(results: PerfResults) -> dict:
    """Convert a single PerfResults to a run dict."""
    run: dict = {
        "timestamp": results.timestamp,
        "engine_version": results.engine_version,
    }

    if results.cold_load:
        run["cold_start_ms"] = round(results.cold_load.elapsed_ms, 1)
        run["cold_start_swap_ms"] = round(results.cold_load.swap_ms, 1)
        run["cold_start_vram_mb"] = results.cold_load.vram_used_mb

    if results.inference:
        inf = results.inference
        run["inference_tok_s"] = round(inf.tok_s, 1)
        run["inference_tokens"] = inf.tokens
        run["inference_ms"] = round(inf.elapsed_ms, 1)
        run["gpu_layers"] = inf.gpu_layers
        run["inference_vram_mb"] = inf.vram_used_mb

    if results.swap:
        swap = results.swap
        run["swap_away_ms"] = round(swap.swap_away_ms, 1)
        run["swap_back_ms"] = round(swap.swap_back_ms, 1)

    return run


def _compute_perf_average(runs_by_date: dict[str, list[dict]]) -> dict:
    """Compute rolling averages across all runs (grouped by date)."""
    all_runs = [r for day_runs in runs_by_date.values() for r in day_runs]
    avg: dict = {"run_count": len(all_runs)}
    for key in [
        "cold_start_ms",
        "inference_tok_s",
        "inference_ms",
        "swap_away_ms",
        "swap_back_ms",
    ]:
        values = [r[key] for r in all_runs if key in r]
        if values:
            avg[key] = round(sum(values) / len(values), 1)
    return avg
