"""
Filesystem MCP server.

Provides file reading, writing, and editing capabilities.
Implements read-before-write safety pattern.
"""

import asyncio
import difflib
import json
from pathlib import Path
from typing import Any

from mcp.types import Tool

from entropi.mcp.servers.base import BaseMCPServer, create_tool, load_tool_description
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
        """Get available filesystem tools - unified read/edit/write pattern."""
        return [
            create_tool(
                name="read_file",
                description=load_tool_description("read_file"),
                properties={
                    "path": {
                        "type": "string",
                        "description": "Path to the file",
                    },
                },
                required=["path"],
            ),
            create_tool(
                name="edit_file",
                description=load_tool_description("edit_file"),
                properties={
                    "path": {
                        "type": "string",
                        "description": "Path to the file to edit",
                    },
                    "old_string": {
                        "type": "string",
                        "description": "STR_REPLACE mode: exact string to find and replace",
                    },
                    "new_string": {
                        "type": "string",
                        "description": "Replacement string (or content to insert)",
                    },
                    "insert_line": {
                        "type": "integer",
                        "description": "INSERT mode: line number to insert AFTER (0 = beginning)",
                    },
                    "replace_all": {
                        "type": "boolean",
                        "description": "STR_REPLACE mode: replace all occurrences (default: false)",
                    },
                },
                required=["path", "new_string"],
            ),
            create_tool(
                name="write_file",
                description=load_tool_description("write_file"),
                properties={
                    "path": {
                        "type": "string",
                        "description": "Path to the file",
                    },
                    "content": {
                        "type": "string",
                        "description": "Full file content",
                    },
                },
                required=["path", "content"],
            ),
        ]

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """Execute a filesystem tool with JSON schema validation."""
        handlers = {
            "read_file": self._handle_read_file,
            "write_file": self._handle_write_file,
            "edit_file": self._handle_edit_file,
        }
        handler = handlers.get(name)
        if not handler:
            return json.dumps({"error": "unknown_tool", "message": f"Unknown tool: {name}"})
        try:
            return await handler(arguments)
        except KeyError as e:
            return json.dumps({"error": "missing_param", "message": f"Missing required parameter: {e}"})
        except TypeError as e:
            return json.dumps({"error": "invalid_type", "message": f"Invalid parameter type: {e}"})
        except Exception as e:
            return json.dumps({"error": "execution_error", "message": str(e)})

    async def _handle_read_file(self, args: dict[str, Any]) -> str:
        return await self._read_file(args["path"])

    async def _handle_write_file(self, args: dict[str, Any]) -> str:
        return await self._write_file(args["path"], args["content"])

    async def _handle_edit_file(self, args: dict[str, Any]) -> str:
        """Route to str_replace or insert mode based on parameters."""
        path = args["path"]
        new_string = args["new_string"]

        # INSERT mode: insert_line is provided
        if "insert_line" in args:
            return await self._insert_at_line(path, args["insert_line"], new_string)

        # STR_REPLACE mode: old_string is required
        if "old_string" not in args:
            return json.dumps({
                "error": "invalid_params",
                "message": "edit_file requires either old_string (str_replace) or insert_line (insert)"
            })

        return await self._str_replace(
            path,
            args["old_string"],
            new_string,
            args.get("replace_all", False),
        )

    def _resolve_path(self, path: str) -> Path:
        """Resolve path relative to root directory."""
        resolved = (self.root_dir / path).resolve()

        # Security: ensure path is within root
        root_resolved = self.root_dir.resolve()
        if not str(resolved).startswith(str(root_resolved)):
            raise ValueError(f"Path outside root directory: {path}")

        return resolved

    async def _read_file(self, path: str) -> str:
        """Read file contents as structured JSON with line numbers."""
        resolved = self._resolve_path(path)

        if not resolved.exists():
            return json.dumps({"error": "not_found", "message": f"File not found: {path}"})

        if not resolved.is_file():
            return json.dumps({"error": "not_a_file", "message": f"Not a file: {path}"})

        # Run in executor for non-blocking I/O
        loop = asyncio.get_event_loop()
        content = await loop.run_in_executor(None, resolved.read_text)

        # Record the ORIGINAL content for write/edit validation
        self._tracker.record_read(resolved, content)

        # Return structured JSON with line numbers as keys
        lines = content.splitlines()
        return json.dumps({
            "path": path,
            "total": len(lines),
            "lines": {str(i): line for i, line in enumerate(lines, 1)},
        })

    async def _write_file(self, path: str, content: str) -> str:
        """Write file contents with read-before-write enforcement."""
        resolved = self._resolve_path(path)

        # For existing files, require prior read
        if resolved.exists():
            if not self._tracker.was_read_recently(resolved):
                return json.dumps({
                    "error": "read_required",
                    "message": f"Cannot write to {path}: file must be read first."
                })

            # Verify file hasn't changed externally
            current_content = resolved.read_text()
            if not self._tracker.verify_unchanged(resolved, current_content):
                return json.dumps({
                    "error": "file_changed",
                    "message": f"File {path} modified since read. Read again first."
                })

        # Create parent directories
        resolved.parent.mkdir(parents=True, exist_ok=True)

        loop = asyncio.get_event_loop()
        await loop.run_in_executor(
            None,
            lambda: resolved.write_text(content),
        )

        # Record the new content as read
        self._tracker.record_read(resolved, content)

        return json.dumps({
            "success": True,
            "bytes_written": len(content),
            "path": path
        })

    async def _str_replace(
        self,
        path: str,
        old_string: str,
        new_string: str,
        replace_all: bool = False,
    ) -> str:
        """Edit a file by replacing exact string matches (str_replace mode)."""
        resolved = self._resolve_path(path)

        if not resolved.exists():
            return json.dumps({"error": "not_found", "message": f"File not found: {path}"})

        # Require prior read
        if not self._tracker.was_read_recently(resolved):
            return json.dumps({
                "error": "read_required",
                "message": f"Cannot edit {path}: file must be read first."
            })

        content = resolved.read_text()

        # Verify file hasn't changed
        if not self._tracker.verify_unchanged(resolved, content):
            return json.dumps({
                "error": "file_changed",
                "message": f"File {path} modified since read. Read again first."
            })

        # Check for matches
        match_count = content.count(old_string)

        if match_count == 0:
            hint = self._find_nearest_match(content, old_string)
            return json.dumps({
                "error": "no_match",
                "message": f"String not found in {path}",
                "debug": hint,
                "tip": "Use edit_file with insert_line for line-based insertion"
            })

        if not replace_all and match_count > 1:
            match_lines = self._find_match_lines(content, old_string)
            return json.dumps({
                "error": "multiple_matches",
                "message": f"Found {match_count} matches in {path}",
                "match_lines": match_lines[:10],
                "tip": "Add more context to old_string, or set replace_all=true"
            })

        # Find line number(s) where replacement occurs
        lines = content.splitlines(keepends=True)
        changes = []

        # Build line start positions for mapping
        line_starts = [0]
        for line in lines:
            line_starts.append(line_starts[-1] + len(line))

        def find_line_number(pos: int) -> int:
            """Find 1-indexed line number for a character position."""
            for i, start in enumerate(line_starts[:-1]):
                if start <= pos < line_starts[i + 1]:
                    return i + 1
            return len(lines)

        # Collect change info before modifying
        search_pos = 0
        while True:
            idx = content.find(old_string, search_pos)
            if idx == -1:
                break
            line_num = find_line_number(idx)
            changes.append({
                "line": line_num,
                "before": old_string,
                "after": new_string
            })
            if not replace_all:
                break
            search_pos = idx + len(old_string)

        # Perform replacement
        if replace_all:
            new_content = content.replace(old_string, new_string)
        else:
            new_content = content.replace(old_string, new_string, 1)

        # Write and update tracker
        resolved.write_text(new_content)
        self._tracker.record_read(resolved, new_content)

        return json.dumps({
            "success": True,
            "path": path,
            "mode": "str_replace",
            "changes": changes
        })

    async def _insert_at_line(
        self,
        path: str,
        insert_line: int,
        new_string: str,
    ) -> str:
        """Insert content at a specific line number (insert mode)."""
        resolved = self._resolve_path(path)

        if not resolved.exists():
            return json.dumps({"error": "not_found", "message": f"File not found: {path}"})

        if not self._tracker.was_read_recently(resolved):
            return json.dumps({
                "error": "read_required",
                "message": f"Cannot edit {path}: file must be read first."
            })

        content = resolved.read_text()

        if not self._tracker.verify_unchanged(resolved, content):
            return json.dumps({
                "error": "file_changed",
                "message": f"File {path} modified since read. Read again first."
            })

        lines = content.splitlines(keepends=True)
        total_lines = len(lines)

        # Validate insert_line (0 = insert at beginning, N = insert after line N)
        if insert_line < 0:
            return json.dumps({
                "error": "invalid_line",
                "message": f"insert_line must be >= 0 (0 = beginning)"
            })
        if insert_line > total_lines:
            return json.dumps({
                "error": "invalid_line",
                "message": f"insert_line {insert_line} > file length ({total_lines} lines)"
            })

        # Ensure new_string ends with newline
        if new_string and not new_string.endswith("\n"):
            new_string += "\n"

        # Insert at position (0 = prepend, N = after line N)
        new_lines = lines[:insert_line] + [new_string] + lines[insert_line:]
        new_file_content = "".join(new_lines)

        resolved.write_text(new_file_content)
        self._tracker.record_read(resolved, new_file_content)

        return json.dumps({
            "success": True,
            "path": path,
            "mode": "insert",
            "changes": [{
                "line": insert_line + 1,
                "inserted": new_string.rstrip("\n")
            }]
        })

    def _find_nearest_match(self, content: str, search: str) -> str:
        """Find and describe nearest matching content for debugging."""
        search_lines = search.splitlines()
        content_lines = content.splitlines()

        if not content_lines:
            return "File is empty."

        best_ratio = 0.0
        best_line = 0

        # Slide window over content to find best match
        window_size = len(search_lines)
        for i in range(max(1, len(content_lines) - window_size + 1)):
            window = "\n".join(content_lines[i : i + window_size])
            ratio = difflib.SequenceMatcher(None, search, window).ratio()
            if ratio > best_ratio:
                best_ratio = ratio
                best_line = i + 1  # 1-indexed

        # Build debug info about the search string
        search_spaces = search.count(" ")
        search_tabs = search.count("\t")
        debug_info = (
            f"DEBUG INFO:\n"
            f"- Your search: {len(search)} chars, {len(search_lines)} line(s)\n"
            f"- Whitespace: {search_spaces} spaces, {search_tabs} tabs"
        )

        if best_ratio > 0.5:
            preview = content_lines[best_line - 1][:60]
            return (
                f"{debug_info}\n\n"
                f"NEAREST MATCH ({best_ratio * 100:.0f}% similar) at line {best_line}:\n"
                f'  "{preview}..."'
            )

        return f"{debug_info}\n\nNo similar content found. File has {len(content_lines)} lines."

    def _find_match_lines(self, content: str, search: str) -> list[int]:
        """Find line numbers where a string match starts."""
        lines = content.splitlines(keepends=True)
        match_lines = []
        pos = 0
        line_num = 1

        for line in lines:
            # Check if match starts within this line
            search_start = 0
            while True:
                idx = content.find(search, pos + search_start)
                if idx == -1 or idx >= pos + len(line):
                    break
                if idx >= pos:
                    match_lines.append(line_num)
                    break  # Only count first match per line
                search_start = idx - pos + 1

            pos += len(line)
            line_num += 1

        return match_lines


# Entry point for running as MCP server
if __name__ == "__main__":
    import sys

    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    server = FilesystemServer(root)
    asyncio.run(server.run())
