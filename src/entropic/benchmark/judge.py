"""Post-hoc LLM judge for quality benchmark results.

Loads the primary (35B) model once and grades all unjudged quality runs
in batch. Grades are injected into existing quality JSON files per-run.
The 35B model's own results are flagged for external review.
"""

from __future__ import annotations

import json
from collections.abc import Callable
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from entropic.config.loader import ConfigLoader
from entropic.config.schema import EntropyConfig
from entropic.core.base import Message, ModelTier
from entropic.core.logging import get_logger
from entropic.inference.orchestrator import ModelOrchestrator
from entropic.prompts import IdentityFrontmatter, load_tier_identity

logger = get_logger("benchmark.judge")

_JUDGE_IDENTITY = "benchmark_judge"


@dataclass
class JudgeResult:
    """Result of judging one run."""

    identity: str
    model: str
    grade: str
    reason: str
    skipped: bool = False
    skip_reason: str = ""


@dataclass
class JudgeResults:
    """Aggregate results from a judging session."""

    timestamp: str
    graded: list[JudgeResult] = field(default_factory=list)
    skipped: int = 0
    errors: int = 0


def _find_unjudged_runs(
    results_dir: Path,
    identity_filter: str | None = None,
    model_filter: str | None = None,
) -> list[tuple[Path, str, str, list[dict[str, Any]]]]:
    """Scan quality results for runs without a judge field.

    Returns list of (file_path, identity_name, date_key, unjudged_runs).
    """
    quality_dir = results_dir / "quality"
    if not quality_dir.exists():
        return []

    found: list[tuple[Path, str, str, list[dict[str, Any]]]] = []
    for identity_dir in sorted(quality_dir.iterdir()):
        if not identity_dir.is_dir():
            continue
        identity_name = identity_dir.name
        if identity_filter and identity_name != identity_filter:
            continue
        _scan_identity_dir(identity_dir, identity_name, model_filter, found)
    return found


def _scan_identity_dir(
    identity_dir: Path,
    identity_name: str,
    model_filter: str | None,
    found: list[tuple[Path, str, str, list[dict[str, Any]]]],
) -> None:
    """Scan one identity directory for unjudged model result files."""
    for model_file in sorted(identity_dir.glob("*.json")):
        if model_filter and model_filter not in model_file.stem:
            continue
        data = _load_json(model_file)
        if not data or "runs" not in data:
            continue
        for date_key, day_runs in data["runs"].items():
            unjudged = [r for r in day_runs if "judge" not in r]
            if unjudged:
                found.append((model_file, identity_name, date_key, unjudged))


def _load_json(path: Path) -> dict[str, Any] | None:
    """Load a JSON file, returning None on error."""
    try:
        return json.loads(path.read_text())
    except (json.JSONDecodeError, OSError) as e:
        logger.warning(f"[judge] Failed to read {path}: {e}")
        return None


@dataclass
class _JudgeContext:
    """Bundle of judge orchestrator state passed through grading functions."""

    orchestrator: ModelOrchestrator
    judge_tier: ModelTier
    judge_fm: IdentityFrontmatter
    judge_model_path: str
    on_grading: Callable[[str, int, int], None] | None = None
    total_runs: int = 0
    graded_count: int = 0


async def run_judge(
    *,
    config: EntropyConfig | None = None,
    results_dir: Path | None = None,
    identity_filter: str | None = None,
    model_filter: str | None = None,
    on_grading: Callable[[str, int, int], None] | None = None,
) -> JudgeResults:
    """Grade unjudged quality benchmark runs using the primary model.

    Loads the judge model once, iterates all unjudged runs, grades each,
    and injects grades into the existing JSON files.

    Args:
        config: Engine config. If None, loads from default config path.
        results_dir: Directory containing benchmark results.
        identity_filter: Only judge this identity's runs.
        model_filter: Only judge runs from models matching this substring.
        on_grading: Callback(identity, current, total) for progress.

    Returns:
        JudgeResults with grading summary.
    """
    if config is None:
        config = ConfigLoader().load()
    if results_dir is None:
        results_dir = Path("benchmark") / "results"

    unjudged = _find_unjudged_runs(results_dir, identity_filter, model_filter)
    if not unjudged:
        logger.info("[judge] No unjudged runs found")
        return JudgeResults(timestamp=datetime.now(timezone.utc).isoformat())

    total_runs = sum(len(runs) for _, _, _, runs in unjudged)
    logger.info(f"[judge] Found {total_runs} unjudged runs across {len(unjudged)} files")

    # Determine which model is the judge (primary tier's model path)
    judge_model_path = _get_judge_model_path(config)

    # Set up orchestrator with judge identity
    judge_tier, orchestrator, judge_fm = await _create_judge_orchestrator(config)

    judge_ctx = _JudgeContext(
        orchestrator=orchestrator,
        judge_tier=judge_tier,
        judge_fm=judge_fm,
        judge_model_path=judge_model_path,
        on_grading=on_grading,
        total_runs=total_runs,
    )
    results = JudgeResults(timestamp=datetime.now(timezone.utc).isoformat())

    try:
        for file_path, identity_name, date_key, _unjudged_refs in unjudged:
            await _judge_file(judge_ctx, results, file_path, identity_name, date_key)
    finally:
        await orchestrator.shutdown()

    logger.info(
        f"[judge] Complete: {len(results.graded)} graded, "
        f"{results.skipped} self-grade skipped, {results.errors} errors"
    )
    return results


async def _judge_file(
    ctx: _JudgeContext,
    results: JudgeResults,
    file_path: Path,
    identity_name: str,
    date_key: str,
) -> None:
    """Grade all unjudged runs in one quality result file."""
    data = _load_json(file_path)
    if data is None:
        return

    run_model = data.get("model", "")
    is_self_judge = _is_same_model(run_model, ctx.judge_model_path)

    for run in data.get("runs", {}).get(date_key, []):
        if "judge" in run:
            continue
        ctx.graded_count += 1
        if ctx.on_grading:
            ctx.on_grading(identity_name, ctx.graded_count, ctx.total_runs)

        if is_self_judge:
            jr = _mark_self_judge(run, identity_name, run_model, ctx, results.timestamp)
            results.skipped += 1
        else:
            jr = await _grade_run(ctx, identity_name, run, results.timestamp)
            if jr.skipped:
                results.errors += 1
        results.graded.append(jr)

    _save_json(file_path, data)


def _mark_self_judge(
    run: dict[str, Any],
    identity_name: str,
    run_model: str,
    ctx: _JudgeContext,
    timestamp: str,
) -> JudgeResult:
    """Flag a run as needing external review (judge model == run model)."""
    run["judge"] = {
        "grade": "external_review",
        "reason": "35B cannot self-grade — flag for human/external review",
        "timestamp": timestamp,
        "grader": Path(ctx.judge_model_path).stem,
    }
    return JudgeResult(
        identity=identity_name,
        model=run_model,
        grade="external_review",
        reason="self-grade skipped",
        skipped=True,
        skip_reason="same model as judge",
    )


async def _grade_run(
    ctx: _JudgeContext,
    identity_name: str,
    run: dict[str, Any],
    timestamp: str,
) -> JudgeResult:
    """Grade a single benchmark run."""
    model = run.get("model", "unknown")

    prompts_data = run.get("prompts", [])
    if not prompts_data:
        run["judge"] = {
            "grade": "skip",
            "reason": "no prompt data in run",
            "timestamp": timestamp,
        }
        return JudgeResult(
            identity=identity_name,
            model=model,
            grade="skip",
            reason="no prompts",
            skipped=True,
            skip_reason="no prompt data",
        )

    # Grade each prompt's output, take the worst grade
    grades: list[str] = []
    reasons: list[str] = []

    for prompt_data in prompts_data:
        prompt = prompt_data.get("prompt", "")
        output = prompt_data.get("output", "")
        if not prompt or not output:
            continue

        grade, reason = await _judge_single_output(ctx, identity_name, prompt, output)
        grades.append(grade)
        reasons.append(f"{prompt[:60]}: {grade} — {reason}")

    if not grades:
        run["judge"] = {
            "grade": "skip",
            "reason": "no prompt/output pairs to grade",
            "timestamp": timestamp,
        }
        return JudgeResult(
            identity=identity_name,
            model=model,
            grade="skip",
            reason="no outputs",
            skipped=True,
            skip_reason="empty prompts",
        )

    overall, overall_reason = _compute_overall_grade(grades, reasons)

    run["judge"] = {
        "grade": overall,
        "reason": overall_reason,
        "timestamp": timestamp,
        "grader": Path(ctx.judge_model_path).stem,
        "per_prompt": [{"grade": g, "reason": r} for g, r in zip(grades, reasons, strict=False)],
    }

    return JudgeResult(identity=identity_name, model=model, grade=overall, reason=overall_reason)


def _compute_overall_grade(grades: list[str], reasons: list[str]) -> tuple[str, str]:
    """Compute overall grade (worst of individual grades) and combined reason."""
    grade_order = ["A", "B", "C", "D", "F"]
    overall = max(grades, key=lambda g: grade_order.index(g) if g in grade_order else 99)
    return overall, "; ".join(reasons)


async def _judge_single_output(
    ctx: _JudgeContext,
    identity_name: str,
    prompt: str,
    output: str,
) -> tuple[str, str]:
    """Judge a single prompt/output pair. Returns (grade, reason)."""
    judge_prompt = (
        f"Grade this model output.\n\n"
        f"Identity: {identity_name}\n"
        f"Prompt: {prompt}\n\n"
        f"Model output:\n{output}\n\n"
        f"Respond with JSON containing grade (A/B/C/D/F) and reason."
    )

    messages = [Message(role="user", content=judge_prompt)]
    try:
        result = await ctx.orchestrator.generate(
            messages, tier=ctx.judge_tier, identity_fm=ctx.judge_fm
        )
        return _parse_judge_response(result.content)
    except Exception as e:
        logger.warning(f"[judge] Generation failed for {identity_name}: {e}")
        return "F", f"judge error: {e}"


def _parse_judge_response(content: str) -> tuple[str, str]:
    """Parse GBNF-constrained judge output into (grade, reason)."""
    try:
        data = json.loads(content)
        grade = data.get("grade", "F")
        reason = data.get("reason", "no reason given")
        if grade not in ("A", "B", "C", "D", "F"):
            return "F", f"invalid grade: {grade}"
        return grade, reason
    except (json.JSONDecodeError, KeyError) as e:
        logger.warning(f"[judge] Failed to parse response: {content!r:.200}: {e}")
        return "F", f"parse error: {e}"


def _get_judge_model_path(config: EntropyConfig) -> str:
    """Get the model path for the primary/judge tier."""
    default_tier = config.models.default
    tc = config.models.tiers.get(default_tier)
    return str(Path(tc.path).expanduser().resolve()) if tc else ""


def _is_same_model(run_model: str, judge_model: str) -> bool:
    """Check if the run's model is the same as the judge model."""
    if not run_model or not judge_model:
        return False
    return Path(run_model).resolve() == Path(judge_model).resolve()


async def _create_judge_orchestrator(
    config: EntropyConfig,
) -> tuple[ModelTier, ModelOrchestrator, IdentityFrontmatter]:
    """Create an orchestrator with the judge identity loaded.

    Returns the judge tier, orchestrator, and judge frontmatter
    (needed for grammar + enable_thinking resolution during generate).
    """
    import entropic

    judge_path = (
        Path(entropic.__file__).parent / "data" / "prompts" / f"identity_{_JUDGE_IDENTITY}.md"
    )
    fm, _ = load_tier_identity(judge_path)

    judge_tier = ModelTier(_JUDGE_IDENTITY, focus=fm.focus, examples=[])

    # Include judge tier + default tier (orchestrator requires default to init)
    default_name = config.models.default
    tiers = [judge_tier]

    # Add default tier if different from judge
    if default_name != _JUDGE_IDENTITY:
        default_path = Path(entropic.__file__).parent / "data" / "prompts"
        default_fm_path = default_path / f"identity_{default_name}.md"
        if default_fm_path.exists():
            dfm, _ = load_tier_identity(default_fm_path)
            tiers.append(ModelTier(default_name, focus=dfm.focus, examples=dfm.examples))

    # Judge uses the primary (default) model — ensure it has a config entry
    if _JUDGE_IDENTITY not in config.models.tiers:
        default_tc = config.models.tiers.get(default_name)
        if default_tc:
            config.models.tiers[_JUDGE_IDENTITY] = default_tc

    orchestrator = ModelOrchestrator(config, tiers=tiers)
    await orchestrator.initialize()

    return judge_tier, orchestrator, fm


def _save_json(path: Path, data: dict[str, Any]) -> None:
    """Write JSON data back to file."""
    path.write_text(json.dumps(data, indent=2))
    logger.info(f"[judge] Updated: {path}")
