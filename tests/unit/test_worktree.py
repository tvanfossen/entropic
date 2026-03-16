"""Tests for git worktree isolation in delegation."""

from __future__ import annotations

from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from entropic.core.worktree import (
    WorktreeInfo,
    WorktreeManager,
    _get_server_instance,
    _swap_git_repo_dir,
    scoped_worktree,
)


def _make_info(tmp_path: Path, tier: str = "eng") -> WorktreeInfo:
    """Create a WorktreeInfo for testing."""
    return WorktreeInfo(
        path=tmp_path / ".worktrees" / "delegation-abc12345",
        branch=f"delegation/{tier}-abc12345",
        delegation_id="abc12345-full-uuid",
    )


class TestWorktreeManager:
    """WorktreeManager creates, merges, and discards git worktrees."""

    @pytest.fixture
    def manager(self, tmp_path: Path) -> WorktreeManager:
        return WorktreeManager(tmp_path)

    @pytest.mark.asyncio
    async def test_create_worktree_returns_info_on_success(self, manager: WorktreeManager) -> None:
        """Successful worktree creation returns WorktreeInfo."""
        with patch("entropic.core.worktree._run_git", new_callable=AsyncMock) as mock_git:
            mock_git.return_value = (True, "")
            result = await manager.create_worktree("abc12345", "eng")

        assert result is not None
        assert isinstance(result, WorktreeInfo)
        assert result.branch == "delegation/eng-abc12345"
        assert "delegation-abc12345" in str(result.path)
        # Last call should be worktree add with develop as start-point
        last_call_args = mock_git.call_args_list[-1][0][1]
        assert last_call_args[0] == "worktree"
        assert "develop" in last_call_args

    @pytest.mark.asyncio
    async def test_ensure_develop_idempotent_when_already_on_develop(
        self, manager: WorktreeManager
    ) -> None:
        """If already on develop, ensure_develop is a no-op after first check."""
        with patch("entropic.core.worktree._run_git", new_callable=AsyncMock) as mock_git:
            mock_git.return_value = (True, "develop")
            assert await manager.ensure_develop() is True
            # Second call should not hit git at all (cached)
            mock_git.reset_mock()
            assert await manager.ensure_develop() is True
            mock_git.assert_not_called()

    @pytest.mark.asyncio
    async def test_ensure_develop_creates_branch_when_missing(
        self, manager: WorktreeManager
    ) -> None:
        """Creates develop branch from HEAD when it doesn't exist."""
        call_log: list[list[str]] = []

        async def fake_git(cwd, args):
            call_log.append(args)
            if args == ["rev-parse", "--abbrev-ref", "HEAD"]:
                return (True, "main")
            if args[:2] == ["rev-parse", "--verify"]:
                return (False, "")  # develop doesn't exist
            return (True, "")

        with patch("entropic.core.worktree._run_git", side_effect=fake_git):
            assert await manager.ensure_develop() is True

        cmds = [c[0] for c in call_log]
        assert "branch" in cmds  # created develop
        assert "checkout" in cmds  # checked out develop

    @pytest.mark.asyncio
    async def test_accept_to_main_merges_develop(self, manager: WorktreeManager) -> None:
        """accept_to_main merges develop into main and returns to develop."""
        call_log: list[list[str]] = []

        async def fake_git(cwd, args):
            call_log.append(args)
            if args[:2] == ["rev-parse", "--verify"] and args[2] == "main":
                return (True, "")
            return (True, "")

        with patch("entropic.core.worktree._run_git", side_effect=fake_git):
            assert await manager.accept_to_main() is True

        cmds = call_log
        # Should checkout main, merge develop, checkout develop
        assert ["checkout", "main"] in cmds
        assert ["merge", "develop", "--no-edit"] in cmds
        assert ["checkout", "develop"] in cmds

    @pytest.mark.asyncio
    async def test_discard_develop_deletes_branch(self, manager: WorktreeManager) -> None:
        """discard_develop switches to main and deletes develop."""
        call_log: list[list[str]] = []

        async def fake_git(cwd, args):
            call_log.append(args)
            if args[:2] == ["rev-parse", "--verify"] and args[2] == "main":
                return (True, "")
            return (True, "")

        with patch("entropic.core.worktree._run_git", side_effect=fake_git):
            assert await manager.discard_develop() is True

        assert ["checkout", "main"] in call_log
        assert ["branch", "-D", "develop"] in call_log

    @pytest.mark.asyncio
    async def test_create_worktree_branch_includes_tier(self, manager: WorktreeManager) -> None:
        """Branch name includes tier for unique identification."""
        with patch("entropic.core.worktree._run_git", new_callable=AsyncMock) as mock_git:
            mock_git.return_value = (True, "")
            result = await manager.create_worktree("abc12345", "qa")

        assert result is not None
        assert result.branch == "delegation/qa-abc12345"

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
    async def test_merge_uses_exact_branch_name(
        self, manager: WorktreeManager, tmp_path: Path
    ) -> None:
        """Merge uses the branch name from WorktreeInfo, not a reconstructed one."""
        info = _make_info(tmp_path)
        merge_args = []

        async def fake_git(cwd, args):
            if args[0] == "merge":
                merge_args.extend(args)
            return (True, "")

        with patch("entropic.core.worktree._run_git", side_effect=fake_git):
            result = await manager.merge_worktree(info)

        assert result is True
        assert "delegation/eng-abc12345" in merge_args

    @pytest.mark.asyncio
    async def test_merge_auto_commits_dirty_worktree(
        self, manager: WorktreeManager, tmp_path: Path
    ) -> None:
        """Modified tracked files trigger auto-commit before merge."""
        info = _make_info(tmp_path)
        info.path.mkdir(parents=True)

        calls: list[tuple[str, list[str]]] = []

        responses = {("status", "--porcelain"): (True, " M file.py")}

        async def fake_git(cwd, args):
            calls.append((str(cwd), args))
            key = (args[0], args[1]) if len(args) > 1 else (args[0],)
            return responses.get(key, (True, ""))

        with patch("entropic.core.worktree._run_git", side_effect=fake_git):
            await manager.merge_worktree(info)

        git_cmds = [c[1] for c in calls]
        assert ["add", "-A"] in git_cmds
        assert any(c[0] == "commit" for c in git_cmds)

    @pytest.mark.asyncio
    async def test_merge_auto_commits_untracked_files(
        self, manager: WorktreeManager, tmp_path: Path
    ) -> None:
        """Untracked (new) files trigger auto-commit before merge.

        This was the root cause of #89: eng created new files in a worktree,
        but ``git diff HEAD`` didn't see untracked files. The merge was a
        no-op and the files were lost.
        """
        info = _make_info(tmp_path)
        info.path.mkdir(parents=True)

        calls: list[tuple[str, list[str]]] = []

        # git status --porcelain shows untracked files with "??" prefix
        responses = {
            ("status", "--porcelain"): (
                True,
                "?? index.html\n?? styles.css\n?? game.js",
            ),
        }

        async def fake_git(cwd, args):
            calls.append((str(cwd), args))
            key = (args[0], args[1]) if len(args) > 1 else (args[0],)
            return responses.get(key, (True, ""))

        with patch("entropic.core.worktree._run_git", side_effect=fake_git):
            await manager.merge_worktree(info)

        git_cmds = [c[1] for c in calls]
        assert ["add", "-A"] in git_cmds
        assert any(c[0] == "commit" for c in git_cmds)

    @pytest.mark.asyncio
    async def test_merge_returns_false_on_conflict(
        self, manager: WorktreeManager, tmp_path: Path
    ) -> None:
        """Failed merge returns False and logs error."""
        info = _make_info(tmp_path)

        async def fake_git(cwd, args):
            if args[0] == "merge":
                return (False, "CONFLICT (content): Merge conflict")
            return (True, "")

        with patch("entropic.core.worktree._run_git", side_effect=fake_git):
            result = await manager.merge_worktree(info)

        assert result is False

    @pytest.mark.asyncio
    async def test_discard_removes_worktree_and_branch(
        self, manager: WorktreeManager, tmp_path: Path
    ) -> None:
        """Discard removes worktree dir and deletes the exact branch."""
        info = _make_info(tmp_path)
        info.path.mkdir(parents=True)

        calls: list[list[str]] = []

        async def fake_git(cwd, args):
            calls.append(args)
            return (True, "")

        with patch("entropic.core.worktree._run_git", side_effect=fake_git):
            await manager.discard_worktree(info)

        assert any("worktree" in c and "remove" in c for c in calls)
        # Branch delete uses exact name from info, not a glob search
        branch_delete = [c for c in calls if "-D" in c]
        assert len(branch_delete) == 1
        assert "delegation/eng-abc12345" in branch_delete[0]

    @pytest.mark.asyncio
    async def test_cleanup_logs_branch_delete_failure(
        self, manager: WorktreeManager, tmp_path: Path
    ) -> None:
        """Failed branch delete is logged as warning, not silent."""
        info = _make_info(tmp_path)

        responses = {
            ("branch", "-D"): (False, "error: branch not found"),
        }

        async def fake_git(cwd, args):
            key = (args[0], args[1]) if len(args) > 1 else (args[0],)
            return responses.get(key, (True, ""))

        with patch("entropic.core.worktree._run_git", side_effect=fake_git):
            # Should not raise — logs warning instead
            await manager.discard_worktree(info)


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
