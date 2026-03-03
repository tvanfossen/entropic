"""Tests for three-state VRAM lifecycle (COLD/WARM/ACTIVE)."""

from pathlib import Path
from unittest.mock import MagicMock

import pytest
from entropic.core.base import ModelState
from entropic.inference.orchestrator import ModelOrchestrator


class MockModelConfig:
    def __init__(self, path: str = "/models/test.gguf"):
        self.path = Path(path)
        self.context_length = 4096
        self.max_output_tokens = 4096
        self.allowed_tools = None


class MockModelBackend:
    """Three-state mock backend."""

    def __init__(self, config: MockModelConfig, tier: str):
        self.config = config
        self.tier = tier
        self._state: ModelState = ModelState.COLD
        self._load_called = 0
        self._unload_called = 0
        self._warm_called = 0
        self._activate_called = 0
        self._deactivate_called = 0

    @property
    def is_loaded(self) -> bool:
        return self._state == ModelState.ACTIVE

    @property
    def state(self) -> ModelState:
        return self._state

    async def warm(self) -> None:
        self._warm_called += 1
        if self._state == ModelState.COLD:
            self._state = ModelState.WARM

    async def activate(self, gpu_layers: int = -1) -> None:
        self._activate_called += 1
        if self._state == ModelState.COLD:
            await self.warm()
        self._state = ModelState.ACTIVE

    async def deactivate(self) -> None:
        self._deactivate_called += 1
        self._state = ModelState.WARM

    async def load(self) -> None:
        self._load_called += 1
        self._state = ModelState.ACTIVE

    async def unload(self) -> None:
        self._unload_called += 1
        self._state = ModelState.COLD


class MockTierConfig:
    def __init__(self, path: str = "/models/test.gguf", warm_on_startup: bool = False):
        self.path = Path(path)
        self.context_length = 4096
        self.max_output_tokens = 4096
        self.gpu_layers = -1
        self.warm_on_startup = warm_on_startup
        self.use_mlock = True
        self.adapter = "qwen2"
        self.temperature = 0.7
        self.top_p = 0.9
        self.top_k = 40
        self.repeat_penalty = 1.1
        self.allowed_tools = None
        self.focus = []


class MockEntropyConfig:
    def __init__(self, warm_non_default: bool = False):
        self.models = MagicMock()
        self.models.default = "normal"
        self.models.tiers = {
            "normal": MockTierConfig("/models/normal.gguf"),
            "code": MockTierConfig("/models/code.gguf", warm_on_startup=warm_non_default),
            "thinking": MockTierConfig("/models/thinking.gguf"),
        }
        self.models.router = None

        self.routing = MagicMock()
        self.routing.enabled = False
        self.routing.fallback_tier = "normal"
        self.routing.tier_map = {}
        self.routing.handoff_rules = {}

        self.thinking = MagicMock()
        self.thinking.enabled = False


def _make_orchestrator_with_mocks(
    config: MockEntropyConfig,
) -> tuple[ModelOrchestrator, dict[str, MockModelBackend]]:
    """Create orchestrator with injected mock backends."""
    orch = ModelOrchestrator(config)

    normal = orch._find_tier("normal")
    code = orch._find_tier("code")
    thinking = orch._find_tier("thinking")

    mocks: dict[str, MockModelBackend] = {
        "normal": MockModelBackend(MockModelConfig("/models/normal.gguf"), "normal"),
        "code": MockModelBackend(MockModelConfig("/models/code.gguf"), "code"),
        "thinking": MockModelBackend(MockModelConfig("/models/thinking.gguf"), "thinking"),
    }
    orch._tiers = {normal: mocks["normal"], code: mocks["code"], thinking: mocks["thinking"]}
    orch._router = None
    return orch, mocks


class TestSwapLeavesModelWarm:
    """Tier swap deactivates old model (ACTIVE → WARM), not unloads it (ACTIVE → COLD)."""

    @pytest.mark.asyncio
    async def test_swap_leaves_previous_in_warm_state(self) -> None:
        config = MockEntropyConfig()
        orch, mocks = _make_orchestrator_with_mocks(config)
        normal_tier = orch._find_tier("normal")
        code_tier = orch._find_tier("code")

        # Start with normal loaded
        await mocks["normal"].load()
        orch._loaded_main_tier = normal_tier

        # Request code — triggers deactivate(normal) + load(code)
        await orch._get_model(code_tier)

        # Normal is WARM (not COLD): pages stay in RAM, fast re-activate
        assert mocks["normal"].state == ModelState.WARM
        assert mocks["normal"]._deactivate_called == 1
        assert mocks["normal"]._unload_called == 0

    @pytest.mark.asyncio
    async def test_swap_activates_new_model(self) -> None:
        config = MockEntropyConfig()
        orch, mocks = _make_orchestrator_with_mocks(config)
        normal_tier = orch._find_tier("normal")
        code_tier = orch._find_tier("code")

        await mocks["normal"].load()
        orch._loaded_main_tier = normal_tier

        result = await orch._get_model(code_tier)

        assert result is mocks["code"]
        assert mocks["code"].state == ModelState.ACTIVE

    @pytest.mark.asyncio
    async def test_cold_to_active_uses_load(self) -> None:
        """First load of a model goes COLD → ACTIVE via load()."""
        config = MockEntropyConfig()
        orch, mocks = _make_orchestrator_with_mocks(config)
        normal_tier = orch._find_tier("normal")

        assert mocks["normal"].state == ModelState.COLD
        await orch._get_model(normal_tier)
        assert mocks["normal"].state == ModelState.ACTIVE
        assert mocks["normal"]._load_called == 1


class TestWarmOnStartup:
    """warm_on_startup=True causes warm() to be called in initialize()."""

    @pytest.mark.asyncio
    async def test_warm_on_startup_calls_warm_for_non_default_tiers(self) -> None:
        config = MockEntropyConfig(warm_non_default=True)
        warm_called: list[str] = []

        def mock_factory(model_config, tier_name):
            backend = MockModelBackend(MockModelConfig(str(model_config.path)), tier_name)

            async def tracking_warm() -> None:
                warm_called.append(tier_name)
                backend._state = ModelState.WARM

            backend.warm = tracking_warm  # type: ignore[method-assign]
            return backend

        orch = ModelOrchestrator(config, backend_factory=mock_factory)
        await orch.initialize()

        # Code tier has warm_on_startup=True and is not the default tier
        assert "code" in warm_called

    @pytest.mark.asyncio
    async def test_warm_on_startup_false_does_not_warm(self) -> None:
        config = MockEntropyConfig(warm_non_default=False)
        warm_called: list[str] = []

        def mock_factory(model_config, tier_name):
            backend = MockModelBackend(MockModelConfig(str(model_config.path)), tier_name)

            async def tracking_warm() -> None:
                warm_called.append(tier_name)
                backend._state = ModelState.WARM

            backend.warm = tracking_warm  # type: ignore[method-assign]
            return backend

        orch = ModelOrchestrator(config, backend_factory=mock_factory)
        await orch.initialize()

        # Code tier has warm_on_startup=False — should NOT be pre-warmed
        assert "code" not in warm_called


class TestThinkingModeDeactivates:
    """set_thinking_mode() deactivates the opposite tier (ACTIVE → WARM)."""

    @pytest.mark.asyncio
    async def test_thinking_mode_on_deactivates_normal(self) -> None:
        config = MockEntropyConfig()
        orch, mocks = _make_orchestrator_with_mocks(config)
        normal_tier = orch._find_tier("normal")

        # Start: normal is loaded, thinking mode off
        await mocks["normal"].load()
        orch._loaded_main_tier = normal_tier
        orch._thinking_mode = False

        result = await orch.set_thinking_mode(True)

        assert result is True
        # Normal deactivated to WARM (not unloaded to COLD)
        assert mocks["normal"].state == ModelState.WARM
        assert mocks["normal"]._deactivate_called == 1
        assert mocks["normal"]._unload_called == 0
        # Thinking now active
        assert mocks["thinking"].state == ModelState.ACTIVE

    @pytest.mark.asyncio
    async def test_thinking_mode_off_deactivates_thinking(self) -> None:
        config = MockEntropyConfig()
        orch, mocks = _make_orchestrator_with_mocks(config)
        thinking_tier = orch._find_tier("thinking")

        # Start: thinking is loaded
        await mocks["thinking"].load()
        orch._loaded_main_tier = thinking_tier
        orch._thinking_mode = True

        result = await orch.set_thinking_mode(False)

        assert result is True
        # Thinking deactivated to WARM
        assert mocks["thinking"].state == ModelState.WARM
        assert mocks["thinking"]._deactivate_called == 1
        assert mocks["thinking"]._unload_called == 0
        # Normal now active
        assert mocks["normal"].state == ModelState.ACTIVE
