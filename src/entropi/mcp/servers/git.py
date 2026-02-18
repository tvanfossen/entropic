"""
Git operations MCP server.

Provides git status, diff, commit, log, and branch operations.
"""

import asyncio
from pathlib import Path
from typing import Any

from entropi.mcp.servers.base import BaseMCPServer
from entropi.mcp.tools import BaseTool


async def _git_command(repo_dir: Path, args: list[str]) -> str:
    """Run a git command and return output."""
    process = await asyncio.create_subprocess_exec(
        "git",
        *args,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        cwd=repo_dir,
    )

    stdout, stderr = await process.communicate()

    if process.returncode != 0:
        error_msg = stderr.decode("utf-8", errors="replace")
        return f"Error: {error_msg}"

    return stdout.decode("utf-8", errors="replace") or "(no output)"


# -- Tool classes ------------------------------------------------------------


class StatusTool(BaseTool):
    """Git status tool."""

    def __init__(self, repo_dir: Path) -> None:
        super().__init__("status", "git")
        self._repo_dir = repo_dir

    async def execute(self, arguments: dict[str, Any]) -> str:
        return await _git_command(self._repo_dir, ["status", "--short"])


class DiffTool(BaseTool):
    """Git diff tool."""

    def __init__(self, repo_dir: Path) -> None:
        super().__init__("diff", "git")
        self._repo_dir = repo_dir

    async def execute(self, arguments: dict[str, Any]) -> str:
        args = ["diff"]
        if arguments.get("staged"):
            args.append("--staged")
        if arguments.get("file"):
            args.append(arguments["file"])
        return await _git_command(self._repo_dir, args)


class LogTool(BaseTool):
    """Git log tool."""

    def __init__(self, repo_dir: Path) -> None:
        super().__init__("log", "git")
        self._repo_dir = repo_dir

    async def execute(self, arguments: dict[str, Any]) -> str:
        count = arguments.get("count", 10)
        args = ["log", f"-{count}"]
        if arguments.get("oneline", True):
            args.append("--oneline")
        return await _git_command(self._repo_dir, args)


class CommitTool(BaseTool):
    """Git commit tool."""

    def __init__(self, repo_dir: Path) -> None:
        super().__init__("commit", "git")
        self._repo_dir = repo_dir

    async def execute(self, arguments: dict[str, Any]) -> str:
        if arguments.get("add_all"):
            add_result = await _git_command(self._repo_dir, ["add", "-A"])
            if "Error" in add_result:
                return add_result
        return await _git_command(self._repo_dir, ["commit", "-m", arguments["message"]])


class BranchTool(BaseTool):
    """Git branch tool."""

    def __init__(self, repo_dir: Path) -> None:
        super().__init__("branch", "git")
        self._repo_dir = repo_dir

    async def execute(self, arguments: dict[str, Any]) -> str:
        if arguments.get("name"):
            return await _git_command(self._repo_dir, ["checkout", "-b", arguments["name"]])
        return await _git_command(self._repo_dir, ["branch", "-a"])


class CheckoutTool(BaseTool):
    """Git checkout tool."""

    def __init__(self, repo_dir: Path) -> None:
        super().__init__("checkout", "git")
        self._repo_dir = repo_dir

    async def execute(self, arguments: dict[str, Any]) -> str:
        return await _git_command(self._repo_dir, ["checkout", arguments["target"]])


class AddTool(BaseTool):
    """Git add tool."""

    def __init__(self, repo_dir: Path) -> None:
        super().__init__("add", "git")
        self._repo_dir = repo_dir

    async def execute(self, arguments: dict[str, Any]) -> str:
        files = arguments["files"].split()
        return await _git_command(self._repo_dir, ["add"] + files)


class ResetTool(BaseTool):
    """Git reset tool."""

    def __init__(self, repo_dir: Path) -> None:
        super().__init__("reset", "git")
        self._repo_dir = repo_dir

    async def execute(self, arguments: dict[str, Any]) -> str:
        files = arguments.get("files", "").split()
        cmd = ["reset", "HEAD"] + files if files else ["reset", "HEAD"]
        return await _git_command(self._repo_dir, cmd)


# -- Server ------------------------------------------------------------------


class GitServer(BaseMCPServer):
    """Git operations MCP server."""

    def __init__(self, repo_dir: Path | None = None) -> None:
        """
        Initialize git server.

        Args:
            repo_dir: Repository directory
        """
        super().__init__("git")
        self.repo_dir = repo_dir or Path.cwd()
        self.register_tool(StatusTool(self.repo_dir))
        self.register_tool(DiffTool(self.repo_dir))
        self.register_tool(LogTool(self.repo_dir))
        self.register_tool(CommitTool(self.repo_dir))
        self.register_tool(BranchTool(self.repo_dir))
        self.register_tool(CheckoutTool(self.repo_dir))
        self.register_tool(AddTool(self.repo_dir))
        self.register_tool(ResetTool(self.repo_dir))

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """Execute a git tool with error handling."""
        try:
            return await self._tool_registry.dispatch(name, arguments)
        except Exception as e:
            return f"Git error: {e}"
