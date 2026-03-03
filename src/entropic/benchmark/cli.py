"""CLI commands for benchmark: entropic benchmark run/sweep."""

import asyncio
from contextlib import nullcontext
from pathlib import Path

import click


def _make_progress_callback():
    """Return (on_phase callback, progress context manager).

    Uses Rich Progress if available, falls back to click.echo status lines.
    The context manager must be entered before the callback is used.
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
    """Measure raw model performance (Layer 1: no engine, no identities)."""


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
    "--gpu-layers",
    "-g",
    default=-1,
    show_default=True,
    help="GPU layers (-1 = all).",
)
@click.option(
    "--sweep-step",
    default=10,
    show_default=True,
    help="Layer increment for GPU sweep.",
)
@click.option(
    "--output",
    "-o",
    type=click.Path(path_type=Path),
    default=None,
    help="Write JSON results to this path.",
)
@click.option(
    "--layer1-only",
    is_flag=True,
    default=True,
    help="Run Layer 1 benchmark (raw llama-cpp, no engine).",
)
def run(
    model_path: Path,
    context_length: int,
    gpu_layers: int,
    sweep_step: int,
    output: Path | None,
    layer1_only: bool,
) -> None:
    """Run benchmark for MODEL_PATH.

    Measures: cold load time, swap latency (COLD→WARM→ACTIVE),
    inference tok/s, and GPU layer sweep with VRAM tracking.
    Validates the P1-022 swap latency target (<3s).
    """
    if not layer1_only:
        click.echo("Only Layer 1 benchmarks are currently available.", err=True)
        return

    from entropic.benchmark.layer1 import run_layer1, save_results
    from entropic.benchmark.report import print_layer1_report
    from entropic.benchmark.runner import make_model_spec

    spec = make_model_spec(
        path=model_path,
        context_length=context_length,
        gpu_layers=gpu_layers,
    )
    on_phase, progress_ctx = _make_progress_callback()
    with progress_ctx:
        results = asyncio.run(run_layer1(spec, sweep_step=sweep_step, on_phase=on_phase))
    print_layer1_report(results)

    if output:
        output.parent.mkdir(parents=True, exist_ok=True)
        from entropic.benchmark.report import results_to_json

        results_to_json(results, output)
        click.echo(f"\nResults saved: {output}")
    else:
        default_dir = Path.home() / ".entropic" / "benchmark" / "results"
        saved = save_results(results, default_dir)
        click.echo(f"\nResults saved: {saved}")


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

    Warms the model once (CPU), then measures tok/s and VRAM at each
    GPU layer increment. Useful for finding the optimal gpu_layers value.
    """
    from entropic.benchmark.report import _print_plain, _print_rich_sweep
    from entropic.benchmark.runner import BenchmarkRunner, make_model_spec

    spec = make_model_spec(path=model_path, context_length=context_length)
    runner = BenchmarkRunner(spec)
    points = asyncio.run(runner.gpu_sweep(step=step, max_layers=max_layers))

    try:
        from rich.console import Console
        from rich.table import Table

        from entropic.benchmark.types import Layer1Results

        fake = Layer1Results(
            model_path=model_path,
            timestamp="",
            engine_version="",
            sweep=points,
        )
        _print_rich_sweep(fake, Console(), Table)
    except ImportError:
        from entropic.benchmark.types import Layer1Results

        fake = Layer1Results(
            model_path=model_path,
            timestamp="",
            engine_version="",
            sweep=points,
        )
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
