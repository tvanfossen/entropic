"""Human-readable output for benchmark results.

Plain text output for all benchmark reports.
"""

from typing import Any

from entropic.benchmark.performance import SWAP_TARGET_MS
from entropic.benchmark.types import PerfResults


def print_perf_report(results: PerfResults) -> None:
    """Print a performance benchmark report to stdout.

    @brief Format and display performance benchmark results.
    @version 2
    """
    _print_plain(results)


def print_sweep_report(results: PerfResults) -> None:
    """Print a GPU layer sweep report to stdout.

    @brief Format and display sweep results only.
    @version 1
    """
    _print_sweep(results)


def _print_plain(results: PerfResults) -> None:
    """Plain text performance report.

    @brief Full performance report: cold start, swap, inference, sweep.
    @version 2
    """
    print(f"\nPerformance Benchmark: {results.model_path.name}")
    print(f"Engine v{results.engine_version} | {results.timestamp[:10]}")

    if results.cold_load:
        cl = results.cold_load
        print(
            f"\nCold Start: {cl.elapsed_ms:.0f}ms total, "
            f"load={cl.swap_ms:.0f}ms, VRAM={cl.vram_used_mb}MB"
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
        print(
            f"\nInference: {inf.tok_s:.1f} tok/s " f"({inf.tokens} tokens, {inf.elapsed_ms:.0f}ms)"
        )

    if results.sweep:
        _print_sweep(results)


def _print_sweep(results: PerfResults) -> None:
    """Print GPU layer sweep table.

    @brief Tabular sweep output with layers, timing, and VRAM.
    @version 1
    """
    print("\nGPU Layer Sweep:")
    print(f"  {'Layers':>6}  {'Load ms':>8}  {'tok/s':>7}  {'VRAM MB':>7}")
    for p in results.sweep:
        oom = "  OOM" if p.oom else ""
        print(f"  {p.gpu_layers:>6}  {p.load_ms:>8.0f}  {p.tok_s:>7.1f}  {p.vram_mb:>7}{oom}")


def print_quality_report(results: Any) -> None:
    """Print a quality benchmark report to stdout.

    @brief Format and display quality benchmark results.
    @version 2
    """
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


def _avg_timing(prompt_results: Any, field: str) -> float:
    """Compute average of a timing field across prompt results.

    @brief Mean of a numeric timing field, ignoring zero values.
    @version 1
    """
    vals = [getattr(pr, field, 0.0) for pr in prompt_results if getattr(pr, field, 0.0) > 0]
    return sum(vals) / len(vals) if vals else 0.0
