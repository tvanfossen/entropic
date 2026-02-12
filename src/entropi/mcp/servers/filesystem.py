"""
Filesystem MCP server.

Provides file reading, writing, and editing capabilities.
Implements read-before-write safety pattern.
"""

from __future__ import annotations

import asyncio
import difflib
import json
import os
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp.types import Tool

from entropi.mcp.servers.base import BaseMCPServer, load_tool_definition
from entropi.mcp.servers.file_tracker import FileAccessTracker

if TYPE_CHECKING:
    from entropi.config.schema import FilesystemConfig
    from entropi.lsp.manager import LSPManager


class FilesystemServer(BaseMCPServer):
    """Filesystem operations MCP server with read-before-write enforcement."""

    def __init__(
        self,
        root_dir: Path | None = None,
        lsp_manager: LSPManager | None = None,
        config: FilesystemConfig | None = None,
    ) -> None:
        """
        Initialize filesystem server.

        Args:
            root_dir: Root directory for operations (default: cwd)
            lsp_manager: Optional LSP manager for diagnostics on edit
            config: Filesystem configuration
        """
        super().__init__("filesystem")
        self.root_dir = root_dir or Path.cwd()
        self._tracker = FileAccessTracker()
        self._lsp_manager = lsp_manager
        self._config = config

    def get_tools(self) -> list[Tool]:
        """Get available filesystem tools - unified read/edit/write pattern."""
        return [
            load_tool_definition("read_file", "filesystem"),
            load_tool_definition("edit_file", "filesystem"),
            load_tool_definition("write_file", "filesystem"),
        ]

    @staticmethod
    def skip_duplicate_check(tool_name: str) -> bool:
        """read_file must always execute — updates FileAccessTracker."""
        return tool_name == "read_file"

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """Execute a filesystem tool with JSON schema validation."""
        handlers = {
            "read_file": self._handle_read_file,
            "write_file": self._handle_write_file,
            "edit_file": self._handle_edit_file,
        }
        handler = handlers.get(name)
        if not handler:
            return self._error_response("unknown_tool", f"Unknown tool: {name}")
        return await self._execute_with_error_handling(handler, arguments)

    async def _execute_with_error_handling(self, handler: Any, arguments: dict[str, Any]) -> str:
        """Execute handler with unified error handling."""
        error_map = {
            KeyError: ("missing_param", "Missing required parameter: {}"),
            TypeError: ("invalid_type", "Invalid parameter type: {}"),
        }
        try:
            return await handler(arguments)
        except (KeyError, TypeError) as e:
            error_type, msg_template = error_map[type(e)]
            return self._error_response(error_type, msg_template.format(e))
        except Exception as e:
            return self._error_response("execution_error", str(e))

    def _error_response(self, error_type: str, message: str) -> str:
        """Create a JSON error response."""
        return json.dumps({"error": error_type, "message": message})

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
            return json.dumps(
                {
                    "error": "invalid_params",
                    "message": "edit_file requires either old_string (str_replace) or insert_line (insert)",
                }
            )

        return await self._str_replace(
            path,
            args["old_string"],
            new_string,
            args.get("replace_all", False),
        )

    def _resolve_path(self, path: str) -> Path:
        """Resolve path relative to root directory, expanding ~ to home."""
        # Expand tilde to home directory
        if path.startswith("~"):
            path = os.path.expanduser(path)
        resolved = (self.root_dir / path).resolve()

        # Security: ensure path is within root
        root_resolved = self.root_dir.resolve()
        if not str(resolved).startswith(str(root_resolved)):
            raise ValueError(f"Path outside root directory: {path}")

        return resolved

    def _supports_diagnostics(self, path: Path) -> bool:
        """Check if file type supports LSP diagnostics."""
        if not self._lsp_manager:
            return False
        return self._lsp_manager.supports_file(path)

    def _should_check_diagnostics(self, resolved: Path) -> bool:
        """Check if diagnostics should be checked for this file."""
        if not self._config or not self._config.diagnostics_on_edit:
            return False
        if not self._lsp_manager or not self._lsp_manager.is_enabled:
            return False
        return self._supports_diagnostics(resolved)

    async def _check_diagnostics_after_edit(
        self,
        resolved: Path,
        original_content: str,
        result_data: dict[str, Any],
    ) -> str:
        """
        Check diagnostics after an edit and rollback if errors found.

        Args:
            resolved: Path to the edited file
            original_content: Content before the edit (for rollback)
            result_data: Success result data to augment with diagnostics

        Returns:
            JSON response (success with diagnostics, or error with rollback)
        """
        # Skip if diagnostics not configured or file type not supported
        if not self._should_check_diagnostics(resolved):
            return json.dumps(result_data)

        # Open file in LSP and wait for diagnostics
        self._lsp_manager.open_file(resolved)
        timeout = self._config.diagnostics_timeout if self._config else 1.0
        diagnostics = await self._lsp_manager.wait_for_diagnostics(resolved, timeout=timeout)

        # Format diagnostics for response
        diag_list = [
            {
                "line": d.line,
                "severity": d.severity,
                "message": d.message,
                "source": d.source,
            }
            for d in diagnostics
        ]

        # Check for errors
        errors = [d for d in diagnostics if d.is_error]

        if errors and self._config and self._config.fail_on_errors:
            # Rollback: restore original content or delete new file
            if original_content:
                resolved.write_text(original_content)
                self._tracker.record_read(resolved, original_content)
            else:
                # New file - delete it
                resolved.unlink(missing_ok=True)

            return json.dumps(
                {
                    "error": "diagnostics_failed",
                    "message": f"Edit introduced {len(errors)} error(s) - rolled back",
                    "diagnostics": diag_list,
                    "tip": "Fix the errors in your edit and try again",
                }
            )

        # Success - include diagnostics in response
        if diag_list:
            result_data["diagnostics"] = diag_list

        return json.dumps(result_data)

    async def _read_file(self, path: str) -> str:
        """Read file contents as structured JSON with line numbers."""
        resolved = self._resolve_path(path)

        if not resolved.exists() or not resolved.is_file():
            kind = "not_found" if not resolved.exists() else "not_a_file"
            label = "File not found" if kind == "not_found" else "Not a file"
            return json.dumps({"error": kind, "message": f"{label}: {path}"})

        # Size gate: block reads that would blow the context window
        file_bytes = resolved.stat().st_size
        max_bytes = self._config.max_read_bytes if self._config else 50_000
        if file_bytes > max_bytes:
            est_tokens = file_bytes // 4
            return json.dumps(
                {
                    "blocked": True,
                    "reason": (
                        f"File '{path}' is {file_bytes:,} bytes (~{est_tokens:,} tokens) "
                        f"which exceeds the max read size ({max_bytes:,} bytes)."
                    ),
                    "suggestion": (
                        "Capture current findings with entropi.todo_write, "
                        "then call entropi.prune_context to free space before reading."
                    ),
                }
            )

        # Run in executor for non-blocking I/O
        loop = asyncio.get_event_loop()
        content = await loop.run_in_executor(None, resolved.read_text)

        # Record the ORIGINAL content for write/edit validation
        self._tracker.record_read(resolved, content)

        # Return structured JSON with line numbers as keys
        lines = content.splitlines()
        return json.dumps(
            {
                "path": path,
                "total": len(lines),
                "bytes": file_bytes,
                "lines": {str(i): line for i, line in enumerate(lines, 1)},
            }
        )

    async def _write_file(self, path: str, content: str) -> str:
        """Write file contents with read-before-write enforcement."""
        resolved = self._resolve_path(path)

        # Save original content for potential rollback
        original_content = ""
        is_new_file = not resolved.exists()

        # For existing files, require prior read
        if resolved.exists():
            if not self._tracker.was_read_recently(resolved):
                return json.dumps(
                    {
                        "error": "read_required",
                        "message": f"Cannot write to {path}: file must be read first.",
                    }
                )

            # Verify file hasn't changed externally
            original_content = resolved.read_text()
            if not self._tracker.verify_unchanged(resolved, original_content):
                return json.dumps(
                    {
                        "error": "file_changed",
                        "message": f"File {path} modified since read. Read again first.",
                    }
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

        # Check diagnostics (may rollback on errors)
        result_data = {"success": True, "bytes_written": len(content), "path": path}

        # For new files, rollback means delete
        if is_new_file:
            original_content = ""  # Signal to delete on rollback

        return await self._check_diagnostics_after_edit(resolved, original_content, result_data)

    def _validate_str_replace(
        self, path: str, resolved: Path, old_string: str, replace_all: bool
    ) -> tuple[str | None, str]:
        """Validate str_replace preconditions. Returns (error_json, content) or (None, content)."""
        # Check file exists and was read
        error = self._validate_file_read_state(path, resolved)
        if error:
            return error, ""

        content = resolved.read_text()

        # Check file unchanged since read
        if not self._tracker.verify_unchanged(resolved, content):
            return self._error_response(
                "file_changed", f"File {path} modified since read. Read again first."
            ), ""

        # Check match conditions
        return self._validate_match_conditions(path, content, old_string, replace_all)

    def _validate_file_read_state(self, path: str, resolved: Path) -> str | None:
        """Validate file exists and was read recently. Returns error JSON or None."""
        if not resolved.exists():
            return self._error_response("not_found", f"File not found: {path}")
        if not self._tracker.was_read_recently(resolved):
            return self._error_response(
                "read_required", f"Cannot edit {path}: file must be read first."
            )
        return None

    def _validate_line_bounds(
        self, insert_line: int, total_lines: int, content: str
    ) -> tuple[str | None, str, int]:
        """Validate line number bounds. Returns (error_json, content, total_lines)."""
        if insert_line < 0:
            return (
                self._error_response("invalid_line", "insert_line must be >= 0 (0 = beginning)"),
                "",
                0,
            )
        if insert_line > total_lines:
            return (
                self._error_response(
                    "invalid_line", f"insert_line {insert_line} > file length ({total_lines} lines)"
                ),
                "",
                0,
            )
        return None, content, total_lines

    def _validate_match_conditions(
        self, path: str, content: str, old_string: str, replace_all: bool
    ) -> tuple[str | None, str]:
        """Validate match conditions. Returns (error_json, content) or (None, content)."""
        match_count = content.count(old_string)

        if match_count == 0:
            hint = self._find_nearest_match(content, old_string)
            return json.dumps(
                {
                    "error": "no_match",
                    "message": f"String not found in {path}",
                    "debug": hint,
                    "tip": "Use edit_file with insert_line for line-based insertion",
                }
            ), ""

        if not replace_all and match_count > 1:
            match_lines = self._find_match_lines(content, old_string)
            return json.dumps(
                {
                    "error": "multiple_matches",
                    "message": f"Found {match_count} matches in {path}",
                    "match_lines": match_lines[:10],
                    "tip": "Add more context to old_string, or set replace_all=true",
                }
            ), ""

        return None, content

    async def _str_replace(
        self,
        path: str,
        old_string: str,
        new_string: str,
        replace_all: bool = False,
    ) -> str:
        """Edit a file by replacing exact string matches (str_replace mode)."""
        resolved = self._resolve_path(path)

        error, content = self._validate_str_replace(path, resolved, old_string, replace_all)
        if error:
            return error

        # Collect changes and perform replacement
        changes = self._collect_changes(content, old_string, new_string, replace_all)
        new_content = (
            content.replace(old_string, new_string)
            if replace_all
            else content.replace(old_string, new_string, 1)
        )

        # Write and update tracker
        resolved.write_text(new_content)
        self._tracker.record_read(resolved, new_content)

        # Check diagnostics (may rollback on errors)
        result_data = {"success": True, "path": path, "mode": "str_replace", "changes": changes}
        return await self._check_diagnostics_after_edit(resolved, content, result_data)

    def _collect_changes(
        self, content: str, old_string: str, new_string: str, replace_all: bool
    ) -> list[dict[str, Any]]:
        """Collect change info for str_replace operation."""
        lines = content.splitlines(keepends=True)
        line_starts = self._build_line_starts(lines)
        changes: list[dict[str, Any]] = []

        search_pos = 0
        while True:
            idx = content.find(old_string, search_pos)
            if idx == -1:
                break
            line_num = self._find_line_number(idx, line_starts, len(lines))
            changes.append({"line": line_num, "before": old_string, "after": new_string})
            if not replace_all:
                break
            search_pos = idx + len(old_string)
        return changes

    def _build_line_starts(self, lines: list[str]) -> list[int]:
        """Build line start positions for line number mapping."""
        line_starts = [0]
        for line in lines:
            line_starts.append(line_starts[-1] + len(line))
        return line_starts

    def _find_line_number(self, pos: int, line_starts: list[int], num_lines: int) -> int:
        """Find 1-indexed line number for a character position."""
        for i, start in enumerate(line_starts[:-1]):
            if start <= pos < line_starts[i + 1]:
                return i + 1
        return num_lines

    def _validate_insert_at_line(
        self, path: str, resolved: Path, insert_line: int
    ) -> tuple[str | None, str, int]:
        """Validate insert_at_line preconditions. Returns (error_json, content, total_lines)."""
        # Check file exists and was read
        error = self._validate_file_read_state(path, resolved)
        if error:
            return error, "", 0

        content = resolved.read_text()

        # Check file unchanged since read
        if not self._tracker.verify_unchanged(resolved, content):
            return (
                self._error_response(
                    "file_changed", f"File {path} modified since read. Read again first."
                ),
                "",
                0,
            )

        # Validate line number
        total_lines = len(content.splitlines(keepends=True))
        return self._validate_line_bounds(insert_line, total_lines, content)

    async def _insert_at_line(
        self,
        path: str,
        insert_line: int,
        new_string: str,
    ) -> str:
        """Insert content at a specific line number (insert mode)."""
        resolved = self._resolve_path(path)

        error, content, _ = self._validate_insert_at_line(path, resolved, insert_line)
        if error:
            return error

        lines = content.splitlines(keepends=True)

        # Ensure new_string ends with newline
        if new_string and not new_string.endswith("\n"):
            new_string += "\n"

        # Insert at position (0 = prepend, N = after line N)
        new_lines = lines[:insert_line] + [new_string] + lines[insert_line:]
        new_file_content = "".join(new_lines)

        resolved.write_text(new_file_content)
        self._tracker.record_read(resolved, new_file_content)

        # Check diagnostics (may rollback on errors)
        result_data = {
            "success": True,
            "path": path,
            "mode": "insert",
            "changes": [{"line": insert_line + 1, "inserted": new_string.rstrip("\n")}],
        }
        return await self._check_diagnostics_after_edit(resolved, content, result_data)

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

    from entropi.config.loader import get_config
    from entropi.lsp.manager import LSPManager as _LSPManager

    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()

    # Load config for filesystem settings
    config = get_config()
    fs_config = config.mcp.filesystem

    # Initialize LSP manager if diagnostics enabled
    lsp_manager = None
    if fs_config.diagnostics_on_edit and config.lsp.enabled:
        lsp_manager = _LSPManager(config.lsp, root)
        lsp_manager.start()

    try:
        server = FilesystemServer(root, lsp_manager=lsp_manager, config=fs_config)
        asyncio.run(server.run())
    finally:
        if lsp_manager:
            lsp_manager.stop()
