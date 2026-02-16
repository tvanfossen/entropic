"""
Bash command execution MCP server.

Provides shell command execution with configurable restrictions.
"""

import asyncio
import os
from pathlib import Path
from typing import Any

from mcp.types import Tool

from entropi.mcp.servers.base import BaseMCPServer, load_tool_definition


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
            load_tool_definition("execute", "bash"),
        ]

    @staticmethod
    def get_permission_pattern(
        tool_name: str,
        arguments: dict[str, Any],
    ) -> str:
        """Generate base-command-level permission pattern.

        Extracts the base command so "Always Allow" on `ls -la /foo`
        saves `bash.execute:ls *` — allowing all `ls` but not `rm`.
        """
        command = arguments.get("command", "")
        base_cmd = command.split()[0] if command.strip() else "*"
        return f"{tool_name}:{base_cmd} *"

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
        """Execute a shell command and format output."""
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
                return f"Command timed out after {self.timeout}s: {command}"

            stdout_text = stdout.decode("utf-8", errors="replace") if stdout else ""
            stderr_text = stderr.decode("utf-8", errors="replace") if stderr else ""

            exit_code = process.returncode or 0
            return (
                self._format_success(stdout_text, stderr_text)
                if exit_code == 0
                else self._format_error(command, stdout_text, stderr_text, exit_code)
            )

        except Exception as e:
            return f"Execution failed: {e}"

    @staticmethod
    def _format_success(stdout: str, stderr: str) -> str:
        """Format output for successful commands (exit code 0)."""
        parts = []
        if stdout:
            parts.append(stdout)
        if stderr:
            parts.append(f"[stderr]\n{stderr}")
        return "\n".join(parts) if parts else "(no output)"

    @staticmethod
    def _format_error(command: str, stdout: str, stderr: str, exit_code: int) -> str:
        """Format structured diagnostic for failed commands."""
        parts = [f"[command] {command}"]
        if stdout:
            parts.append(f"[stdout]\n{stdout}")
        if stderr:
            parts.append(f"[stderr]\n{stderr}")
        parts.append(f"[exit code: {exit_code}]")
        return "\n".join(parts)
