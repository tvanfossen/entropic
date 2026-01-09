"""
Python LSP client using pyright.

Provides Python type checking and error detection.

Install: npm install -g pyright
"""

import subprocess
import shutil

from entropi.lsp.base import BaseLSPClient


class PyrightClient(BaseLSPClient):
    """
    Python LSP client using pyright.

    Pyright provides fast, full-featured Python type checking.
    """

    language = "python"
    extensions = [".py", ".pyi"]

    def __init__(self, *args, **kwargs):
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
        from entropi.lsp.base import HAS_LSP
        if not HAS_LSP:
            return False

        # Try different ways to invoke pyright-langserver
        commands_to_try = [
            ["pyright-langserver", "--stdio"],
            ["npx", "pyright-langserver", "--stdio"],
        ]

        for cmd in commands_to_try:
            # Check if command exists
            if shutil.which(cmd[0]) is None:
                continue

            # For npx, just check that npx exists
            if cmd[0] == "npx":
                self._langserver_cmd = cmd
                return True

            # For direct command, verify it can start
            try:
                proc = subprocess.Popen(
                    cmd,
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                proc.terminate()
                proc.wait(timeout=2)
                self._langserver_cmd = cmd
                return True
            except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
                continue

        # Last resort: check if pyright CLI exists (langserver may be available)
        if shutil.which("pyright"):
            # pyright exists, assume langserver is available too
            self._langserver_cmd = ["pyright-langserver", "--stdio"]
            return True

        return False
