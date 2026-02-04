"""Tests for model loading and swapping in ModelOrchestrator."""

import asyncio
from pathlib import Path
from unittest.mock import MagicMock

import pytest

from entropi.inference.orchestrator import ModelOrchestrator, ModelTier


class MockModelConfig:
    """Mock model configuration."""

    def __init__(self, path: str = "/models/test.gguf", context_length: int = 4096):
        self.path = Path(path)
        self.context_length = context_length


class MockModelBackend:
    """Mock model backend for testing."""

    def __init__(self, config: MockModelConfig, tier: str):
        self.config = config
        self.tier = tier
        self.is_loaded = False
        self._load_called = 0
        self._unload_called = 0

    async def load(self):
        self._load_called += 1
        self.is_loaded = True

    async def unload(self):
        self._unload_called += 1
        self.is_loaded = False


class MockEntropyConfig:
    """Mock entropy configuration."""

    def __init__(self):
        self.models = MagicMock()
        self.models.default = "normal"
        self.models.thinking = MagicMock()
        self.models.normal = MagicMock()
        self.models.code = MagicMock()
        self.models.simple = MagicMock()
        self.models.router = MagicMock()

        self.routing = MagicMock()
        self.routing.enabled = False
        self.routing.fallback_model = "normal"

        self.thinking = MagicMock()
        self.thinking.enabled = False

        self.prompts_dir = None


class TestModelOrchestratorLoading:
    """Tests for model loading behavior."""

    def setup_method(self):
        """Set up test fixtures."""
        self.config = MockEntropyConfig()

    def _create_orchestrator_with_mocks(
        self,
        same_file: bool = False,
    ) -> tuple[ModelOrchestrator, dict[ModelTier, MockModelBackend]]:
        """
        Create an orchestrator with mock backends.

        Args:
            same_file: If True, NORMAL and SIMPLE use same model file

        Returns:
            Tuple of (orchestrator, dict of mock backends by tier)
        """
        orchestrator = ModelOrchestrator(self.config)

        # Create mock backends
        normal_path = "/models/normal.gguf"
        simple_path = normal_path if same_file else "/models/simple.gguf"

        mocks = {
            ModelTier.NORMAL: MockModelBackend(MockModelConfig(normal_path), "normal"),
            ModelTier.CODE: MockModelBackend(MockModelConfig("/models/code.gguf"), "code"),
            ModelTier.THINKING: MockModelBackend(MockModelConfig("/models/thinking.gguf"), "thinking"),
            ModelTier.SIMPLE: MockModelBackend(MockModelConfig(simple_path), "simple"),
            ModelTier.ROUTER: MockModelBackend(MockModelConfig("/models/router.gguf"), "router"),
        }

        orchestrator._models = mocks
        return orchestrator, mocks

    @pytest.mark.asyncio
    async def test_only_one_main_model_loaded_after_init(self) -> None:
        """Only default + router loaded at start."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()

        # Simulate initialize() - load default (normal) and router
        await mocks[ModelTier.NORMAL].load()
        orchestrator._loaded_main_tier = ModelTier.NORMAL
        await mocks[ModelTier.ROUTER].load()

        loaded = orchestrator.get_loaded_models()

        # Only normal (default main) and router should be loaded
        assert "normal" in loaded
        assert "router" in loaded
        assert "thinking" not in loaded
        assert "code" not in loaded
        assert "simple" not in loaded

        # Only 2 models loaded
        assert len(loaded) == 2

    @pytest.mark.asyncio
    async def test_model_swap_unloads_previous(self) -> None:
        """Switching tiers unloads old model."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()

        # Start with normal loaded
        await mocks[ModelTier.NORMAL].load()
        orchestrator._loaded_main_tier = ModelTier.NORMAL

        assert mocks[ModelTier.NORMAL].is_loaded
        assert not mocks[ModelTier.CODE].is_loaded

        # Request CODE model - should swap
        result = await orchestrator._get_model(ModelTier.CODE)

        # CODE should now be loaded
        assert result.is_loaded
        assert mocks[ModelTier.CODE].is_loaded
        # NORMAL should be unloaded
        assert not mocks[ModelTier.NORMAL].is_loaded
        assert mocks[ModelTier.NORMAL]._unload_called == 1

    @pytest.mark.asyncio
    async def test_same_file_no_swap(self) -> None:
        """Same file for different tiers = no swap, returns current model."""
        orchestrator, mocks = self._create_orchestrator_with_mocks(same_file=True)

        # Start with normal loaded
        await mocks[ModelTier.NORMAL].load()
        orchestrator._loaded_main_tier = ModelTier.NORMAL

        # Request SIMPLE (which uses same file)
        result = await orchestrator._get_model(ModelTier.SIMPLE)

        # Should return the NORMAL model (already loaded, same file)
        assert result is mocks[ModelTier.NORMAL]
        # NORMAL should still be loaded (not unloaded)
        assert mocks[ModelTier.NORMAL].is_loaded
        assert mocks[ModelTier.NORMAL]._unload_called == 0
        # SIMPLE should NOT be loaded (we reuse NORMAL)
        assert not mocks[ModelTier.SIMPLE].is_loaded

    @pytest.mark.asyncio
    async def test_get_loaded_models_accurate(self) -> None:
        """get_loaded_models() returns correct list."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()

        # Nothing loaded initially
        assert orchestrator.get_loaded_models() == []

        # Load normal
        await mocks[ModelTier.NORMAL].load()
        assert "normal" in orchestrator.get_loaded_models()

        # Load router
        await mocks[ModelTier.ROUTER].load()
        loaded = orchestrator.get_loaded_models()
        assert "normal" in loaded
        assert "router" in loaded

        # Unload normal
        await mocks[ModelTier.NORMAL].unload()
        loaded = orchestrator.get_loaded_models()
        assert "normal" not in loaded
        assert "router" in loaded

    @pytest.mark.asyncio
    async def test_aux_tier_loads_without_swap(self) -> None:
        """Auxiliary tiers (ROUTER) load without affecting main tiers."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()

        # Start with normal loaded
        await mocks[ModelTier.NORMAL].load()
        orchestrator._loaded_main_tier = ModelTier.NORMAL

        # Request ROUTER - should NOT unload normal
        result = await orchestrator._get_model(ModelTier.ROUTER)

        assert result is mocks[ModelTier.ROUTER]
        assert mocks[ModelTier.ROUTER].is_loaded
        # NORMAL should still be loaded
        assert mocks[ModelTier.NORMAL].is_loaded
        assert mocks[ModelTier.NORMAL]._unload_called == 0

    @pytest.mark.asyncio
    async def test_already_loaded_returns_immediately(self) -> None:
        """Requesting already-loaded model returns it without swap."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()

        # Start with normal loaded
        await mocks[ModelTier.NORMAL].load()
        orchestrator._loaded_main_tier = ModelTier.NORMAL
        initial_load_count = mocks[ModelTier.NORMAL]._load_called

        # Request normal again
        result = await orchestrator._get_model(ModelTier.NORMAL)

        assert result is mocks[ModelTier.NORMAL]
        # Should not have called load again
        assert mocks[ModelTier.NORMAL]._load_called == initial_load_count

    @pytest.mark.asyncio
    async def test_fallback_when_tier_not_configured(self) -> None:
        """Falls back to default when requested tier not in _models."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()

        # Remove THINKING from models
        del orchestrator._models[ModelTier.THINKING]

        # Start with normal loaded (fallback)
        await mocks[ModelTier.NORMAL].load()
        orchestrator._loaded_main_tier = ModelTier.NORMAL

        # Request THINKING - should fallback to normal
        result = await orchestrator._get_model(ModelTier.THINKING)

        assert result is mocks[ModelTier.NORMAL]

    @pytest.mark.asyncio
    async def test_concurrent_requests_no_race(self) -> None:
        """Concurrent requests for same model don't cause race conditions."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()
        orchestrator._loaded_main_tier = None

        # Add a small delay to load to make race more likely
        original_load = mocks[ModelTier.NORMAL].load

        async def slow_load():
            await asyncio.sleep(0.01)
            await original_load()

        mocks[ModelTier.NORMAL].load = slow_load

        # Make concurrent requests
        results = await asyncio.gather(
            orchestrator._get_model(ModelTier.NORMAL),
            orchestrator._get_model(ModelTier.NORMAL),
            orchestrator._get_model(ModelTier.NORMAL),
        )

        # All should return the same model
        assert all(r is mocks[ModelTier.NORMAL] for r in results)
        # Model should only be loaded once (lock prevents duplicates)
        assert mocks[ModelTier.NORMAL].is_loaded

    @pytest.mark.asyncio
    async def test_tracks_loaded_main_tier(self) -> None:
        """_loaded_main_tier correctly tracks which main model is loaded."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()

        assert orchestrator._loaded_main_tier is None

        # Load normal
        await orchestrator._get_model(ModelTier.NORMAL)
        assert orchestrator._loaded_main_tier == ModelTier.NORMAL

        # Swap to code
        await orchestrator._get_model(ModelTier.CODE)
        assert orchestrator._loaded_main_tier == ModelTier.CODE

        # Swap to thinking
        await orchestrator._get_model(ModelTier.THINKING)
        assert orchestrator._loaded_main_tier == ModelTier.THINKING
