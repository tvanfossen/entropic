"""Git worktree management for delegation isolation.

Creates isolated git worktrees for child delegation loops so that
filesystem operations in child contexts don't affect the parent's
working tree. Merges changes back on success, discards on failure.
"""

from __future__ import annotations

import asyncio
import shutil
from contextlib import asynccontextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

from entropic.core.logging import get_logger

if TYPE_CHECKING:
    from collections.abc import AsyncIterator

    from entropic.core.engine import AgentEngine

logger = get_logger("core.worktree")

# Directory under repo root where worktrees are created
WORKTREE_DIR = ".worktrees"


async def _run_git(cwd: Path, args: list[str]) -> tuple[bool, str]:
    """Run a git command, return (success, output)."""
    process = await asyncio.create_subprocess_exec(
        "git",
        *args,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        cwd=cwd,
    )
    stdout, stderr = await process.communicate()
    output = stdout.decode("utf-8", errors="replace").strip()
    error = stderr.decode("utf-8", errors="replace").strip()
    success = process.returncode == 0
    if not success:
        logger.warning("[WORKTREE] git %s failed: %s", " ".join(args), error)
    return success, output or error


@dataclass
class WorktreeInfo:
    """Tracks a created worktree's path and branch for later merge/discard."""

    path: Path
    branch: str
    delegation_id: str


class WorktreeManager:
    """Create, merge, and discard git worktrees for delegation isolation."""

    def __init__(self, repo_dir: Path) -> None:
        self._repo_dir = repo_dir
        self._worktree_base = repo_dir / WORKTREE_DIR

    async def create_worktree(self, delegation_id: str, tier: str) -> WorktreeInfo | None:
        """Create a git worktree for a delegation.

        Returns WorktreeInfo with path and branch, or None on failure.
        """
        short_id = delegation_id[:8]
        branch_name = f"delegation/{tier}-{short_id}"
        worktree_path = self._worktree_base / f"delegation-{short_id}"

        self._worktree_base.mkdir(parents=True, exist_ok=True)

        success, output = await _run_git(
            self._repo_dir,
            ["worktree", "add", str(worktree_path), "-b", branch_name],
        )

        if not success:
            logger.error("[WORKTREE] Failed to create worktree: %s", output)
            return None

        logger.info(
            "[WORKTREE] Created worktree: %s (branch: %s)",
            worktree_path,
            branch_name,
        )
        return WorktreeInfo(path=worktree_path, branch=branch_name, delegation_id=delegation_id)

    async def merge_worktree(self, info: WorktreeInfo) -> bool:
        """Merge worktree branch back to current branch.

        Returns True if merge succeeded (or nothing to merge).
        """
        await self._auto_commit_if_dirty(info)

        success, output = await _run_git(
            self._repo_dir,
            ["merge", info.branch, "--no-edit"],
        )

        if not success:
            logger.error("[WORKTREE] Merge failed for branch %s: %s", info.branch, output)
            await self._cleanup(info)
            return False

        logger.info("[WORKTREE] Merged branch %s successfully", info.branch)
        await self._cleanup(info)
        return True

    async def discard_worktree(self, info: WorktreeInfo) -> None:
        """Remove worktree and delete branch without merging."""
        await self._cleanup(info)
        logger.info(
            "[WORKTREE] Discarded worktree %s (branch: %s)",
            info.path.name,
            info.branch,
        )

    async def _auto_commit_if_dirty(self, info: WorktreeInfo) -> None:
        """Auto-commit uncommitted changes in worktree before merge."""
        if not info.path.exists():
            logger.warning("[WORKTREE] Worktree path missing: %s", info.path)
            return

        success, diff = await _run_git(info.path, ["diff", "HEAD", "--stat"])
        if not success or not diff.strip():
            return

        logger.info("[WORKTREE] Auto-committing dirty worktree: %s", info.path.name)
        await _run_git(info.path, ["add", "-A"])
        await _run_git(
            info.path,
            ["commit", "-m", f"delegation/{info.delegation_id[:8]}: child work"],
        )

    async def _cleanup(self, info: WorktreeInfo) -> None:
        """Remove worktree directory and delete branch."""
        if info.path.exists():
            success, _ = await _run_git(
                self._repo_dir,
                ["worktree", "remove", str(info.path), "--force"],
            )
            if not success:
                logger.warning(
                    "[WORKTREE] git worktree remove failed, falling back to rmtree: %s",
                    info.path,
                )
                shutil.rmtree(info.path, ignore_errors=True)

        # Delete the branch
        success, output = await _run_git(self._repo_dir, ["branch", "-D", info.branch])
        if success:
            logger.info("[WORKTREE] Deleted branch %s", info.branch)
        else:
            logger.warning("[WORKTREE] Failed to delete branch %s: %s", info.branch, output)


def _get_server_instance(engine: AgentEngine, name: str) -> Any:
    """Get underlying server instance from engine's server manager."""
    sm = engine.server_manager
    if sm is None:
        return None
    client = sm._clients.get(name)
    if client is None:
        return None
    return getattr(client, "_server", None)


def _swap_git_repo_dir(server: Any, new_dir: Path) -> None:
    """Update repo_dir on GitServer and all its registered tools."""
    server.repo_dir = new_dir
    for tool in server._tool_registry._tools.values():
        if hasattr(tool, "_repo_dir"):
            tool._repo_dir = new_dir


@asynccontextmanager
async def scoped_worktree(
    engine: AgentEngine,
    worktree_path: Path,
) -> AsyncIterator[None]:
    """Swap server directories to worktree, restore on exit.

    Swaps filesystem root_dir, bash working_dir, and git repo_dir
    to point at the worktree. Restores all on exit (success or failure).
    """
    saved: dict[str, Any] = {}

    fs_server = _get_server_instance(engine, "filesystem")
    if fs_server is not None:
        saved["fs_root"] = fs_server.root_dir
        fs_server.root_dir = worktree_path

    bash_server = _get_server_instance(engine, "bash")
    if bash_server is not None:
        saved["bash_dir"] = bash_server.working_dir
        bash_server.working_dir = worktree_path

    git_server = _get_server_instance(engine, "git")
    if git_server is not None:
        saved["git_dir"] = git_server.repo_dir
        _swap_git_repo_dir(git_server, worktree_path)

    try:
        yield
    finally:
        if fs_server is not None and "fs_root" in saved:
            fs_server.root_dir = saved["fs_root"]

        if bash_server is not None and "bash_dir" in saved:
            bash_server.working_dir = saved["bash_dir"]

        if git_server is not None and "git_dir" in saved:
            _swap_git_repo_dir(git_server, saved["git_dir"])
