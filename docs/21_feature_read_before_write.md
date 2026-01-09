# Doc 21: Read-Before-Write Filesystem Safety Pattern

**Status:** Implemented
**Priority:** High (prevents data loss)
**Complexity:** Low-Medium

## Implementation Status

| Component | Status | Location |
|-----------|--------|----------|
| FileAccessRecord | Done | `src/entropi/mcp/servers/file_tracker.py` |
| FileAccessTracker | Done | `src/entropi/mcp/servers/file_tracker.py` |
| edit_file tool | Done | `src/entropi/mcp/servers/filesystem.py` |
| read_file tracking | Done | `src/entropi/mcp/servers/filesystem.py` |
| write_file enforcement | Done | `src/entropi/mcp/servers/filesystem.py` |

---

## Overview

Implement a safety pattern for filesystem tools that requires reading a file before writing or editing it. This pattern prevents blind modifications, ensures the model has current context, and enables precise content-based edits rather than full file overwrites.

## Motivation

When an AI model modifies files without first reading them:
- It may overwrite content it didn't intend to change
- It cannot verify the file's current state matches expectations
- It may edit the wrong location in a file
- Race conditions can cause data loss if file changed externally

The Read-before-Write pattern addresses these issues by:
1. Requiring a recent read before any write/edit operation
2. Using exact string matching for edits (not line numbers which can shift)
3. Optionally validating file hasn't changed between read and write

## Design

### File Access Tracking

```python
from dataclasses import dataclass, field
from datetime import datetime, timedelta
from hashlib import sha256
from pathlib import Path


@dataclass
class FileAccessRecord:
    """Record of a file read operation."""

    path: Path
    content_hash: str
    read_at: datetime
    content_preview: str  # First 200 chars for debugging

    def is_stale(self, max_age: timedelta = timedelta(minutes=5)) -> bool:
        """Check if this read is too old to trust."""
        return datetime.utcnow() - self.read_at > max_age


class FileAccessTracker:
    """
    Tracks file reads to enforce read-before-write.

    Maintains a record of recently read files with their
    content hashes to detect external modifications.
    """

    def __init__(self, max_age_minutes: int = 5) -> None:
        self.max_age = timedelta(minutes=max_age_minutes)
        self._records: dict[Path, FileAccessRecord] = {}

    def record_read(self, path: Path, content: str) -> None:
        """Record that a file was read."""
        self._records[path.resolve()] = FileAccessRecord(
            path=path.resolve(),
            content_hash=sha256(content.encode()).hexdigest(),
            read_at=datetime.utcnow(),
            content_preview=content[:200],
        )

    def get_record(self, path: Path) -> FileAccessRecord | None:
        """Get the read record for a file."""
        record = self._records.get(path.resolve())
        if record and record.is_stale(self.max_age):
            # Clean up stale record
            del self._records[path.resolve()]
            return None
        return record

    def was_read_recently(self, path: Path) -> bool:
        """Check if file was read recently."""
        return self.get_record(path) is not None

    def verify_unchanged(self, path: Path, current_content: str) -> bool:
        """Verify file content matches what was read."""
        record = self.get_record(path)
        if not record:
            return False
        current_hash = sha256(current_content.encode()).hexdigest()
        return record.content_hash == current_hash

    def clear(self, path: Path | None = None) -> None:
        """Clear records for a path or all paths."""
        if path:
            self._records.pop(path.resolve(), None)
        else:
            self._records.clear()
```

### Enhanced Filesystem Tools

```python
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass
class ToolResult:
    """Result of a tool execution."""
    success: bool
    result: Any
    error: str | None = None


class FilesystemTools:
    """
    Filesystem tools with read-before-write enforcement.

    All write operations require a prior read of the target file.
    Edit operations use exact string matching for precision.
    """

    def __init__(self) -> None:
        self._tracker = FileAccessTracker()

    async def read_file(self, path: str) -> ToolResult:
        """
        Read a file's contents.

        Records the read for future write/edit validation.
        """
        file_path = Path(path).resolve()

        if not file_path.exists():
            return ToolResult(
                success=False,
                result=None,
                error=f"File not found: {path}",
            )

        if not file_path.is_file():
            return ToolResult(
                success=False,
                result=None,
                error=f"Not a file: {path}",
            )

        try:
            content = file_path.read_text()
            self._tracker.record_read(file_path, content)

            return ToolResult(
                success=True,
                result={
                    "path": str(file_path),
                    "content": content,
                    "size": len(content),
                    "lines": content.count("\n") + 1,
                },
            )
        except Exception as e:
            return ToolResult(
                success=False,
                result=None,
                error=f"Failed to read file: {e}",
            )

    async def write_file(self, path: str, content: str) -> ToolResult:
        """
        Write content to a file (full replacement).

        Requires the file to have been read first if it exists.
        New files can be created without prior read.
        """
        file_path = Path(path).resolve()

        # For existing files, require prior read
        if file_path.exists():
            if not self._tracker.was_read_recently(file_path):
                return ToolResult(
                    success=False,
                    result=None,
                    error=(
                        f"Cannot write to {path}: file must be read first. "
                        "Use filesystem.read_file before writing to existing files."
                    ),
                )

            # Optionally verify file hasn't changed
            current_content = file_path.read_text()
            if not self._tracker.verify_unchanged(file_path, current_content):
                return ToolResult(
                    success=False,
                    result=None,
                    error=(
                        f"File {path} has been modified since it was read. "
                        "Read the file again to get current content."
                    ),
                )

        try:
            # Create parent directories if needed
            file_path.parent.mkdir(parents=True, exist_ok=True)
            file_path.write_text(content)

            # Record the new content as read
            self._tracker.record_read(file_path, content)

            return ToolResult(
                success=True,
                result={
                    "path": str(file_path),
                    "bytes_written": len(content.encode()),
                },
            )
        except Exception as e:
            return ToolResult(
                success=False,
                result=None,
                error=f"Failed to write file: {e}",
            )

    async def edit_file(
        self,
        path: str,
        old_string: str,
        new_string: str,
        replace_all: bool = False,
    ) -> ToolResult:
        """
        Edit a file by replacing exact string matches.

        This is the preferred method for modifications as it:
        - Requires precise knowledge of current content
        - Fails safely if content doesn't match
        - Preserves all other file content exactly

        Args:
            path: File path
            old_string: Exact string to find and replace
            new_string: Replacement string
            replace_all: If True, replace all occurrences; if False, require unique match
        """
        file_path = Path(path).resolve()

        if not file_path.exists():
            return ToolResult(
                success=False,
                result=None,
                error=f"File not found: {path}",
            )

        # Require prior read
        if not self._tracker.was_read_recently(file_path):
            return ToolResult(
                success=False,
                result=None,
                error=(
                    f"Cannot edit {path}: file must be read first. "
                    "Use filesystem.read_file before editing."
                ),
            )

        try:
            content = file_path.read_text()

            # Verify file hasn't changed
            if not self._tracker.verify_unchanged(file_path, content):
                return ToolResult(
                    success=False,
                    result=None,
                    error=(
                        f"File {path} has been modified since it was read. "
                        "Read the file again to get current content."
                    ),
                )

            # Check for matches
            match_count = content.count(old_string)

            if match_count == 0:
                return ToolResult(
                    success=False,
                    result=None,
                    error=(
                        f"String not found in {path}. "
                        "The file content may have changed or the search string is incorrect."
                    ),
                )

            if not replace_all and match_count > 1:
                return ToolResult(
                    success=False,
                    result=None,
                    error=(
                        f"Found {match_count} matches in {path}. "
                        "Provide more context to uniquely identify the location, "
                        "or set replace_all=true to replace all occurrences."
                    ),
                )

            # Perform replacement
            if replace_all:
                new_content = content.replace(old_string, new_string)
                replacements = match_count
            else:
                new_content = content.replace(old_string, new_string, 1)
                replacements = 1

            # Write and update tracker
            file_path.write_text(new_content)
            self._tracker.record_read(file_path, new_content)

            return ToolResult(
                success=True,
                result={
                    "path": str(file_path),
                    "replacements": replacements,
                },
            )

        except Exception as e:
            return ToolResult(
                success=False,
                result=None,
                error=f"Failed to edit file: {e}",
            )
```

### MCP Tool Definitions

```python
FILESYSTEM_TOOLS = [
    {
        "name": "filesystem.read_file",
        "description": """Read a file's contents.

You must read a file before you can edit or write to it.
This ensures you have the current content and can make precise edits.

Returns the file content along with metadata (size, line count).""",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Path to the file to read",
                },
            },
            "required": ["path"],
        },
    },
    {
        "name": "filesystem.write_file",
        "description": """Write content to a file (full replacement).

IMPORTANT: If the file already exists, you MUST read it first using
filesystem.read_file. This prevents accidental overwrites.

For modifying existing files, prefer filesystem.edit_file which uses
precise string replacement instead of full file replacement.""",
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Path to the file to write",
                },
                "content": {
                    "type": "string",
                    "description": "Content to write to the file",
                },
            },
            "required": ["path", "content"],
        },
    },
    {
        "name": "filesystem.edit_file",
        "description": """Edit a file by replacing exact string matches.

This is the PREFERRED method for modifying files because:
- It requires you to specify exactly what to change
- It fails safely if the content doesn't match
- It preserves all other content exactly

REQUIREMENTS:
1. You must read the file first with filesystem.read_file
2. The old_string must match EXACTLY (including whitespace/indentation)
3. If old_string appears multiple times, provide more context to make it unique
   OR set replace_all=true to replace all occurrences

TIPS:
- Include surrounding lines for unique context
- Preserve exact indentation from the file
- Use replace_all for renaming variables/functions across a file""",
        "inputSchema": {
            "type": "object",
            "properties": {
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
                    "description": "Replace all occurrences (default: false, requires unique match)",
                    "default": False,
                },
            },
            "required": ["path", "old_string", "new_string"],
        },
    },
]
```

## Integration with MCP Server

```python
class FilesystemMCPServer:
    """MCP server providing filesystem tools with safety enforcement."""

    def __init__(self) -> None:
        self._tools = FilesystemTools()

    def get_tools(self) -> list[dict]:
        """Return tool definitions."""
        return FILESYSTEM_TOOLS

    async def execute(self, name: str, arguments: dict) -> ToolResult:
        """Execute a filesystem tool."""
        handlers = {
            "filesystem.read_file": self._handle_read,
            "filesystem.write_file": self._handle_write,
            "filesystem.edit_file": self._handle_edit,
        }

        handler = handlers.get(name)
        if not handler:
            return ToolResult(
                success=False,
                result=None,
                error=f"Unknown tool: {name}",
            )

        return await handler(arguments)

    async def _handle_read(self, args: dict) -> ToolResult:
        return await self._tools.read_file(args["path"])

    async def _handle_write(self, args: dict) -> ToolResult:
        return await self._tools.write_file(args["path"], args["content"])

    async def _handle_edit(self, args: dict) -> ToolResult:
        return await self._tools.edit_file(
            path=args["path"],
            old_string=args["old_string"],
            new_string=args["new_string"],
            replace_all=args.get("replace_all", False),
        )
```

## System Prompt Guidance

Add to the model's system prompt:

```
## File Editing Guidelines

When modifying files:

1. **Always read before writing** - Use `filesystem.read_file` before any edit or write operation
2. **Prefer edit over write** - Use `filesystem.edit_file` for modifications instead of rewriting entire files
3. **Be precise with edits** - Include enough surrounding context in old_string to uniquely identify the location
4. **Preserve formatting** - Match the exact indentation and whitespace from the file

Example workflow:
1. Read: `filesystem.read_file` to see current content
2. Edit: `filesystem.edit_file` with exact old_string and new_string
3. Verify: `filesystem.read_file` again if needed to confirm changes

NEVER guess file contents - always read first.
```

## Benefits

1. **Prevents blind modifications** - Model must see current content before changing
2. **Precise edits** - String matching is more reliable than line numbers
3. **Race condition detection** - Hash verification catches external changes
4. **Clear error messages** - Guides model to correct behavior
5. **Audit trail** - Track what was read and when

## Testing

```python
import pytest
from pathlib import Path
import tempfile


@pytest.fixture
def tools():
    return FilesystemTools()


@pytest.fixture
def temp_file():
    with tempfile.NamedTemporaryFile(mode="w", delete=False, suffix=".txt") as f:
        f.write("Hello, World!")
        return Path(f.name)


async def test_write_requires_read(tools, temp_file):
    """Writing to existing file without reading should fail."""
    result = await tools.write_file(str(temp_file), "New content")
    assert not result.success
    assert "must be read first" in result.error


async def test_write_after_read(tools, temp_file):
    """Writing after reading should succeed."""
    await tools.read_file(str(temp_file))
    result = await tools.write_file(str(temp_file), "New content")
    assert result.success


async def test_edit_unique_match(tools, temp_file):
    """Edit with unique match should succeed."""
    await tools.read_file(str(temp_file))
    result = await tools.edit_file(str(temp_file), "World", "Universe")
    assert result.success
    assert temp_file.read_text() == "Hello, Universe!"


async def test_edit_multiple_matches_fails(tools):
    """Edit with multiple matches should fail without replace_all."""
    with tempfile.NamedTemporaryFile(mode="w", delete=False) as f:
        f.write("foo bar foo")
        path = f.name

    tools = FilesystemTools()
    await tools.read_file(path)
    result = await tools.edit_file(path, "foo", "baz")
    assert not result.success
    assert "2 matches" in result.error


async def test_edit_replace_all(tools):
    """Edit with replace_all should replace all occurrences."""
    with tempfile.NamedTemporaryFile(mode="w", delete=False) as f:
        f.write("foo bar foo")
        path = f.name

    tools = FilesystemTools()
    await tools.read_file(path)
    result = await tools.edit_file(path, "foo", "baz", replace_all=True)
    assert result.success
    assert Path(path).read_text() == "baz bar baz"


async def test_external_modification_detected(tools, temp_file):
    """External file modification should be detected."""
    await tools.read_file(str(temp_file))

    # Simulate external modification
    temp_file.write_text("Modified externally")

    result = await tools.edit_file(str(temp_file), "Hello", "Hi")
    assert not result.success
    assert "modified since" in result.error
```

## Implementation Priority

This pattern should be implemented as part of the core filesystem MCP server before the specialized Python and C servers (docs 19, 20), as those servers will build upon these primitives.

## Dependencies

- None (uses standard library only)
- Should be integrated with existing MCP server infrastructure
