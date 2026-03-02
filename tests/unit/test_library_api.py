"""Tests for library API gaps (P1-011 Phase 4).

Covers: register_server(), config-tier validation.
"""

from pathlib import Path
from unittest.mock import MagicMock

import pytest
from entropic.config.schema import PermissionsConfig
from entropic.core.base import ModelTier
from entropic.mcp.manager import ServerManager
from entropic.mcp.provider import InProcessProvider

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
