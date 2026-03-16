"""Tests for auto-chain tier delegation on token exhaustion and grammar completion."""

import warnings
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

    # Wire get_tier_param to resolve from config (same as real orchestrator)
    def _get_tier_param(tier: ModelTier, attr: str, dflt: object = None) -> object:
        tc = config.models.tiers.get(tier.name)
        val = getattr(tc, attr, None) if tc else None
        return val if val is not None else dflt

    orchestrator.get_tier_param.side_effect = _get_tier_param

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


def _setup_handoff_mocks(engine: AgentEngine) -> ModelTier:
    """Wire orchestrator mocks for a successful thinker→executor handoff."""
    executor_tier = ModelTier("executor", focus=["executing"])
    engine.orchestrator.get_handoff_targets.return_value = [executor_tier]
    engine.orchestrator.route_among = AsyncMock(return_value=executor_tier)
    engine.orchestrator.can_handoff.return_value = True
    engine.orchestrator._find_tier.return_value = executor_tier
    engine.orchestrator.get_adapter.return_value = MagicMock()
    engine.orchestrator.get_allowed_tools.return_value = None
    return executor_tier


# ===========================================================================
# TierConfig parsing
# ===========================================================================


class TestTierConfigAutoChain:
    """Tests for auto_chain on TierConfig."""

    def test_auto_chain_default_none(self) -> None:
        """auto_chain defaults to None (defer to identity frontmatter)."""
        tc = _tier_config()
        assert tc.auto_chain is None

    def test_auto_chain_true(self) -> None:
        """auto_chain=True parses correctly."""
        tc = _tier_config(auto_chain=True)
        assert tc.auto_chain is True


# ===========================================================================
# Auto-chain logic
# ===========================================================================


class TestAutoChain:
    """Tests for _try_auto_chain in AgentEngine."""

    @pytest.mark.asyncio
    async def test_fires_on_length(self) -> None:
        """finish_reason=length + auto_chain=True → handoff fires."""
        engine = _make_engine(handoff_rules={"thinker": ["executor"]})
        ctx = _make_ctx("thinker")
        executor_tier = _setup_handoff_mocks(engine)

        result = await engine._try_auto_chain(ctx, finish_reason="length")

        assert result is True
        assert ctx.locked_tier == executor_tier

    @pytest.mark.asyncio
    async def test_fires_on_grammar_stop(self) -> None:
        """finish_reason=stop + grammar + auto_chain=True → handoff fires."""
        engine = _make_engine(
            tiers={
                "thinker": _tier_config(auto_chain=True, grammar=Path("/test.gbnf")),
                "executor": _tier_config(),
            },
            handoff_rules={"thinker": ["executor"]},
        )
        ctx = _make_ctx("thinker")
        executor_tier = _setup_handoff_mocks(engine)

        result = await engine._try_auto_chain(ctx, finish_reason="stop")

        assert result is True
        assert ctx.locked_tier == executor_tier

    @pytest.mark.asyncio
    async def test_fires_on_stop_with_complete_response(self) -> None:
        """finish_reason=stop + auto_chain + complete response → chains."""
        engine = _make_engine(handoff_rules={"thinker": ["executor"]})
        ctx = _make_ctx("thinker")
        _setup_handoff_mocks(engine)

        # Mock adapter to say response is complete
        engine.orchestrator.get_adapter.return_value.is_response_complete.return_value = True

        result = await engine._try_auto_chain(ctx, finish_reason="stop", content="Done.")

        assert result is True

    @pytest.mark.asyncio
    async def test_skips_stop_with_incomplete_response(self) -> None:
        """finish_reason=stop + auto_chain + incomplete response → does NOT chain."""
        engine = _make_engine(handoff_rules={"thinker": ["executor"]})
        ctx = _make_ctx("thinker")

        # Mock adapter to say response is NOT complete
        engine.orchestrator.get_adapter.return_value.is_response_complete.return_value = False

        result = await engine._try_auto_chain(ctx, finish_reason="stop", content="Let me think...")

        assert result is False

    @pytest.mark.asyncio
    async def test_skipped_when_disabled(self) -> None:
        """auto_chain=False → doesn't fire regardless of finish_reason."""
        engine = _make_engine(
            tiers={
                "thinker": _tier_config(auto_chain=False, grammar=Path("/test.gbnf")),
                "executor": _tier_config(),
            },
            handoff_rules={"thinker": ["executor"]},
        )
        ctx = _make_ctx("thinker")

        result = await engine._try_auto_chain(ctx, finish_reason="stop")

        assert result is False

    @pytest.mark.asyncio
    async def test_skipped_no_targets(self) -> None:
        """auto_chain=True + grammar + no handoff targets → warns, returns False."""
        engine = _make_engine(
            tiers={
                "thinker": _tier_config(auto_chain=True, grammar=Path("/test.gbnf")),
                "executor": _tier_config(),
            },
            handoff_rules={},
        )
        ctx = _make_ctx("thinker")
        engine.orchestrator.get_handoff_targets.return_value = []

        result = await engine._try_auto_chain(ctx, finish_reason="stop")

        assert result is False

    @pytest.mark.asyncio
    async def test_skipped_unknown_tier(self) -> None:
        """auto_chain check with unknown locked tier → doesn't fire."""
        engine = _make_engine()
        ctx = _make_ctx("unknown")

        result = await engine._try_auto_chain(ctx, finish_reason="length")

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
            result = await engine._try_auto_chain(ctx, finish_reason="length")

        assert result is True
        mock_handler.assert_called_once()
        call_args = mock_handler.call_args
        assert isinstance(call_args[0][1], TierChange)
        assert call_args[0][1].tier == "executor"
        assert call_args[0][1].reason == "auto_chain"


# ===========================================================================
# Tool call sorting (delegate-last)
# ===========================================================================


class TestToolCallSorting:
    """Tests for _sort_tool_calls — entropic.delegate always last."""

    @staticmethod
    def _tc(name: str) -> MagicMock:
        tc = MagicMock()
        tc.name = name
        return tc

    def test_delegate_moved_to_end(self) -> None:
        """Delegate appearing first is moved to end."""
        from entropic.core.tool_executor import ToolExecutor

        calls = [self._tc("entropic.delegate"), self._tc("entropic.todo_write")]
        result = ToolExecutor._sort_tool_calls(calls)
        assert [c.name for c in result] == ["entropic.todo_write", "entropic.delegate"]

    def test_delegate_already_last_unchanged(self) -> None:
        """Delegate already at end — order preserved."""
        from entropic.core.tool_executor import ToolExecutor

        calls = [self._tc("entropic.todo_write"), self._tc("entropic.delegate")]
        result = ToolExecutor._sort_tool_calls(calls)
        assert [c.name for c in result] == ["entropic.todo_write", "entropic.delegate"]

    def test_non_delegate_order_preserved(self) -> None:
        """Relative order of non-delegate calls is stable."""
        from entropic.core.tool_executor import ToolExecutor

        calls = [
            self._tc("entropic.todo_write"),
            self._tc("chess.make_move"),
            self._tc("entropic.delegate"),
            self._tc("entropic.prune_context"),
        ]
        result = ToolExecutor._sort_tool_calls(calls)
        names = [c.name for c in result]
        assert names == [
            "entropic.todo_write",
            "chess.make_move",
            "entropic.prune_context",
            "entropic.delegate",
        ]

    def test_no_delegate_unchanged(self) -> None:
        """No delegate in list — order unchanged."""
        from entropic.core.tool_executor import ToolExecutor

        calls = [self._tc("entropic.todo_write"), self._tc("chess.make_move")]
        result = ToolExecutor._sort_tool_calls(calls)
        assert [c.name for c in result] == ["entropic.todo_write", "chess.make_move"]

    def test_empty_list(self) -> None:
        """Empty list returns empty."""
        from entropic.core.tool_executor import ToolExecutor

        assert ToolExecutor._sort_tool_calls([]) == []


# ===========================================================================
# Auto-chain after blocked tool calls
# ===========================================================================


class TestAutoChainBlockedTools:
    """Tests for auto-chain fallback when all tool calls are blocked."""

    @pytest.mark.asyncio
    async def test_auto_chain_mechanism_works(self) -> None:
        """_try_auto_chain fires when auto_chain configured + stop reason.

        Note: in the integrated flow, _evaluate_no_tool_decision guards
        against this when tools_were_attempted=True. This tests the mechanism.
        """
        engine = _make_engine(
            tiers={
                "thinker": _tier_config(auto_chain=True, grammar=Path("/test.gbnf")),
                "executor": _tier_config(),
            },
            handoff_rules={"thinker": ["executor"]},
        )
        ctx = _make_ctx("thinker")
        _setup_handoff_mocks(engine)

        # Simulate: model produced tool calls, all were blocked
        ctx.effective_tool_calls = 0
        engine.orchestrator.last_finish_reason = "stop"

        result = await engine._try_auto_chain(ctx, finish_reason="stop")

        assert result is True
        assert ctx.locked_tier.name == "executor"

    def test_no_fallback_when_tools_succeeded(self) -> None:
        """Some tool calls succeeded → effective_tool_calls > 0 → no fallback."""
        ctx = _make_ctx("thinker")
        ctx.effective_tool_calls = 2

        # effective_tool_calls > 0 means the call site never reaches _try_auto_chain
        # This test documents the semantic: nonzero effective calls = normal loop
        assert ctx.effective_tool_calls > 0

    @pytest.mark.asyncio
    async def test_blocked_tools_without_auto_chain_no_fire(self) -> None:
        """All tools blocked but auto_chain=False → no chain."""
        engine = _make_engine(
            tiers={
                "thinker": _tier_config(auto_chain=False, grammar=Path("/test.gbnf")),
                "executor": _tier_config(),
            },
            handoff_rules={"thinker": ["executor"]},
        )
        ctx = _make_ctx("thinker")
        ctx.effective_tool_calls = 0
        engine.orchestrator.last_finish_reason = "stop"

        result = await engine._try_auto_chain(ctx, finish_reason="stop")

        assert result is False


# ===========================================================================
# _evaluate_no_tool_decision: blocked tools vs no tools
# ===========================================================================


class TestEvaluateNoToolDecision:
    """Tests for loop decision when no effective tool calls occurred."""

    @pytest.mark.asyncio
    async def test_no_tools_attempted_completes(self) -> None:
        """No tool calls at all + complete content → COMPLETE."""
        from entropic.core.engine_types import AgentState

        engine = _make_engine(
            tiers={"lead": _tier_config(auto_chain=False)},
            default="lead",
        )
        ctx = _make_ctx("lead")
        engine.orchestrator.last_finish_reason = "stop"

        adapter = MagicMock()
        adapter.is_response_complete.return_value = True
        engine.orchestrator.get_adapter.return_value = adapter

        await engine._evaluate_no_tool_decision(ctx, "Here is my response.")

        assert ctx.state == AgentState.COMPLETE

    @pytest.mark.asyncio
    async def test_tools_attempted_but_blocked_continues(self) -> None:
        """Tools attempted but all blocked → do NOT complete, let model retry."""
        from entropic.core.engine_types import AgentState

        engine = _make_engine(
            tiers={"lead": _tier_config(auto_chain=False)},
            default="lead",
        )
        ctx = _make_ctx("lead")
        ctx.state = AgentState.EXECUTING
        engine.orchestrator.last_finish_reason = "stop"

        await engine._evaluate_no_tool_decision(
            ctx, "Let me update the todo list.", tools_were_attempted=True
        )

        # State should NOT be COMPLETE — model gets another turn
        assert ctx.state != AgentState.COMPLETE

    @pytest.mark.asyncio
    async def test_explicit_completion_prevents_heuristic(self) -> None:
        """explicit_completion=True + prior tool use → skip heuristic, wait for entropic.complete."""
        from entropic.core.engine_types import AgentState

        engine = _make_engine(
            tiers={"lead": _tier_config(auto_chain=False)},
            default="lead",
        )
        ctx = _make_ctx("lead")
        ctx.state = AgentState.EXECUTING
        ctx.metrics.tool_calls = 3  # Simulate prior tool usage (multi-step work)
        engine.orchestrator.last_finish_reason = "stop"

        # Make get_tier_param return True for explicit_completion
        original_side_effect = engine.orchestrator.get_tier_param.side_effect

        def _with_explicit(tier: ModelTier, attr: str, dflt: object = None) -> object:
            if attr == "explicit_completion":
                return True
            return original_side_effect(tier, attr, dflt)

        engine.orchestrator.get_tier_param.side_effect = _with_explicit

        # Adapter would say "complete" — but explicit_completion overrides
        adapter = MagicMock()
        adapter.is_response_complete.return_value = True
        engine.orchestrator.get_adapter.return_value = adapter

        await engine._evaluate_no_tool_decision(ctx, "Here is my response.")

        # Should NOT complete — explicit_completion requires entropic.complete
        assert ctx.state != AgentState.COMPLETE
        # Adapter should never be consulted
        adapter.is_response_complete.assert_not_called()

    @pytest.mark.asyncio
    async def test_explicit_completion_allows_simple_qa(self) -> None:
        """explicit_completion=True but no prior tool use → heuristic applies (simple Q&A)."""
        from entropic.core.engine_types import AgentState

        engine = _make_engine(
            tiers={"lead": _tier_config(auto_chain=False)},
            default="lead",
        )
        ctx = _make_ctx("lead")
        ctx.metrics.tool_calls = 0  # No tools used — simple response
        engine.orchestrator.last_finish_reason = "stop"

        original_side_effect = engine.orchestrator.get_tier_param.side_effect

        def _with_explicit(tier: ModelTier, attr: str, dflt: object = None) -> object:
            if attr == "explicit_completion":
                return True
            return original_side_effect(tier, attr, dflt)

        engine.orchestrator.get_tier_param.side_effect = _with_explicit

        adapter = MagicMock()
        adapter.is_response_complete.return_value = True
        engine.orchestrator.get_adapter.return_value = adapter

        await engine._evaluate_no_tool_decision(ctx, "Hello! How can I help?")

        # Should complete normally — no tools used, so explicit_completion doesn't apply
        assert ctx.state == AgentState.COMPLETE

    @pytest.mark.asyncio
    async def test_tools_attempted_blocks_auto_chain(self) -> None:
        """Tools blocked + auto_chain → guard prevents auto_chain, model retries."""
        from entropic.core.engine_types import AgentState

        engine = _make_engine(
            tiers={
                "eng": _tier_config(auto_chain=True),
                "lead": _tier_config(),
            },
            handoff_rules={"eng": ["lead"]},
            default="eng",
        )
        ctx = _make_ctx("eng")
        ctx.state = AgentState.EXECUTING
        engine.orchestrator.last_finish_reason = "stop"

        # tools_were_attempted=True blocks both auto_chain and completion
        await engine._evaluate_no_tool_decision(
            ctx, "Let me check the files.", tools_were_attempted=True
        )

        # Neither auto_chain nor COMPLETE — model gets another turn
        assert ctx.locked_tier.name == "eng"
        assert ctx.state != AgentState.COMPLETE


# ===========================================================================
# Config validation: auto_chain without targets
# ===========================================================================


class TestAutoChainConfigValidation:
    """Tests for auto_chain + handoff_rules config validation."""

    def test_warns_auto_chain_without_handoff_rules(self) -> None:
        """auto_chain=True + no handoff_rules entry → warning at config load."""
        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            EntropyConfig(
                models={
                    "tiers": {
                        "thinker": _tier_config(auto_chain=True),
                        "executor": _tier_config(),
                    },
                    "default": "thinker",
                },
                routing={
                    "enabled": False,
                    "fallback_tier": "thinker",
                    "handoff_rules": {},
                },
            )
        auto_chain_warnings = [x for x in w if "auto_chain" in str(x.message)]
        assert len(auto_chain_warnings) == 1
        assert "thinker" in str(auto_chain_warnings[0].message)

    def test_no_warning_when_handoff_rules_present(self) -> None:
        """auto_chain=True + matching handoff_rules entry → no warning."""
        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            EntropyConfig(
                models={
                    "tiers": {
                        "thinker": _tier_config(auto_chain=True),
                        "executor": _tier_config(),
                    },
                    "default": "thinker",
                },
                routing={
                    "enabled": False,
                    "fallback_tier": "thinker",
                    "handoff_rules": {"thinker": ["executor"]},
                },
            )
        auto_chain_warnings = [x for x in w if "auto_chain" in str(x.message)]
        assert len(auto_chain_warnings) == 0


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
