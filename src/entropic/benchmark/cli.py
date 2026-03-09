"""CLI commands for benchmark: entropic benchmark run/sweep/list."""

import asyncio
import time
from collections.abc import Callable
from contextlib import nullcontext
from pathlib import Path
from typing import Any

import click


def _make_progress_callback():
    """Return (on_phase callback, progress context manager).

    Uses Rich Progress if available, falls back to click.echo status lines.
    """
    try:
        from rich.progress import BarColumn, Progress, SpinnerColumn, TextColumn, TimeElapsedColumn

        progress = Progress(
            SpinnerColumn(),
            TextColumn("[bold cyan]{task.description}"),
            BarColumn(bar_width=30),
            TextColumn("[progress.percentage]{task.percentage:>3.0f}%"),
            TimeElapsedColumn(),
        )
        task_id = progress.add_task("Starting...", total=4)

        def on_phase(description: str) -> None:
            progress.update(task_id, advance=1, description=description)

        return on_phase, progress
    except ImportError:

        def on_phase_plain(description: str) -> None:
            click.echo(f"  {description}...")

        return on_phase_plain, nullcontext()


@click.group()
def benchmark() -> None:
    """Benchmark engine performance and identity quality."""


@benchmark.command()
@click.option(
    "--candidate",
    type=click.Path(exists=True, path_type=Path),
    default=None,
    help="Test a candidate model against existing benchmarks (overrides configured paths).",
)
@click.option(
    "--perf-only",
    is_flag=True,
    default=False,
    help="Run only performance benchmarks (cold start, tok/s, swap latency).",
)
@click.option(
    "--quality-only",
    is_flag=True,
    default=False,
    help="Run only quality benchmarks (identity output evaluation).",
)
@click.option(
    "--identity",
    default=None,
    help="Run benchmarks for a specific identity only (filters both perf and quality).",
)
def run(
    candidate: Path | None,
    perf_only: bool,
    quality_only: bool,
    identity: str | None,
) -> None:
    """Run engine benchmarks.

    Steady state (default): benchmark configured models against bundled identities.
    Candidate mode (--candidate): test a new model against existing benchmarks.

    By default runs both performance and quality. Use --perf-only or --quality-only
    to run just one.
    """
    default_dir = Path("benchmark") / "results"
    run_perf = not quality_only
    run_qual = not perf_only

    if run_perf:
        _run_perf(candidate, identity, default_dir)

    if run_qual:
        _run_quality(candidate, identity, default_dir)


def _run_perf(candidate: Path | None, identity: str | None, default_dir: Path) -> None:
    """Execute performance benchmark through the engine."""
    from entropic.benchmark.performance import run_performance, save_results
    from entropic.benchmark.report import print_perf_report

    on_phase, progress_ctx = _make_progress_callback()
    with progress_ctx:
        results = asyncio.run(
            run_performance(candidate_model=candidate, tier_name=identity, on_phase=on_phase)
        )
    print_perf_report(results)

    saved = save_results(results, default_dir)
    click.echo(f"\nPerformance results saved: {saved}")


def _run_quality(candidate: Path | None, identity_filter: str | None, default_dir: Path) -> None:
    """Execute quality benchmark through the engine."""
    from entropic.benchmark.quality import run_quality, save_results
    from entropic.benchmark.report import print_quality_report

    on_identity, ctx = _make_quality_callback()
    with ctx:
        results = asyncio.run(
            run_quality(
                candidate_model=candidate,
                identity_filter=identity_filter,
                on_identity=on_identity,
            )
        )
    print_quality_report(results)

    saved = save_results(results, default_dir)
    for path in saved:
        click.echo(f"  Quality saved: {path}")


def _make_quality_callback() -> tuple[Callable[[str], None], Any]:
    """Create a Rich status callback for quality benchmarks."""
    try:
        from rich.console import Console
        from rich.live import Live

        console = Console()
        start_time = time.perf_counter()
        live = Live(console=console, refresh_per_second=2)

        def on_identity(name: str) -> None:
            elapsed = time.perf_counter() - start_time
            live.update(f"  [bold cyan]Quality:[/bold cyan] {name}  [{elapsed:.0f}s]")

        return on_identity, live
    except ImportError:

        def on_identity_plain(name: str) -> None:
            click.echo(f"  Quality: {name}...")

        return on_identity_plain, nullcontext()


@benchmark.command()
@click.argument("model_path", type=click.Path(exists=True, path_type=Path))
@click.option(
    "--context-length",
    "-c",
    default=4096,
    show_default=True,
    help="Context length for the model.",
)
@click.option(
    "--step",
    default=10,
    show_default=True,
    help="Layer increment between sweep points.",
)
@click.option(
    "--max-layers",
    default=None,
    type=int,
    help="Upper bound on GPU layers (default: model block count).",
)
@click.option(
    "--output",
    "-o",
    type=click.Path(path_type=Path),
    default=None,
    help="Write JSON results to this path.",
)
def sweep(
    model_path: Path,
    context_length: int,
    step: int,
    max_layers: int | None,
    output: Path | None,
) -> None:
    """Run GPU layer sweep for MODEL_PATH.

    Loads the engine's router (to occupy its VRAM), then sweeps the target
    model through GPU layer counts measuring tok/s and VRAM at each point.
    Finds the maximum layers that fit alongside the router.
    """
    from entropic.benchmark.report import _print_plain, _print_rich_sweep
    from entropic.benchmark.runner import BenchmarkRunner, make_model_spec

    spec = make_model_spec(path=model_path, context_length=context_length)
    runner = BenchmarkRunner(spec)
    points = asyncio.run(runner.gpu_sweep(step=step, max_layers=max_layers))

    try:
        from rich.console import Console
        from rich.table import Table

        from entropic.benchmark.types import PerfResults

        fake = PerfResults(model_path=model_path, timestamp="", engine_version="", sweep=points)
        _print_rich_sweep(fake, Console(), Table)
    except ImportError:
        from entropic.benchmark.types import PerfResults

        fake = PerfResults(model_path=model_path, timestamp="", engine_version="", sweep=points)
        _print_plain(fake)

    if output:
        import json

        data = [
            {
                "gpu_layers": p.gpu_layers,
                "load_ms": round(p.load_ms, 1),
                "tok_s": round(p.tok_s, 1),
                "tokens": p.tokens,
                "vram_mb": p.vram_mb,
                "oom": p.oom,
            }
            for p in points
        ]
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(data, indent=2))
        click.echo(f"\nResults saved: {output}")


@benchmark.command()
@click.option(
    "--identity",
    default=None,
    help="Only judge runs for this identity.",
)
@click.option(
    "--model",
    default=None,
    help="Only judge runs from models matching this substring.",
)
@click.option(
    "--results-dir",
    type=click.Path(path_type=Path),
    default=None,
    help="Results directory (default: benchmark/results).",
)
def judge(
    identity: str | None,
    model: str | None,
    results_dir: Path | None,
) -> None:
    """Grade unjudged quality benchmark runs using the primary (35B) model.

    Loads the judge model once, grades all unjudged runs in batch,
    and injects letter grades (A-F) into existing quality JSON files.
    Runs from the judge's own model are flagged for external review.
    """
    from entropic.benchmark.judge import run_judge

    on_grading, ctx = _make_judge_callback()
    with ctx:
        results = asyncio.run(
            run_judge(
                identity_filter=identity,
                model_filter=model,
                results_dir=results_dir,
                on_grading=on_grading,
            )
        )

    graded = len(results.graded)
    click.echo(f"\nJudge complete: {graded} runs graded, {results.skipped} self-grade skipped")

    # Summary by grade
    grade_counts: dict[str, int] = {}
    for jr in results.graded:
        grade_counts[jr.grade] = grade_counts.get(jr.grade, 0) + 1
    if grade_counts:
        click.echo("Grade distribution:")
        for grade in ["A", "B", "C", "D", "F", "external_review", "skip"]:
            count = grade_counts.get(grade, 0)
            if count:
                click.echo(f"  {grade}: {count}")


def _make_judge_callback() -> tuple[Callable[[str, int, int], None], Any]:
    """Create progress callback for judge."""
    try:
        from rich.console import Console
        from rich.live import Live

        console = Console()
        live = Live(console=console, refresh_per_second=2)

        def on_grading(identity: str, current: int, total: int) -> None:
            live.update(f"  [bold cyan]Judging:[/bold cyan] {identity} ({current}/{total})")

        return on_grading, live
    except ImportError:

        def on_grading_plain(identity: str, current: int, total: int) -> None:
            click.echo(f"  Judging: {identity} ({current}/{total})...")

        return on_grading_plain, nullcontext()


@benchmark.command("list")
def list_benchmarks() -> None:
    """List identities with benchmark definitions."""
    from entropic.benchmark.quality import discover_benchmarks

    benchmarks = discover_benchmarks()
    if not benchmarks:
        click.echo("No identities have benchmark definitions.")
        return

    click.echo(f"\n{len(benchmarks)} identities with benchmarks:\n")
    for name, fm, spec in benchmarks:
        n_prompts = len(spec.prompts)
        grammar = fm.grammar or "none"
        click.echo(f"  {name:20s}  {n_prompts} prompts  grammar={grammar}")
