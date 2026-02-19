"""
Python LSP client using pyright.

Provides Python type checking and error detection.

Install: npm install -g pyright
"""

import shutil
import subprocess
from typing import Any

from entropic.lsp.base import BaseLSPClient


class PyrightClient(BaseLSPClient):
    """
    Python LSP client using pyright.

    Pyright provides fast, full-featured Python type checking.
    """

    language = "python"
    extensions = [".py", ".pyi"]

    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self._langserver_cmd: list[str] | None = None

    @property
    def command(self) -> list[str]:
        """Command to start pyright language server."""
        if self._langserver_cmd:
            return self._langserver_cmd
        # Default - will be checked in is_available
        return ["pyright-langserver", "--stdio"]

    @property
    def is_available(self) -> bool:
        """Check if pyright language server is available."""
        from entropic.lsp.base import HAS_LSP

        if not HAS_LSP:
            return False

        # Try to find working command
        cmd = self._find_working_command()
        if cmd:
            self._langserver_cmd = cmd
            return True
        return False

    def _find_working_command(self) -> list[str] | None:
        """Find a working pyright-langserver command."""
        commands_to_try = [
            ["pyright-langserver", "--stdio"],
            ["npx", "pyright-langserver", "--stdio"],
        ]

        for cmd in commands_to_try:
            if shutil.which(cmd[0]) is None:
                continue
            if cmd[0] == "npx" or self._can_start_command(cmd):
                return cmd

        # Last resort: check if pyright CLI exists
        if shutil.which("pyright"):
            return ["pyright-langserver", "--stdio"]
        return None

    def _can_start_command(self, cmd: list[str]) -> bool:
        """Check if command can start successfully."""
        try:
            proc = subprocess.Popen(
                cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
            proc.terminate()
            proc.wait(timeout=2)
            return True
        except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
            return False
