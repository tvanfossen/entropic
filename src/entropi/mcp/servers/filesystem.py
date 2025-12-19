"""
Filesystem MCP server.

Provides file reading, writing, listing, and searching capabilities.
"""

import asyncio
import re
from pathlib import Path
from typing import Any

from mcp.types import Tool

from entropi.mcp.servers.base import BaseMCPServer, create_tool


class FilesystemServer(BaseMCPServer):
    """Filesystem operations MCP server."""

    def __init__(self, root_dir: Path | None = None) -> None:
        """
        Initialize filesystem server.

        Args:
            root_dir: Root directory for operations (default: cwd)
        """
        super().__init__("filesystem")
        self.root_dir = root_dir or Path.cwd()

    def get_tools(self) -> list[Tool]:
        """Get available filesystem tools."""
        return [
            create_tool(
                name="read_file",
                description="Read the contents of a file",
                properties={
                    "path": {
                        "type": "string",
                        "description": "Path to the file (relative to project root)",
                    },
                },
                required=["path"],
            ),
            create_tool(
                name="write_file",
                description="Write content to a file (creates parent directories)",
                properties={
                    "path": {
                        "type": "string",
                        "description": "Path to the file",
                    },
                    "content": {
                        "type": "string",
                        "description": "Content to write",
                    },
                },
                required=["path", "content"],
            ),
            create_tool(
                name="list_directory",
                description="List contents of a directory",
                properties={
                    "path": {
                        "type": "string",
                        "description": "Path to directory (default: project root)",
                    },
                    "recursive": {
                        "type": "boolean",
                        "description": "List recursively",
                    },
                },
            ),
            create_tool(
                name="search_files",
                description="Search for files matching a pattern",
                properties={
                    "pattern": {
                        "type": "string",
                        "description": "Glob pattern (e.g., '*.py', 'src/**/*.ts')",
                    },
                    "content_pattern": {
                        "type": "string",
                        "description": "Optional regex to search file contents",
                    },
                },
                required=["pattern"],
            ),
            create_tool(
                name="file_exists",
                description="Check if a file or directory exists",
                properties={
                    "path": {
                        "type": "string",
                        "description": "Path to check",
                    },
                },
                required=["path"],
            ),
        ]

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """Execute a filesystem tool."""
        handlers = {
            "read_file": self._handle_read_file,
            "write_file": self._handle_write_file,
            "list_directory": self._handle_list_directory,
            "search_files": self._handle_search_files,
            "file_exists": self._handle_file_exists,
        }
        handler = handlers.get(name)
        if not handler:
            return f"Unknown tool: {name}"
        try:
            return await handler(arguments)
        except Exception as e:
            return f"Error: {e}"

    async def _handle_read_file(self, args: dict[str, Any]) -> str:
        return await self._read_file(args["path"])

    async def _handle_write_file(self, args: dict[str, Any]) -> str:
        return await self._write_file(args["path"], args["content"])

    async def _handle_list_directory(self, args: dict[str, Any]) -> str:
        return await self._list_directory(args.get("path", "."), args.get("recursive", False))

    async def _handle_search_files(self, args: dict[str, Any]) -> str:
        return await self._search_files(args["pattern"], args.get("content_pattern"))

    async def _handle_file_exists(self, args: dict[str, Any]) -> str:
        return await self._file_exists(args["path"])

    def _resolve_path(self, path: str) -> Path:
        """Resolve path relative to root directory."""
        resolved = (self.root_dir / path).resolve()

        # Security: ensure path is within root
        root_resolved = self.root_dir.resolve()
        if not str(resolved).startswith(str(root_resolved)):
            raise ValueError(f"Path outside root directory: {path}")

        return resolved

    async def _read_file(self, path: str) -> str:
        """Read file contents."""
        resolved = self._resolve_path(path)

        if not resolved.exists():
            return f"File not found: {path}"

        if not resolved.is_file():
            return f"Not a file: {path}"

        # Run in executor for non-blocking I/O
        loop = asyncio.get_event_loop()
        content = await loop.run_in_executor(None, resolved.read_text)

        return content

    async def _write_file(self, path: str, content: str) -> str:
        """Write file contents."""
        resolved = self._resolve_path(path)

        # Create parent directories
        resolved.parent.mkdir(parents=True, exist_ok=True)

        loop = asyncio.get_event_loop()
        await loop.run_in_executor(
            None,
            lambda: resolved.write_text(content),
        )

        return f"Written {len(content)} bytes to {path}"

    # Patterns to skip in directory listings
    _SKIP_PATTERNS = {".git", "node_modules", "__pycache__", ".venv", "venv", ".mypy_cache"}

    async def _list_directory(self, path: str, recursive: bool) -> str:
        """List directory contents."""
        resolved = self._resolve_path(path)

        if not resolved.exists():
            return f"Directory not found: {path}"

        if not resolved.is_dir():
            return f"Not a directory: {path}"

        lines = self._list_recursive(resolved) if recursive else self._list_flat(resolved)
        return "\n".join(lines) if lines else "(empty directory)"

    def _list_recursive(self, resolved: Path) -> list[str]:
        """List directory recursively."""
        lines = []
        for item in sorted(resolved.rglob("*")):
            rel_path = item.relative_to(resolved)
            if self._should_skip_path(rel_path):
                continue
            prefix = "[dir] " if item.is_dir() else "[file] "
            lines.append(f"{prefix}{rel_path}")
        return lines

    def _list_flat(self, resolved: Path) -> list[str]:
        """List directory non-recursively."""
        lines = []
        for item in sorted(resolved.iterdir()):
            if item.name.startswith(".") or item.name in self._SKIP_PATTERNS:
                continue
            prefix = "[dir] " if item.is_dir() else "[file] "
            lines.append(f"{prefix}{item.name}")
        return lines

    def _should_skip_path(self, rel_path: Path) -> bool:
        """Check if path should be skipped in listings."""
        if any(part.startswith(".") for part in rel_path.parts):
            return True
        return any(part in self._SKIP_PATTERNS for part in rel_path.parts)

    async def _search_files(
        self,
        pattern: str,
        content_pattern: str | None,
    ) -> str:
        """Search for files."""
        matches = []
        for path in self.root_dir.rglob(pattern):
            rel_path = path.relative_to(self.root_dir)
            if self._should_skip_path(rel_path):
                continue
            if self._matches_content(path, content_pattern):
                matches.append(str(rel_path))
        return "\n".join(matches) if matches else "No matches found"

    def _matches_content(self, path: Path, content_pattern: str | None) -> bool:
        """Check if file matches content pattern (or no pattern specified)."""
        if not content_pattern or not path.is_file():
            return True
        try:
            content = path.read_text()
            return bool(re.search(content_pattern, content))
        except Exception:
            return False

    async def _file_exists(self, path: str) -> str:
        """Check if file exists."""
        resolved = self._resolve_path(path)
        if resolved.exists():
            kind = "directory" if resolved.is_dir() else "file"
            return f"Yes, {path} exists ({kind})"
        return f"No, {path} does not exist"


# Entry point for running as MCP server
if __name__ == "__main__":
    import sys

    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    server = FilesystemServer(root)
    asyncio.run(server.run())
