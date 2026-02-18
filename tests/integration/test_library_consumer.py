"""Integration test validating entropi's public library API.

This test imports ONLY from the `entropi` top-level package — never from
internal modules. It proves the library surface works for consumers who
define custom tiers, construct config programmatically, and inject custom
backends via BackendFactory.
"""

import asyncio
from pathlib import Path
from unittest.mock import MagicMock

import pytest

# === Public API imports only ===
from entropi import (
    BackendFactory,
    EntropyConfig,
    GenerationResult,
    Message,
    ModelBackend,
    ModelOrchestrator,
    ModelTier,
    RoutingResult,
    TierConfig,
    ToolCall,
    ToolResult,
)

# === Custom tier subclass (consumer domain) ===


class EducationTier(ModelTier):
    """Consumer-defined tier with domain-specific metadata."""

    def __init__(self, name: str, *, focus: list[str], grade_level: str):
        super().__init__(name, focus=focus)
        self.grade_level = grade_level


# === Mock backend (stands in for real inference) ===


class MockBackend:
    """Minimal mock satisfying the backend contract via BackendFactory."""

    def __init__(self, config, tier_name: str):
        self.config = config
        self.tier_name = tier_name
        self.is_loaded = False
        self._last_finish_reason = "stop"

    async def load(self):
        self.is_loaded = True

    async def unload(self):
        self.is_loaded = False

    async def generate(self, messages, **kwargs):
        return GenerationResult(content=f"Mock response from {self.tier_name}")

    def count_tokens(self, text):
        return len(text) // 4

    @property
    def context_length(self):
        return self.config.context_length

    @property
    def last_finish_reason(self):
        return self._last_finish_reason

    @property
    def adapter(self):
        mock = MagicMock()
        mock.parse_tool_calls.side_effect = lambda content: (content, [])
        return mock


def mock_backend_factory(config, tier_name: str):
    """Consumer's custom backend factory."""
    return MockBackend(config, tier_name)


# === Tests ===


class TestPublicAPIImports:
    """Verify all expected types are importable from entropi."""

    def test_core_types_available(self) -> None:
        """Core types are importable from top-level package."""
        assert ModelTier is not None
        assert Message is not None
        assert ToolCall is not None
        assert ToolResult is not None
        assert GenerationResult is not None
        assert ModelBackend is not None

    def test_config_types_available(self) -> None:
        """Config types are importable from top-level package."""
        assert EntropyConfig is not None
        assert TierConfig is not None

    def test_orchestrator_types_available(self) -> None:
        """Orchestrator types are importable from top-level package."""
        assert ModelOrchestrator is not None
        assert RoutingResult is not None
        assert BackendFactory is not None


class TestCustomTiers:
    """Verify consumers can define custom ModelTier subclasses."""

    def test_subclass_with_domain_metadata(self) -> None:
        """Custom ModelTier subclass preserves focus and adds metadata."""
        tier = EducationTier(
            "algebra",
            focus=["linear equations", "polynomials"],
            grade_level="high school",
        )
        assert tier.name == "algebra"
        assert "linear equations" in tier.focus
        assert tier.grade_level == "high school"

    def test_equality_with_string(self) -> None:
        """ModelTier.__eq__ supports string comparison for dict lookups."""
        tier = ModelTier("custom", focus=["testing"])
        assert tier == "custom"
        assert tier != "other"

    def test_hashable_as_dict_key(self) -> None:
        """ModelTier instances work as dict keys."""
        t1 = ModelTier("a", focus=["x"])
        t2 = ModelTier("b", focus=["y"])
        d = {t1: "first", t2: "second"}
        assert d[t1] == "first"

    def test_focus_required(self) -> None:
        """ModelTier raises if focus is empty."""
        with pytest.raises(ValueError, match="requires at least one focus point"):
            ModelTier("empty", focus=[])


class TestProgrammaticConfig:
    """Verify EntropyConfig works without ConfigLoader or files on disk."""

    def test_minimal_config(self) -> None:
        """Construct config with just custom tiers and routing disabled."""
        config = EntropyConfig(
            models={
                "tiers": {
                    "math": {"path": "/tmp/math.gguf"},
                    "science": {"path": "/tmp/science.gguf"},
                },
                "default": "math",
            },
            routing={"enabled": False, "fallback_tier": "math"},
            use_bundled_prompts=False,
        )
        assert "math" in config.models.tiers
        assert "science" in config.models.tiers
        assert config.models.default == "math"
        assert config.use_bundled_prompts is False

    def test_config_with_router(self) -> None:
        """Config with router model for classification."""
        config = EntropyConfig(
            models={
                "tiers": {"fast": {"path": "/tmp/fast.gguf"}},
                "router": {"path": "/tmp/router.gguf"},
                "default": "fast",
            },
            routing={"fallback_tier": "fast"},
        )
        assert config.models.router is not None
        assert config.models.router.path == Path("/tmp/router.gguf")


class TestOrchestratorWithCustomFactory:
    """Verify orchestrator works with consumer-injected backend factory."""

    def _make_config(self) -> EntropyConfig:
        return EntropyConfig(
            models={
                "tiers": {
                    "math": {"path": "/tmp/math.gguf", "focus": ["arithmetic", "algebra"]},
                    "reading": {"path": "/tmp/reading.gguf", "focus": ["comprehension"]},
                },
                "default": "math",
            },
            routing={"enabled": False, "fallback_tier": "math"},
            use_bundled_prompts=False,
        )

    def test_custom_factory_creates_backends(self) -> None:
        """Backend factory is called for each tier during initialize."""
        config = self._make_config()
        created_tiers: list[str] = []

        def tracking_factory(model_config, tier_name):
            created_tiers.append(tier_name)
            return MockBackend(model_config, tier_name)

        orch = ModelOrchestrator(config, backend_factory=tracking_factory)
        asyncio.get_event_loop().run_until_complete(orch.initialize())

        assert "math" in created_tiers
        assert "reading" in created_tiers

    def test_custom_tiers_param(self) -> None:
        """Consumer-provided ModelTier list overrides config-derived tiers."""
        config = self._make_config()
        custom = [
            EducationTier("math", focus=["algebra"], grade_level="9th"),
            EducationTier("reading", focus=["comprehension"], grade_level="7th"),
        ]

        orch = ModelOrchestrator(config, tiers=custom, backend_factory=mock_backend_factory)

        # Tier list should be the consumer-provided one
        assert len(orch._tier_list) == 2
        assert orch._tier_list[0].name == "math"
        assert isinstance(orch._tier_list[0], EducationTier)

    def test_auto_generated_tier_map(self) -> None:
        """Without explicit tier_map, tiers are auto-numbered 1..N."""
        config = self._make_config()
        orch = ModelOrchestrator(config, backend_factory=mock_backend_factory)

        # Should have auto-numbered map: "1" -> math, "2" -> reading
        assert "1" in orch._tier_map
        assert "2" in orch._tier_map

    @pytest.mark.asyncio
    async def test_generate_with_mock_backend(self) -> None:
        """Full generate path works with custom backend factory."""
        config = self._make_config()
        orch = ModelOrchestrator(config, backend_factory=mock_backend_factory)
        await orch.initialize()

        math_tier = orch._find_tier("math")
        assert math_tier is not None

        result = await orch.generate(
            [Message(role="user", content="What is 2+2?")],
            tier=math_tier,
        )
        assert "Mock response from math" in result.content

        await orch.shutdown()
