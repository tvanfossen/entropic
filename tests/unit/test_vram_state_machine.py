"""Tests for three-state VRAM lifecycle (COLD/WARM/ACTIVE)."""

import random
from pathlib import Path
from unittest.mock import MagicMock

import entropic
import pytest
from entropic.core.base import ModelState
from entropic.inference.orchestrator import ModelOrchestrator

# ---------------------------------------------------------------------------
# Identity discovery — avoids hardcoding tier names that rot on every rename
# ---------------------------------------------------------------------------
_PROMPTS_DIR = Path(entropic.__file__).parent / "data" / "prompts"


def _available_identity_names() -> list[str]:
    """Return names of all bundled identities (routable or not)."""
    result = []
    for path in sorted(_PROMPTS_DIR.glob("identity_*.md")):
        name = path.stem.removeprefix("identity_")
        result.append(name)
    return result


# ---------------------------------------------------------------------------
# Mock infrastructure
# ---------------------------------------------------------------------------


class MockModelConfig:
    def __init__(self, path: str = "/models/test.gguf", keep_warm: bool = False):
        self.path = Path(path)
        self.context_length = 4096
        self.allowed_tools = None
        self.keep_warm = keep_warm


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
    def __init__(self, path: str = "/models/test.gguf", keep_warm: bool = False):
        self.path = Path(path)
        self.context_length = 4096
        self.gpu_layers = -1
        self.keep_warm = keep_warm
        self.use_mlock = True
        self.adapter = "qwen2"
        self.logits_all = False
        self.allowed_tools = None
        self.identity = None
        self.routable = True


class MockEntropyConfig:
    """Config that samples two real identity names to avoid hardcoding tier names."""

    def __init__(self, warm_non_default: bool = False):
        names = random.sample(_available_identity_names(), 2)
        self.default_tier, self.alt_tier = names[0], names[1]

        self.models = MagicMock()
        self.models.default = self.default_tier
        self.models.tiers = {
            self.default_tier: MockTierConfig(f"/models/{self.default_tier}.gguf"),
            self.alt_tier: MockTierConfig(
                f"/models/{self.alt_tier}.gguf", keep_warm=warm_non_default
            ),
        }
        self.models.router = None

        self.routing = MagicMock()
        self.routing.enabled = False
        self.routing.fallback_tier = self.default_tier
        self.routing.tier_map = {}
        self.routing.handoff_rules = {}


def _make_orchestrator_with_mocks(
    config: MockEntropyConfig,
) -> tuple[ModelOrchestrator, dict[str, MockModelBackend]]:
    """Create orchestrator with injected mock backends."""
    orch = ModelOrchestrator(config)  # type: ignore[arg-type]

    default_tier = orch._find_tier(config.default_tier)
    alt_tier = orch._find_tier(config.alt_tier)
    assert default_tier is not None, f"Tier '{config.default_tier}' not found"
    assert alt_tier is not None, f"Tier '{config.alt_tier}' not found"

    mocks: dict[str, MockModelBackend] = {
        config.default_tier: MockModelBackend(
            MockModelConfig(f"/models/{config.default_tier}.gguf"), config.default_tier
        ),
        config.alt_tier: MockModelBackend(
            MockModelConfig(f"/models/{config.alt_tier}.gguf"), config.alt_tier
        ),
    }
    orch._tiers = {  # type: ignore[assignment]
        default_tier: mocks[config.default_tier],
        alt_tier: mocks[config.alt_tier],
    }
    orch._router = None  # type: ignore[assignment]
    return orch, mocks


# ---------------------------------------------------------------------------
# Tests: Tier swap target state is config-driven (keep_warm)
# ---------------------------------------------------------------------------


class TestSwapTargetState:
    """Swap target state depends on keep_warm config of the outgoing model."""

    @pytest.mark.asyncio
    async def test_swap_unloads_to_cold_when_keep_warm_false(self) -> None:
        """Default: keep_warm=False → full unload (ACTIVE → COLD)."""
        config = MockEntropyConfig()
        orch, mocks = _make_orchestrator_with_mocks(config)

        default_tier = orch._find_tier(config.default_tier)
        alt_tier = orch._find_tier(config.alt_tier)
        assert default_tier is not None
        assert alt_tier is not None

        await mocks[config.default_tier].load()
        orch._loaded_main_tier = default_tier

        await orch._get_model(alt_tier)

        # keep_warm=False → full unload to COLD
        assert mocks[config.default_tier].state == ModelState.COLD
        assert mocks[config.default_tier]._unload_called == 1
        assert mocks[config.default_tier]._deactivate_called == 0

    @pytest.mark.asyncio
    async def test_swap_deactivates_to_warm_when_keep_warm_true(self) -> None:
        """keep_warm=True → deactivate only (ACTIVE → WARM)."""
        config = MockEntropyConfig()
        orch, mocks = _make_orchestrator_with_mocks(config)

        # Override the default tier's config to have keep_warm=True
        mocks[config.default_tier].config.keep_warm = True

        default_tier = orch._find_tier(config.default_tier)
        alt_tier = orch._find_tier(config.alt_tier)
        assert default_tier is not None
        assert alt_tier is not None

        await mocks[config.default_tier].load()
        orch._loaded_main_tier = default_tier

        await orch._get_model(alt_tier)

        # keep_warm=True → deactivate to WARM (keep CPU pages locked)
        assert mocks[config.default_tier].state == ModelState.WARM
        assert mocks[config.default_tier]._deactivate_called == 1
        assert mocks[config.default_tier]._unload_called == 0

    @pytest.mark.asyncio
    async def test_swap_activates_new_model(self) -> None:
        config = MockEntropyConfig()
        orch, mocks = _make_orchestrator_with_mocks(config)

        default_tier = orch._find_tier(config.default_tier)
        alt_tier = orch._find_tier(config.alt_tier)
        assert default_tier is not None
        assert alt_tier is not None

        await mocks[config.default_tier].load()
        orch._loaded_main_tier = default_tier

        result = await orch._get_model(alt_tier)

        assert result is mocks[config.alt_tier]
        assert mocks[config.alt_tier].state == ModelState.ACTIVE

    @pytest.mark.asyncio
    async def test_cold_to_active_uses_load(self) -> None:
        """First load of a model goes COLD → ACTIVE via load()."""
        config = MockEntropyConfig()
        orch, mocks = _make_orchestrator_with_mocks(config)

        default_tier = orch._find_tier(config.default_tier)
        assert default_tier is not None

        assert mocks[config.default_tier].state == ModelState.COLD
        await orch._get_model(default_tier)
        assert mocks[config.default_tier].state == ModelState.ACTIVE
        assert mocks[config.default_tier]._load_called == 1


# ---------------------------------------------------------------------------
# Tests: keep_warm
# ---------------------------------------------------------------------------


class TestWarmOnStartup:
    """keep_warm=True causes warm() to be called in initialize()."""

    @pytest.mark.asyncio
    async def test_keep_warm_calls_warm_for_non_default_tiers(self) -> None:
        config = MockEntropyConfig(warm_non_default=True)
        warm_called: list[str] = []

        def mock_factory(model_config, tier_name):
            backend = MockModelBackend(MockModelConfig(str(model_config.path)), tier_name)

            async def tracking_warm() -> None:
                warm_called.append(tier_name)
                backend._state = ModelState.WARM

            backend.warm = tracking_warm  # type: ignore[method-assign]
            return backend

        orch = ModelOrchestrator(config, backend_factory=mock_factory)  # type: ignore[arg-type]
        await orch.initialize()

        # alt tier has keep_warm=True and is not the default tier
        assert config.alt_tier in warm_called

    @pytest.mark.asyncio
    async def test_keep_warm_false_does_not_warm(self) -> None:
        config = MockEntropyConfig(warm_non_default=False)
        warm_called: list[str] = []

        def mock_factory(model_config, tier_name):
            backend = MockModelBackend(MockModelConfig(str(model_config.path)), tier_name)

            async def tracking_warm() -> None:
                warm_called.append(tier_name)
                backend._state = ModelState.WARM

            backend.warm = tracking_warm  # type: ignore[method-assign]
            return backend

        orch = ModelOrchestrator(config, backend_factory=mock_factory)  # type: ignore[arg-type]
        await orch.initialize()

        # alt tier has keep_warm=False — should NOT be pre-warmed
        assert config.alt_tier not in warm_called
