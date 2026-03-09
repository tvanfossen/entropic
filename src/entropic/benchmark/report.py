"""Human-readable output for benchmark results.

Uses Rich for formatted tables when available; falls back to plain text.
"""

from typing import Any

from entropic.benchmark.performance import SWAP_TARGET_MS
from entropic.benchmark.types import PerfResults


def print_perf_report(results: PerfResults) -> None:
    """Print a performance benchmark report to stdout."""
    try:
        from rich.console import Console
        from rich.table import Table

        _print_rich(results, Console(), Table)
    except ImportError:
        _print_plain(results)


def _print_rich(results: PerfResults, console: Any, table_cls: Any) -> None:
    """Print formatted tables using Rich."""
    console.print(f"\n[bold]Performance Benchmark[/bold]: {results.model_path.name}")
    console.print(f"Engine v{results.engine_version} | {results.timestamp[:10]}")

    if results.gpu_info:
        gpu = results.gpu_info
        console.print(
            f"GPU: {gpu.get('gpu_name', 'unknown')} " f"({gpu.get('vram_total_mb', '?')} MB VRAM)"
        )

    if results.cold_load:
        _print_rich_load(results, console, table_cls)

    if results.swap:
        _print_rich_swap(results, console, table_cls)

    if results.inference:
        inf = results.inference
        console.print(
            f"\n[bold]Inference[/bold]: {inf.tok_s:.1f} tok/s "
            f"({inf.tokens} tokens, {inf.elapsed_ms:.0f}ms, "
            f"gpu_layers={inf.gpu_layers}, vram={inf.vram_used_mb}MB)"
        )

    if results.sweep:
        _print_rich_sweep(results, console, table_cls)


def _print_rich_load(results: PerfResults, console: Any, table_cls: Any) -> None:
    """Print the cold start timing table."""
    t = table_cls(title="Cold Start")
    t.add_column("Metric")
    t.add_column("Value", justify="right")
    cl = results.cold_load
    assert cl is not None
    t.add_row("Total (load + inference)", f"{cl.elapsed_ms:.0f} ms")
    t.add_row("Model load/swap", f"{cl.swap_ms:.0f} ms")
    t.add_row("VRAM after load", f"{cl.vram_used_mb} MB")
    console.print(t)


def _print_rich_swap(results: PerfResults, console: Any, table_cls: Any) -> None:
    """Print the swap latency table with P1-022 target assessment."""
    swap = results.swap
    assert swap is not None

    def _verdict(ms: float) -> str:
        return "[green]MET[/green]" if ms < SWAP_TARGET_MS else "[red]MISSED[/red]"

    t = table_cls(title=f"Swap Latency (target: < {SWAP_TARGET_MS:.0f}ms)")
    t.add_column("Direction")
    t.add_column("Time (ms)", justify="right")
    t.add_column("Target", justify="center")
    t.add_row("Swap away", f"{swap.swap_away_ms:.0f}", _verdict(swap.swap_away_ms))
    t.add_row("Swap back", f"{swap.swap_back_ms:.0f}", _verdict(swap.swap_back_ms))
    console.print(t)


def _print_rich_sweep(results: PerfResults, console: Any, table_cls: Any) -> None:
    """Print the GPU layer sweep table."""
    t = table_cls(title="GPU Layer Sweep")
    t.add_column("Layers", justify="right")
    t.add_column("Load (ms)", justify="right")
    t.add_column("tok/s", justify="right")
    t.add_column("VRAM (MB)", justify="right")
    t.add_column("OOM")
    for p in results.sweep:
        oom_str = "[red]OOM[/red]" if p.oom else ""
        t.add_row(
            str(p.gpu_layers),
            f"{p.load_ms:.0f}",
            f"{p.tok_s:.1f}",
            str(p.vram_mb),
            oom_str,
        )
    console.print(t)


def _print_plain(results: PerfResults) -> None:
    """Plain text fallback when Rich is not installed."""
    print(f"\nPerformance Benchmark: {results.model_path.name}")
    print(f"Engine v{results.engine_version} | {results.timestamp[:10]}")

    if results.cold_load:
        cl = results.cold_load
        print(
            f"\nCold Start: {cl.elapsed_ms:.0f}ms total, load={cl.swap_ms:.0f}ms, VRAM={cl.vram_used_mb}MB"
        )

    if results.swap:
        swap = results.swap
        print(f"\nSwap Latency (target: < {SWAP_TARGET_MS:.0f}ms)")
        away_ok = "MET" if swap.swap_away_ms < SWAP_TARGET_MS else "MISSED"
        print(f"  Swap away: {swap.swap_away_ms:.0f}ms  [{away_ok}]")
        back_ok = "MET" if swap.swap_back_ms < SWAP_TARGET_MS else "MISSED"
        print(f"  Swap back: {swap.swap_back_ms:.0f}ms  [{back_ok}]")

    if results.inference:
        inf = results.inference
        print(f"\nInference: {inf.tok_s:.1f} tok/s ({inf.tokens} tokens, {inf.elapsed_ms:.0f}ms)")

    if results.sweep:
        print("\nGPU Layer Sweep:")
        print(f"  {'Layers':>6}  {'Load ms':>8}  {'tok/s':>7}  {'VRAM MB':>7}")
        for p in results.sweep:
            oom = "  OOM" if p.oom else ""
            print(f"  {p.gpu_layers:>6}  {p.load_ms:>8.0f}  {p.tok_s:>7.1f}  {p.vram_mb:>7}{oom}")


def print_quality_report(results: Any) -> None:
    """Print a quality benchmark report to stdout."""
    try:
        from rich.console import Console
        from rich.table import Table

        _print_quality_rich(results, Console(), Table)
    except ImportError:
        _print_quality_plain(results)


def _print_quality_rich(results: Any, console: Any, table_cls: Any) -> None:
    """Print quality results using Rich."""
    console.print(f"\n[bold]Quality Benchmark[/bold]: {results.config_source}")
    console.print(f"Engine v{results.engine_version} | {results.timestamp[:10]}")
    console.print(
        f"Total: [green]{results.total_passed} passed[/green], "
        f"[red]{results.total_failed} failed[/red]"
    )

    t = table_cls(title="Identity Results")
    t.add_column("Identity")
    t.add_column("Passed", justify="right")
    t.add_column("Failed", justify="right")
    t.add_column("Total ms", justify="right")
    t.add_column("Swap ms", justify="right")
    t.add_column("Infer ms", justify="right")
    t.add_column("Status")
    for ir in results.identities:
        status = "[green]PASS[/green]" if ir.failed == 0 else "[red]FAIL[/red]"
        if ir.skipped:
            status = f"[yellow]SKIP: {ir.skip_reason}[/yellow]"
            t.add_row(ir.name, "-", "-", "-", "-", "-", status)
            continue
        avg_total = _avg_timing(ir.prompt_results, "total_ms")
        avg_swap = _avg_timing(ir.prompt_results, "swap_ms")
        avg_infer = _avg_timing(ir.prompt_results, "inference_ms")
        t.add_row(
            ir.name,
            str(ir.passed),
            str(ir.failed),
            f"{avg_total:.0f}" if avg_total else "-",
            f"{avg_swap:.0f}" if avg_swap else "-",
            f"{avg_infer:.0f}" if avg_infer else "-",
            status,
        )
    console.print(t)


def _avg_timing(prompt_results: Any, field: str) -> float:
    """Compute average of a timing field across prompt results."""
    vals = [getattr(pr, field, 0.0) for pr in prompt_results if getattr(pr, field, 0.0) > 0]
    return sum(vals) / len(vals) if vals else 0.0


def _print_quality_plain(results: Any) -> None:
    """Plain text quality report."""
    print(f"\nQuality Benchmark: {results.config_source}")
    print(f"Engine v{results.engine_version} | {results.timestamp[:10]}")
    print(f"Total: {results.total_passed} passed, {results.total_failed} failed")
    for ir in results.identities:
        status = "PASS" if ir.failed == 0 else "FAIL"
        if ir.skipped:
            status = f"SKIP: {ir.skip_reason}"
            print(f"  {ir.name}: [{status}]")
            continue
        avg_total = _avg_timing(ir.prompt_results, "total_ms")
        timing = f" ({avg_total:.0f}ms)" if avg_total else ""
        print(f"  {ir.name}: {ir.passed} passed, {ir.failed} failed [{status}]{timing}")
