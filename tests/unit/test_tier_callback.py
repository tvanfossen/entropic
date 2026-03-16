"""Tests for tier selection callback firing on pre-locked contexts."""

import threading
from unittest.mock import MagicMock

import pytest
from entropic.core.engine_types import EngineCallbacks, GenerationEvents, LoopConfig, LoopContext
from entropic.core.response_generator import ResponseGenerator


def _make_rg(callbacks: EngineCallbacks) -> ResponseGenerator:
    """Build a ResponseGenerator with mocked dependencies."""
    orchestrator = MagicMock()
    config = MagicMock()
    loop_config = LoopConfig()
    events = GenerationEvents(interrupt=threading.Event(), pause=threading.Event())
    return ResponseGenerator(orchestrator, config, loop_config, callbacks, events)


class TestPreLockedTierCallback:
    """on_tier_selected fires even when tier is already locked (delegation child)."""

    @pytest.mark.asyncio
    async def test_callback_fires_when_tier_pre_locked(self) -> None:
        """Pre-locked tier (delegation child) still fires on_tier_selected."""
        cb = EngineCallbacks(on_tier_selected=MagicMock())
        rg = _make_rg(cb)

        tier = MagicMock()
        tier.name = "eng"
        ctx = LoopContext()
        ctx.locked_tier = tier

        await rg._lock_tier_if_needed(ctx)

        cb.on_tier_selected.assert_called_once_with("eng")

    @pytest.mark.asyncio
    async def test_no_routing_when_pre_locked(self) -> None:
        """Pre-locked tier skips routing — no orchestrator.route() call."""
        rg = _make_rg(EngineCallbacks())

        tier = MagicMock()
        tier.name = "lead"
        ctx = LoopContext()
        ctx.locked_tier = tier

        await rg._lock_tier_if_needed(ctx)

        rg._orchestrator.route.assert_not_called()

    @pytest.mark.asyncio
    async def test_no_callback_when_none(self) -> None:
        """No error when on_tier_selected is None with pre-locked tier."""
        rg = _make_rg(EngineCallbacks())

        tier = MagicMock()
        tier.name = "eng"
        ctx = LoopContext()
        ctx.locked_tier = tier

        await rg._lock_tier_if_needed(ctx)  # Should not raise
