"""Tests for auto-chain tier handoff on token exhaustion."""

from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from entropic.config.schema import EntropyConfig, TierConfig
from entropic.core.base import GenerationResult, ModelTier
from entropic.core.directives import TierChange
from entropic.core.engine import AgentEngine, LoopConfig, LoopContext
from entropic.inference.adapters.base import GenericAdapter
from entropic.inference.adapters.qwen3 import Qwen3Adapter

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _tier_config(path: str = "/test.gguf", **overrides: object) -> TierConfig:
    """Create a minimal TierConfig for testing."""
    return TierConfig(path=Path(path), **overrides)


def _make_engine(
    tiers: dict[str, TierConfig] | None = None,
    handoff_rules: dict[str, list[str]] | None = None,
    default: str = "thinker",
) -> AgentEngine:
    """Create an AgentEngine with mocked internals for auto-chain testing."""
    if tiers is None:
        tiers = {
            "thinker": _tier_config(auto_chain=True),
            "executor": _tier_config(),
        }
    config = EntropyConfig(
        models={"tiers": tiers, "default": default},
        routing={
            "enabled": False,
            "fallback_tier": default,
            "handoff_rules": handoff_rules or {},
        },
    )
    orchestrator = MagicMock()
    orchestrator.tier_names = list(tiers.keys())
    server_manager = MagicMock()
    with patch.object(AgentEngine, "_get_max_context_tokens", return_value=16384):
        engine = AgentEngine(orchestrator, server_manager, config, LoopConfig())
    return engine


def _make_ctx(locked_tier_name: str = "thinker") -> LoopContext:
    """Create a LoopContext with a locked tier."""
    ctx = LoopContext()
    ctx.locked_tier = ModelTier(locked_tier_name, focus=["test"])
    ctx.all_tools = []
    ctx.base_system = "test system"
    ctx.messages = [MagicMock()]  # system message slot
    return ctx


# ===========================================================================
# TierConfig parsing
# ===========================================================================


class TestTierConfigAutoChain:
    """Tests for auto_chain and enable_thinking on TierConfig."""

    def test_auto_chain_default_false(self) -> None:
        """auto_chain defaults to False."""
        tc = _tier_config()
        assert tc.auto_chain is False

    def test_auto_chain_true(self) -> None:
        """auto_chain=True parses correctly."""
        tc = _tier_config(auto_chain=True)
        assert tc.auto_chain is True

    def test_enable_thinking_default_true(self) -> None:
        """enable_thinking defaults to True."""
        tc = _tier_config()
        assert tc.enable_thinking is True

    def test_enable_thinking_false(self) -> None:
        """enable_thinking=False parses correctly."""
        tc = _tier_config(enable_thinking=False)
        assert tc.enable_thinking is False


# ===========================================================================
# Auto-chain logic
# ===========================================================================


class TestAutoChain:
    """Tests for _try_auto_chain in AgentEngine."""

    @pytest.mark.asyncio
    async def test_fires_on_length_no_tools(self) -> None:
        """finish_reason=length + no tools + auto_chain=True → handoff fires."""
        engine = _make_engine(
            handoff_rules={"thinker": ["executor"]},
        )
        ctx = _make_ctx("thinker")

        # Mock orchestrator methods
        executor_tier = ModelTier("executor", focus=["executing"])
        engine.orchestrator.get_handoff_targets.return_value = [executor_tier]
        engine.orchestrator.route_among = AsyncMock(return_value=executor_tier)
        engine.orchestrator.can_handoff.return_value = True
        engine.orchestrator._find_tier.return_value = executor_tier
        engine.orchestrator.get_adapter.return_value = MagicMock()
        engine.orchestrator.get_allowed_tools.return_value = None

        result = await engine._try_auto_chain(ctx)

        assert result is True
        assert ctx.locked_tier == executor_tier

    @pytest.mark.asyncio
    async def test_skipped_when_disabled(self) -> None:
        """auto_chain=False → doesn't fire."""
        engine = _make_engine(
            tiers={
                "thinker": _tier_config(auto_chain=False),
                "executor": _tier_config(),
            },
            handoff_rules={"thinker": ["executor"]},
        )
        ctx = _make_ctx("thinker")

        result = await engine._try_auto_chain(ctx)

        assert result is False

    @pytest.mark.asyncio
    async def test_skipped_no_targets(self) -> None:
        """auto_chain=True but no handoff_rules targets → doesn't fire."""
        engine = _make_engine(handoff_rules={})
        ctx = _make_ctx("thinker")

        # No targets configured
        engine.orchestrator.get_handoff_targets.return_value = []

        result = await engine._try_auto_chain(ctx)

        assert result is False

    @pytest.mark.asyncio
    async def test_skipped_unknown_tier(self) -> None:
        """auto_chain check with unknown locked tier → doesn't fire."""
        engine = _make_engine()
        ctx = _make_ctx("unknown")

        result = await engine._try_auto_chain(ctx)

        assert result is False

    @pytest.mark.asyncio
    async def test_reuses_directive_tier_change(self) -> None:
        """Verify _directive_tier_change is called (DRY — reuses existing handoff)."""
        engine = _make_engine(handoff_rules={"thinker": ["executor"]})
        ctx = _make_ctx("thinker")

        executor_tier = ModelTier("executor", focus=["executing"])
        engine.orchestrator.get_handoff_targets.return_value = [executor_tier]
        engine.orchestrator.route_among = AsyncMock(return_value=executor_tier)

        with patch.object(engine, "_directive_tier_change") as mock_handler:
            mock_handler.side_effect = lambda c, d, r: setattr(r, "tier_changed", True)
            result = await engine._try_auto_chain(ctx)

        assert result is True
        mock_handler.assert_called_once()
        call_args = mock_handler.call_args
        assert isinstance(call_args[0][1], TierChange)
        assert call_args[0][1].tier == "executor"
        assert call_args[0][1].reason == "auto_chain"


# ===========================================================================
# Qwen3 /no-think
# ===========================================================================


class TestEnableThinking:
    """Tests for enable_thinking propagation to adapters."""

    def test_qwen3_injects_no_think(self) -> None:
        """Qwen3 adapter appends /no-think when enable_thinking=False."""
        adapter = Qwen3Adapter(tier="executor")
        result = adapter.format_system_prompt("test prompt", enable_thinking=False)
        assert result.endswith("/no-think")

    def test_qwen3_default_no_suffix(self) -> None:
        """Qwen3 adapter does not append /no-think by default."""
        adapter = Qwen3Adapter(tier="thinker")
        result = adapter.format_system_prompt("test prompt")
        assert "/no-think" not in result

    def test_qwen3_explicit_true_no_suffix(self) -> None:
        """Qwen3 adapter with enable_thinking=True — no /no-think."""
        adapter = Qwen3Adapter(tier="thinker")
        result = adapter.format_system_prompt("test prompt", enable_thinking=True)
        assert "/no-think" not in result

    def test_base_adapter_ignores_enable_thinking(self) -> None:
        """Base adapter (GenericAdapter) ignores enable_thinking kwarg."""
        adapter = GenericAdapter(tier="test")
        result_on = adapter.format_system_prompt("test prompt", enable_thinking=True)
        result_off = adapter.format_system_prompt("test prompt", enable_thinking=False)
        # GenericAdapter doesn't add /no-think — results should be identical
        assert result_on == result_off


# ===========================================================================
# route_among
# ===========================================================================


class TestRouteAmong:
    """Tests for orchestrator.route_among."""

    @pytest.mark.asyncio
    async def test_single_target_returns_directly(self) -> None:
        """Single candidate returns directly without routing."""
        from entropic.inference.orchestrator import ModelOrchestrator

        target = ModelTier("executor", focus=["executing"])
        # Use a real method via mock to test the logic
        orchestrator = MagicMock(spec=ModelOrchestrator)
        orchestrator.route_among = ModelOrchestrator.route_among.__get__(orchestrator)

        result = await orchestrator.route_among([target])
        assert result == target

    @pytest.mark.asyncio
    async def test_multiple_targets_returns_first(self) -> None:
        """Multiple candidates returns first (router integration deferred)."""
        from entropic.inference.orchestrator import ModelOrchestrator

        t1 = ModelTier("executor", focus=["executing"])
        t2 = ModelTier("thinker", focus=["thinking"])
        orchestrator = MagicMock(spec=ModelOrchestrator)
        orchestrator.route_among = ModelOrchestrator.route_among.__get__(orchestrator)

        result = await orchestrator.route_among([t1, t2])
        assert result == t1


# ===========================================================================
# GenerationResult raw_content
# ===========================================================================


class TestGenerationResultRawContent:
    """Tests for raw_content field on GenerationResult."""

    def test_raw_content_default_empty(self) -> None:
        """raw_content defaults to empty string."""
        result = GenerationResult(content="test")
        assert result.raw_content == ""

    def test_raw_content_preserved(self) -> None:
        """raw_content can be set explicitly."""
        result = GenerationResult(content="cleaned", raw_content="<think>analysis</think>cleaned")
        assert result.raw_content == "<think>analysis</think>cleaned"
        assert result.content == "cleaned"
