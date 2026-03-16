"""Unit tests for BenchmarkRunner measurement primitives."""

from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from entropic.benchmark.runner import BenchmarkRunner, _build_sweep_layers, make_model_spec
from entropic.benchmark.types import ModelSpec
from entropic.core.base import ModelState


def _make_spec(path: str = "/models/test.gguf") -> ModelSpec:
    return make_model_spec(Path(path), context_length=4096, gpu_layers=-1)


def _make_mock_backend() -> MagicMock:
    """Mock LlamaCppBackend with async lifecycle methods."""
    backend = MagicMock()
    backend._model = None
    backend._state = ModelState.COLD
    backend.load = AsyncMock()
    backend.unload = AsyncMock()
    backend.warm = AsyncMock()
    backend.activate = AsyncMock()
    backend.deactivate = AsyncMock()

    mock_result = MagicMock()
    mock_result.token_count = 64
    backend.complete = AsyncMock(return_value=mock_result)

    return backend


class TestBuildSweepLayers:
    """Tests for _build_sweep_layers() helper."""

    def test_includes_zero_and_max(self) -> None:
        layers = _build_sweep_layers(step=10, max_n=40)
        assert 0 in layers
        assert 40 in layers

    def test_increments_by_step(self) -> None:
        layers = _build_sweep_layers(step=10, max_n=40)
        assert layers == [0, 10, 20, 30, 40]

    def test_max_not_duplicated(self) -> None:
        """max_n already on a step boundary should not appear twice."""
        layers = _build_sweep_layers(step=10, max_n=30)
        assert layers.count(30) == 1

    def test_single_point(self) -> None:
        layers = _build_sweep_layers(step=100, max_n=40)
        assert layers == [0, 40]


class TestMakeModelSpec:
    """Tests for make_model_spec() convenience constructor."""

    def test_defaults(self) -> None:
        spec = make_model_spec(Path("/models/foo.gguf"))
        assert spec.context_length == 4096
        assert spec.gpu_layers == -1

    def test_custom_params(self) -> None:
        spec = make_model_spec(Path("/models/foo.gguf"), context_length=2048, gpu_layers=20)
        assert spec.context_length == 2048
        assert spec.gpu_layers == 20


class TestBenchmarkRunnerTimedColdLoad:
    """Tests for timed_cold_load()."""

    @pytest.mark.asyncio
    async def test_returns_load_result(self) -> None:
        """Returns a LoadResult with phase and timing."""
        spec = _make_spec()
        runner = BenchmarkRunner(spec)
        backend = _make_mock_backend()

        with (
            patch.object(runner, "_make_backend", return_value=backend),
            patch("entropic.benchmark.runner.get_vram_mb", return_value=(2048, 24576)),
            patch("entropic.benchmark.runner.free_model"),
        ):
            result = await runner.timed_cold_load()

        assert result.phase == "cold_to_active"
        assert result.elapsed_ms >= 0
        assert result.vram_used_mb == 2048
        backend.load.assert_awaited_once()
        backend.unload.assert_awaited_once()

    @pytest.mark.asyncio
    async def test_frees_model_if_present(self) -> None:
        """free_model() called when backend._model is not None."""
        spec = _make_spec()
        runner = BenchmarkRunner(spec)
        backend = _make_mock_backend()
        backend._model = MagicMock()  # non-None model

        with (
            patch.object(runner, "_make_backend", return_value=backend),
            patch("entropic.benchmark.runner.get_vram_mb", return_value=(0, 0)),
            patch("entropic.benchmark.runner.free_model") as mock_free,
        ):
            await runner.timed_cold_load()

        mock_free.assert_called_once_with(backend._model)


class TestBenchmarkRunnerTimedSwap:
    """Tests for timed_swap()."""

    @pytest.mark.asyncio
    async def test_returns_swap_result(self) -> None:
        """Returns a SwapResult with four transition timings."""
        spec = _make_spec()
        runner = BenchmarkRunner(spec)
        backend = _make_mock_backend()

        with (
            patch.object(runner, "_make_backend", return_value=backend),
            patch("entropic.benchmark.runner.get_vram_mb", return_value=(2048, 24576)),
            patch("entropic.benchmark.runner.free_model"),
        ):
            result = await runner.timed_swap()

        assert result.warm_ms >= 0
        assert result.activate_ms >= 0
        assert result.deactivate_ms >= 0
        assert result.reactivate_ms >= 0
        assert result.vram_warm_mb == 2048
        assert result.vram_active_mb == 2048

    @pytest.mark.asyncio
    async def test_calls_full_sequence(self) -> None:
        """Executes warm → activate → deactivate → activate in order."""
        spec = _make_spec()
        runner = BenchmarkRunner(spec)
        backend = _make_mock_backend()
        call_order = []
        backend.warm.side_effect = lambda: call_order.append("warm")
        backend.activate.side_effect = lambda gl: call_order.append(f"activate({gl})")
        backend.deactivate.side_effect = lambda: call_order.append("deactivate")

        with (
            patch.object(runner, "_make_backend", return_value=backend),
            patch("entropic.benchmark.runner.get_vram_mb", return_value=(0, 0)),
            patch("entropic.benchmark.runner.free_model"),
        ):
            await runner.timed_swap()

        assert call_order == ["warm", "activate(-1)", "deactivate", "activate(-1)"]


class TestBenchmarkRunnerTimedInference:
    """Tests for timed_inference()."""

    @pytest.mark.asyncio
    async def test_returns_inference_result(self) -> None:
        """Returns InferenceResult with token count and tok/s."""
        spec = _make_spec()
        runner = BenchmarkRunner(spec)
        backend = _make_mock_backend()

        with (
            patch.object(runner, "_make_backend", return_value=backend),
            patch("entropic.benchmark.runner.get_vram_mb", return_value=(3000, 24576)),
            patch("entropic.benchmark.runner.free_model"),
        ):
            result = await runner.timed_inference()

        assert result.tokens == 64
        assert result.elapsed_ms >= 0
        assert result.vram_used_mb == 3000
        assert result.gpu_layers == -1

    @pytest.mark.asyncio
    async def test_custom_gpu_layers(self) -> None:
        """Custom gpu_layers is passed to backend and stored in result."""
        spec = _make_spec()
        runner = BenchmarkRunner(spec)
        backend = _make_mock_backend()

        with (
            patch.object(runner, "_make_backend", return_value=backend),
            patch("entropic.benchmark.runner.get_vram_mb", return_value=(0, 0)),
            patch("entropic.benchmark.runner.free_model"),
        ):
            result = await runner.timed_inference(gpu_layers=20)

        assert result.gpu_layers == 20


class TestBenchmarkRunnerSweepOneLayer:
    """Tests for _sweep_one_layer()."""

    @pytest.mark.asyncio
    async def test_returns_sweep_point(self) -> None:
        """Returns a SweepPoint with timing data."""
        spec = _make_spec()
        runner = BenchmarkRunner(spec)
        backend = _make_mock_backend()

        with patch("entropic.benchmark.runner.get_vram_mb", return_value=(5000, 24576)):
            point = await runner._sweep_one_layer(backend, 20)

        assert point.gpu_layers == 20
        assert point.tok_s >= 0
        assert point.tokens == 64
        assert not point.oom
        backend.activate.assert_awaited_once_with(gpu_layers=20)
        backend.deactivate.assert_awaited_once()

    @pytest.mark.asyncio
    async def test_returns_oom_point_on_error(self) -> None:
        """Returns SweepPoint with oom=True if activate raises."""
        spec = _make_spec()
        runner = BenchmarkRunner(spec)
        backend = _make_mock_backend()
        backend.activate.side_effect = RuntimeError("CUDA out of memory")

        point = await runner._sweep_one_layer(backend, 40)

        assert point.gpu_layers == 40
        assert point.oom is True
        assert point.tok_s == 0
