"""Tests for the delegation system (child inference loops)."""

from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from entropic.config.schema import LibraryConfig, TierConfig
from entropic.core.base import Message, ModelTier
from entropic.core.delegation import DelegationManager, DelegationResult
from entropic.core.engine import MAX_DELEGATION_DEPTH, AgentEngine, LoopConfig, LoopContext
from entropic.core.engine_types import AgentState

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _tier_config(path: str = "/test.gguf", **overrides: object) -> TierConfig:
    return TierConfig(path=Path(path), **overrides)


def _make_engine(
    tiers: dict[str, TierConfig] | None = None,
    default: str = "lead",
    auto_chain_map: dict[str, str] | None = None,
) -> AgentEngine:
    """Create an AgentEngine with mocked internals for delegation testing.

    auto_chain_map: Maps tier name → auto_chain target name (string).
    TierConfig.auto_chain is bool; the actual tier name comes from identity
    frontmatter via orchestrator.get_tier_param(). This map simulates that.
    """
    if tiers is None:
        tiers = {
            "lead": _tier_config(),
            "eng": _tier_config(auto_chain=True),
        }
    chain_map = auto_chain_map or {}
    config = LibraryConfig(
        models={"tiers": tiers, "default": default},
        routing={"enabled": False, "fallback_tier": default},
    )
    orchestrator = MagicMock()
    orchestrator.tier_names = list(tiers.keys())

    def _get_tier_param(tier: ModelTier, attr: str, dflt: object = None) -> object:
        # auto_chain: return string target from chain_map if available
        if attr == "auto_chain" and tier.name in chain_map:
            return chain_map[tier.name]
        tc = config.models.tiers.get(tier.name)
        val = getattr(tc, attr, None) if tc else None
        return val if val is not None else dflt

    orchestrator.get_tier_param.side_effect = _get_tier_param

    server_manager = MagicMock()
    with patch.object(AgentEngine, "_get_max_context_tokens", return_value=16384):
        engine = AgentEngine(orchestrator, server_manager, config, LoopConfig())
    return engine


def _make_parent_ctx(tier_name: str = "lead") -> LoopContext:
    ctx = LoopContext()
    ctx.locked_tier = ModelTier(tier_name, focus=["test"])
    ctx.all_tools = []
    ctx.base_system = "test system"
    ctx.messages = [Message(role="system", content="test")]
    return ctx


# ---------------------------------------------------------------------------
# DelegationResult
# ---------------------------------------------------------------------------


class TestDelegationResult:
    def test_basic_fields(self):
        result = DelegationResult(
            summary="Done",
            success=True,
            target_tier="eng",
            task="write code",
            turns_used=3,
        )
        assert result.success is True
        assert result.turns_used == 3
        assert result.child_messages == []

    def test_failed_result(self):
        result = DelegationResult(
            summary="Error: boom",
            success=False,
            target_tier="eng",
            task="write code",
            turns_used=1,
            child_messages=[Message(role="assistant", content="partial")],
        )
        assert result.success is False
        assert len(result.child_messages) == 1


# ---------------------------------------------------------------------------
# DelegationManager
# ---------------------------------------------------------------------------


class TestDelegationManager:
    def test_unknown_tier_returns_error(self):
        engine = _make_engine()
        engine.orchestrator._find_tier.return_value = None
        manager = DelegationManager(engine)

        parent_ctx = _make_parent_ctx()

        async def run():
            return await manager.execute_delegation(parent_ctx, "nonexistent", "do stuff")

        import asyncio

        result = asyncio.get_event_loop().run_until_complete(run())
        assert result.success is False
        assert "unknown tier" in result.summary

    def test_build_child_context_sets_depth(self):
        engine = _make_engine()
        engine._response_generator = MagicMock()
        engine._response_generator._build_formatted_system_prompt.return_value = "system"
        manager = DelegationManager(engine)

        parent_ctx = _make_parent_ctx()
        parent_ctx.delegation_depth = 0

        eng_tier = ModelTier("eng", focus=["code"])
        child_ctx = manager._build_child_context(parent_ctx, eng_tier, "write tests")

        assert child_ctx.delegation_depth == 1
        assert child_ctx.locked_tier == eng_tier
        assert len(child_ctx.messages) == 2
        assert child_ctx.messages[0].role == "system"
        assert child_ctx.messages[1].role == "user"
        assert child_ctx.messages[1].content == "write tests"

    def test_build_child_context_inherits_tools_and_base_system(self):
        engine = _make_engine()
        engine._response_generator = MagicMock()
        engine._response_generator._build_formatted_system_prompt.return_value = "sys"
        manager = DelegationManager(engine)

        parent_ctx = _make_parent_ctx()
        parent_ctx.all_tools = [{"name": "bash"}, {"name": "read"}]
        parent_ctx.base_system = "base system prompt"

        eng_tier = ModelTier("eng", focus=["code"])
        child_ctx = manager._build_child_context(parent_ctx, eng_tier, "task")

        assert child_ctx.all_tools == parent_ctx.all_tools
        assert child_ctx.base_system == parent_ctx.base_system

    def test_extract_final_summary_finds_last_assistant(self):
        manager = DelegationManager(MagicMock())
        ctx = LoopContext()
        ctx.messages = [
            Message(role="system", content="sys"),
            Message(role="user", content="task"),
            Message(role="assistant", content="thinking..."),
            Message(role="user", content="tool result"),
            Message(role="assistant", content="Final answer here."),
        ]
        assert manager._extract_final_summary(ctx) == "Final answer here."

    def test_extract_final_summary_no_assistant(self):
        manager = DelegationManager(MagicMock())
        ctx = LoopContext()
        ctx.messages = [
            Message(role="system", content="sys"),
            Message(role="user", content="task"),
        ]
        assert manager._extract_final_summary(ctx) == "(No response from delegate)"

    def test_max_turns_saves_and_restores_loop_config(self):
        """max_turns override must restore original loop_config after child completes."""
        engine = _make_engine()
        engine._response_generator = MagicMock()
        engine._response_generator._build_formatted_system_prompt.return_value = "sys"
        engine.orchestrator._find_tier.return_value = ModelTier("eng", focus=["code"])
        engine._context_anchors = {}

        original_config = engine.loop_config
        original_max = original_config.max_iterations

        manager = DelegationManager(engine)

        # Mock _loop to immediately complete
        async def fake_loop(ctx):
            # Verify config was overridden during child execution
            assert engine.loop_config.max_iterations == 5
            ctx.state = AgentState.COMPLETE
            return
            yield  # noqa: RET504 — make it an async generator

        engine._loop = fake_loop

        parent_ctx = _make_parent_ctx()

        import asyncio

        result = asyncio.get_event_loop().run_until_complete(
            manager.execute_delegation(parent_ctx, "eng", "task", max_turns=5)
        )

        # Config restored after delegation
        assert engine.loop_config is original_config
        assert engine.loop_config.max_iterations == original_max
        assert result.success is True

    def test_context_anchors_saved_and_restored(self):
        """Child loop must not pollute parent's context anchors."""
        engine = _make_engine()
        engine._response_generator = MagicMock()
        engine._response_generator._build_formatted_system_prompt.return_value = "sys"
        engine.orchestrator._find_tier.return_value = ModelTier("eng", focus=["code"])
        engine._context_anchors = {"parent_key": "parent_value"}

        manager = DelegationManager(engine)

        async def fake_loop(ctx):
            # Child mutates anchors
            engine._context_anchors["child_key"] = "child_value"
            engine._context_anchors.pop("parent_key", None)
            ctx.state = AgentState.COMPLETE
            return
            yield  # noqa: RET504

        engine._loop = fake_loop

        parent_ctx = _make_parent_ctx()

        import asyncio

        asyncio.get_event_loop().run_until_complete(
            manager.execute_delegation(parent_ctx, "eng", "task")
        )

        # Parent anchors restored
        assert engine._context_anchors == {"parent_key": "parent_value"}

    def test_child_loop_error_returns_failed_result(self):
        """Exceptions in child loop produce a failed DelegationResult."""
        engine = _make_engine()
        engine._response_generator = MagicMock()
        engine._response_generator._build_formatted_system_prompt.return_value = "sys"
        engine.orchestrator._find_tier.return_value = ModelTier("eng", focus=["code"])
        engine._context_anchors = {}

        manager = DelegationManager(engine)

        async def exploding_loop(ctx):
            raise RuntimeError("child boom")
            yield  # noqa: RET504

        engine._loop = exploding_loop

        parent_ctx = _make_parent_ctx()

        import asyncio

        result = asyncio.get_event_loop().run_until_complete(
            manager.execute_delegation(parent_ctx, "eng", "task")
        )

        assert result.success is False
        assert "child boom" in result.summary


# ---------------------------------------------------------------------------
# Engine: auto_chain depth behavior
# ---------------------------------------------------------------------------


class TestAutoChainDepth:
    @pytest.mark.asyncio
    async def test_auto_chain_at_depth_zero_fires_tier_change(self):
        """At depth 0, auto_chain should attempt TierChange (existing behavior)."""
        engine = _make_engine(
            tiers={
                "eng": _tier_config(auto_chain=True),
                "lead": _tier_config(),
            },
            auto_chain_map={"eng": "lead"},
        )
        ctx = _make_parent_ctx("eng")
        ctx.delegation_depth = 0

        lead_tier = ModelTier("lead", focus=["leading"])
        engine.orchestrator._find_tier.return_value = lead_tier
        engine.orchestrator.can_handoff.return_value = True
        engine.orchestrator.get_adapter.return_value = MagicMock()
        engine.orchestrator.get_allowed_tools.return_value = None

        result = await engine._try_auto_chain(ctx, "stop", "Complete response.")
        assert result is True
        # Should NOT have set COMPLETE — should have done tier change
        assert ctx.state != AgentState.COMPLETE

    @pytest.mark.asyncio
    async def test_auto_chain_at_depth_one_fires_complete(self):
        """At depth > 0, auto_chain should set COMPLETE (return to parent)."""
        engine = _make_engine(
            tiers={
                "eng": _tier_config(auto_chain=True),
                "lead": _tier_config(),
            },
            auto_chain_map={"eng": "lead"},
        )
        ctx = _make_parent_ctx("eng")
        ctx.delegation_depth = 1

        adapter = MagicMock()
        adapter.is_response_complete.return_value = True
        engine.orchestrator.get_adapter.return_value = adapter

        result = await engine._try_auto_chain(ctx, "stop", "Complete response.")
        assert result is True
        assert ctx.state == AgentState.COMPLETE


# ---------------------------------------------------------------------------
# Engine: _execute_pending_delegation
# ---------------------------------------------------------------------------


class TestExecutePendingDelegation:
    @pytest.mark.asyncio
    async def test_delegation_result_injected_into_parent(self):
        """After delegation, result message is injected into parent context."""
        engine = _make_engine()
        ctx = _make_parent_ctx()
        ctx.metadata["pending_delegation"] = {
            "target": "eng",
            "task": "implement feature",
            "max_turns": 5,
        }

        mock_result = DelegationResult(
            summary="Feature implemented successfully.",
            success=True,
            target_tier="eng",
            task="implement feature",
            turns_used=3,
        )

        with patch("entropic.core.delegation.DelegationManager") as mock_manager_cls:
            instance = mock_manager_cls.return_value
            instance.execute_delegation = AsyncMock(return_value=mock_result)

            messages = []
            async for msg in engine._execute_pending_delegation(ctx):
                messages.append(msg)

        assert len(messages) == 1
        assert "DELEGATION COMPLETE: eng" in messages[0].content
        assert "Feature implemented" in messages[0].content
        # pending_delegation should be consumed
        assert "pending_delegation" not in ctx.metadata

    @pytest.mark.asyncio
    async def test_failed_delegation_shows_failure(self):
        """Failed delegation injects FAILED status."""
        engine = _make_engine()
        ctx = _make_parent_ctx()
        ctx.metadata["pending_delegation"] = {
            "target": "eng",
            "task": "break things",
        }

        mock_result = DelegationResult(
            summary="Delegation failed: timeout",
            success=False,
            target_tier="eng",
            task="break things",
            turns_used=10,
        )

        with patch("entropic.core.delegation.DelegationManager") as mock_manager_cls:
            instance = mock_manager_cls.return_value
            instance.execute_delegation = AsyncMock(return_value=mock_result)

            messages = []
            async for msg in engine._execute_pending_delegation(ctx):
                messages.append(msg)

        assert "DELEGATION FAILED: eng" in messages[0].content

    @pytest.mark.asyncio
    async def test_delegation_callbacks_fired(self):
        """on_delegation_start and on_delegation_complete callbacks fire."""
        engine = _make_engine()
        ctx = _make_parent_ctx()
        ctx.metadata["pending_delegation"] = {
            "target": "eng",
            "task": "do work",
        }

        from entropic.core.engine_types import EngineCallbacks

        start_calls = []
        complete_calls = []
        engine.set_callbacks(
            EngineCallbacks(
                on_delegation_start=lambda *args: start_calls.append(args),
                on_delegation_complete=lambda *args: complete_calls.append(args),
            )
        )

        mock_result = DelegationResult(
            summary="Done.",
            success=True,
            target_tier="eng",
            task="do work",
            turns_used=2,
        )

        with patch("entropic.core.delegation.DelegationManager") as mock_manager_cls:
            instance = mock_manager_cls.return_value
            instance.execute_delegation = AsyncMock(return_value=mock_result)

            async for _ in engine._execute_pending_delegation(ctx):
                pass

        assert len(start_calls) == 1
        assert start_calls[0][1] == "eng"  # target tier
        assert start_calls[0][2] == "do work"  # task
        assert len(complete_calls) == 1
        assert complete_calls[0][3] is True  # success


# ---------------------------------------------------------------------------
# Depth enforcement
# ---------------------------------------------------------------------------


class TestDelegationDepthEnforcement:
    @pytest.mark.asyncio
    async def test_delegation_rejected_at_max_depth(self):
        """Delegation at MAX_DELEGATION_DEPTH is rejected with error message."""
        engine = _make_engine()
        ctx = _make_parent_ctx()
        ctx.delegation_depth = MAX_DELEGATION_DEPTH
        ctx.metadata["pending_delegation"] = {
            "target": "eng",
            "task": "too deep",
        }

        messages = []
        async for msg in engine._execute_pending_delegation(ctx):
            messages.append(msg)

        assert len(messages) == 1
        assert "DELEGATION REJECTED" in messages[0].content
        assert messages[0].metadata.get("delegation_rejected") is True
        # DelegationManager should NOT have been called
        assert ctx.state != AgentState.DELEGATING

    @pytest.mark.asyncio
    async def test_delegation_allowed_below_max_depth(self):
        """Delegation below MAX_DELEGATION_DEPTH proceeds normally."""
        engine = _make_engine()
        ctx = _make_parent_ctx()
        ctx.delegation_depth = MAX_DELEGATION_DEPTH - 1
        ctx.metadata["pending_delegation"] = {
            "target": "eng",
            "task": "still ok",
        }

        mock_result = DelegationResult(
            summary="Done.",
            success=True,
            target_tier="eng",
            task="still ok",
            turns_used=1,
        )

        with patch("entropic.core.delegation.DelegationManager") as mock_cls:
            mock_cls.return_value.execute_delegation = AsyncMock(return_value=mock_result)
            messages = []
            async for msg in engine._execute_pending_delegation(ctx):
                messages.append(msg)

        assert len(messages) == 1
        assert "DELEGATION COMPLETE" in messages[0].content

    def test_max_depth_constant_is_reasonable(self):
        """MAX_DELEGATION_DEPTH should be small to prevent runaway chains."""
        assert 1 <= MAX_DELEGATION_DEPTH <= 5


# ---------------------------------------------------------------------------
# Repo root caching (_get_repo_dir)
# ---------------------------------------------------------------------------


class TestRepoRootCaching:
    def test_repo_dir_cached_on_first_call(self, tmp_path: Path):
        """_get_repo_dir caches the filesystem root on first call."""
        (tmp_path / ".git").mkdir()

        engine = _make_engine()
        fs_server = MagicMock()
        fs_server.root_dir = tmp_path

        with patch("entropic.core.worktree._get_server_instance", return_value=fs_server):
            result1 = engine._get_repo_dir()
            assert result1 == tmp_path

            # Simulate worktree swap changing root_dir
            fs_server.root_dir = Path("/worktree/path")
            result2 = engine._get_repo_dir()

            # Should still return original, not swapped path
            assert result2 == tmp_path

    def test_repo_dir_returns_none_when_no_server(self):
        """_get_repo_dir returns None when no filesystem server exists."""
        engine = _make_engine()
        with patch("entropic.core.worktree._get_server_instance", return_value=None):
            assert engine._get_repo_dir() is None

    def test_auto_inits_git_when_no_dotgit(self, tmp_path: Path):
        """_get_repo_dir auto-initializes git repo when .git is missing."""
        engine = _make_engine()
        fs_server = MagicMock()
        fs_server.root_dir = tmp_path

        def fake_git_init(*_args, **_kwargs):
            """Simulate git init creating .git directory."""
            (tmp_path / ".git").mkdir(exist_ok=True)
            return MagicMock(returncode=0)

        with patch("entropic.core.worktree._get_server_instance", return_value=fs_server):
            with patch(
                "entropic.core.engine.subprocess.run", side_effect=fake_git_init
            ) as mock_run:
                result = engine._get_repo_dir()

        assert result == tmp_path
        # git init + git add -A + git commit --allow-empty
        assert mock_run.call_count == 3
        assert mock_run.call_args_list[0][0][0] == ["git", "init"]

    def test_custom_repo_init_hook(self, tmp_path: Path):
        """Consumer-provided repo_init hook overrides default git init."""
        engine = _make_engine()
        fs_server = MagicMock()
        fs_server.root_dir = tmp_path

        custom_init = MagicMock(return_value=True)
        engine._callbacks.repo_init = custom_init

        with patch("entropic.core.worktree._get_server_instance", return_value=fs_server):
            result = engine._get_repo_dir()

        custom_init.assert_called_once_with(tmp_path)
        assert result == tmp_path

    def test_repo_init_hook_false_disables_worktrees(self, tmp_path: Path):
        """repo_init returning False disables worktree isolation."""
        engine = _make_engine()
        fs_server = MagicMock()
        fs_server.root_dir = tmp_path

        engine._callbacks.repo_init = MagicMock(return_value=False)

        with patch("entropic.core.worktree._get_server_instance", return_value=fs_server):
            result = engine._get_repo_dir()

        assert result is None
