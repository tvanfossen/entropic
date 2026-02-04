"""
Bash command execution MCP server.

Provides shell command execution with configurable restrictions.
"""

import asyncio
import os
from pathlib import Path
from typing import Any

from mcp.types import Tool

from entropi.mcp.servers.base import BaseMCPServer, create_tool, load_tool_description


class BashServer(BaseMCPServer):
    """Bash command execution MCP server."""

    # Commands that are always blocked
    BLOCKED_COMMANDS = {
        "rm -rf /",
        "rm -rf /*",
        "dd if=/dev/zero",
        "mkfs",
        ":(){:|:&};:",  # Fork bomb
        "> /dev/sda",
        "chmod -R 777 /",
    }

    # Patterns that indicate dangerous commands
    DANGEROUS_PATTERNS = [
        "rm -rf /",
        "> /dev/",
        "mkfs.",
        "dd if=",
        ":(){",
    ]

    def __init__(
        self,
        working_dir: Path | None = None,
        timeout: int = 30,
    ) -> None:
        """
        Initialize bash server.

        Args:
            working_dir: Working directory for commands
            timeout: Command timeout in seconds
        """
        super().__init__("bash")
        self.working_dir = working_dir or Path.cwd()
        self.timeout = timeout

    def get_tools(self) -> list[Tool]:
        """Get available bash tools."""
        return [
            create_tool(
                name="execute",
                description=load_tool_description("execute", "bash"),
                properties={
                    "command": {
                        "type": "string",
                        "description": "Command to execute",
                    },
                    "working_dir": {
                        "type": "string",
                        "description": "Working directory (optional)",
                    },
                },
                required=["command"],
            ),
        ]

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """Execute a bash tool."""
        if name != "execute":
            return f"Unknown tool: {name}"

        command = arguments["command"]
        working_dir = arguments.get("working_dir")

        # Security check
        if not self._is_safe_command(command):
            return f"Command blocked for security: {command}"

        return await self._execute_command(command, working_dir)

    def _is_safe_command(self, command: str) -> bool:
        """Check if command is safe to execute."""
        command_lower = command.lower().strip()
        return not self._matches_any_unsafe_pattern(command_lower)

    def _matches_any_unsafe_pattern(self, command: str) -> bool:
        """Check if command matches any unsafe pattern."""
        checks = [
            self._contains_blocked_command(command),
            self._contains_dangerous_pattern(command),
            command.startswith(("sudo ", "su ")),
            "cd /" in command and "rm" in command,
        ]
        return any(checks)

    def _contains_blocked_command(self, command: str) -> bool:
        """Check if command contains a blocked command."""
        return any(blocked in command for blocked in self.BLOCKED_COMMANDS)

    def _contains_dangerous_pattern(self, command: str) -> bool:
        """Check if command contains a dangerous pattern."""
        return any(pattern in command for pattern in self.DANGEROUS_PATTERNS)

    async def _execute_command(
        self,
        command: str,
        working_dir: str | None,
    ) -> str:
        """Execute a shell command."""
        cwd = Path(working_dir) if working_dir else self.working_dir

        try:
            process = await asyncio.create_subprocess_shell(
                command,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                cwd=cwd,
                env={**os.environ, "TERM": "dumb"},
            )

            try:
                stdout, stderr = await asyncio.wait_for(
                    process.communicate(),
                    timeout=self.timeout,
                )
            except TimeoutError:
                process.kill()
                return f"Command timed out after {self.timeout}s"

            output_parts = []

            if stdout:
                output_parts.append(stdout.decode("utf-8", errors="replace"))

            if stderr:
                stderr_text = stderr.decode("utf-8", errors="replace")
                output_parts.append(f"[stderr]\n{stderr_text}")

            if process.returncode != 0:
                output_parts.append(f"\n[exit code: {process.returncode}]")

            return "\n".join(output_parts) if output_parts else "(no output)"

        except Exception as e:
            return f"Execution failed: {e}"


# Entry point for running as MCP server
if __name__ == "__main__":
    import sys

    working_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    server = BashServer(working_dir)
    asyncio.run(server.run())
