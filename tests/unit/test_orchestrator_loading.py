"""Tests for model loading and swapping in ModelOrchestrator."""

import asyncio
import random
from pathlib import Path
from unittest.mock import MagicMock

import entropic
import pytest
from entropic.core.base import ModelState, ModelTier
from entropic.inference.orchestrator import ModelOrchestrator

# ---------------------------------------------------------------------------
# Identity discovery — avoids hardcoding tier names that rot on every rename
# ---------------------------------------------------------------------------
_PROMPTS_DIR = Path(entropic.__file__).parent / "data" / "prompts"


def _available_identity_names() -> list[str]:
    """Return names of all bundled routable identities."""
    import yaml

    result = []
    for path in sorted(_PROMPTS_DIR.glob("identity_*.md")):
        name = path.stem.removeprefix("identity_")
        text = path.read_text()
        if not text.startswith("---"):
            continue
        end = text.index("---", 3)
        fm = yaml.safe_load(text[3:end])
        if fm.get("routable", True):
            result.append(name)
    return result


# ---------------------------------------------------------------------------
# Mock infrastructure
# ---------------------------------------------------------------------------


class MockModelConfig:
    """Mock model configuration."""

    def __init__(self, path: str = "/models/test.gguf", context_length: int = 4096):
        self.path = Path(path)
        self.context_length = context_length
        self.allowed_tools = None
        self.keep_warm = False


class MockModelBackend:
    """Mock model backend for testing (three-state lifecycle)."""

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
        """Convenience: warm() + activate()."""
        self._load_called += 1
        self._state = ModelState.ACTIVE

    async def unload(self) -> None:
        self._unload_called += 1
        self._state = ModelState.COLD


class MockTierConfig:
    """Mock tier config — hardware params only (no inference params)."""

    def __init__(self, path: str = "/models/test.gguf", context_length: int = 4096):
        self.path = Path(path)
        self.context_length = context_length
        self.gpu_layers = -1
        self.keep_warm = False
        self.use_mlock = True
        self.adapter = "qwen2"
        self.logits_all = False
        self.allowed_tools = None
        self.identity = None
        self.routable = True


class MockEntropyConfig:
    """Config that samples four real identity names to avoid hardcoding tier names."""

    def __init__(self):
        names = random.sample(_available_identity_names(), 4)
        # Semantic roles used in tests — actual names are random
        self.default_tier = names[0]  # loaded by default
        self.tier_b = names[1]  # first swap target
        self.tier_c = names[2]  # second swap target
        self.tier_d = names[3]  # same-file tier in same_file tests

        self.models = MagicMock()
        self.models.default = self.default_tier
        self.models.tiers = {
            self.default_tier: MockTierConfig(f"/models/{self.default_tier}.gguf"),
            self.tier_b: MockTierConfig(f"/models/{self.tier_b}.gguf"),
            self.tier_c: MockTierConfig(f"/models/{self.tier_c}.gguf"),
            self.tier_d: MockTierConfig(f"/models/{self.tier_d}.gguf"),
        }
        self.models.router = MockTierConfig("/models/router.gguf")

        self.routing = MagicMock()
        self.routing.enabled = False
        self.routing.fallback_tier = self.default_tier
        self.routing.tier_map = {}
        self.routing.handoff_rules = {}


class TestModelOrchestratorLoading:
    """Tests for model loading behavior."""

    def setup_method(self):
        """Set up test fixtures."""
        self.config = MockEntropyConfig()

    def _create_orchestrator_with_mocks(
        self,
        same_file: bool = False,
    ) -> tuple[ModelOrchestrator, dict[ModelTier, MockModelBackend]]:
        """Create an orchestrator with mock backends."""
        orchestrator = ModelOrchestrator(self.config)  # type: ignore[arg-type]
        cfg = self.config

        default_path = f"/models/{cfg.default_tier}.gguf"

        t_default = orchestrator._find_tier(cfg.default_tier)
        t_b = orchestrator._find_tier(cfg.tier_b)
        t_c = orchestrator._find_tier(cfg.tier_c)
        t_d = orchestrator._find_tier(cfg.tier_d)
        assert t_default and t_b and t_c and t_d

        default_backend = MockModelBackend(MockModelConfig(default_path), cfg.default_tier)
        mocks: dict[ModelTier, MockModelBackend] = {
            t_default: default_backend,
            t_b: MockModelBackend(MockModelConfig(f"/models/{cfg.tier_b}.gguf"), cfg.tier_b),
            t_c: MockModelBackend(MockModelConfig(f"/models/{cfg.tier_c}.gguf"), cfg.tier_c),
            # same_file=True → share the same backend instance (dedup behavior)
            t_d: default_backend
            if same_file
            else MockModelBackend(MockModelConfig(f"/models/{cfg.tier_d}.gguf"), cfg.tier_d),
        }

        orchestrator._router = MockModelBackend(MockModelConfig("/models/router.gguf"), "router")  # type: ignore[assignment]
        orchestrator._tiers = mocks  # type: ignore[assignment]
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
        cfg = self.config
        t_default = self._tier(orchestrator, cfg.default_tier)

        # Simulate initialize() — load default and router
        await mocks[t_default].load()
        orchestrator._loaded_main_tier = t_default
        assert orchestrator._router is not None
        await orchestrator._router.load()

        loaded = orchestrator.get_loaded_models()

        assert cfg.default_tier in loaded
        assert "router" in loaded
        assert cfg.tier_b not in loaded
        assert cfg.tier_c not in loaded
        assert cfg.tier_d not in loaded
        assert len(loaded) == 2

    @pytest.mark.asyncio
    async def test_model_swap_unloads_previous(self) -> None:
        """Switching tiers fully unloads old model (ACTIVE → COLD) by default."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()
        cfg = self.config
        t_default = self._tier(orchestrator, cfg.default_tier)
        t_b = self._tier(orchestrator, cfg.tier_b)

        await mocks[t_default].load()
        orchestrator._loaded_main_tier = t_default

        assert mocks[t_default].is_loaded
        assert not mocks[t_b].is_loaded

        result = await orchestrator._get_model(t_b)

        assert result.is_loaded
        assert mocks[t_b].is_loaded
        # Default has keep_warm=False → full unload to COLD
        assert not mocks[t_default].is_loaded
        assert mocks[t_default]._unload_called == 1
        assert mocks[t_default]._deactivate_called == 0

    @pytest.mark.asyncio
    async def test_same_file_no_swap(self) -> None:
        """Same file for different tiers = shared backend, no swap needed."""
        orchestrator, mocks = self._create_orchestrator_with_mocks(same_file=True)
        cfg = self.config
        t_default = self._tier(orchestrator, cfg.default_tier)
        t_d = self._tier(orchestrator, cfg.tier_d)

        await mocks[t_default].load()
        orchestrator._loaded_main_tier = t_default

        # tier_d shares the same backend instance as default (dedup)
        result = await orchestrator._get_model(t_d)

        assert result is mocks[t_default]
        assert result is mocks[t_d]  # same object — backend dedup
        assert mocks[t_default].is_loaded
        assert mocks[t_default]._unload_called == 0
        assert mocks[t_default]._deactivate_called == 0

    @pytest.mark.asyncio
    async def test_get_loaded_models_accurate(self) -> None:
        """get_loaded_models() returns correct list."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()
        cfg = self.config
        t_default = self._tier(orchestrator, cfg.default_tier)

        assert orchestrator.get_loaded_models() == []

        await mocks[t_default].load()
        assert cfg.default_tier in orchestrator.get_loaded_models()

        assert orchestrator._router is not None
        await orchestrator._router.load()
        loaded = orchestrator.get_loaded_models()
        assert cfg.default_tier in loaded
        assert "router" in loaded

        await mocks[t_default].unload()
        loaded = orchestrator.get_loaded_models()
        assert cfg.default_tier not in loaded
        assert "router" in loaded

    @pytest.mark.asyncio
    async def test_already_loaded_returns_immediately(self) -> None:
        """Requesting already-loaded model returns it without swap."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()
        cfg = self.config
        t_default = self._tier(orchestrator, cfg.default_tier)

        await mocks[t_default].load()
        orchestrator._loaded_main_tier = t_default
        initial_load_count = mocks[t_default]._load_called

        result = await orchestrator._get_model(t_default)

        assert result is mocks[t_default]
        assert mocks[t_default]._load_called == initial_load_count

    @pytest.mark.asyncio
    async def test_fallback_when_tier_not_configured(self) -> None:
        """Falls back to default when requested tier not in _tiers."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()
        cfg = self.config
        t_default = self._tier(orchestrator, cfg.default_tier)
        t_c = self._tier(orchestrator, cfg.tier_c)

        # Remove tier_c from tiers
        del orchestrator._tiers[t_c]

        await mocks[t_default].load()
        orchestrator._loaded_main_tier = t_default

        result = await orchestrator._get_model(t_c)

        assert result is mocks[t_default]

    @pytest.mark.asyncio
    async def test_concurrent_requests_no_race(self) -> None:
        """Concurrent requests for same model don't cause race conditions."""
        orchestrator, mocks = self._create_orchestrator_with_mocks()
        cfg = self.config
        t_default = self._tier(orchestrator, cfg.default_tier)
        orchestrator._loaded_main_tier = None

        original_load = mocks[t_default].load

        async def slow_load():
            await asyncio.sleep(0.01)
            await original_load()

        mocks[t_default].load = slow_load

        results = await asyncio.gather(
            orchestrator._get_model(t_default),
            orchestrator._get_model(t_default),
            orchestrator._get_model(t_default),
        )

        assert all(r is mocks[t_default] for r in results)
        assert mocks[t_default].is_loaded

    @pytest.mark.asyncio
    async def test_tracks_loaded_main_tier(self) -> None:
        """_loaded_main_tier correctly tracks which main model is loaded."""
        orchestrator, _ = self._create_orchestrator_with_mocks()
        cfg = self.config
        t_default = self._tier(orchestrator, cfg.default_tier)
        t_b = self._tier(orchestrator, cfg.tier_b)
        t_c = self._tier(orchestrator, cfg.tier_c)

        assert orchestrator._loaded_main_tier is None

        await orchestrator._get_model(t_default)
        assert orchestrator._loaded_main_tier == t_default

        await orchestrator._get_model(t_b)
        assert orchestrator._loaded_main_tier == t_b

        await orchestrator._get_model(t_c)
        assert orchestrator._loaded_main_tier == t_c


class TestBackendFactory:
    """Tests for custom backend factory injection."""

    def setup_method(self):
        self.config = MockEntropyConfig()

    @pytest.mark.asyncio
    async def test_custom_factory_called_for_each_tier(self) -> None:
        """Custom backend_factory is used instead of LlamaCppBackend."""
        created: list[tuple[str, str]] = []

        def mock_factory(model_config, tier_name):
            created.append((str(model_config.path), tier_name))
            return MockModelBackend(MockModelConfig(str(model_config.path)), tier_name)

        orchestrator = ModelOrchestrator(self.config, backend_factory=mock_factory)  # type: ignore[arg-type]
        await orchestrator.initialize()

        cfg = self.config
        tier_names = [name for _, name in created]
        assert cfg.default_tier in tier_names
        assert cfg.tier_b in tier_names
        assert cfg.tier_c in tier_names
        assert cfg.tier_d in tier_names
        assert "router" in tier_names

    def test_default_factory_when_none_provided(self) -> None:
        """Without backend_factory, _default_backend_factory is used."""
        orchestrator = ModelOrchestrator(self.config)  # type: ignore[arg-type]
        assert orchestrator._backend_factory == orchestrator._default_backend_factory


class TestIdentityFileValidation:
    """Tests for identity file validation via PromptManager."""

    def setup_method(self):
        self.config = MockEntropyConfig()

    def test_custom_identity_missing_raises(self, tmp_path: Path) -> None:
        """PromptManager raises when custom identity path doesn't exist."""
        tier_name = self.config.default_tier
        self.config.models.tiers[tier_name].identity = tmp_path / "nonexistent.md"

        with pytest.raises(FileNotFoundError):
            ModelOrchestrator(self.config)  # type: ignore[arg-type]

    def test_custom_identity_exists_passes(self, tmp_path: Path) -> None:
        """PromptManager loads when custom identity path exists."""
        tier_name = self.config.default_tier
        identity_path = tmp_path / f"identity_{tier_name}.md"
        identity_path.write_text(
            f"---\ntype: identity\nversion: 1\nname: {tier_name}\n"
            "focus:\n  - testing\n---\n# Custom Identity\n"
        )
        self.config.models.tiers[tier_name].identity = identity_path

        orchestrator = ModelOrchestrator(self.config)  # type: ignore[arg-type]
        assert orchestrator._prompt_manager.get_identity(tier_name) is not None

    def test_bundled_default_loads(self) -> None:
        """Bundled identity loads when tier identity is None (default)."""
        tier_name = self.config.default_tier
        orchestrator = ModelOrchestrator(self.config)  # type: ignore[arg-type]
        assert orchestrator._prompt_manager.get_identity(tier_name) is not None

    def test_identity_disabled(self) -> None:
        """Identity disabled when tier identity is False."""
        tier_name = self.config.default_tier
        self.config.models.tiers[tier_name].identity = False

        orchestrator = ModelOrchestrator(self.config)  # type: ignore[arg-type]
        assert orchestrator._prompt_manager.get_identity(tier_name) is None
