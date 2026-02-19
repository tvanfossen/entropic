"""
Diagnostics MCP server.

Provides code diagnostics from LSP language servers.
"""

from pathlib import Path
from typing import TYPE_CHECKING, Any

from entropic.mcp.servers.base import BaseMCPServer
from entropic.mcp.tools import BaseTool

if TYPE_CHECKING:
    from entropic.lsp.manager import LSPManager


class DiagnosticsTool(BaseTool):
    """Get diagnostics for a file or all files."""

    def __init__(self, server: "DiagnosticsServer") -> None:
        super().__init__("diagnostics", "diagnostics")
        self._server = server

    async def execute(self, arguments: dict[str, Any]) -> str:
        return await self._server._handle_diagnostics(arguments)


class CheckErrorsTool(BaseTool):
    """Check if a file has errors."""

    def __init__(self, server: "DiagnosticsServer") -> None:
        super().__init__("check_errors", "diagnostics")
        self._server = server

    async def execute(self, arguments: dict[str, Any]) -> str:
        return await self._server._handle_check_errors(arguments)


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
        self.register_tool(DiagnosticsTool(self))
        self.register_tool(CheckErrorsTool(self))

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """Execute a diagnostics tool with error handling."""
        try:
            return await self._tool_registry.dispatch(name, arguments)
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
