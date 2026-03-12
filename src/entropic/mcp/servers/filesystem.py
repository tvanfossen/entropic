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
import re
from pathlib import Path
from typing import TYPE_CHECKING, Any

from entropic.mcp.servers.base import BaseMCPServer
from entropic.mcp.servers.file_tracker import FileAccessTracker
from entropic.mcp.tools import BaseTool

# Directories to skip during glob/grep/list_directory traversal
_SKIP_DIRS = frozenset(
    {
        ".git",
        "node_modules",
        "__pycache__",
        ".venv",
        "venv",
        ".tox",
        ".mypy_cache",
        ".pytest_cache",
        ".ruff_cache",
        ".worktrees",
    }
)

# Max results to prevent context bloat
_MAX_GLOB_RESULTS = 500
_MAX_GREP_MATCHES = 100

# Regex to find a single brace group like {a,b,c}
_BRACE_RE = re.compile(r"\{([^{}]+)\}")


def _expand_braces(pattern: str) -> list[str]:
    """Expand brace groups in a glob pattern.

    ``pathlib.glob()`` does not support ``{a,b,c}`` syntax.  This
    expands the *first* brace group found, then recurses to handle
    nested/multiple groups.  Returns ``[pattern]`` unchanged when no
    braces are present.

    Example::

        _expand_braces("**/*.{html,js}") → ["**/*.html", "**/*.js"]
    """
    m = _BRACE_RE.search(pattern)
    if not m:
        return [pattern]
    prefix, suffix = pattern[: m.start()], pattern[m.end() :]
    alternatives = m.group(1).split(",")
    expanded: list[str] = []
    for alt in alternatives:
        expanded.extend(_expand_braces(prefix + alt + suffix))
    return expanded


if TYPE_CHECKING:
    from entropic.config.schema import FilesystemConfig
    from entropic.lsp.manager import LSPManager


def _is_searchable_file(filepath: Path, root: Path) -> bool:
    """Check if a file should be included in grep results."""
    parts = filepath.relative_to(root).parts
    if any(p in _SKIP_DIRS for p in parts):
        return False
    return filepath.is_file()


def _grep_single_file(
    filepath: Path,
    root: Path,
    compiled: re.Pattern[str],
    results: list[dict[str, Any]],
) -> None:
    """Search a single file for regex matches, appending to results."""
    try:
        content = filepath.read_text(errors="strict")
    except (UnicodeDecodeError, PermissionError):
        return
    rel_path = str(filepath.relative_to(root))
    for line_num, line in enumerate(content.splitlines(), 1):
        if compiled.search(line):
            results.append({"path": rel_path, "line": line_num, "content": line.rstrip()})
            if len(results) >= _MAX_GREP_MATCHES:
                return


class ReadFileTool(BaseTool):
    """Read file contents as structured JSON with line numbers."""

    def __init__(self, server: FilesystemServer) -> None:
        super().__init__("read_file", "filesystem")
        self._server = server

    async def execute(self, arguments: dict[str, Any]) -> str:
        return await self._server._execute_with_error_handling(
            self._server._handle_read_file, arguments
        )


class WriteFileTool(BaseTool):
    """Write file contents with read-before-write enforcement."""

    def __init__(self, server: FilesystemServer) -> None:
        super().__init__("write_file", "filesystem")
        self._server = server

    async def execute(self, arguments: dict[str, Any]) -> str:
        return await self._server._execute_with_error_handling(
            self._server._handle_write_file, arguments
        )


class EditFileTool(BaseTool):
    """Edit files via str_replace or line insertion."""

    def __init__(self, server: FilesystemServer) -> None:
        super().__init__("edit_file", "filesystem")
        self._server = server

    async def execute(self, arguments: dict[str, Any]) -> str:
        return await self._server._execute_with_error_handling(
            self._server._handle_edit_file, arguments
        )


class GlobTool(BaseTool):
    """Find files matching a glob pattern within root_dir."""

    def __init__(self, server: FilesystemServer) -> None:
        super().__init__("glob", "filesystem")
        self._server = server

    async def execute(self, arguments: dict[str, Any]) -> str:
        return await self._server._execute_with_error_handling(self._server._handle_glob, arguments)


class GrepTool(BaseTool):
    """Search file contents by regex pattern within root_dir."""

    def __init__(self, server: FilesystemServer) -> None:
        super().__init__("grep", "filesystem")
        self._server = server

    async def execute(self, arguments: dict[str, Any]) -> str:
        return await self._server._execute_with_error_handling(self._server._handle_grep, arguments)


class ListDirectoryTool(BaseTool):
    """List directory contents, optionally recursive."""

    def __init__(self, server: FilesystemServer) -> None:
        super().__init__("list_directory", "filesystem")
        self._server = server

    async def execute(self, arguments: dict[str, Any]) -> str:
        return await self._server._execute_with_error_handling(
            self._server._handle_list_directory, arguments
        )


class FilesystemServer(BaseMCPServer):
    """Filesystem operations MCP server with read-before-write enforcement."""

    def __init__(
        self,
        root_dir: Path | None = None,
        lsp_manager: LSPManager | None = None,
        config: FilesystemConfig | None = None,
        model_context_bytes: int | None = None,
    ) -> None:
        """
        Initialize filesystem server.

        Args:
            root_dir: Root directory for operations (default: cwd)
            lsp_manager: Optional LSP manager for diagnostics on edit
            config: Filesystem configuration
            model_context_bytes: Model context window in bytes (context_length * 4)
        """
        super().__init__("filesystem")
        self.root_dir = root_dir or Path.cwd()
        self._tracker = FileAccessTracker()
        self._lsp_manager = lsp_manager
        self._config = config
        self._max_read_bytes = self._compute_max_read_bytes(config, model_context_bytes)
        self.register_tool(ReadFileTool(self))
        self.register_tool(WriteFileTool(self))
        self.register_tool(EditFileTool(self))
        self.register_tool(GlobTool(self))
        self.register_tool(GrepTool(self))
        self.register_tool(ListDirectoryTool(self))

    @staticmethod
    def _compute_max_read_bytes(
        config: FilesystemConfig | None,
        model_context_bytes: int | None,
    ) -> int:
        """Compute max read bytes from config or model context.

        Priority: explicit config value > dynamic from model context > fallback 50K.
        """
        if config and config.max_read_bytes is not None:
            return config.max_read_bytes
        pct = config.max_read_context_pct if config else 0.25
        context_bytes = model_context_bytes or 50_000
        return max(1_000, int(context_bytes * pct))

    @staticmethod
    def skip_duplicate_check(tool_name: str) -> bool:
        """read_file must always execute — updates FileAccessTracker."""
        return tool_name == "read_file"

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """Execute a filesystem tool with JSON error format for unknown tools."""
        if not self._tool_registry.has_tool(name):
            return self._error_response("unknown_tool", f"Unknown tool: {name}")
        return await self._tool_registry.dispatch(name, arguments)

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

    async def _handle_glob(self, args: dict[str, Any]) -> str:
        """Find files matching a glob pattern within root_dir."""
        pattern = args["pattern"]
        root = self.root_dir.resolve()

        loop = asyncio.get_event_loop()
        matches = await loop.run_in_executor(None, lambda: self._glob_walk(root, pattern))

        return json.dumps(
            {
                "pattern": pattern,
                "matches": matches[:_MAX_GLOB_RESULTS],
                "total": len(matches),
                "truncated": len(matches) > _MAX_GLOB_RESULTS,
            }
        )

    @staticmethod
    def _glob_walk(root: Path, pattern: str) -> list[str]:
        """Run glob and filter results, skipping ignored directories.

        Supports brace expansion (e.g. ``**/*.{html,js,css}``) which
        ``pathlib.glob()`` does not handle natively.  Braces are expanded
        into multiple patterns that are each globbed separately.
        """
        seen: set[str] = set()
        results: list[str] = []
        for expanded in _expand_braces(pattern):
            for match in root.glob(expanded):
                parts = match.relative_to(root).parts
                if any(p in _SKIP_DIRS for p in parts):
                    continue
                if match.is_file():
                    rel = str(match.relative_to(root))
                    if rel not in seen:
                        seen.add(rel)
                        results.append(rel)
        results.sort()
        return results

    async def _handle_grep(self, args: dict[str, Any]) -> str:
        """Search file contents for a regex pattern."""
        pattern = args["pattern"]
        file_glob = args.get("glob", "**/*")

        try:
            compiled = re.compile(pattern)
        except re.error as e:
            return self._error_response("invalid_regex", f"Invalid regex: {e}")

        root = self.root_dir.resolve()

        loop = asyncio.get_event_loop()
        matches = await loop.run_in_executor(
            None, lambda: self._grep_walk(root, compiled, file_glob)
        )

        return json.dumps(
            {
                "pattern": pattern,
                "matches": matches[:_MAX_GREP_MATCHES],
                "total": len(matches),
                "truncated": len(matches) > _MAX_GREP_MATCHES,
            }
        )

    @staticmethod
    def _grep_walk(root: Path, compiled: re.Pattern[str], file_glob: str) -> list[dict[str, Any]]:
        """Walk files matching glob and search for regex matches."""
        results: list[dict[str, Any]] = []
        for filepath in sorted(root.glob(file_glob)):
            if not _is_searchable_file(filepath, root):
                continue
            _grep_single_file(filepath, root, compiled, results)
            if len(results) >= _MAX_GREP_MATCHES:
                break
        return results

    async def _handle_list_directory(self, args: dict[str, Any]) -> str:
        """List directory contents, optionally recursive."""
        path_str = args.get("path", ".")
        recursive = args.get("recursive", False)
        max_depth = args.get("max_depth", 3)

        resolved = self._resolve_path(path_str)
        if not resolved.is_dir():
            return self._error_response("not_a_directory", f"Not a directory: {path_str}")

        loop = asyncio.get_event_loop()
        entries = await loop.run_in_executor(
            None,
            lambda: self._list_entries(resolved, recursive, max_depth),
        )

        return json.dumps(
            {
                "path": path_str,
                "entries": entries,
                "total": len(entries),
            }
        )

    def _list_entries(
        self, directory: Path, recursive: bool, max_depth: int
    ) -> list[dict[str, Any]]:
        """List directory entries, optionally recursive with depth limit."""
        root = self.root_dir.resolve()
        entries: list[dict[str, Any]] = []

        if recursive:
            self._walk_recursive(directory, root, entries, 0, max_depth)
        else:
            self._walk_flat(directory, root, entries)

        return entries

    def _walk_flat(self, directory: Path, root: Path, entries: list[dict[str, Any]]) -> None:
        """List immediate children of a directory."""
        for entry in sorted(directory.iterdir()):
            if entry.name in _SKIP_DIRS:
                continue
            info = self._entry_info(entry, root)
            entries.append(info)

    def _walk_recursive(
        self,
        directory: Path,
        root: Path,
        entries: list[dict[str, Any]],
        depth: int,
        max_depth: int,
    ) -> None:
        """Recursively list directory contents with depth limit."""
        if depth > max_depth:
            return
        for entry in sorted(directory.iterdir()):
            if entry.name in _SKIP_DIRS:
                continue
            info = self._entry_info(entry, root)
            entries.append(info)
            if entry.is_dir():
                self._walk_recursive(entry, root, entries, depth + 1, max_depth)

    @staticmethod
    def _entry_info(entry: Path, root: Path) -> dict[str, Any]:
        """Build entry info dict for a single path."""
        rel = str(entry.relative_to(root))
        if entry.is_dir():
            return {"name": rel + "/", "type": "dir"}
        try:
            size = entry.stat().st_size
        except OSError:
            size = 0
        return {"name": rel, "type": "file", "size": size}

    def _resolve_path(self, path: str) -> Path:
        """Resolve path relative to root directory, expanding ~ to home."""
        # Expand tilde to home directory
        if path.startswith("~"):
            path = os.path.expanduser(path)
        resolved = (self.root_dir / path).resolve()

        # Security: ensure path is within root (unless config allows outside)
        allow_outside = self._config.allow_outside_root if self._config else False
        if not allow_outside:
            root_resolved = self.root_dir.resolve()
            if not resolved.is_relative_to(root_resolved):
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
        if file_bytes > self._max_read_bytes:
            est_tokens = file_bytes // 4
            return json.dumps(
                {
                    "blocked": True,
                    "reason": (
                        f"File '{path}' is {file_bytes:,} bytes (~{est_tokens:,} tokens) "
                        f"which exceeds the max read size ({self._max_read_bytes:,} bytes)."
                    ),
                    "suggestion": (
                        "Read specific sections with bash: "
                        "grep -n 'pattern' to find lines, "
                        "head -n 100 / tail -n 100 for sections. "
                        "Or read smaller related files instead."
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
