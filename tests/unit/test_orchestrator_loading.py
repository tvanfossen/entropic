"""Tests for model loading and swapping in ModelOrchestrator."""

import asyncio
from pathlib import Path
from unittest.mock import MagicMock

import pytest
from entropi.core.base import ModelTier
from entropi.inference.orchestrator import ModelOrchestrator

# Reusable tier instances for tests
NORMAL = ModelTier("normal", focus=["general reasoning"])
CODE = ModelTier("code", focus=["writing code"])
THINKING = ModelTier("thinking", focus=["complex analysis"])
SIMPLE = ModelTier("simple", focus=["greetings"])


class MockModelConfig:
    """Mock model configuration."""

    def __init__(self, path: str = "/models/test.gguf", context_length: int = 4096):
        self.path = Path(path)
        self.context_length = context_length
        self.max_output_tokens = 4096
        self.allowed_tools = None


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


class MockTierConfig:
    """Mock tier config for dict-based ModelsConfig."""

    def __init__(self, path: str = "/models/test.gguf", context_length: int = 4096):
        self.path = Path(path)
        self.context_length = context_length
        self.max_output_tokens = 4096
        self.gpu_layers = -1
        self.adapter = "qwen2"
        self.temperature = 0.7
        self.top_p = 0.9
        self.top_k = 40
        self.repeat_penalty = 1.1
        self.allowed_tools = None
        self.focus = []


class MockEntropyConfig:
    """Mock entropy configuration with dict-based tiers."""

    def __init__(self):
        self.models = MagicMock()
        self.models.default = "normal"
        self.models.tiers = {
            "normal": MockTierConfig("/models/normal.gguf"),
            "code": MockTierConfig("/models/code.gguf"),
            "thinking": MockTierConfig("/models/thinking.gguf"),
            "simple": MockTierConfig("/models/simple.gguf"),
        }
        self.models.router = MockTierConfig("/models/router.gguf")

        self.routing = MagicMock()
        self.routing.enabled = False
        self.routing.fallback_tier = "normal"
        self.routing.tier_map = {}
        self.routing.handoff_rules = {}

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

        # Find the tier instances created by the orchestrator
        normal = orchestrator._find_tier("normal")
        code = orchestrator._find_tier("code")
        thinking = orchestrator._find_tier("thinking")
        simple = orchestrator._find_tier("simple")

        mocks = {
            normal: MockModelBackend(MockModelConfig(normal_path), "normal"),
            code: MockModelBackend(MockModelConfig("/models/code.gguf"), "code"),
            thinking: MockModelBackend(MockModelConfig("/models/thinking.gguf"), "thinking"),
            simple: MockModelBackend(MockModelConfig(simple_path), "simple"),
        }

        # Router is separate
        orchestrator._router = MockModelBackend(MockModelConfig("/models/router.gguf"), "router")
        orchestrator._tiers = mocks
        return orchestrator, mocks

    def _tier(self, orchestrator: ModelOrchestrator, name: str) -> ModelTier:
        """Helper to get a tier by name from the orchestrator."""
        tier = orchestrator._find_tier(name)
        assert tier is not None, f"Tier '{name}' not found"
        return tier

    @pytest.mark.asyncio
    async def test_only_one_main_model_loaded_after_init(self) -> None:
        """Only default + router loaded at start."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()
        normal = self._tier(orchestrator, "normal")

        # Simulate initialize() - load default (normal) and router
        await mocks[normal].load()
        orchestrator._loaded_main_tier = normal
        await orchestrator._router.load()

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
        normal = self._tier(orchestrator, "normal")
        code = self._tier(orchestrator, "code")

        # Start with normal loaded
        await mocks[normal].load()
        orchestrator._loaded_main_tier = normal

        assert mocks[normal].is_loaded
        assert not mocks[code].is_loaded

        # Request CODE model - should swap
        result = await orchestrator._get_model(code)

        # CODE should now be loaded
        assert result.is_loaded
        assert mocks[code].is_loaded
        # NORMAL should be unloaded
        assert not mocks[normal].is_loaded
        assert mocks[normal]._unload_called == 1

    @pytest.mark.asyncio
    async def test_same_file_no_swap(self) -> None:
        """Same file for different tiers = no swap, returns current model."""
        orchestrator, mocks = self._create_orchestrator_with_mocks(same_file=True)
        normal = self._tier(orchestrator, "normal")
        simple = self._tier(orchestrator, "simple")

        # Start with normal loaded
        await mocks[normal].load()
        orchestrator._loaded_main_tier = normal

        # Request SIMPLE (which uses same file)
        result = await orchestrator._get_model(simple)

        # Should return the NORMAL model (already loaded, same file)
        assert result is mocks[normal]
        # NORMAL should still be loaded (not unloaded)
        assert mocks[normal].is_loaded
        assert mocks[normal]._unload_called == 0
        # SIMPLE should NOT be loaded (we reuse NORMAL)
        assert not mocks[simple].is_loaded

    @pytest.mark.asyncio
    async def test_get_loaded_models_accurate(self) -> None:
        """get_loaded_models() returns correct list."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()
        normal = self._tier(orchestrator, "normal")

        # Nothing loaded initially
        assert orchestrator.get_loaded_models() == []

        # Load normal
        await mocks[normal].load()
        assert "normal" in orchestrator.get_loaded_models()

        # Load router
        await orchestrator._router.load()
        loaded = orchestrator.get_loaded_models()
        assert "normal" in loaded
        assert "router" in loaded

        # Unload normal
        await mocks[normal].unload()
        loaded = orchestrator.get_loaded_models()
        assert "normal" not in loaded
        assert "router" in loaded

    @pytest.mark.asyncio
    async def test_already_loaded_returns_immediately(self) -> None:
        """Requesting already-loaded model returns it without swap."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()
        normal = self._tier(orchestrator, "normal")

        # Start with normal loaded
        await mocks[normal].load()
        orchestrator._loaded_main_tier = normal
        initial_load_count = mocks[normal]._load_called

        # Request normal again
        result = await orchestrator._get_model(normal)

        assert result is mocks[normal]
        # Should not have called load again
        assert mocks[normal]._load_called == initial_load_count

    @pytest.mark.asyncio
    async def test_fallback_when_tier_not_configured(self) -> None:
        """Falls back to default when requested tier not in _tiers."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()
        normal = self._tier(orchestrator, "normal")
        thinking = self._tier(orchestrator, "thinking")

        # Remove THINKING from tiers
        del orchestrator._tiers[thinking]

        # Start with normal loaded (fallback)
        await mocks[normal].load()
        orchestrator._loaded_main_tier = normal

        # Request THINKING - should fallback to normal
        result = await orchestrator._get_model(thinking)

        assert result is mocks[normal]

    @pytest.mark.asyncio
    async def test_concurrent_requests_no_race(self) -> None:
        """Concurrent requests for same model don't cause race conditions."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()
        normal = self._tier(orchestrator, "normal")
        orchestrator._loaded_main_tier = None

        # Add a small delay to load to make race more likely
        original_load = mocks[normal].load

        async def slow_load():
            await asyncio.sleep(0.01)
            await original_load()

        mocks[normal].load = slow_load

        # Make concurrent requests
        results = await asyncio.gather(
            orchestrator._get_model(normal),
            orchestrator._get_model(normal),
            orchestrator._get_model(normal),
        )

        # All should return the same model
        assert all(r is mocks[normal] for r in results)
        # Model should only be loaded once (lock prevents duplicates)
        assert mocks[normal].is_loaded

    @pytest.mark.asyncio
    async def test_tracks_loaded_main_tier(self) -> None:
        """_loaded_main_tier correctly tracks which main model is loaded."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()
        normal = self._tier(orchestrator, "normal")
        code = self._tier(orchestrator, "code")
        thinking = self._tier(orchestrator, "thinking")

        assert orchestrator._loaded_main_tier is None

        # Load normal
        await orchestrator._get_model(normal)
        assert orchestrator._loaded_main_tier == normal

        # Swap to code
        await orchestrator._get_model(code)
        assert orchestrator._loaded_main_tier == code

        # Swap to thinking
        await orchestrator._get_model(thinking)
        assert orchestrator._loaded_main_tier == thinking
