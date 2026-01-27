"""
Filesystem MCP server.

Provides file reading, writing, listing, and searching capabilities.
Implements read-before-write safety pattern.
"""

import asyncio
import re
from pathlib import Path
from typing import Any

from mcp.types import Tool

from entropi.mcp.servers.base import BaseMCPServer, create_tool
from entropi.mcp.servers.file_tracker import FileAccessTracker


class FilesystemServer(BaseMCPServer):
    """Filesystem operations MCP server with read-before-write enforcement."""

    def __init__(self, root_dir: Path | None = None) -> None:
        """
        Initialize filesystem server.

        Args:
            root_dir: Root directory for operations (default: cwd)
        """
        super().__init__("filesystem")
        self.root_dir = root_dir or Path.cwd()
        self._tracker = FileAccessTracker()

    def get_tools(self) -> list[Tool]:
        """Get available filesystem tools."""
        return [
            create_tool(
                name="read_file",
                description=(
                    "Read a file's contents. You MUST read a file before you can "
                    "edit or write to it. This ensures you have the current content."
                ),
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
                description=(
                    "Create or overwrite a file. IMPORTANT: If the file exists, "
                    "you MUST read it first using read_file. For modifying files, "
                    "prefer edit_file which uses precise string replacement."
                ),
                properties={
                    "path": {
                        "type": "string",
                        "description": "Path to the file to create/overwrite",
                    },
                    "content": {
                        "type": "string",
                        "description": "Full content to write to the file",
                    },
                },
                required=["path", "content"],
            ),
            create_tool(
                name="edit_file",
                description=(
                    "Edit a file by replacing exact string matches. PREFERRED method "
                    "for modifications. Requirements: 1) Read the file first, "
                    "2) old_string must match EXACTLY including whitespace, "
                    "3) If multiple matches, provide more context or use replace_all."
                ),
                properties={
                    "path": {
                        "type": "string",
                        "description": "Path to the file to edit",
                    },
                    "old_string": {
                        "type": "string",
                        "description": "Exact string to find and replace",
                    },
                    "new_string": {
                        "type": "string",
                        "description": "Replacement string",
                    },
                    "replace_all": {
                        "type": "boolean",
                        "description": "Replace all occurrences (default: false)",
                    },
                },
                required=["path", "old_string", "new_string"],
            ),
            create_tool(
                name="list_directory",
                description="List all files and directories in a path. Use this to see what exists.",
                properties={
                    "path": {
                        "type": "string",
                        "description": "Path to directory (default: current directory)",
                    },
                    "recursive": {
                        "type": "boolean",
                        "description": "If true, list all nested contents",
                    },
                },
            ),
            create_tool(
                name="search_files",
                description="Find files matching a glob pattern, optionally searching their contents.",
                properties={
                    "pattern": {
                        "type": "string",
                        "description": "Glob pattern (e.g., '*.py', 'src/**/*.ts')",
                    },
                    "content_pattern": {
                        "type": "string",
                        "description": "Optional regex to search within matching files",
                    },
                },
                required=["pattern"],
            ),
            create_tool(
                name="file_exists",
                description="Check if a specific file or directory exists.",
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
            "edit_file": self._handle_edit_file,
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

    async def _handle_edit_file(self, args: dict[str, Any]) -> str:
        return await self._edit_file(
            args["path"],
            args["old_string"],
            args["new_string"],
            args.get("replace_all", False),
        )

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
        """Read file contents and record for tracking."""
        resolved = self._resolve_path(path)

        if not resolved.exists():
            return f"File not found: {path}"

        if not resolved.is_file():
            return f"Not a file: {path}"

        # Run in executor for non-blocking I/O
        loop = asyncio.get_event_loop()
        content = await loop.run_in_executor(None, resolved.read_text)

        # Record the read for write/edit validation
        self._tracker.record_read(resolved, content)

        return content

    async def _write_file(self, path: str, content: str) -> str:
        """Write file contents with read-before-write enforcement."""
        resolved = self._resolve_path(path)

        # For existing files, require prior read
        if resolved.exists():
            if not self._tracker.was_read_recently(resolved):
                return (
                    f"Cannot write to {path}: file must be read first. "
                    "Use read_file before writing to existing files."
                )

            # Verify file hasn't changed externally
            current_content = resolved.read_text()
            if not self._tracker.verify_unchanged(resolved, current_content):
                return (
                    f"File {path} has been modified since it was read. "
                    "Read the file again to get current content."
                )

        # Create parent directories
        resolved.parent.mkdir(parents=True, exist_ok=True)

        loop = asyncio.get_event_loop()
        await loop.run_in_executor(
            None,
            lambda: resolved.write_text(content),
        )

        # Record the new content as read
        self._tracker.record_read(resolved, content)

        return f"Written {len(content)} bytes to {path}"

    async def _edit_file(
        self,
        path: str,
        old_string: str,
        new_string: str,
        replace_all: bool = False,
    ) -> str:
        """Edit a file by replacing exact string matches."""
        resolved = self._resolve_path(path)

        if not resolved.exists():
            return f"File not found: {path}"

        # Require prior read
        if not self._tracker.was_read_recently(resolved):
            return f"Cannot edit {path}: file must be read first. " "Use read_file before editing."

        content = resolved.read_text()

        # Verify file hasn't changed
        if not self._tracker.verify_unchanged(resolved, content):
            return (
                f"File {path} has been modified since it was read. "
                "Read the file again to get current content."
            )

        # Check for matches
        match_count = content.count(old_string)

        if match_count == 0:
            return (
                f"String not found in {path}. "
                "The file content may have changed or the search string is incorrect."
            )

        if not replace_all and match_count > 1:
            return (
                f"Found {match_count} matches in {path}. "
                "Provide more context to uniquely identify the location, "
                "or set replace_all=true to replace all occurrences."
            )

        # Perform replacement
        if replace_all:
            new_content = content.replace(old_string, new_string)
            replacements = match_count
        else:
            new_content = content.replace(old_string, new_string, 1)
            replacements = 1

        # Write and update tracker
        resolved.write_text(new_content)
        self._tracker.record_read(resolved, new_content)

        return f"Replaced {replacements} occurrence(s) in {path}"

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
