"""Tests for pipeline delegation — multi-stage sequential execution."""

from __future__ import annotations

import json
from unittest.mock import MagicMock

import pytest
from entropic.core.delegation import DelegationManager, DelegationResult
from entropic.core.directives import Pipeline, StopProcessing
from entropic.core.engine_types import LoopContext
from entropic.mcp.servers.base import ServerResponse
from entropic.mcp.servers.entropic import EntropicServer, PipelineTool


class TestPipelineDirective:
    """Pipeline directive dataclass behavior."""

    def test_fields(self) -> None:
        p = Pipeline(stages=["eng", "qa"], task="build feature")
        assert p.stages == ["eng", "qa"]
        assert p.task == "build feature"

    def test_equality(self) -> None:
        a = Pipeline(stages=["a", "b"], task="t")
        b = Pipeline(stages=["a", "b"], task="t")
        assert a == b


class TestPipelineTool:
    """PipelineTool validates stages and emits directives."""

    @pytest.mark.asyncio
    async def test_emits_pipeline_and_stop(self) -> None:
        """Valid pipeline returns Pipeline + StopProcessing directives."""
        tool = PipelineTool(tier_names=["eng", "qa"])
        result = await tool.execute({"stages": ["eng", "qa"], "task": "do work"})
        assert isinstance(result, ServerResponse)
        assert len(result.directives) == 2
        assert isinstance(result.directives[0], Pipeline)
        assert result.directives[0].stages == ["eng", "qa"]
        assert result.directives[0].task == "do work"
        assert isinstance(result.directives[1], StopProcessing)

    @pytest.mark.asyncio
    async def test_rejects_single_stage(self) -> None:
        """Pipeline requires at least 2 stages."""
        tool = PipelineTool(tier_names=["eng", "qa"])
        result = await tool.execute({"stages": ["eng"], "task": "test"})
        assert isinstance(result, str)
        data = json.loads(result)
        assert "error" in data
        assert "at least 2" in data["error"]

    @pytest.mark.asyncio
    async def test_rejects_unknown_tier(self) -> None:
        """Pipeline rejects stages with unknown tier names."""
        tool = PipelineTool(tier_names=["eng", "qa"])
        result = await tool.execute({"stages": ["eng", "unknown"], "task": "test"})
        assert isinstance(result, str)
        data = json.loads(result)
        assert "error" in data
        assert "Unknown tier" in data["error"]

    @pytest.mark.asyncio
    async def test_accepts_valid_stages(self) -> None:
        """Pipeline accepts valid tier names."""
        tool = PipelineTool(tier_names=["eng", "qa", "arch"])
        result = await tool.execute({"stages": ["eng", "qa"], "task": "review code"})
        assert isinstance(result, ServerResponse)
        assert "eng → qa" in result.result

    def test_enum_patched(self) -> None:
        """Stage items enum reflects custom tier names."""
        tool = PipelineTool(tier_names=["suggest", "validate"])
        defn = tool.definition
        stages = defn.inputSchema["properties"]["stages"]
        assert stages["items"]["enum"] == ["suggest", "validate"]

    @pytest.mark.asyncio
    async def test_no_tier_names_skips_validation(self) -> None:
        """Pipeline without tier_names skips tier validation."""
        tool = PipelineTool(tier_names=None)
        result = await tool.execute({"stages": ["a", "b"], "task": "test"})
        assert isinstance(result, ServerResponse)

    @pytest.mark.asyncio
    async def test_stages_string_parsed_as_list(self) -> None:
        """Stringified JSON array is parsed into a real list."""
        tool = PipelineTool(tier_names=["eng", "qa"])
        result = await tool.execute({"stages": '["eng", "qa"]', "task": "test"})
        assert isinstance(result, ServerResponse)
        assert result.directives[0].stages == ["eng", "qa"]


class TestPipelineExecution:
    """DelegationManager.execute_pipeline runs stages sequentially."""

    def _make_engine(self) -> MagicMock:
        engine = MagicMock()
        engine._context_anchors = {}
        engine.loop_config = MagicMock()
        engine.server_manager = None
        return engine

    def _make_ctx(self) -> LoopContext:
        ctx = LoopContext()
        ctx.locked_tier = MagicMock()
        ctx.locked_tier.__str__ = MagicMock(return_value="lead")
        ctx.all_tools = []
        ctx.base_system = ""
        return ctx

    @pytest.mark.asyncio
    async def test_stages_execute_sequentially(self) -> None:
        """Each stage calls execute_delegation with correct inputs."""
        engine = self._make_engine()
        manager = DelegationManager(engine)

        call_args = []

        async def mock_execute(parent_ctx, target_tier, task, **kwargs):
            call_args.append((target_tier, task))
            return DelegationResult(
                summary=f"Output from {target_tier}",
                success=True,
                target_tier=target_tier,
                task=task,
                turns_used=2,
            )

        manager.execute_delegation = mock_execute

        ctx = self._make_ctx()
        await manager.execute_pipeline(ctx, ["eng", "qa"], "build feature")

        assert len(call_args) == 2
        assert call_args[0][0] == "eng"
        assert call_args[0][1] == "build feature"
        assert call_args[1][0] == "qa"
        assert "Previous stage (eng) output:" in call_args[1][1]
        assert "Output from eng" in call_args[1][1]

    @pytest.mark.asyncio
    async def test_failed_stage_short_circuits(self) -> None:
        """Pipeline stops on first failed stage."""
        engine = self._make_engine()
        manager = DelegationManager(engine)

        call_count = 0

        async def mock_execute(parent_ctx, target_tier, task, **kwargs):
            nonlocal call_count
            call_count += 1
            return DelegationResult(
                summary="Stage failed",
                success=False,
                target_tier=target_tier,
                task=task,
                turns_used=1,
            )

        manager.execute_delegation = mock_execute

        ctx = self._make_ctx()
        result = await manager.execute_pipeline(ctx, ["eng", "qa"], "task")

        assert call_count == 1
        assert result.success is False
        assert result.target_tier == "eng"

    @pytest.mark.asyncio
    async def test_returns_final_stage_result(self) -> None:
        """Pipeline returns the last successful stage's result."""
        engine = self._make_engine()
        manager = DelegationManager(engine)

        async def mock_execute(parent_ctx, target_tier, task, **kwargs):
            return DelegationResult(
                summary=f"Final from {target_tier}",
                success=True,
                target_tier=target_tier,
                task=task,
                turns_used=3,
            )

        manager.execute_delegation = mock_execute

        ctx = self._make_ctx()
        result = await manager.execute_pipeline(ctx, ["eng", "qa"], "task")

        assert result.success is True
        assert result.target_tier == "qa"
        assert result.summary == "Final from qa"


class TestEntropicServerPipelineRegistration:
    """Pipeline tool registration in EntropicServer."""

    def test_multi_tier_has_pipeline(self) -> None:
        """Multi-tier server registers pipeline alongside delegate."""
        server = EntropicServer(tier_names=["eng", "qa"])
        tool_names = [t.name for t in server.get_tools()]
        assert "pipeline" in tool_names
        assert "delegate" in tool_names

    def test_single_tier_no_pipeline(self) -> None:
        """Single-tier server omits pipeline (same as delegate)."""
        server = EntropicServer(tier_names=["normal"])
        tool_names = [t.name for t in server.get_tools()]
        assert "pipeline" not in tool_names
        assert "delegate" not in tool_names

    def test_default_server_has_pipeline(self) -> None:
        """Default server (no tier_names) registers pipeline."""
        server = EntropicServer()
        tool_names = [t.name for t in server.get_tools()]
        assert "pipeline" in tool_names

    @pytest.mark.asyncio
    async def test_pipeline_via_execute_tool(self) -> None:
        """Pipeline accessible via execute_tool dispatch."""
        server = EntropicServer(tier_names=["eng", "qa"])
        result = await server.execute_tool("pipeline", {"stages": ["eng", "qa"], "task": "test"})
        assert isinstance(result, ServerResponse)
        assert isinstance(result.directives[0], Pipeline)
