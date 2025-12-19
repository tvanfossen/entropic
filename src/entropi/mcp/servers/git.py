"""
Git operations MCP server.

Provides git status, diff, commit, log, and branch operations.
"""

import asyncio
from pathlib import Path
from typing import Any

from mcp.types import Tool

from entropi.mcp.servers.base import BaseMCPServer, create_tool


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

    def get_tools(self) -> list[Tool]:
        """Get available git tools."""
        return [
            create_tool(
                name="status",
                description="Get git status",
                properties={},
            ),
            create_tool(
                name="diff",
                description="Get git diff",
                properties={
                    "staged": {
                        "type": "boolean",
                        "description": "Show staged changes only",
                    },
                    "file": {
                        "type": "string",
                        "description": "Specific file to diff",
                    },
                },
            ),
            create_tool(
                name="log",
                description="Get git log",
                properties={
                    "count": {
                        "type": "integer",
                        "description": "Number of commits",
                    },
                    "oneline": {
                        "type": "boolean",
                        "description": "One line per commit",
                    },
                },
            ),
            create_tool(
                name="commit",
                description="Create a git commit",
                properties={
                    "message": {
                        "type": "string",
                        "description": "Commit message",
                    },
                    "add_all": {
                        "type": "boolean",
                        "description": "Stage all changes first",
                    },
                },
                required=["message"],
            ),
            create_tool(
                name="branch",
                description="List or create branches",
                properties={
                    "name": {
                        "type": "string",
                        "description": "Branch name to create (omit to list)",
                    },
                },
            ),
            create_tool(
                name="checkout",
                description="Checkout a branch or file",
                properties={
                    "target": {
                        "type": "string",
                        "description": "Branch name or file path",
                    },
                },
                required=["target"],
            ),
            create_tool(
                name="add",
                description="Stage files for commit",
                properties={
                    "files": {
                        "type": "string",
                        "description": "Files to add (space-separated, or '.' for all)",
                    },
                },
                required=["files"],
            ),
            create_tool(
                name="reset",
                description="Unstage files",
                properties={
                    "files": {
                        "type": "string",
                        "description": "Files to unstage (space-separated)",
                    },
                },
            ),
        ]

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """Execute a git tool."""
        handlers = {
            "status": self._handle_status,
            "diff": self._diff,
            "log": self._log,
            "commit": self._commit,
            "branch": self._branch,
            "checkout": self._handle_checkout,
            "add": self._handle_add,
            "reset": self._handle_reset,
        }
        handler = handlers.get(name)
        if not handler:
            return f"Unknown tool: {name}"
        try:
            return await handler(arguments)
        except Exception as e:
            return f"Git error: {e}"

    async def _handle_status(self, arguments: dict[str, Any]) -> str:
        return await self._git_command(["status", "--short"])

    async def _handle_checkout(self, arguments: dict[str, Any]) -> str:
        return await self._git_command(["checkout", arguments["target"]])

    async def _handle_add(self, arguments: dict[str, Any]) -> str:
        files = arguments["files"].split()
        return await self._git_command(["add"] + files)

    async def _handle_reset(self, arguments: dict[str, Any]) -> str:
        files = arguments.get("files", "").split()
        cmd = ["reset", "HEAD"] + files if files else ["reset", "HEAD"]
        return await self._git_command(cmd)

    async def _git_command(self, args: list[str]) -> str:
        """Run a git command."""
        process = await asyncio.create_subprocess_exec(
            "git",
            *args,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=self.repo_dir,
        )

        stdout, stderr = await process.communicate()

        if process.returncode != 0:
            error_msg = stderr.decode("utf-8", errors="replace")
            return f"Error: {error_msg}"

        return stdout.decode("utf-8", errors="replace") or "(no output)"

    async def _diff(self, arguments: dict[str, Any]) -> str:
        """Get git diff."""
        args = ["diff"]

        if arguments.get("staged"):
            args.append("--staged")

        if arguments.get("file"):
            args.append(arguments["file"])

        return await self._git_command(args)

    async def _log(self, arguments: dict[str, Any]) -> str:
        """Get git log."""
        count = arguments.get("count", 10)
        args = ["log", f"-{count}"]

        if arguments.get("oneline", True):
            args.append("--oneline")

        return await self._git_command(args)

    async def _commit(self, arguments: dict[str, Any]) -> str:
        """Create a commit."""
        if arguments.get("add_all"):
            add_result = await self._git_command(["add", "-A"])
            if "Error" in add_result:
                return add_result

        return await self._git_command(["commit", "-m", arguments["message"]])

    async def _branch(self, arguments: dict[str, Any]) -> str:
        """List or create branch."""
        if arguments.get("name"):
            return await self._git_command(["checkout", "-b", arguments["name"]])
        else:
            return await self._git_command(["branch", "-a"])


# Entry point for running as MCP server
if __name__ == "__main__":
    import sys

    repo_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    server = GitServer(repo_dir)
    asyncio.run(server.run())
