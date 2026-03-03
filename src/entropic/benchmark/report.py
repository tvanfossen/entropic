"""Human-readable output for benchmark results.

Uses Rich for formatted tables when available; falls back to plain text.
"""

from pathlib import Path
from typing import Any

from entropic.benchmark.layer1 import SWAP_TARGET_MS
from entropic.benchmark.types import Layer1Results


def print_layer1_report(results: Layer1Results) -> None:
    """Print a Layer 1 benchmark report to stdout.

    Uses Rich tables if available, otherwise plain text.
    """
    try:
        from rich.console import Console
        from rich.table import Table

        _print_rich(results, Console(), Table)
    except ImportError:
        _print_plain(results)


def _print_rich(results: Layer1Results, console: Any, table_cls: Any) -> None:
    """Print formatted tables using Rich."""
    console.print(f"\n[bold]Layer 1 Benchmark[/bold]: {results.model_path.name}")
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


def _print_rich_load(results: Layer1Results, console: Any, table_cls: Any) -> None:
    """Print the load time table."""
    t = table_cls(title="Load Times")
    t.add_column("Phase")
    t.add_column("Time (ms)", justify="right")
    t.add_column("VRAM (MB)", justify="right")
    ld = results.cold_load
    assert ld is not None
    t.add_row(ld.phase, f"{ld.elapsed_ms:.0f}", str(ld.vram_used_mb))
    console.print(t)


def _print_rich_swap(results: Layer1Results, console: Any, table_cls: Any) -> None:
    """Print the swap latency table with per-transition P1-022 target assessment."""
    swap = results.swap
    assert swap is not None

    def _verdict(ms: float) -> str:
        return "[green]✓[/green]" if ms < SWAP_TARGET_MS else "[red]✗[/red]"

    t = table_cls(title=f"Swap Latency (target: each WARM→ACTIVE < {SWAP_TARGET_MS:.0f}ms)")
    t.add_column("Transition")
    t.add_column("Time (ms)", justify="right")
    t.add_column("VRAM (MB)", justify="right")
    t.add_column("Target", justify="center")
    t.add_row("COLD → WARM", f"{swap.warm_ms:.0f}", str(swap.vram_warm_mb), "[dim]startup[/dim]")
    t.add_row(
        "WARM → ACTIVE",
        f"{swap.activate_ms:.0f}",
        str(swap.vram_active_mb),
        _verdict(swap.activate_ms),
    )
    t.add_row("ACTIVE → WARM", f"{swap.deactivate_ms:.0f}", str(swap.vram_warm_mb), "[dim]—[/dim]")
    t.add_row(
        "WARM → ACTIVE (re)",
        f"{swap.reactivate_ms:.0f}",
        str(swap.vram_active_mb),
        _verdict(swap.reactivate_ms),
    )
    console.print(t)


def _print_rich_sweep(results: Layer1Results, console: Any, table_cls: Any) -> None:
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


def _print_plain(results: Layer1Results) -> None:
    """Plain text fallback when Rich is not installed."""
    print(f"\nLayer 1 Benchmark: {results.model_path.name}")
    print(f"Engine v{results.engine_version} | {results.timestamp[:10]}")

    if results.cold_load:
        ld = results.cold_load
        print(f"\nLoad ({ld.phase}): {ld.elapsed_ms:.0f}ms, VRAM={ld.vram_used_mb}MB")

    if results.swap:
        swap = results.swap
        print(f"\nSwap Latency (target: each WARM→ACTIVE < {SWAP_TARGET_MS:.0f}ms)")
        print(f"  COLD→WARM:       {swap.warm_ms:.0f}ms  [startup]")
        act_ok = "MET" if swap.activate_ms < SWAP_TARGET_MS else "MISSED"
        print(f"  WARM→ACTIVE:     {swap.activate_ms:.0f}ms  [{act_ok}]")
        print(f"  ACTIVE→WARM:     {swap.deactivate_ms:.0f}ms")
        react_ok = "MET" if swap.reactivate_ms < SWAP_TARGET_MS else "MISSED"
        print(f"  WARM→ACTIVE(re): {swap.reactivate_ms:.0f}ms  [{react_ok}]")

    if results.inference:
        inf = results.inference
        print(f"\nInference: {inf.tok_s:.1f} tok/s ({inf.tokens} tokens, {inf.elapsed_ms:.0f}ms)")

    if results.sweep:
        print("\nGPU Layer Sweep:")
        print(f"  {'Layers':>6}  {'Load ms':>8}  {'tok/s':>7}  {'VRAM MB':>7}")
        for p in results.sweep:
            oom = "  OOM" if p.oom else ""
            print(f"  {p.gpu_layers:>6}  {p.load_ms:>8.0f}  {p.tok_s:>7.1f}  {p.vram_mb:>7}{oom}")


def results_to_json(results: Layer1Results, output_path: Path) -> None:
    """Write results as JSON to output_path."""
    import json

    from entropic.benchmark.layer1 import _results_to_dict

    data = _results_to_dict(results)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(data, indent=2))
