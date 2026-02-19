"""Tests for library API gaps (P1-011 Phase 4).

Covers: register_server(), graceful identity fallback, constitution
always bundled, config-tier validation.
"""

from pathlib import Path
from unittest.mock import MagicMock

import pytest
from entropic.config.schema import PermissionsConfig
from entropic.core.base import ModelTier
from entropic.mcp.manager import ServerManager
from entropic.mcp.provider import InProcessProvider
from entropic.prompts import get_constitution_prompt, get_identity_prompt

# ── Gap 1: register_server() ─────────────────────────────────────


class FakeServer:
    """Minimal BaseMCPServer stand-in for testing registration."""

    def __init__(self, name: str) -> None:
        self.name = name
        self._tools = [
            {
                "name": "ping",
                "description": "Ping tool",
                "inputSchema": {"type": "object", "properties": {}},
            }
        ]

    def get_tools(self) -> list[dict[str, object]]:
        return self._tools


class TestRegisterServer:
    """Tests for ServerManager.register_server()."""

    def _create_manager(self) -> ServerManager:
        """Create a minimal ServerManager without full config."""
        manager = ServerManager.__new__(ServerManager)
        manager._permissions = PermissionsConfig(allow=[], deny=[])
        manager._clients = {}
        manager._server_classes = {}
        manager._project_dir = Path(".")
        manager._lsp_manager = None
        return manager

    def test_register_adds_client(self) -> None:
        """register_server() creates InProcessProvider in _clients."""
        manager = self._create_manager()
        server = FakeServer("chess")
        manager.register_server(server)

        assert "chess" in manager._clients
        assert isinstance(manager._clients["chess"], InProcessProvider)

    def test_register_adds_server_class(self) -> None:
        """register_server() records the server class for permission patterns."""
        manager = self._create_manager()
        server = FakeServer("chess")
        manager.register_server(server)

        assert "chess" in manager._server_classes
        assert manager._server_classes["chess"] is FakeServer

    def test_register_replaces_existing(self) -> None:
        """Registering same name twice replaces the previous entry."""
        manager = self._create_manager()
        server1 = FakeServer("chess")
        server2 = FakeServer("chess")
        manager.register_server(server1)
        manager.register_server(server2)

        # Should still have exactly one entry
        assert len([k for k in manager._clients if k == "chess"]) == 1


# ── Gap 4: Graceful identity fallback ────────────────────────────


class TestGracefulIdentityFallback:
    """get_identity_prompt() returns constitution alone when no identity file."""

    def test_returns_constitution_when_no_identity(self) -> None:
        """With use_bundled=True and no identity file, returns bundled constitution."""
        result = get_identity_prompt("nonexistent_tier", use_bundled=True)
        constitution = get_constitution_prompt()

        assert result == constitution
        assert len(result) > 0

    def test_returns_constitution_plus_identity_when_available(self, tmp_path: Path) -> None:
        """With a valid identity file and custom constitution, returns both."""
        constitution = tmp_path / "constitution.md"
        constitution.write_text("# Custom Safety\n\nBe a good chess engine.")

        identity = tmp_path / "identity_custom.md"
        identity.write_text("---\nname: custom\nfocus:\n  - testing\n---\n\nCustom body here")

        result = get_identity_prompt("custom", prompts_dir=tmp_path, use_bundled=False)

        assert "Custom body here" in result
        assert "Custom Safety" in result


# ── Gap 5: Constitution replacement ──────────────────────────────


class TestConstitutionReplacement:
    """Constitution can be replaced by consumers via prompts_dir."""

    def test_bundled_constitution_loads_by_default(self) -> None:
        """get_constitution_prompt() loads bundled constitution with defaults."""
        result = get_constitution_prompt(use_bundled=True)
        assert "Core Principles" in result

    def test_use_bundled_false_without_prompts_dir_raises(self) -> None:
        """use_bundled=False with no prompts_dir raises FileNotFoundError."""
        with pytest.raises(FileNotFoundError):
            get_constitution_prompt(use_bundled=False)

    def test_custom_constitution_overrides_bundled(self, tmp_path: Path) -> None:
        """Consumer constitution.md in prompts_dir takes precedence."""
        custom = tmp_path / "constitution.md"
        custom.write_text("# Chess Engine Rules\n\nPlay fair.")

        result = get_constitution_prompt(prompts_dir=tmp_path)
        assert "Chess Engine Rules" in result
        assert "Core Principles" not in result

    def test_bundled_constitution_is_universal_safety(self) -> None:
        """Bundled constitution contains safety guardrails, not app-specific content."""
        result = get_constitution_prompt()
        assert "Harm Avoidance" in result
        assert "Intellectual Honesty" in result
        assert "You are **Entropi**" not in result


# ── Gap 6: Config-tier validation ────────────────────────────────


class MockTierConfig:
    """Mock tier config for orchestrator tests."""

    def __init__(self, path: str = "/models/test.gguf") -> None:
        self.path = Path(path)
        self.context_length = 4096
        self.max_output_tokens = 4096
        self.gpu_layers = -1
        self.adapter = "qwen2"
        self.temperature = 0.7
        self.top_p = 0.9
        self.top_k = 40
        self.repeat_penalty = 1.1
        self.allowed_tools = None
        self.focus: list[str] = []


class MockEntropyConfig:
    """Mock config for orchestrator tests."""

    def __init__(self, tier_names: list[str] | None = None) -> None:
        tier_names = tier_names or ["normal"]
        self.models = MagicMock()
        self.models.default = tier_names[0]
        self.models.tiers = {name: MockTierConfig() for name in tier_names}
        self.models.router = None

        self.routing = MagicMock()
        self.routing.enabled = False
        self.routing.fallback_tier = tier_names[0]
        self.routing.tier_map = {}
        self.routing.handoff_rules = {}

        self.thinking = MagicMock()
        self.thinking.enabled = False

        self.prompts_dir = None
        self.use_bundled_prompts = True


class MockModelBackend:
    """Mock model backend."""

    def __init__(self, config: MockTierConfig, tier: str) -> None:
        self.config = config
        self.tier = tier
        self.is_loaded = False

    async def load(self) -> None:
        self.is_loaded = True

    async def unload(self) -> None:
        self.is_loaded = False


class TestConfigTierValidation:
    """Orchestrator validates that all tiers have config entries."""

    @pytest.mark.asyncio
    async def test_raises_when_tier_missing_config(self) -> None:
        """A ModelTier with no config entry raises ValueError."""
        from entropic.inference.orchestrator import ModelOrchestrator

        config = MockEntropyConfig(["normal"])

        # Provide a tier that has no config entry
        extra_tier = ModelTier("extra", focus=["extra stuff"])
        normal_tier = ModelTier("normal", focus=["general"])

        def mock_factory(model_config: MockTierConfig, tier_name: str) -> MockModelBackend:
            return MockModelBackend(model_config, tier_name)

        orchestrator = ModelOrchestrator(
            config,
            tiers=[normal_tier, extra_tier],
            backend_factory=mock_factory,
        )

        with pytest.raises(ValueError, match="'extra' has no config entry"):
            await orchestrator.initialize()

    @pytest.mark.asyncio
    async def test_succeeds_when_all_tiers_have_config(self) -> None:
        """All tiers with config entries initializes successfully."""
        from entropic.inference.orchestrator import ModelOrchestrator

        config = MockEntropyConfig(["normal", "code"])
        normal_tier = ModelTier("normal", focus=["general"])
        code_tier = ModelTier("code", focus=["coding"])

        def mock_factory(model_config: MockTierConfig, tier_name: str) -> MockModelBackend:
            return MockModelBackend(model_config, tier_name)

        orchestrator = ModelOrchestrator(
            config,
            tiers=[normal_tier, code_tier],
            backend_factory=mock_factory,
        )

        # Should not raise
        await orchestrator.initialize()
        assert len(orchestrator._tiers) == 2
