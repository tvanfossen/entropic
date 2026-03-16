"""Quality benchmark: identity-driven output evaluation via the engine.

Discovers benchmark definitions from bundled identity frontmatter, runs
prompts through the orchestrator (adapter + grammar + identity system prompt),
and evaluates output against check primitives from ``checks.py``.
"""

from __future__ import annotations

import json
from collections.abc import Callable
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import entropic
from entropic import __version__
from entropic.benchmark.checks import CheckResult, run_checks
from entropic.benchmark.gpu import get_gpu_info
from entropic.config.loader import ConfigLoader
from entropic.config.schema import EntropyConfig
from entropic.core.base import Message, ModelTier
from entropic.core.logging import get_logger
from entropic.inference.orchestrator import ModelOrchestrator
from entropic.prompts import BenchmarkSpec, IdentityFrontmatter, load_tier_identity

logger = get_logger("benchmark.quality")

_PROMPTS_DIR = Path(entropic.__file__).parent / "data" / "prompts"


# ---------------------------------------------------------------------------
# Result types
# ---------------------------------------------------------------------------


@dataclass
class PromptResult:
    """Result of one benchmark prompt for one identity."""

    prompt: str
    output: str
    checks: list[CheckResult]
    all_passed: bool
    total_ms: float = 0.0
    routing_ms: float = 0.0
    swap_ms: float = 0.0
    inference_ms: float = 0.0
    token_count: int = 0


@dataclass
class IdentityResult:
    """Benchmark results for one identity."""

    name: str
    model_path: str
    prompt_results: list[PromptResult] = field(default_factory=list)
    passed: int = 0
    failed: int = 0
    skipped: bool = False
    skip_reason: str = ""


@dataclass
class QualityResults:
    """Complete quality benchmark results."""

    timestamp: str
    engine_version: str
    gpu_info: dict[str, Any] = field(default_factory=dict)
    config_source: str = ""
    identities: list[IdentityResult] = field(default_factory=list)

    @property
    def total_passed(self) -> int:
        return sum(i.passed for i in self.identities)

    @property
    def total_failed(self) -> int:
        return sum(i.failed for i in self.identities)


# ---------------------------------------------------------------------------
# Identity discovery
# ---------------------------------------------------------------------------


def discover_benchmarks() -> list[tuple[str, IdentityFrontmatter, BenchmarkSpec]]:
    """Discover all identities that have benchmark definitions.

    Returns:
        List of (identity_name, frontmatter, benchmark_spec) tuples.
    """
    results = []
    for path in sorted(_PROMPTS_DIR.glob("identity_*.md")):
        try:
            fm, _ = load_tier_identity(path)
        except (ValueError, Exception) as e:
            logger.warning(f"Skipping {path.name}: {e}")
            continue
        if fm.benchmark and fm.benchmark.prompts:
            results.append((fm.name, fm, fm.benchmark))
    return results


# ---------------------------------------------------------------------------
# Benchmark execution
# ---------------------------------------------------------------------------


async def run_quality(
    *,
    config: EntropyConfig | None = None,
    candidate_model: Path | None = None,
    identity_filter: str | None = None,
    on_identity: Callable[[str], None] | None = None,
) -> QualityResults:
    """Run quality benchmarks through the engine.

    Two modes:
      - **Steady state** (default): Use configured model assignments from config.
      - **Candidate**: Override all tier model paths with ``candidate_model``
        to evaluate a new model against existing benchmark definitions.

    Args:
        config: Engine config. If None, loads from default config path.
        candidate_model: If set, override all tier model paths with this file.
        identity_filter: If set, only benchmark this identity.
        on_identity: Callback invoked with identity name before each.

    Returns:
        QualityResults with per-identity, per-prompt check results.
    """
    if config is None:
        config = ConfigLoader().load()

    if candidate_model:
        _override_model_paths(config, candidate_model)

    benchmarks = discover_benchmarks()
    if identity_filter:
        benchmarks = [(n, fm, bs) for n, fm, bs in benchmarks if n == identity_filter]

    if not benchmarks:
        logger.warning("[quality] No benchmark definitions found")
        return QualityResults(
            timestamp=datetime.now(timezone.utc).isoformat(),
            engine_version=__version__,
        )

    # Build tier list including ALL identities (not just routable ones).
    # The orchestrator normally excludes non-routable tiers from routing,
    # but benchmarks need backends + adapters for every identity.
    all_tiers = _build_all_tiers(config, benchmarks)
    orchestrator = ModelOrchestrator(config, tiers=all_tiers)
    await orchestrator.initialize()

    results = QualityResults(
        timestamp=datetime.now(timezone.utc).isoformat(),
        engine_version=__version__,
        gpu_info=get_gpu_info(),
        config_source=str(candidate_model) if candidate_model else "configured",
    )

    try:
        for name, fm, bench_spec in benchmarks:
            if on_identity:
                on_identity(name)
            logger.info(f"[quality] Benchmarking identity: {name}")

            tier = orchestrator._find_tier(name)
            if tier is None:
                logger.warning(f"[quality] Tier '{name}' not found in orchestrator, skipping")
                ir = IdentityResult(name=name, model_path="", skipped=True, skip_reason="no tier")
                results.identities.append(ir)
                continue

            model_path = _get_tier_model_path(config, name)
            identity_result = await _benchmark_identity(
                orchestrator, tier, fm, bench_spec, model_path
            )
            results.identities.append(identity_result)
    finally:
        await orchestrator.shutdown()

    return results


async def _benchmark_identity(
    orchestrator: ModelOrchestrator,
    tier: Any,
    fm: IdentityFrontmatter,
    bench_spec: BenchmarkSpec,
    model_path: str,
) -> IdentityResult:
    """Run all benchmark prompts for one identity through the orchestrator."""
    result = IdentityResult(name=fm.name, model_path=model_path)
    for bp in bench_spec.prompts:
        prompt_result = await _run_prompt(orchestrator, tier, fm, bp.prompt, bp.checks)
        result.prompt_results.append(prompt_result)
        if prompt_result.all_passed:
            result.passed += 1
        else:
            result.failed += 1
    return result


async def _run_prompt(
    orchestrator: ModelOrchestrator,
    tier: Any,
    fm: IdentityFrontmatter,
    prompt: str,
    check_specs: list[dict[str, Any]],
) -> PromptResult:
    """Run a single prompt through the orchestrator and evaluate checks.

    Builds the system prompt with identity + filtered tools, matching
    what the engine produces at runtime. This ensures benchmarks test
    model behavior with the same prompt the model sees in production.
    """
    tools = _load_tools_for_identity(fm)
    adapter = orchestrator.get_adapter(tier)
    enable_thinking = fm.enable_thinking if fm is not None else False
    system_prompt = adapter.format_system_prompt("", tools, enable_thinking=enable_thinking)

    messages = [
        Message(role="system", content=system_prompt),
        Message(role="user", content=prompt),
    ]
    gen_result = await orchestrator.generate(messages, tier=tier, identity_fm=fm)

    # Use raw_content (pre-parsing) so checks can match tool calls and text
    output = gen_result.raw_content or gen_result.content
    check_results = run_checks(output, check_specs)
    all_passed = all(c.passed for c in check_results)

    if not all_passed:
        failed = [c for c in check_results if not c.passed]
        logger.info(
            f"[quality] FAIL identity={fm.name} prompt={prompt!r:.60} "
            f"failures={[(c.check_type, c.detail) for c in failed]}"
        )

    return PromptResult(
        prompt=prompt,
        output=output,
        checks=check_results,
        all_passed=all_passed,
        total_ms=gen_result.total_ms,
        routing_ms=gen_result.routing_ms,
        swap_ms=gen_result.swap_ms,
        inference_ms=gen_result.generation_time_ms,
        token_count=gen_result.token_count,
    )


# ---------------------------------------------------------------------------
# Tool loading
# ---------------------------------------------------------------------------

_TOOLS_DIR = Path(entropic.__file__).parent / "data" / "tools"


def _load_tools_for_identity(fm: IdentityFrontmatter) -> list[dict[str, Any]]:
    """Load tool JSON definitions filtered by identity's allowed_tools.

    Reads tool definitions from the bundled data/tools/ directory,
    matching the server.tool_name format used in allowed_tools.
    Returns empty list if allowed_tools is None or empty.
    """
    if not fm.allowed_tools:
        return []

    tools = []
    for qualified_name in fm.allowed_tools:
        parts = qualified_name.split(".", 1)
        if len(parts) != 2:
            continue
        server, tool_name = parts
        tool_path = _TOOLS_DIR / server / f"{tool_name}.json"
        if not tool_path.exists():
            logger.warning("[quality] Tool definition not found: %s", tool_path)
            continue
        tool_def = json.loads(tool_path.read_text())
        # Prefix name with server for consistency with MCP naming
        tool_def["name"] = qualified_name
        tools.append(tool_def)

    return tools


# ---------------------------------------------------------------------------
# Config helpers
# ---------------------------------------------------------------------------


def _build_all_tiers(
    config: EntropyConfig,
    benchmarks: list[tuple[str, IdentityFrontmatter, BenchmarkSpec]],
) -> list[ModelTier]:
    """Build ModelTier objects for every identity being benchmarked.

    Includes non-routable identities that the orchestrator would normally skip.
    Always includes the default tier (orchestrator requires it to initialize).
    """
    benchmark_names = {name for name, _, _ in benchmarks}
    default_name = config.models.default
    needed = benchmark_names | {default_name}
    tiers = []
    for name in config.models.tiers:
        if name not in needed:
            continue
        # Find frontmatter from benchmark list first
        fm = next((f for n, f, _ in benchmarks if n == name), None)
        if fm is None and name == default_name:
            # Default tier not in benchmark list — load its frontmatter directly
            fm = _load_identity_frontmatter(name)
        if fm is None:
            continue
        tiers.append(ModelTier(name, focus=fm.focus, examples=fm.examples))
    return tiers


def _load_identity_frontmatter(name: str) -> IdentityFrontmatter | None:
    """Load identity frontmatter by name from bundled prompts."""
    path = _PROMPTS_DIR / f"identity_{name}.md"
    if not path.exists():
        logger.warning(f"[quality] No bundled identity file for '{name}'")
        return None
    try:
        fm, _ = load_tier_identity(path)
        return fm
    except (ValueError, Exception) as e:
        logger.warning(f"[quality] Failed to load identity '{name}': {e}")
        return None


def _override_model_paths(config: EntropyConfig, candidate: Path) -> None:
    """Override all tier model paths with the candidate model file."""
    resolved = candidate.expanduser().resolve()
    for tier_config in config.models.tiers.values():
        tier_config.path = resolved


def _get_tier_model_path(config: EntropyConfig, tier_name: str) -> str:
    """Get the configured model path for a tier."""
    tier_config = config.models.tiers.get(tier_name)
    if tier_config:
        return str(Path(tier_config.path).expanduser())
    return ""


# ---------------------------------------------------------------------------
# Serialization
# ---------------------------------------------------------------------------


def save_results(results: QualityResults, output_dir: Path) -> list[Path]:
    """Save QualityResults as one JSON per identity+model combo, accumulating runs.

    Layout: ``output_dir/quality/{identity}/{model-stem}.json``

    Each file contains:
    - ``identity``, ``model``, ``gpu_info``: stable metadata
    - ``runs``: list of individual run results (appended each time)
    - ``average``: rolling averages across all runs

    Returns:
        List of paths written.
    """
    import json

    saved: list[Path] = []
    for ir in results.identities:
        if ir.skipped:
            continue
        model_stem = Path(ir.model_path).stem if ir.model_path else "unknown"
        identity_dir = output_dir / "quality" / ir.name
        identity_dir.mkdir(parents=True, exist_ok=True)
        out_path = identity_dir / f"{model_stem}.json"

        existing = _load_existing_quality(out_path)
        run = _identity_result_to_run(ir, results)
        date_key = results.timestamp[:10]
        existing["runs"].setdefault(date_key, []).append(run)
        existing["identity"] = ir.name
        existing["model"] = ir.model_path
        existing["gpu_info"] = results.gpu_info
        existing["average"] = _compute_quality_average(existing["runs"])

        out_path.write_text(json.dumps(existing, indent=2))
        n = sum(len(v) for v in existing["runs"].values())
        saved.append(out_path)
        logger.info(f"[quality] Saved: {out_path} ({n} run{'s' if n != 1 else ''})")
    return saved


def _load_existing_quality(path: Path) -> dict[str, Any]:
    """Load existing quality results file or return empty structure."""
    import json

    if path.exists():
        try:
            data = json.loads(path.read_text())
            if "runs" in data:
                return data
        except (json.JSONDecodeError, KeyError):
            logger.warning(f"[quality] Corrupt results file, starting fresh: {path}")
    return {"identity": "", "model": "", "gpu_info": {}, "runs": {}, "average": {}}


def _identity_result_to_run(ir: IdentityResult, results: QualityResults) -> dict[str, Any]:
    """Convert a single IdentityResult to a run dict."""
    return {
        "timestamp": results.timestamp,
        "engine_version": results.engine_version,
        "passed": ir.passed,
        "failed": ir.failed,
        "prompts": [
            {
                "prompt": pr.prompt,
                "output": pr.output,
                "all_passed": pr.all_passed,
                "total_ms": round(pr.total_ms, 1),
                "routing_ms": round(pr.routing_ms, 1),
                "swap_ms": round(pr.swap_ms, 1),
                "inference_ms": round(pr.inference_ms, 1),
                "token_count": pr.token_count,
                "checks": [
                    {
                        "type": cr.check_type,
                        "passed": cr.passed,
                        "detail": cr.detail,
                    }
                    for cr in pr.checks
                ],
            }
            for pr in ir.prompt_results
        ],
    }


def _compute_quality_average(runs_by_date: dict[str, list[dict[str, Any]]]) -> dict[str, Any]:
    """Compute rolling averages across all quality runs (grouped by date)."""
    all_runs = [r for day_runs in runs_by_date.values() for r in day_runs]
    total_passed = sum(r.get("passed", 0) for r in all_runs)
    total_failed = sum(r.get("failed", 0) for r in all_runs)
    total = total_passed + total_failed

    avg: dict[str, Any] = {
        "run_count": len(all_runs),
        "pass_rate": round(total_passed / total, 3) if total > 0 else 0.0,
        "total_passed": total_passed,
        "total_failed": total_failed,
    }
    _add_timing_averages(avg, all_runs)
    return avg


def _add_timing_averages(avg: dict[str, Any], runs: list[dict[str, Any]]) -> None:
    """Extract and average timing fields from prompt-level data."""
    timing_fields = {
        "total_ms": "avg_total_ms",
        "inference_ms": "avg_inference_ms",
        "swap_ms": "avg_swap_ms",
    }
    for src_key, avg_key in timing_fields.items():
        vals = [p[src_key] for r in runs for p in r.get("prompts", []) if src_key in p]
        if vals:
            avg[avg_key] = round(sum(vals) / len(vals), 1)
