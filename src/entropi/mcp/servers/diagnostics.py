"""
Diagnostics MCP server.

Provides code diagnostics from LSP language servers.
"""

import asyncio
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp.types import Tool

from entropi.mcp.servers.base import BaseMCPServer, load_tool_definition

if TYPE_CHECKING:
    from entropi.lsp.manager import LSPManager


class DiagnosticsServer(BaseMCPServer):
    """Diagnostics MCP server backed by LSP."""

    def __init__(
        self,
        lsp_manager: "LSPManager",
        root_dir: Path | None = None,
    ) -> None:
        """
        Initialize diagnostics server.

        Args:
            lsp_manager: LSP manager instance
            root_dir: Root directory for path resolution
        """
        super().__init__("diagnostics")
        self.lsp_manager = lsp_manager
        self.root_dir = root_dir or Path.cwd()

    def get_tools(self) -> list[Tool]:
        """Get available diagnostics tools."""
        return [
            load_tool_definition("diagnostics", "diagnostics"),
            load_tool_definition("check_errors", "diagnostics"),
        ]

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """Execute a diagnostics tool."""
        handlers = {
            "diagnostics": self._handle_diagnostics,
            "check_errors": self._handle_check_errors,
        }
        handler = handlers.get(name)
        if not handler:
            return f"Unknown tool: {name}"
        try:
            return await handler(arguments)
        except Exception as e:
            return f"Error: {e}"

    def _resolve_path(self, path: str) -> Path:
        """Resolve path relative to root directory."""
        resolved = (self.root_dir / path).resolve()
        return resolved

    async def _handle_diagnostics(self, args: dict[str, Any]) -> str:
        """Get diagnostics for a file or all files."""
        file_path = args["file_path"]

        if not self.lsp_manager.is_enabled:
            return "LSP is not enabled. Configure LSP in config.yaml to use diagnostics."

        if file_path.lower() == "all":
            return await self._get_all_diagnostics()
        else:
            return await self._get_file_diagnostics(file_path)

    async def _get_file_diagnostics(self, file_path: str) -> str:
        """Get diagnostics for a single file."""
        path = self._resolve_path(file_path)

        if not path.exists():
            return f"File not found: {file_path}"

        # Open file in LSP (idempotent)
        self.lsp_manager.open_file(path)

        # Wait for LSP to publish diagnostics (with proper timeout)
        diags = await self.lsp_manager.wait_for_diagnostics(path, timeout=2.0)

        if not diags:
            return f"No diagnostics for {file_path}"

        lines = [f"Diagnostics for {file_path}:"]
        for d in diags:
            lines.append(f"  {d.format()}")

        return "\n".join(lines)

    async def _get_all_diagnostics(self) -> str:
        """Get diagnostics for all open files."""
        all_diags = self.lsp_manager.get_all_diagnostics()

        if not all_diags:
            return "No diagnostics found in any files."

        lines = []
        for path, diags in all_diags.items():
            if diags:
                rel_path = (
                    path.relative_to(self.root_dir) if path.is_relative_to(self.root_dir) else path
                )
                lines.append(f"\n{rel_path}:")
                for d in diags:
                    lines.append(f"  {d.format()}")

        if not lines:
            return "No diagnostics found in any files."

        return "\n".join(lines)

    async def _handle_check_errors(self, args: dict[str, Any]) -> str:
        """Check if a file has errors."""
        file_path = args["file_path"]
        path = self._resolve_path(file_path)

        # Validate preconditions
        error_msg = self._validate_check_errors(path, file_path)
        if error_msg:
            return error_msg

        # Open file and wait for LSP to publish diagnostics
        self.lsp_manager.open_file(path)
        await self.lsp_manager.wait_for_diagnostics(path, timeout=2.0)

        errors = self.lsp_manager.get_errors(path)
        if errors:
            error_lines = "\n".join(f"  {e.format()}" for e in errors)
            return f"Yes, {file_path} has {len(errors)} error(s):\n{error_lines}"
        return f"No, {file_path} has no errors."

    def _validate_check_errors(self, path: Path, file_path: str) -> str | None:
        """Validate preconditions for check_errors, return error message or None."""
        if not path.exists():
            return f"File not found: {file_path}"
        if not self.lsp_manager.is_enabled:
            return "LSP is not enabled."
        return None


# Entry point for running as MCP server
if __name__ == "__main__":
    import os
    import sys

    from entropi.config.schema import LSPConfig
    from entropi.lsp.manager import LSPManager as _LSPManager

    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()

    # Create LSP manager with default config
    config = LSPConfig()
    manager = _LSPManager(config, root)
    manager.start()

    try:
        server = DiagnosticsServer(manager, root)
        asyncio.run(server.run())
    finally:
        # Suppress stderr during shutdown to avoid "server quit" errors
        # from pylspclient threads when file handles are closing
        try:
            sys.stderr = open(os.devnull, "w")
            sys.stdout = open(os.devnull, "w")
        except Exception:
            pass
        try:
            manager.stop()
        except Exception:
            pass
