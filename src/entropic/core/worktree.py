"""Git worktree management for delegation isolation.

Branch model:
  main     — pristine, only updated on explicit acceptance
  develop  — working branch, created from main at first delegation
  delegation/* — child branches off develop, merge back to develop

Creates isolated git worktrees for child delegation loops so that
filesystem operations in child contexts don't affect the parent's
working tree. Merges changes back to develop on success, discards on
failure. Develop merges to main only on explicit acceptance.
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

# Branch names
DEVELOP_BRANCH = "develop"


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


async def _current_branch(cwd: Path) -> str:
    """Get the current branch name."""
    _, output = await _run_git(cwd, ["rev-parse", "--abbrev-ref", "HEAD"])
    return output


@dataclass
class WorktreeInfo:
    """Tracks a created worktree's path and branch for later merge/discard."""

    path: Path
    branch: str
    delegation_id: str


class WorktreeManager:
    """Create, merge, and discard git worktrees for delegation isolation.

    Ensures a ``develop`` branch exists before creating delegation
    worktrees. The main working tree is checked out to ``develop``
    so that lead operates there. Delegation branches are created from
    and merged back to ``develop``. Main is only updated via
    ``accept_to_main()``.
    """

    def __init__(self, repo_dir: Path) -> None:
        self._repo_dir = repo_dir
        self._worktree_base = repo_dir / WORKTREE_DIR
        self._develop_ready = False

    async def ensure_develop(self) -> bool:
        """Create develop branch from current HEAD and check it out.

        Idempotent — if develop exists and is checked out, this is a no-op.
        Returns True if the working tree is on develop.
        """
        if self._develop_ready:
            return True

        current = await _current_branch(self._repo_dir)
        if current == DEVELOP_BRANCH:
            self._develop_ready = True
            return True

        ready = await self._create_and_checkout_develop(current)
        self._develop_ready = ready
        return ready

    async def _create_and_checkout_develop(self, current: str) -> bool:
        """Create develop branch if missing and check it out."""
        exists, _ = await _run_git(self._repo_dir, ["rev-parse", "--verify", DEVELOP_BRANCH])
        if not exists:
            success, output = await _run_git(self._repo_dir, ["branch", DEVELOP_BRANCH])
            if not success:
                logger.error("[WORKTREE] Failed to create develop branch: %s", output)
                return False
            logger.info("[WORKTREE] Created develop branch from %s", current)

        success, output = await _run_git(self._repo_dir, ["checkout", DEVELOP_BRANCH])
        if not success:
            logger.error("[WORKTREE] Failed to checkout develop: %s", output)
            return False

        logger.info("[WORKTREE] Working tree now on develop")
        return True

    async def create_worktree(self, delegation_id: str, tier: str) -> WorktreeInfo | None:
        """Create a git worktree for a delegation, branching from develop.

        Returns WorktreeInfo with path and branch, or None on failure.
        """
        if not await self.ensure_develop():
            logger.error("[WORKTREE] Cannot create worktree: develop branch not ready")
            return None

        short_id = delegation_id[:8]
        branch_name = f"delegation/{tier}-{short_id}"
        worktree_path = self._worktree_base / f"delegation-{short_id}"

        self._worktree_base.mkdir(parents=True, exist_ok=True)

        success, output = await _run_git(
            self._repo_dir,
            ["worktree", "add", str(worktree_path), "-b", branch_name, DEVELOP_BRANCH],
        )

        if not success:
            logger.error("[WORKTREE] Failed to create worktree: %s", output)
            return None

        logger.info(
            "[WORKTREE] Created worktree: %s (branch: %s from develop)",
            worktree_path,
            branch_name,
        )
        return WorktreeInfo(path=worktree_path, branch=branch_name, delegation_id=delegation_id)

    async def merge_worktree(self, info: WorktreeInfo) -> bool:
        """Merge worktree branch back to develop.

        The main working tree must be on develop (ensured by create_worktree).
        Returns True if merge succeeded (or nothing to merge).
        """
        await self._auto_commit_if_dirty(info)

        success, output = await _run_git(
            self._repo_dir,
            ["merge", info.branch, "--no-edit"],
        )

        if not success:
            logger.error("[WORKTREE] Merge to develop failed for %s: %s", info.branch, output)
            await self._cleanup(info)
            return False

        logger.info("[WORKTREE] Merged %s into develop", info.branch)
        await self._cleanup(info)
        return True

    async def accept_to_main(self) -> bool:
        """Merge develop into main. Called on explicit acceptance.

        Switches to main, merges develop, switches back to develop.
        Returns True on success.
        """
        # Find the main branch name (main or master)
        main_branch = await self._find_main_branch()
        if main_branch is None:
            logger.error("[WORKTREE] Cannot find main/master branch")
            return False

        success, output = await _run_git(self._repo_dir, ["checkout", main_branch])
        if not success:
            logger.error("[WORKTREE] Failed to checkout %s: %s", main_branch, output)
            return False

        success, output = await _run_git(self._repo_dir, ["merge", DEVELOP_BRANCH, "--no-edit"])
        merge_ok = success
        if not success:
            logger.error("[WORKTREE] Merge develop → %s failed: %s", main_branch, output)

        # Always return to develop
        await _run_git(self._repo_dir, ["checkout", DEVELOP_BRANCH])

        if merge_ok:
            logger.info("[WORKTREE] Accepted: develop merged into %s", main_branch)
        return merge_ok

    async def discard_develop(self) -> bool:
        """Discard develop branch, returning to main. Resets session state.

        Called when the user rejects all delegation work.
        """
        main_branch = await self._find_main_branch()
        if main_branch is None:
            return False

        await _run_git(self._repo_dir, ["checkout", main_branch])
        await _run_git(self._repo_dir, ["branch", "-D", DEVELOP_BRANCH])
        self._develop_ready = False
        logger.info("[WORKTREE] Discarded develop branch, back on %s", main_branch)
        return True

    async def _find_main_branch(self) -> str | None:
        """Find the main branch name (main or master)."""
        for name in ("main", "master"):
            success, _ = await _run_git(self._repo_dir, ["rev-parse", "--verify", name])
            if success:
                return name
        return None

    async def discard_worktree(self, info: WorktreeInfo) -> None:
        """Remove worktree and delete branch without merging."""
        await self._cleanup(info)
        logger.info(
            "[WORKTREE] Discarded worktree %s (branch: %s)",
            info.path.name,
            info.branch,
        )

    async def _auto_commit_if_dirty(self, info: WorktreeInfo) -> None:
        """Auto-commit uncommitted changes in worktree before merge.

        Uses ``git status --porcelain`` so that both modified tracked files
        AND new untracked files are detected.  The previous ``git diff HEAD``
        approach silently missed untracked files, causing child work products
        to be lost on merge.
        """
        if not info.path.exists():
            logger.warning("[WORKTREE] Worktree path missing: %s", info.path)
            return

        success, status = await _run_git(info.path, ["status", "--porcelain"])
        if not success or not status.strip():
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
    to the worktree root.  Worktrees are full repo checkouts, so the
    worktree root is the correct working directory — it contains all
    tracked content.  Restores all on exit (success or failure).
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
