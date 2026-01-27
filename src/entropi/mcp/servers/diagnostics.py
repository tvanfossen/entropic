"""
Diagnostics MCP server.

Provides code diagnostics from LSP language servers.
"""

import asyncio
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp.types import Tool

from entropi.mcp.servers.base import BaseMCPServer, create_tool

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
            create_tool(
                name="diagnostics",
                description=(
                    "Get code errors and warnings for a file from the language server. "
                    "Returns type errors, syntax errors, undefined variables, etc. "
                    "Use 'all' to get diagnostics for all open files."
                ),
                properties={
                    "file_path": {
                        "type": "string",
                        "description": "Path to the file to check, or 'all' for all files",
                    },
                },
                required=["file_path"],
            ),
            create_tool(
                name="check_errors",
                description=(
                    "Check if a file has any errors (not warnings). "
                    "Returns true/false with error count."
                ),
                properties={
                    "file_path": {
                        "type": "string",
                        "description": "Path to the file to check",
                    },
                },
                required=["file_path"],
            ),
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

        # Small delay to allow diagnostics to be published
        await asyncio.sleep(0.2)

        diags = self.lsp_manager.get_diagnostics(path)

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

        if not path.exists():
            return f"File not found: {file_path}"

        if not self.lsp_manager.is_enabled:
            return "LSP is not enabled."

        # Open file and wait briefly for diagnostics
        self.lsp_manager.open_file(path)
        await asyncio.sleep(0.2)

        has_errors = self.lsp_manager.has_errors(path)
        errors = self.lsp_manager.get_errors(path)

        if has_errors:
            return f"Yes, {file_path} has {len(errors)} error(s):\n" + "\n".join(
                f"  {e.format()}" for e in errors
            )
        else:
            return f"No, {file_path} has no errors."


# Entry point for running as MCP server
if __name__ == "__main__":
    import sys

    from entropi.config.schema import LSPConfig
    from entropi.lsp.manager import LSPManager

    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()

    # Create LSP manager with default config
    config = LSPConfig()
    manager = LSPManager(config, root)
    manager.start()

    try:
        server = DiagnosticsServer(manager, root)
        asyncio.run(server.run())
    finally:
        manager.stop()
