"""MoE VRAM Benchmark — validation for P1-20260226-022.

Comprehensive benchmark measuring model load times, tier switching latency,
GPU inference speed, and quality lossiness across dense and MoE models at
different quantizations.

Tests: Q4_K_M vs Q2_K (Qwen3.5-35B-A3B MoE) and Dense 8B (Qwen3-8B).
GPU layer sweep finds optimal n_gpu_layers for each quant.
Cross-quant transitions measure swap cost between Q4 and Q2 variants.
Quality comparison tests factual recall, reasoning, code generation,
tool calling, and open-ended generation across quantizations.

Usage:
    python main.py
"""

from __future__ import annotations

import asyncio
import gc
import json
import os
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from entropic import (
    AgentEngine,
    ConfigLoader,
    EngineCallbacks,
    LoopConfig,
    ModelOrchestrator,
    setup_logging,
)
from entropic.inference.orchestrator import RoutingResult

EXAMPLE_ROOT = Path(__file__).resolve().parent

INFERENCE_PROMPT = (
    "Explain why MoE architectures reduce inference cost compared "
    "to dense models of equivalent parameter count. Two paragraphs max."
)
SIMPLE_PROMPT = "What is 2+2? Answer with just the number."
NOTHINK_SUFFIX = " /no_think"


# =============================================================================
# Data
# =============================================================================


@dataclass
class ModelSpec:
    """Identifies a model for benchmarking."""

    label: str  # e.g. "MoE Q4_K_M", "MoE Q2_K", "Dense 8B"
    path: str
    gpu_layers: int
    ctx: int


@dataclass
class LoadResult:
    """Timing for a model load operation."""

    label: str
    model: str
    load_ms: float
    settings: str = ""
    notes: list[str] = field(default_factory=list)


@dataclass
class InferenceResult:
    """Timing + throughput for an inference operation."""

    label: str
    model: str
    load_ms: float
    tokens: int
    generation_s: float
    total_s: float
    swap_action: str = "none"
    notes: list[str] = field(default_factory=list)

    @property
    def tok_s(self) -> float:
        """Tokens per second (generation only, excludes load)."""
        return self.tokens / self.generation_s if self.generation_s > 0 else 0.0


@dataclass
class SweepResult:
    """Result from a single n_gpu_layers probe."""

    n_gpu_layers: int
    load_ms: float
    tok_s: float
    tokens: int
    generation_s: float
    vram_used_mb: int
    vram_total_mb: int
    oom: bool = False
    notes: str = ""


@dataclass
class QualityPrompt:
    """A prompt for quality comparison across quantizations."""

    category: str
    prompt: str
    expected: str | None = None  # substring for auto-check
    tools: list[dict] | None = None  # tool definitions for tool calling test


@dataclass
class QualityResult:
    """Response quality from a single prompt on a single model."""

    category: str
    model: str
    response: str
    tokens: int
    generation_s: float
    auto_check: str  # "PASS", "FAIL", or "—" (subjective)


@dataclass
class BenchmarkResults:
    """Aggregated results from all benchmark sections."""

    switching: list[InferenceResult] = field(default_factory=list)
    q4: ModelSpec | None = None
    q4_loads: list[LoadResult] = field(default_factory=list)
    q4_inference: list[InferenceResult] = field(default_factory=list)
    q4_sweep: list[SweepResult] = field(default_factory=list)
    q2: ModelSpec | None = None
    q2_loads: list[LoadResult] = field(default_factory=list)
    q2_inference: list[InferenceResult] = field(default_factory=list)
    q2_sweep: list[SweepResult] = field(default_factory=list)
    normal_loads: list[LoadResult] = field(default_factory=list)
    normal_inference: list[InferenceResult] = field(default_factory=list)
    cross_quant: list[LoadResult] = field(default_factory=list)
    q2_flash_attn: list[InferenceResult] = field(default_factory=list)
    q2_kv_cache: list[InferenceResult] = field(default_factory=list)
    q4_quality_think: list[QualityResult] = field(default_factory=list)
    q4_quality_nothink: list[QualityResult] = field(default_factory=list)
    q2_quality_think: list[QualityResult] = field(default_factory=list)
    q2_quality_nothink: list[QualityResult] = field(default_factory=list)


# =============================================================================
# Low-level helpers
# =============================================================================


class _SuppressStderr:
    """Context manager to redirect fd 2 to /dev/null (suppresses llama.cpp noise)."""

    def __enter__(self) -> _SuppressStderr:
        self._saved = os.dup(2)
        self._devnull = os.open(os.devnull, os.O_WRONLY)
        os.dup2(self._devnull, 2)
        return self

    def __exit__(self, *_: object) -> None:
        os.dup2(self._saved, 2)
        os.close(self._saved)
        os.close(self._devnull)


def _get_vram_mb() -> tuple[int, int]:
    """Query GPU VRAM via nvidia-smi. Returns (used_mb, total_mb)."""
    result = subprocess.run(
        ["nvidia-smi", "--query-gpu=memory.used,memory.total", "--format=csv,noheader,nounits"],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return 0, 0
    parts = result.stdout.strip().split(",")
    return int(parts[0].strip()), int(parts[1].strip())


def _get_block_count(model_path: str) -> int:
    """Get total layer count from model metadata."""
    from llama_cpp import Llama

    with _SuppressStderr():
        model = Llama(model_path=model_path, n_gpu_layers=0, n_ctx=512, verbose=False)
    for key, value in model.metadata.items():
        if "block_count" in key:
            _free(model)
            return int(value)
    _free(model)
    return 40  # fallback


def _timed_load(model_path: str, **kwargs) -> tuple[object, float]:  # noqa: ANN003
    """Load a Llama model and return (model, elapsed_seconds)."""
    from llama_cpp import Llama

    gc.collect()
    start = time.perf_counter()
    with _SuppressStderr():
        model = Llama(model_path=model_path, verbose=False, **kwargs)
    elapsed = time.perf_counter() - start
    return model, elapsed


def _timed_inference(model: object, prompt: str, max_tokens: int = 256) -> tuple[int, float]:
    """Run inference and return (token_count, elapsed_seconds)."""
    start = time.perf_counter()
    with _SuppressStderr():
        result = model.create_chat_completion(  # type: ignore[union-attr]
            messages=[{"role": "user", "content": prompt}],
            max_tokens=max_tokens,
            temperature=0.7,
        )
    elapsed = time.perf_counter() - start
    tokens = result.get("usage", {}).get("completion_tokens", 0)
    return tokens, elapsed


VRAM_IDLE_THRESHOLD_MB = 1500  # baseline VRAM when no model is loaded


def _free(model: object) -> None:
    """Delete a model and force GC + wait for CUDA VRAM release."""
    del model
    gc.collect()
    # llama.cpp CUDA cleanup is async — poll until VRAM returns to idle
    for _ in range(40):
        time.sleep(0.25)
        vram_now, _ = _get_vram_mb()
        if vram_now < VRAM_IDLE_THRESHOLD_MB:
            return


ENGINE_TIMEOUT_S = 120.0


def _counting_chunk_cb(counter: list[int]) -> Callable[[str], None]:
    """Create a stream chunk callback that counts invocations."""

    def _cb(_chunk: str) -> None:
        counter.append(1)

    return _cb


async def _consume_engine(
    engine: AgentEngine,
    prompt: str,
    captured: list[RoutingResult],
    token_count: list[int],
) -> None:
    """Consume engine output, counting tokens for progress."""
    engine.set_callbacks(
        EngineCallbacks(
            on_stream_chunk=_counting_chunk_cb(token_count),
            on_tier_selected=lambda _: None,
            on_routing_complete=lambda r: captured.append(r),
        )
    )
    async for _ in engine.run(prompt):
        pass


async def _engine_inference(
    engine: AgentEngine,
    prompt: str,
) -> tuple[RoutingResult | None, float]:
    """Run prompt through engine with timeout, return (routing_result, total_seconds)."""
    captured: list[RoutingResult] = []
    token_count: list[int] = []
    start = time.perf_counter()
    try:
        await asyncio.wait_for(
            _consume_engine(engine, prompt, captured, token_count),
            timeout=ENGINE_TIMEOUT_S,
        )
    except asyncio.TimeoutError:
        elapsed = time.perf_counter() - start
        print(f"         TIMEOUT after {elapsed:.0f}s ({len(token_count)} chunks)")
        return (captured[-1] if captured else None), elapsed
    elapsed = time.perf_counter() - start
    return (captured[-1] if captured else None), elapsed


# =============================================================================
# Printing helpers
# =============================================================================


def _section(title: str) -> None:
    """Print a section header."""
    print()
    print("=" * 74)
    print(f"  {title}")
    print("=" * 74)
    print()


def _step(n: int, total: int, desc: str) -> None:
    """Print a step indicator."""
    print(f"  [{n}/{total}] {desc}")


def _print_load(r: LoadResult) -> None:
    """Print a load result inline."""
    print(f"         {r.load_ms:>8.0f} ms  {r.settings}")


def _print_inference(r: InferenceResult) -> None:
    """Print an inference result inline."""
    print(
        f"         load={r.load_ms:>7.0f}ms  "
        f"gen={r.generation_s:>5.2f}s  "
        f"{r.tokens}tok  "
        f"{r.tok_s:>5.1f} tok/s"
    )


def _print_sweep(r: SweepResult) -> None:
    """Print a single sweep result inline."""
    if r.oom:
        print(f"         n_gpu={r.n_gpu_layers:<3}  OOM  VRAM={r.vram_used_mb}/{r.vram_total_mb}MB")
        return
    print(
        f"         n_gpu={r.n_gpu_layers:<3}  "
        f"{r.tok_s:>5.1f} tok/s  "
        f"load={r.load_ms:>6.0f}ms  "
        f"VRAM={r.vram_used_mb}/{r.vram_total_mb}MB"
    )


# =============================================================================
# Benchmarks: Orchestrator tier switching
# =============================================================================


async def _bench_tier_switching(engine: AgentEngine) -> list[InferenceResult]:
    """Measure end-to-end tier switching through the orchestrator."""
    results: list[InferenceResult] = []
    prompts = [
        ("Cold load → thinking", INFERENCE_PROMPT, "First load from disk"),
        ("Swap → normal", SIMPLE_PROMPT, "Unload thinking, load normal"),
        ("Warm reload → thinking", INFERENCE_PROMPT, "MoE pages should be cached"),
        ("Swap → normal (warm)", SIMPLE_PROMPT, "Dense pages should be cached"),
    ]
    for i, (label, prompt, note) in enumerate(prompts, 1):
        _step(i, len(prompts), label)
        routing, total_s = await _engine_inference(engine, prompt)
        tier = routing.tier.name if routing else "?"
        swap = routing.swap_action if routing else "?"
        load_ms = routing.routing_ms if routing else 0.0
        gen_s = total_s - (load_ms / 1000)
        r = InferenceResult(
            label=label,
            model=tier,
            load_ms=load_ms,
            tokens=0,
            generation_s=gen_s,
            total_s=total_s,
            swap_action=swap,
            notes=[note],
        )
        _print_inference(r)
        results.append(r)
    return results


# =============================================================================
# Benchmarks: Load tests (cold/warm state transitions)
# =============================================================================


def _bench_load_suite(spec: ModelSpec) -> list[LoadResult]:
    """Run full load benchmark suite for a model."""
    results: list[LoadResult] = []
    tests = [
        ("Cold load (GPU)", lambda: _load_cold(spec)),
        ("Cold load (GPU+mlock)", lambda: _load_cold_mlock(spec)),
        ("WARM state (CPU mmap)", lambda: _load_warm_cpu(spec)),
        ("WARM state (CPU mmap+mlock)", lambda: _load_warm_mlock(spec)),
        ("WARM → ACTIVE (mlock → GPU)", lambda: _load_warm_to_active(spec)),
        ("ACTIVE → WARM (free GPU)", lambda: _load_deactivate(spec)),
    ]
    for i, (desc, fn) in enumerate(tests, 1):
        _step(i, len(tests), desc)
        r = fn()
        _print_load(r)
        results.append(r)
    return results


def _load_cold(spec: ModelSpec) -> LoadResult:
    """Cold load with GPU."""
    model, elapsed = _timed_load(
        spec.path,
        n_gpu_layers=spec.gpu_layers,
        n_ctx=spec.ctx,
        use_mmap=True,
    )
    _free(model)
    return LoadResult(
        label="Cold load (GPU)",
        model=spec.label,
        load_ms=elapsed * 1000,
        settings=f"n_gpu={spec.gpu_layers} mmap mlock=no",
    )


def _load_cold_mlock(spec: ModelSpec) -> LoadResult:
    """Cold load with GPU + mlock."""
    model, elapsed = _timed_load(
        spec.path,
        n_gpu_layers=spec.gpu_layers,
        n_ctx=spec.ctx,
        use_mmap=True,
        use_mlock=True,
    )
    _free(model)
    return LoadResult(
        label="Cold load (GPU+mlock)",
        model=spec.label,
        load_ms=elapsed * 1000,
        settings=f"n_gpu={spec.gpu_layers} mmap mlock",
    )


def _load_warm_cpu(spec: ModelSpec) -> LoadResult:
    """WARM state: CPU mmap only."""
    model, elapsed = _timed_load(
        spec.path,
        n_gpu_layers=0,
        n_ctx=512,
        use_mmap=True,
    )
    _free(model)
    return LoadResult(
        label="WARM state (CPU mmap)",
        model=spec.label,
        load_ms=elapsed * 1000,
        settings="n_gpu=0 mmap mlock=no",
    )


def _load_warm_mlock(spec: ModelSpec) -> LoadResult:
    """WARM state: CPU mmap + mlock."""
    model, elapsed = _timed_load(
        spec.path,
        n_gpu_layers=0,
        n_ctx=512,
        use_mmap=True,
        use_mlock=True,
    )
    _free(model)
    return LoadResult(
        label="WARM state (CPU mmap+mlock)",
        model=spec.label,
        load_ms=elapsed * 1000,
        settings="n_gpu=0 mmap mlock",
    )


def _load_warm_to_active(spec: ModelSpec) -> LoadResult:
    """WARM → ACTIVE: mlock'd pages → GPU."""
    warm, _ = _timed_load(
        spec.path,
        n_gpu_layers=0,
        n_ctx=512,
        use_mmap=True,
        use_mlock=True,
    )
    _free(warm)
    active, elapsed = _timed_load(
        spec.path,
        n_gpu_layers=spec.gpu_layers,
        n_ctx=spec.ctx,
        use_mmap=True,
        use_mlock=True,
    )
    _free(active)
    return LoadResult(
        label="WARM → ACTIVE (mlock → GPU)",
        model=spec.label,
        load_ms=elapsed * 1000,
        settings=f"n_gpu={spec.gpu_layers} mmap mlock (from pinned pages)",
    )


def _load_deactivate(spec: ModelSpec) -> LoadResult:
    """ACTIVE → WARM: free GPU, keep in RAM."""
    model, elapsed = _timed_load(
        spec.path,
        n_gpu_layers=0,
        n_ctx=512,
        use_mmap=True,
        use_mlock=True,
    )
    _free(model)
    return LoadResult(
        label="ACTIVE → WARM (free GPU)",
        model=spec.label,
        load_ms=elapsed * 1000,
        settings="n_gpu=0 mmap mlock",
    )


# =============================================================================
# Benchmarks: GPU inference speed
# =============================================================================


def _bench_inference_suite(spec: ModelSpec) -> list[InferenceResult]:
    """Run GPU inference: cold, warm→active, think vs no-think."""
    results: list[InferenceResult] = []
    tests = [
        (f"GPU cold (n_gpu={spec.gpu_layers})", INFERENCE_PROMPT, False),
        (f"WARM→ACTIVE (n_gpu={spec.gpu_layers})", INFERENCE_PROMPT, True),
        ("Think mode", INFERENCE_PROMPT, True),
        ("No-think mode", INFERENCE_PROMPT + NOTHINK_SUFFIX, True),
    ]
    for i, (label, prompt, use_mlock) in enumerate(tests, 1):
        _step(i, len(tests), label)
        if use_mlock and i == 2:
            _warm_model(spec)
        r = _infer_single(spec, label, prompt, use_mlock)
        _print_inference(r)
        results.append(r)
    return results


def _infer_single(
    spec: ModelSpec,
    label: str,
    prompt: str,
    use_mlock: bool,
) -> InferenceResult:
    """Load model, run one inference prompt, return result."""
    model, load_elapsed = _timed_load(
        spec.path,
        n_gpu_layers=spec.gpu_layers,
        n_ctx=spec.ctx,
        use_mmap=True,
        use_mlock=use_mlock,
    )
    tokens, gen_elapsed = _timed_inference(model, prompt)
    _free(model)
    return InferenceResult(
        label=label,
        model=spec.label,
        load_ms=load_elapsed * 1000,
        tokens=tokens,
        generation_s=gen_elapsed,
        total_s=load_elapsed + gen_elapsed,
    )


# =============================================================================
# Benchmarks: GPU layer sweep
# =============================================================================


def _sweep_gpu_layers(spec: ModelSpec, total_layers: int) -> list[SweepResult]:
    """Sweep n_gpu_layers from 50% to 100%, measuring tok/s + VRAM.

    Below ~50% GPU offload the model is CPU-dominated and too slow to be
    practical. Starting at the midpoint saves significant benchmark time.
    """
    results: list[SweepResult] = []
    floor = total_layers // 2
    step = 5
    points = list(range(floor, total_layers + 1, step))
    if points[-1] != total_layers:
        points.append(total_layers)

    for i, n_gpu in enumerate(points, 1):
        _step(i, len(points), f"n_gpu_layers={n_gpu}")
        r = _sweep_single_point(spec.path, n_gpu, spec.ctx)
        _print_sweep(r)
        results.append(r)
        if r.oom:
            break
    return results


def _sweep_single_point(model_path: str, n_gpu: int, ctx: int) -> SweepResult:
    """Test a single n_gpu_layers value."""
    try:
        model, load_elapsed = _timed_load(
            model_path,
            n_gpu_layers=n_gpu,
            n_ctx=ctx,
            use_mmap=True,
            use_mlock=True,
        )
    except Exception as e:
        vram_used, vram_total = _get_vram_mb()
        return SweepResult(
            n_gpu_layers=n_gpu,
            load_ms=0,
            tok_s=0,
            tokens=0,
            generation_s=0,
            vram_used_mb=vram_used,
            vram_total_mb=vram_total,
            oom=True,
            notes=str(e),
        )
    vram_used, vram_total = _get_vram_mb()
    tokens, gen_elapsed = _timed_inference(model, SIMPLE_PROMPT, max_tokens=64)
    _free(model)
    tok_s = tokens / gen_elapsed if gen_elapsed > 0 else 0.0
    return SweepResult(
        n_gpu_layers=n_gpu,
        load_ms=load_elapsed * 1000,
        tok_s=tok_s,
        tokens=tokens,
        generation_s=gen_elapsed,
        vram_used_mb=vram_used,
        vram_total_mb=vram_total,
    )


# =============================================================================
# Benchmarks: Inference optimization flags
# =============================================================================


def _bench_flag_variants(
    spec: ModelSpec,
    configs: list[tuple[str, dict]],
) -> list[InferenceResult]:
    """Test a list of (label, extra_kwargs) load configs at full GPU offload."""
    results: list[InferenceResult] = []
    for i, (label, extra_kwargs) in enumerate(configs, 1):
        _step(i, len(configs), label)
        try:
            model, load_elapsed = _timed_load(
                spec.path,
                n_gpu_layers=spec.gpu_layers,
                n_ctx=spec.ctx,
                use_mmap=True,
                use_mlock=True,
                **extra_kwargs,
            )
        except (ValueError, RuntimeError) as e:
            print(f"         UNSUPPORTED: {e}")
            results.append(
                InferenceResult(
                    label=f"{label} (UNSUPPORTED)",
                    model=spec.label,
                    load_ms=0,
                    tokens=0,
                    generation_s=0,
                    total_s=0,
                )
            )
            continue
        tokens, gen_elapsed = _timed_inference(model, INFERENCE_PROMPT)
        vram_used, _ = _get_vram_mb()
        _free(model)
        r = InferenceResult(
            label=f"{label} (VRAM={vram_used}MB)",
            model=spec.label,
            load_ms=load_elapsed * 1000,
            tokens=tokens,
            generation_s=gen_elapsed,
            total_s=load_elapsed + gen_elapsed,
        )
        _print_inference(r)
        results.append(r)
    return results


FLASH_ATTN_CONFIGS: list[tuple[str, dict]] = [
    ("Baseline", {}),
    ("flash_attn=True", {"flash_attn": True}),
]

KV_CACHE_CONFIGS: list[tuple[str, dict]] = [
    ("Baseline (KV F16)", {}),
    ("KV Q8_0", {"type_k": 8, "type_v": 8}),
    ("KV Q4_0", {"type_k": 2, "type_v": 2}),
]


# =============================================================================
# Benchmarks: Cross-quant transitions
# =============================================================================


def _cold_swap(source: ModelSpec, target: ModelSpec) -> LoadResult:
    """Load source (active), free it, then cold-load target. Times the target load."""
    model_src, _ = _timed_load(
        source.path,
        n_gpu_layers=source.gpu_layers,
        n_ctx=source.ctx,
        use_mmap=True,
        use_mlock=True,
    )
    _free(model_src)
    model_tgt, elapsed = _timed_load(
        target.path,
        n_gpu_layers=target.gpu_layers,
        n_ctx=target.ctx,
        use_mmap=True,
        use_mlock=True,
    )
    _free(model_tgt)
    return LoadResult(
        label=f"{source.label} → {target.label} (cold)",
        model="cross-quant",
        load_ms=elapsed * 1000,
        settings=f"n_gpu={target.gpu_layers} (target cold, source was active)",
    )


def _warm_swap(source: ModelSpec, target: ModelSpec) -> LoadResult:
    """Warm both models (mlock), then load target to GPU. Times the target load."""
    _warm_model(source)
    _warm_model(target)
    model_tgt, elapsed = _timed_load(
        target.path,
        n_gpu_layers=target.gpu_layers,
        n_ctx=target.ctx,
        use_mmap=True,
        use_mlock=True,
    )
    _free(model_tgt)
    return LoadResult(
        label=f"{source.label} → {target.label} (both warm)",
        model="cross-quant",
        load_ms=elapsed * 1000,
        settings=f"n_gpu={target.gpu_layers} (both mlock'd in RAM)",
    )


def _bench_cross_quant(spec_a: ModelSpec, spec_b: ModelSpec) -> list[LoadResult]:
    """Measure swap cost between two model variants."""
    swaps = [
        (
            1,
            f"{spec_a.label} active → {spec_b.label} cold swap",
            lambda: _cold_swap(spec_a, spec_b),
        ),
        (
            2,
            f"{spec_b.label} active → {spec_a.label} cold swap",
            lambda: _cold_swap(spec_b, spec_a),
        ),
        (3, f"Both warm: {spec_a.label} → {spec_b.label}", lambda: _warm_swap(spec_a, spec_b)),
        (4, f"Both warm: {spec_b.label} → {spec_a.label}", lambda: _warm_swap(spec_b, spec_a)),
    ]
    results: list[LoadResult] = []
    for step_num, desc, fn in swaps:
        _step(step_num, len(swaps), desc)
        try:
            r = fn()
        except (ValueError, RuntimeError) as e:
            print(f"         FAILED: {e}")
            r = LoadResult(label=desc, model="cross-quant", load_ms=0, settings="FAILED")
        _print_load(r)
        results.append(r)
    return results


def _warm_model(spec: ModelSpec) -> None:
    """Load model CPU-only with mlock to populate page cache, then free."""
    model, _ = _timed_load(
        spec.path,
        n_gpu_layers=0,
        n_ctx=512,
        use_mmap=True,
        use_mlock=True,
    )
    _free(model)


# =============================================================================
# Benchmarks: Quality comparison (Q4 vs Q2 lossiness)
# =============================================================================

WEATHER_TOOL: dict = {
    "type": "function",
    "function": {
        "name": "get_weather",
        "description": "Get current weather for a city",
        "parameters": {
            "type": "object",
            "properties": {
                "city": {"type": "string", "description": "City name"},
            },
            "required": ["city"],
        },
    },
}

QUALITY_PROMPTS: list[QualityPrompt] = [
    QualityPrompt(
        "Factual recall",
        "What year did the Apollo 11 mission first land on the Moon? Answer with just the year.",
        expected="1969",
    ),
    QualityPrompt(
        "Reasoning",
        "If all roses are flowers, and all flowers need water, do roses need water? "
        "Explain your reasoning in 2-3 sentences.",
    ),
    QualityPrompt(
        "Code generation",
        "Write a Python function called is_palindrome that checks if a string is a "
        "palindrome. Return True or False. Just the function, no explanation.",
        expected="def is_palindrome",
    ),
    QualityPrompt(
        "Tool calling",
        "What is the current weather in Tokyo?",
        tools=[WEATHER_TOOL],
    ),
    QualityPrompt("MoE explanation", INFERENCE_PROMPT),
]


def _extract_quality_response(result: dict, has_tools: bool) -> str:
    """Extract response text from chat completion, handling tool calls.

    Tool calls may appear in the tool_calls field (JSON format) or
    embedded in content (Qwen3.5 XML format).
    """
    choice = result["choices"][0]["message"]
    if has_tools and choice.get("tool_calls"):
        return json.dumps(choice["tool_calls"], indent=2)
    content = choice.get("content", "") or ""
    return content


def _auto_check_quality(response: str, qp: QualityPrompt) -> str:
    """Auto-check response against expected value. Returns PASS/FAIL/— (subjective)."""
    if qp.tools:
        return _check_tool_call(response)
    if qp.expected:
        return "PASS" if qp.expected in response else "FAIL"
    return "—"


def _check_tool_call(response: str) -> str:
    """Validate that response contains a parseable tool call.

    Supports JSON (Qwen3), XML (Qwen3.5), and tagged JSON formats.
    """
    has_json = _has_json_tool_call(response)
    has_xml = "<function=" in response and "</function>" in response
    has_tagged = "<tool_call>" in response and '"name"' in response

    return "PASS" if (has_json or has_xml or has_tagged) else "FAIL (no tool call)"


def _has_json_tool_call(response: str) -> bool:
    """Check if response contains a valid JSON tool call."""
    try:
        return bool(json.loads(response))
    except (json.JSONDecodeError, TypeError):
        return False


def _run_quality_prompt(
    model: object,
    model_label: str,
    qp: QualityPrompt,
    *,
    thinking: bool,
) -> QualityResult:
    """Run a single quality prompt and capture the response."""
    prompt = qp.prompt if thinking else qp.prompt + NOTHINK_SUFFIX
    messages = [{"role": "user", "content": prompt}]
    kwargs: dict = {"messages": messages, "max_tokens": 256, "temperature": 0.7}
    if qp.tools:
        kwargs["tools"] = qp.tools
    start = time.perf_counter()
    with _SuppressStderr():
        result = model.create_chat_completion(**kwargs)  # type: ignore[union-attr]
    elapsed = time.perf_counter() - start
    tokens = result.get("usage", {}).get("completion_tokens", 0)
    response = _extract_quality_response(result, qp.tools is not None)
    return QualityResult(
        category=qp.category,
        model=model_label,
        response=response,
        tokens=tokens,
        generation_s=elapsed,
        auto_check=_auto_check_quality(response, qp),
    )


def _bench_quality_suite(spec: ModelSpec, *, thinking: bool) -> list[QualityResult]:
    """Load model once and run all quality prompts in think or no-think mode."""
    model, _ = _timed_load(
        spec.path,
        n_gpu_layers=spec.gpu_layers,
        n_ctx=spec.ctx,
        use_mmap=True,
        use_mlock=True,
    )
    mode = "think" if thinking else "no-think"
    results: list[QualityResult] = []
    for i, qp in enumerate(QUALITY_PROMPTS, 1):
        _step(i, len(QUALITY_PROMPTS), f"{qp.category} ({mode})")
        r = _run_quality_prompt(model, spec.label, qp, thinking=thinking)
        results.append(r)
        check = f" [{r.auto_check}]" if r.auto_check != "—" else ""
        preview = r.response[:80].replace("\n", " ")
        print(f"         {r.tokens} tok  {r.generation_s:.2f}s{check}")
        print(f"         {preview}...")
    _free(model)
    return results


# =============================================================================
# Report: tables
# =============================================================================


def _print_load_table(title: str, results: list[LoadResult]) -> None:
    """Print a load comparison table."""
    if not results:
        return
    print()
    print(f"  {title}")
    print(f"  {'─' * 70}")
    print(f"  {'Test':<35} {'Load':>10}  Settings")
    print(f"  {'─' * 70}")
    for r in results:
        print(f"  {r.label:<35} {r.load_ms:>8.0f}ms  {r.settings}")
    print(f"  {'─' * 70}")
    _print_load_comparisons(results)


def _print_load_comparisons(results: list[LoadResult]) -> None:
    """Print relative speedups vs first result as baseline."""
    if len(results) < 2:
        return
    baseline = results[0]
    if baseline.load_ms <= 0:
        return
    print()
    print(f"  vs baseline ({baseline.label}: {baseline.load_ms:.0f}ms):")
    for r in results[1:]:
        if r.load_ms > 0:
            ratio = baseline.load_ms / r.load_ms
            delta = baseline.load_ms - r.load_ms
            direction = "faster" if delta > 0 else "slower"
            print(f"    {r.label:<33} {abs(delta):>7.0f}ms {direction} ({ratio:.1f}x)")


def _print_inference_table(title: str, results: list[InferenceResult]) -> None:
    """Print an inference comparison table."""
    if not results:
        return
    print()
    print(f"  {title}")
    print(f"  {'─' * 70}")
    print(f"  {'Config':<35} {'Load':>8} {'Gen':>7} {'Tok':>5} {'Tok/s':>7}")
    print(f"  {'─' * 70}")
    for r in results:
        print(
            f"  {r.label:<35} {r.load_ms:>6.0f}ms "
            f"{r.generation_s:>5.2f}s {r.tokens:>5} {r.tok_s:>6.1f}"
        )
    print(f"  {'─' * 70}")


def _print_sweep_table(title: str, results: list[SweepResult]) -> None:
    """Print sweep results with ASCII throughput bar chart."""
    if not results:
        return
    print()
    print(f"  {title}")
    print(f"  {'─' * 74}")
    print(f"  {'n_gpu':>5} {'Tok/s':>7} {'Load':>8} {'VRAM':>12}  Throughput")
    print(f"  {'─' * 74}")
    max_tok_s = max((r.tok_s for r in results), default=1.0) or 1.0
    for r in results:
        if r.oom:
            print(
                f"  {r.n_gpu_layers:>5}     OOM                {r.vram_used_mb:>5}/{r.vram_total_mb}MB  X"
            )
            continue
        bar_len = int(30 * r.tok_s / max_tok_s) if max_tok_s > 0 else 0
        print(
            f"  {r.n_gpu_layers:>5} {r.tok_s:>6.1f} {r.load_ms:>6.0f}ms "
            f"{r.vram_used_mb:>5}/{r.vram_total_mb}MB  {'█' * bar_len}"
        )
    print(f"  {'─' * 74}")
    _print_sweep_optimal(results)


def _print_sweep_optimal(results: list[SweepResult]) -> None:
    """Print the optimal n_gpu_layers from sweep results."""
    valid = [r for r in results if not r.oom]
    if not valid:
        return
    best = max(valid, key=lambda r: r.tok_s)
    print(
        f"\n  Optimal: n_gpu_layers={best.n_gpu_layers} → {best.tok_s:.1f} tok/s "
        f"(VRAM: {best.vram_used_mb}/{best.vram_total_mb}MB)"
    )


def _print_tier_switching_table(results: list[InferenceResult]) -> None:
    """Print orchestrator tier switching table."""
    if not results:
        return
    print()
    print("  Orchestrator tier switching (end-to-end)")
    print(f"  {'─' * 70}")
    print(f"  {'Step':<30} {'Route/Load':>10} {'Gen':>7} {'Total':>7}  Swap")
    print(f"  {'─' * 70}")
    for r in results:
        print(
            f"  {r.label:<30} {r.load_ms:>8.0f}ms "
            f"{r.generation_s:>5.2f}s {r.total_s:>5.2f}s  {r.swap_action}"
        )
    print(f"  {'─' * 70}")


# =============================================================================
# Report: side-by-side comparisons
# =============================================================================


def _print_load_side_by_side(
    label_a: str,
    results_a: list[LoadResult],
    label_b: str,
    results_b: list[LoadResult],
) -> None:
    """Print side-by-side load comparison between two models."""
    if not results_a or not results_b:
        return
    print()
    print(f"  Load comparison: {label_a} vs {label_b}")
    print(f"  {'─' * 74}")
    print(f"  {'Test':<30} {label_a:>12} {label_b:>12}  Ratio")
    print(f"  {'─' * 74}")
    for ra, rb in zip(results_a, results_b, strict=False):
        ratio_str = ""
        if ra.load_ms > 0 and rb.load_ms > 0:
            ratio = ra.load_ms / rb.load_ms
            ratio_str = f"{ratio:.2f}x"
        print(f"  {ra.label:<30} {ra.load_ms:>10.0f}ms {rb.load_ms:>10.0f}ms  {ratio_str}")
    print(f"  {'─' * 74}")


def _print_inference_side_by_side(
    label_a: str,
    results_a: list[InferenceResult],
    label_b: str,
    results_b: list[InferenceResult],
) -> None:
    """Print side-by-side inference comparison."""
    if not results_a or not results_b:
        return
    print()
    print(f"  Inference comparison: {label_a} vs {label_b}")
    print(f"  {'─' * 74}")
    print(f"  {'Config':<25} {label_a + ' tok/s':>15} {label_b + ' tok/s':>15}  Ratio")
    print(f"  {'─' * 74}")
    for ra, rb in zip(results_a, results_b, strict=False):
        ratio_str = ""
        if ra.tok_s > 0 and rb.tok_s > 0:
            ratio = rb.tok_s / ra.tok_s
            ratio_str = f"{ratio:.2f}x"
        print(f"  {ra.label:<25} {ra.tok_s:>14.1f} {rb.tok_s:>14.1f}  {ratio_str}")
    print(f"  {'─' * 74}")


def _sweep_cell(result: SweepResult | None) -> tuple[str, str]:
    """Format tok/s and VRAM strings for a sweep result (or missing entry)."""
    if result is None:
        return "—", "—"
    if result.oom:
        return "OOM", "OOM"
    return f"{result.tok_s:.1f}", f"{result.vram_used_mb}MB"


def _print_sweep_side_by_side(
    label_a: str,
    sweep_a: list[SweepResult],
    label_b: str,
    sweep_b: list[SweepResult],
) -> None:
    """Print side-by-side sweep comparison at matching n_gpu_layers."""
    if not sweep_a or not sweep_b:
        return
    print()
    print(f"  GPU layer sweep comparison: {label_a} vs {label_b}")
    print(f"  {'─' * 74}")
    print(
        f"  {'n_gpu':>5} {label_a + ' tok/s':>15} {label_b + ' tok/s':>15}  {'VRAM A':>8} {'VRAM B':>8}"
    )
    print(f"  {'─' * 74}")
    b_map = {r.n_gpu_layers: r for r in sweep_b}
    for ra in sweep_a:
        a_str, a_vram = _sweep_cell(ra)
        b_str, b_vram = _sweep_cell(b_map.get(ra.n_gpu_layers))
        print(f"  {ra.n_gpu_layers:>5} {a_str:>15} {b_str:>15}  {a_vram:>8} {b_vram:>8}")
    print(f"  {'─' * 74}")
    _print_sweep_comparison(label_a, sweep_a, label_b, sweep_b)


def _print_sweep_comparison(
    label_a: str,
    sweep_a: list[SweepResult],
    label_b: str,
    sweep_b: list[SweepResult],
) -> None:
    """Print optimal n_gpu_layers comparison between two sweeps."""
    valid_a = [r for r in sweep_a if not r.oom]
    valid_b = [r for r in sweep_b if not r.oom]
    if not valid_a or not valid_b:
        return
    best_a = max(valid_a, key=lambda r: r.tok_s)
    best_b = max(valid_b, key=lambda r: r.tok_s)
    print(f"\n  {label_a} optimal: n_gpu={best_a.n_gpu_layers} → {best_a.tok_s:.1f} tok/s")
    print(f"  {label_b} optimal: n_gpu={best_b.n_gpu_layers} → {best_b.tok_s:.1f} tok/s")
    if best_a.tok_s > 0:
        print(f"  Ratio: {best_b.tok_s / best_a.tok_s:.2f}x")


# =============================================================================
# Report: quality comparison
# =============================================================================


def _print_quality_response(label: str, r: QualityResult) -> None:
    """Print a single model's quality response."""
    check = f" [{r.auto_check}]" if r.auto_check != "—" else ""
    print(f"\n  [{label}] {r.tokens} tok, {r.generation_s:.2f}s{check}")
    for line in r.response.splitlines():
        print(f"    {line}")


def _print_quality_side_by_side(
    label_a: str,
    results_a: list[QualityResult],
    label_b: str,
    results_b: list[QualityResult],
) -> None:
    """Print side-by-side quality comparison between two quantizations."""
    if not results_a or not results_b:
        return
    print()
    print(f"  Quality comparison: {label_a} vs {label_b}")
    for ra, rb in zip(results_a, results_b, strict=False):
        print(f"\n  {'─' * 74}")
        print(f"  {ra.category}")
        print(f"  {'─' * 74}")
        _print_quality_response(label_a, ra)
        _print_quality_response(label_b, rb)
    print(f"\n  {'─' * 74}")

    # Summary table
    print(f"\n  {'Category':<20} {label_a + ' check':>15} {label_b + ' check':>15}")
    print(f"  {'─' * 52}")
    for ra, rb in zip(results_a, results_b, strict=False):
        print(f"  {ra.category:<20} {ra.auto_check:>15} {rb.auto_check:>15}")


# =============================================================================
# Report: final
# =============================================================================


def _print_per_model_tables(results: BenchmarkResults) -> None:
    """Print individual model tables (loads, inference, sweeps, optimizations)."""
    q4, q2 = results.q4, results.q2
    _print_load_table(f"{q4.label} load times" if q4 else "Q4 load times", results.q4_loads)
    if q2:
        _print_load_table(f"{q2.label} load times", results.q2_loads)
    _print_load_table("Normal (Dense 8B) load times", results.normal_loads)

    _print_inference_table(
        f"{q4.label} GPU inference" if q4 else "Q4 inference", results.q4_inference
    )
    if q2:
        _print_inference_table(f"{q2.label} GPU inference", results.q2_inference)
    _print_inference_table("Normal (Dense 8B) GPU inference", results.normal_inference)

    _print_sweep_table(f"{q4.label} GPU layer sweep" if q4 else "Q4 sweep", results.q4_sweep)
    if q2:
        _print_sweep_table(f"{q2.label} GPU layer sweep", results.q2_sweep)

    if results.q2_flash_attn:
        _print_inference_table(
            f"{q2.label} flash attention" if q2 else "Flash attn", results.q2_flash_attn
        )
    if results.q2_kv_cache:
        _print_inference_table(
            f"{q2.label} KV cache quant" if q2 else "KV cache", results.q2_kv_cache
        )

    if results.cross_quant:
        _print_load_table("Cross-quant transitions (Q4 ↔ Q2)", results.cross_quant)


def _print_final_report(results: BenchmarkResults) -> None:
    """Print the unified final report."""
    _section("BENCHMARK RESULTS")

    _print_tier_switching_table(results.switching)
    _print_per_model_tables(results)

    q4, q2 = results.q4, results.q2
    if q4 and q2:
        _print_load_side_by_side(q4.label, results.q4_loads, q2.label, results.q2_loads)
        _print_inference_side_by_side(
            q4.label, results.q4_inference, q2.label, results.q2_inference
        )
        _print_sweep_side_by_side(q4.label, results.q4_sweep, q2.label, results.q2_sweep)
        _print_quality_side_by_side(
            q4.label,
            results.q4_quality_nothink,
            q2.label,
            results.q2_quality_nothink,
        )

    if q4:
        _print_quality_side_by_side(
            f"{q4.label} think",
            results.q4_quality_think,
            f"{q4.label} no-think",
            results.q4_quality_nothink,
        )
    if q2:
        _print_quality_side_by_side(
            f"{q2.label} think",
            results.q2_quality_think,
            f"{q2.label} no-think",
            results.q2_quality_nothink,
        )


# =============================================================================
# Main
# =============================================================================


def _build_specs(config: object) -> tuple[ModelSpec, ModelSpec | None, ModelSpec | None]:
    """Build ModelSpec instances from config tiers."""
    tiers = config.models.tiers  # type: ignore[union-attr]
    q4_cfg = tiers.get("thinking_q4") or tiers.get("thinking")
    q2_cfg = tiers.get("thinking_q2")
    normal_cfg = tiers.get("normal")

    if not q4_cfg:
        msg = "No thinking_q4 or thinking tier configured"
        raise ValueError(msg)

    q4 = ModelSpec(
        "MoE Q4_K_M", str(q4_cfg.path.expanduser()), q4_cfg.gpu_layers, q4_cfg.context_length
    )
    q2 = (
        ModelSpec(
            "MoE Q2_K", str(q2_cfg.path.expanduser()), q2_cfg.gpu_layers, q2_cfg.context_length
        )
        if q2_cfg
        else None
    )
    normal = (
        ModelSpec(
            "Dense 8B",
            str(normal_cfg.path.expanduser()),
            normal_cfg.gpu_layers,
            normal_cfg.context_length,
        )
        if normal_cfg
        else None
    )
    return q4, q2, normal


def _run_rate_benchmark(
    q4: ModelSpec,
    q2: ModelSpec | None,
    normal: ModelSpec | None,
    results: BenchmarkResults,
) -> None:
    """Sections 2-5: loads, inference, sweeps, cross-quant."""
    _section(f"2a. Load benchmarks: {q4.label}")
    results.q4_loads = _bench_load_suite(q4)
    if q2:
        _section(f"2b. Load benchmarks: {q2.label}")
        results.q2_loads = _bench_load_suite(q2)
    if normal:
        _section("2c. Load benchmarks: Dense 8B")
        results.normal_loads = _bench_load_suite(normal)

    _section(f"3a. GPU inference: {q4.label}")
    results.q4_inference = _bench_inference_suite(q4)
    if q2:
        _section(f"3b. GPU inference: {q2.label}")
        results.q2_inference = _bench_inference_suite(q2)
    if normal:
        _section("3c. GPU inference: Dense 8B")
        results.normal_inference = _bench_inference_suite(normal)

    n_layers = _get_block_count(q4.path)
    _section(f"4a. GPU layer sweep: {q4.label} ({n_layers} layers)")
    results.q4_sweep = _sweep_gpu_layers(q4, n_layers)
    if q2:
        _section(f"4b. GPU layer sweep: {q2.label} ({n_layers} layers)")
        results.q2_sweep = _sweep_gpu_layers(q2, n_layers)

    if q2:
        _section(f"4c. Flash attention: {q2.label}")
        results.q2_flash_attn = _bench_flag_variants(q2, FLASH_ATTN_CONFIGS)
        _section(f"4d. KV cache quantization: {q2.label}")
        results.q2_kv_cache = _bench_flag_variants(q2, KV_CACHE_CONFIGS)

    if q2:
        _section("5. Cross-quant transitions (Q4 ↔ Q2)")
        results.cross_quant = _bench_cross_quant(q4, q2)


def _run_quality_benchmark(
    q4: ModelSpec,
    q2: ModelSpec | None,
    results: BenchmarkResults,
) -> None:
    """Section 6: quality comparison (think + no-think per model)."""
    _section(f"6a. Quality: {q4.label} (think)")
    results.q4_quality_think = _bench_quality_suite(q4, thinking=True)
    _section(f"6b. Quality: {q4.label} (no-think)")
    results.q4_quality_nothink = _bench_quality_suite(q4, thinking=False)
    if q2:
        _section(f"6c. Quality: {q2.label} (think)")
        results.q2_quality_think = _bench_quality_suite(q2, thinking=True)
        _section(f"6d. Quality: {q2.label} (no-think)")
        results.q2_quality_nothink = _bench_quality_suite(q2, thinking=False)


async def run_benchmark(mode: str = "all") -> None:
    """Run the benchmark.

    Args:
        mode: "rate" (sections 1-5), "quality" (section 6), or "all".
    """
    loader = ConfigLoader(
        project_root=EXAMPLE_ROOT,
        app_dir_name=".moe-vram-benchmark",
        default_config_path=EXAMPLE_ROOT / "config.yaml",
        global_config_dir=None,
    )
    # Load config.yaml as CLI override so edits always take effect
    # (ConfigLoader seeds config.local.yaml once and reads from that;
    # without this, changes to config.yaml are silently ignored)
    from entropic.config.loader import load_yaml_config

    overrides = load_yaml_config(EXAMPLE_ROOT / "config.yaml")
    config = loader.load(cli_overrides=overrides)
    setup_logging(config, project_dir=EXAMPLE_ROOT, app_dir_name=".moe-vram-benchmark")

    q4, q2, normal = _build_specs(config)
    run_rate = mode in ("rate", "all")
    run_quality = mode in ("quality", "all")

    vram_used, vram_total = _get_vram_mb()
    print(f"  GPU: {vram_total} MB VRAM ({vram_used} MB used)")
    print(f"  Models: {q4.label} ({Path(q4.path).name})")
    if q2:
        print(f"          {q2.label} ({Path(q2.path).name})")
    if normal:
        print(f"          {normal.label} ({Path(normal.path).name})")
    print(f"  Mode: {mode}")

    results = BenchmarkResults(q4=q4, q2=q2)

    if run_rate:
        # Section 1: Orchestrator tier switching
        _section("1. Orchestrator tier switching")
        orchestrator = ModelOrchestrator(config)
        await orchestrator.initialize()
        engine = AgentEngine(
            orchestrator,
            config=config,
            loop_config=LoopConfig(max_iterations=3, auto_approve_tools=True),
        )
        results.switching = await _bench_tier_switching(engine)
        if engine.server_manager:
            await engine.server_manager.shutdown()
        await orchestrator.shutdown()

        # Sections 2-5: Load, inference, sweep, cross-quant
        _run_rate_benchmark(q4, q2, normal, results)

    if run_quality:
        _run_quality_benchmark(q4, q2, results)

    _print_final_report(results)
    print()
    print("  Benchmark complete.")
    print()


def _parse_mode() -> str:
    """Parse benchmark mode from CLI args."""
    valid = {"rate", "quality", "all"}
    if len(sys.argv) > 1:
        arg = sys.argv[1].lower()
        if arg in valid:
            return arg
        print(f"  Usage: python main.py [{' | '.join(sorted(valid))}]")
        sys.exit(1)
    return "all"


if __name__ == "__main__":
    try:
        asyncio.run(run_benchmark(_parse_mode()))
    except KeyboardInterrupt:
        print("\nInterrupted.")
    sys.exit(0)
