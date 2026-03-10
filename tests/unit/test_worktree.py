"""Tests for git worktree isolation in delegation."""

from __future__ import annotations

from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from entropic.core.worktree import (
    WorktreeManager,
    _get_server_instance,
    _swap_git_repo_dir,
    scoped_worktree,
)


class TestWorktreeManager:
    """WorktreeManager creates, merges, and discards git worktrees."""

    @pytest.fixture
    def manager(self, tmp_path: Path) -> WorktreeManager:
        return WorktreeManager(tmp_path)

    @pytest.mark.asyncio
    async def test_create_worktree_returns_path_on_success(
        self, manager: WorktreeManager, tmp_path: Path
    ) -> None:
        """Successful worktree creation returns a Path."""
        with patch("entropic.core.worktree._run_git", new_callable=AsyncMock) as mock_git:
            mock_git.return_value = (True, "")
            result = await manager.create_worktree("abc123", "eng")

        assert result is not None
        assert "delegation-abc12345" in str(result) or "delegation-abc123" in str(result)
        mock_git.assert_called_once()
        args = mock_git.call_args[0][1]
        assert "worktree" in args
        assert "add" in args

    @pytest.mark.asyncio
    async def test_create_worktree_returns_none_on_failure(self, manager: WorktreeManager) -> None:
        """Failed git worktree add returns None."""
        with patch("entropic.core.worktree._run_git", new_callable=AsyncMock) as mock_git:
            mock_git.return_value = (False, "fatal: already exists")
            result = await manager.create_worktree("abc123", "eng")

        assert result is None

    @pytest.mark.asyncio
    async def test_create_worktree_makes_parent_dir(
        self, manager: WorktreeManager, tmp_path: Path
    ) -> None:
        """Worktree creation ensures .worktrees/ directory exists."""
        with patch("entropic.core.worktree._run_git", new_callable=AsyncMock) as mock_git:
            mock_git.return_value = (True, "")
            await manager.create_worktree("abc123", "eng")

        assert (tmp_path / ".worktrees").exists()

    @pytest.mark.asyncio
    async def test_merge_worktree_calls_merge(self, manager: WorktreeManager) -> None:
        """Successful merge calls git merge then cleanup."""
        calls: list[list[str]] = []

        # Response lookup: key on (args[0], args[1] if present)
        responses = {
            ("diff", "HEAD"): (True, ""),
            ("branch", "--list"): (True, "  delegation/abc12345-eng"),
        }

        async def fake_git(cwd, args):
            calls.append(args)
            key = (args[0], args[1]) if len(args) > 1 else (args[0],)
            return responses.get(key, (True, ""))

        with patch("entropic.core.worktree._run_git", side_effect=fake_git):
            result = await manager.merge_worktree("abc12345")

        assert result is True

    @pytest.mark.asyncio
    async def test_merge_worktree_auto_commits_uncommitted(
        self, manager: WorktreeManager, tmp_path: Path
    ) -> None:
        """Uncommitted changes in worktree are auto-committed before merge."""
        calls: list[tuple[str, list[str]]] = []

        # Create the worktree directory so the diff check works
        wt_dir = tmp_path / ".worktrees" / "delegation-abc12345"
        wt_dir.mkdir(parents=True)

        responses = {
            ("diff", "HEAD"): (True, " file.py | 5 +++++"),
            ("branch", "--list"): (True, "  delegation/abc12345-eng"),
        }

        async def fake_git(cwd, args):
            calls.append((str(cwd), args))
            key = (args[0], args[1]) if len(args) > 1 else (args[0],)
            return responses.get(key, (True, ""))

        with patch("entropic.core.worktree._run_git", side_effect=fake_git):
            await manager.merge_worktree("abc12345")

        # Verify add + commit were called
        git_cmds = [c[1] for c in calls]
        assert ["add", "-A"] in git_cmds
        assert any(c[0] == "commit" for c in git_cmds)

    @pytest.mark.asyncio
    async def test_discard_worktree_removes_and_deletes_branch(
        self, manager: WorktreeManager, tmp_path: Path
    ) -> None:
        """Discard removes worktree and deletes branch."""
        # Create the worktree directory
        wt_dir = tmp_path / ".worktrees" / "delegation-abc12345"
        wt_dir.mkdir(parents=True)

        calls = []

        async def fake_git(cwd, args):
            calls.append(args)
            if args[:2] == ["branch", "--list"]:
                return (True, "  delegation/abc12345-eng")
            return (True, "")

        with patch("entropic.core.worktree._run_git", side_effect=fake_git):
            await manager.discard_worktree("abc12345")

        git_cmds = calls
        assert any("worktree" in c and "remove" in c for c in git_cmds)
        assert any("branch" in c and "-D" in c for c in git_cmds)


class TestSwapGitRepoDir:
    """_swap_git_repo_dir updates server and all tools."""

    def test_updates_server_and_tools(self) -> None:
        """All tools with _repo_dir get updated."""
        tool_a = MagicMock()
        tool_a._repo_dir = Path("/old")
        tool_b = MagicMock()
        tool_b._repo_dir = Path("/old")

        server = MagicMock()
        server._tool_registry._tools = {"a": tool_a, "b": tool_b}

        new_dir = Path("/new/worktree")
        _swap_git_repo_dir(server, new_dir)

        assert server.repo_dir == new_dir
        assert tool_a._repo_dir == new_dir
        assert tool_b._repo_dir == new_dir


class TestGetServerInstance:
    """_get_server_instance extracts server from engine."""

    def test_returns_server(self) -> None:
        engine = MagicMock()
        server = MagicMock()
        client = MagicMock()
        client._server = server
        engine.server_manager._clients = {"filesystem": client}

        result = _get_server_instance(engine, "filesystem")
        assert result is server

    def test_returns_none_no_manager(self) -> None:
        engine = MagicMock()
        engine.server_manager = None
        assert _get_server_instance(engine, "filesystem") is None

    def test_returns_none_no_client(self) -> None:
        engine = MagicMock()
        engine.server_manager._clients = {}
        assert _get_server_instance(engine, "filesystem") is None


class TestScopedWorktree:
    """scoped_worktree context manager swaps and restores server dirs."""

    @pytest.mark.asyncio
    async def test_swaps_and_restores_filesystem(self) -> None:
        """Filesystem root_dir is swapped and restored."""
        engine = MagicMock()
        fs_server = MagicMock()
        fs_server.root_dir = Path("/original")

        fs_client = MagicMock()
        fs_client._server = fs_server
        engine.server_manager._clients = {"filesystem": fs_client}

        worktree = Path("/worktree")
        async with scoped_worktree(engine, worktree):
            assert fs_server.root_dir == worktree

        assert fs_server.root_dir == Path("/original")

    @pytest.mark.asyncio
    async def test_swaps_and_restores_bash(self) -> None:
        """Bash working_dir is swapped and restored."""
        engine = MagicMock()
        bash_server = MagicMock()
        bash_server.working_dir = Path("/original")

        bash_client = MagicMock()
        bash_client._server = bash_server
        engine.server_manager._clients = {"bash": bash_client}

        worktree = Path("/worktree")
        async with scoped_worktree(engine, worktree):
            assert bash_server.working_dir == worktree

        assert bash_server.working_dir == Path("/original")

    @pytest.mark.asyncio
    async def test_restores_on_exception(self) -> None:
        """Server dirs restored even when exception occurs."""
        engine = MagicMock()
        fs_server = MagicMock()
        fs_server.root_dir = Path("/original")

        fs_client = MagicMock()
        fs_client._server = fs_server
        engine.server_manager._clients = {"filesystem": fs_client}

        worktree = Path("/worktree")
        with pytest.raises(RuntimeError):
            async with scoped_worktree(engine, worktree):
                raise RuntimeError("child failed")

        assert fs_server.root_dir == Path("/original")

    @pytest.mark.asyncio
    async def test_no_servers_is_noop(self) -> None:
        """No servers available — runs without error."""
        engine = MagicMock()
        engine.server_manager._clients = {}

        async with scoped_worktree(engine, Path("/worktree")):
            pass  # Should not raise
