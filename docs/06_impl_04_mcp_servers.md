# Implementation 04: MCP Servers

> Built-in filesystem, bash, and git MCP servers

**Prerequisites:** Implementation 03 complete
**Estimated Time:** 3-4 hours with Claude Code
**Checkpoint:** Can execute filesystem and bash tools

---

## Objectives

1. Implement filesystem MCP server (read, write, list, search)
2. Implement bash MCP server with sandboxing
3. Implement git MCP server
4. Create server launcher infrastructure

---

## 1. Base Server Class

### File: `src/entropi/mcp/servers/base.py`

```python
"""
Base class for MCP servers.

Provides common functionality for implementing MCP servers
using the official Python SDK.
"""
import asyncio
from abc import ABC, abstractmethod
from typing import Any

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import Tool, TextContent


class BaseMCPServer(ABC):
    """Base class for MCP servers."""

    def __init__(self, name: str, version: str = "1.0.0") -> None:
        """
        Initialize server.

        Args:
            name: Server name
            version: Server version
        """
        self.name = name
        self.version = version
        self.server = Server(name)

        # Register handlers
        self._register_handlers()

    def _register_handlers(self) -> None:
        """Register MCP handlers."""

        @self.server.list_tools()
        async def list_tools() -> list[Tool]:
            return self.get_tools()

        @self.server.call_tool()
        async def call_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:
            result = await self.execute_tool(name, arguments)
            return [TextContent(type="text", text=result)]

    @abstractmethod
    def get_tools(self) -> list[Tool]:
        """Get list of available tools."""
        pass

    @abstractmethod
    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """
        Execute a tool.

        Args:
            name: Tool name
            arguments: Tool arguments

        Returns:
            Result string
        """
        pass

    async def run(self) -> None:
        """Run the server."""
        async with stdio_server() as (read_stream, write_stream):
            await self.server.run(
                read_stream,
                write_stream,
                self.server.create_initialization_options(),
            )


def create_tool(
    name: str,
    description: str,
    properties: dict[str, dict[str, Any]],
    required: list[str] | None = None,
) -> Tool:
    """
    Helper to create a Tool definition.

    Args:
        name: Tool name
        description: Tool description
        properties: Parameter properties
        required: Required parameters

    Returns:
        Tool definition
    """
    return Tool(
        name=name,
        description=description,
        inputSchema={
            "type": "object",
            "properties": properties,
            "required": required or [],
        },
    )
```

---

## 2. Filesystem Server

### File: `src/entropi/mcp/servers/filesystem.py`

```python
"""
Filesystem MCP server.

Provides file reading, writing, listing, and searching capabilities.
"""
import asyncio
import fnmatch
import os
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
                        "default": ".",
                    },
                    "recursive": {
                        "type": "boolean",
                        "description": "List recursively",
                        "default": False,
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
        try:
            if name == "read_file":
                return await self._read_file(arguments["path"])
            elif name == "write_file":
                return await self._write_file(arguments["path"], arguments["content"])
            elif name == "list_directory":
                return await self._list_directory(
                    arguments.get("path", "."),
                    arguments.get("recursive", False),
                )
            elif name == "search_files":
                return await self._search_files(
                    arguments["pattern"],
                    arguments.get("content_pattern"),
                )
            elif name == "file_exists":
                return await self._file_exists(arguments["path"])
            else:
                return f"Unknown tool: {name}"
        except Exception as e:
            return f"Error: {e}"

    def _resolve_path(self, path: str) -> Path:
        """Resolve path relative to root directory."""
        resolved = (self.root_dir / path).resolve()

        # Security: ensure path is within root
        if not str(resolved).startswith(str(self.root_dir.resolve())):
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
        content = await loop.run_in_executor(
            None,
            resolved.read_text,
        )

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

    async def _list_directory(self, path: str, recursive: bool) -> str:
        """List directory contents."""
        resolved = self._resolve_path(path)

        if not resolved.exists():
            return f"Directory not found: {path}"

        if not resolved.is_dir():
            return f"Not a directory: {path}"

        lines = []

        if recursive:
            for item in sorted(resolved.rglob("*")):
                rel_path = item.relative_to(resolved)
                # Skip hidden files and common ignore patterns
                if any(part.startswith(".") for part in rel_path.parts):
                    continue
                if "node_modules" in rel_path.parts:
                    continue
                if "__pycache__" in rel_path.parts:
                    continue

                prefix = "📁 " if item.is_dir() else "📄 "
                lines.append(f"{prefix}{rel_path}")
        else:
            for item in sorted(resolved.iterdir()):
                if item.name.startswith("."):
                    continue

                prefix = "📁 " if item.is_dir() else "📄 "
                lines.append(f"{prefix}{item.name}")

        return "\n".join(lines) if lines else "(empty directory)"

    async def _search_files(
        self,
        pattern: str,
        content_pattern: str | None,
    ) -> str:
        """Search for files."""
        import re

        matches = []

        for path in self.root_dir.rglob(pattern):
            # Skip hidden and ignored
            rel_path = path.relative_to(self.root_dir)
            if any(part.startswith(".") for part in rel_path.parts):
                continue
            if "node_modules" in rel_path.parts:
                continue

            if content_pattern and path.is_file():
                try:
                    content = path.read_text()
                    if re.search(content_pattern, content):
                        matches.append(str(rel_path))
                except Exception:
                    continue
            else:
                matches.append(str(rel_path))

        return "\n".join(matches) if matches else "No matches found"

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
```

---

## 3. Bash Server

### File: `src/entropi/mcp/servers/bash.py`

```python
"""
Bash command execution MCP server.

Provides shell command execution with configurable restrictions.
"""
import asyncio
import os
import shlex
from pathlib import Path
from typing import Any

from mcp.types import Tool

from entropi.mcp.servers.base import BaseMCPServer, create_tool


class BashServer(BaseMCPServer):
    """Bash command execution MCP server."""

    # Commands that are always blocked
    BLOCKED_COMMANDS = {
        "rm -rf /",
        "rm -rf /*",
        "dd if=/dev/zero",
        "mkfs",
        ":(){:|:&};:",  # Fork bomb
    }

    def __init__(
        self,
        working_dir: Path | None = None,
        timeout: int = 30,
    ) -> None:
        """
        Initialize bash server.

        Args:
            working_dir: Working directory for commands
            timeout: Command timeout in seconds
        """
        super().__init__("bash")
        self.working_dir = working_dir or Path.cwd()
        self.timeout = timeout

    def get_tools(self) -> list[Tool]:
        """Get available bash tools."""
        return [
            create_tool(
                name="execute",
                description="Execute a shell command",
                properties={
                    "command": {
                        "type": "string",
                        "description": "Command to execute",
                    },
                    "working_dir": {
                        "type": "string",
                        "description": "Working directory (optional)",
                    },
                },
                required=["command"],
            ),
        ]

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """Execute a bash tool."""
        if name != "execute":
            return f"Unknown tool: {name}"

        command = arguments["command"]
        working_dir = arguments.get("working_dir")

        # Security check
        if not self._is_safe_command(command):
            return f"Command blocked for security: {command}"

        return await self._execute_command(command, working_dir)

    def _is_safe_command(self, command: str) -> bool:
        """Check if command is safe to execute."""
        command_lower = command.lower().strip()

        # Check blocked commands
        for blocked in self.BLOCKED_COMMANDS:
            if blocked in command_lower:
                return False

        # Block sudo/su
        if command_lower.startswith(("sudo ", "su ")):
            return False

        return True

    async def _execute_command(
        self,
        command: str,
        working_dir: str | None,
    ) -> str:
        """Execute a shell command."""
        cwd = Path(working_dir) if working_dir else self.working_dir

        try:
            process = await asyncio.create_subprocess_shell(
                command,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                cwd=cwd,
                env={**os.environ, "TERM": "dumb"},
            )

            try:
                stdout, stderr = await asyncio.wait_for(
                    process.communicate(),
                    timeout=self.timeout,
                )
            except asyncio.TimeoutError:
                process.kill()
                return f"Command timed out after {self.timeout}s"

            output_parts = []

            if stdout:
                output_parts.append(stdout.decode("utf-8", errors="replace"))

            if stderr:
                output_parts.append(f"[stderr]\n{stderr.decode('utf-8', errors='replace')}")

            if process.returncode != 0:
                output_parts.append(f"\n[exit code: {process.returncode}]")

            return "\n".join(output_parts) if output_parts else "(no output)"

        except Exception as e:
            return f"Execution failed: {e}"


if __name__ == "__main__":
    import sys

    working_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    server = BashServer(working_dir)
    asyncio.run(server.run())
```

---

## 4. Git Server

### File: `src/entropi/mcp/servers/git.py`

```python
"""
Git operations MCP server.

Provides git status, diff, commit, log, and branch operations.
"""
import asyncio
from pathlib import Path
from typing import Any

from mcp.types import Tool

from entropi.mcp.servers.base import BaseMCPServer, create_tool


class GitServer(BaseMCPServer):
    """Git operations MCP server."""

    def __init__(self, repo_dir: Path | None = None) -> None:
        """
        Initialize git server.

        Args:
            repo_dir: Repository directory
        """
        super().__init__("git")
        self.repo_dir = repo_dir or Path.cwd()

    def get_tools(self) -> list[Tool]:
        """Get available git tools."""
        return [
            create_tool(
                name="status",
                description="Get git status",
                properties={},
            ),
            create_tool(
                name="diff",
                description="Get git diff",
                properties={
                    "staged": {
                        "type": "boolean",
                        "description": "Show staged changes only",
                        "default": False,
                    },
                    "file": {
                        "type": "string",
                        "description": "Specific file to diff",
                    },
                },
            ),
            create_tool(
                name="log",
                description="Get git log",
                properties={
                    "count": {
                        "type": "integer",
                        "description": "Number of commits",
                        "default": 10,
                    },
                    "oneline": {
                        "type": "boolean",
                        "description": "One line per commit",
                        "default": True,
                    },
                },
            ),
            create_tool(
                name="commit",
                description="Create a git commit",
                properties={
                    "message": {
                        "type": "string",
                        "description": "Commit message",
                    },
                    "add_all": {
                        "type": "boolean",
                        "description": "Stage all changes first",
                        "default": False,
                    },
                },
                required=["message"],
            ),
            create_tool(
                name="branch",
                description="List or create branches",
                properties={
                    "name": {
                        "type": "string",
                        "description": "Branch name to create (omit to list)",
                    },
                },
            ),
            create_tool(
                name="checkout",
                description="Checkout a branch or file",
                properties={
                    "target": {
                        "type": "string",
                        "description": "Branch name or file path",
                    },
                },
                required=["target"],
            ),
        ]

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """Execute a git tool."""
        try:
            if name == "status":
                return await self._git_command(["status", "--short"])
            elif name == "diff":
                return await self._diff(arguments)
            elif name == "log":
                return await self._log(arguments)
            elif name == "commit":
                return await self._commit(arguments)
            elif name == "branch":
                return await self._branch(arguments)
            elif name == "checkout":
                return await self._git_command(["checkout", arguments["target"]])
            else:
                return f"Unknown tool: {name}"
        except Exception as e:
            return f"Git error: {e}"

    async def _git_command(self, args: list[str]) -> str:
        """Run a git command."""
        process = await asyncio.create_subprocess_exec(
            "git",
            *args,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=self.repo_dir,
        )

        stdout, stderr = await process.communicate()

        if process.returncode != 0:
            return f"Error: {stderr.decode()}"

        return stdout.decode() or "(no output)"

    async def _diff(self, arguments: dict[str, Any]) -> str:
        """Get git diff."""
        args = ["diff"]

        if arguments.get("staged"):
            args.append("--staged")

        if arguments.get("file"):
            args.append(arguments["file"])

        return await self._git_command(args)

    async def _log(self, arguments: dict[str, Any]) -> str:
        """Get git log."""
        count = arguments.get("count", 10)
        args = ["log", f"-{count}"]

        if arguments.get("oneline", True):
            args.append("--oneline")

        return await self._git_command(args)

    async def _commit(self, arguments: dict[str, Any]) -> str:
        """Create a commit."""
        if arguments.get("add_all"):
            add_result = await self._git_command(["add", "-A"])
            if "Error" in add_result:
                return add_result

        return await self._git_command(["commit", "-m", arguments["message"]])

    async def _branch(self, arguments: dict[str, Any]) -> str:
        """List or create branch."""
        if arguments.get("name"):
            return await self._git_command(["checkout", "-b", arguments["name"]])
        else:
            return await self._git_command(["branch", "-a"])


if __name__ == "__main__":
    import sys

    repo_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    server = GitServer(repo_dir)
    asyncio.run(server.run())
```

---

## 5. Tests

### File: `tests/unit/test_mcp_servers.py`

```python
"""Tests for MCP servers."""
import pytest
import tempfile
from pathlib import Path

from entropi.mcp.servers.filesystem import FilesystemServer
from entropi.mcp.servers.bash import BashServer


class TestFilesystemServer:
    """Tests for filesystem server."""

    @pytest.fixture
    def server(self, tmp_path: Path) -> FilesystemServer:
        """Create server with temp directory."""
        return FilesystemServer(tmp_path)

    @pytest.mark.asyncio
    async def test_read_file(self, server: FilesystemServer, tmp_path: Path) -> None:
        """Test reading a file."""
        (tmp_path / "test.txt").write_text("hello world")
        result = await server._read_file("test.txt")
        assert result == "hello world"

    @pytest.mark.asyncio
    async def test_write_file(self, server: FilesystemServer, tmp_path: Path) -> None:
        """Test writing a file."""
        await server._write_file("new.txt", "content")
        assert (tmp_path / "new.txt").read_text() == "content"

    @pytest.mark.asyncio
    async def test_path_traversal_blocked(self, server: FilesystemServer) -> None:
        """Test that path traversal is blocked."""
        with pytest.raises(ValueError, match="outside root"):
            server._resolve_path("../../../etc/passwd")


class TestBashServer:
    """Tests for bash server."""

    def test_blocked_commands(self) -> None:
        """Test dangerous commands are blocked."""
        server = BashServer()
        assert not server._is_safe_command("rm -rf /")
        assert not server._is_safe_command("sudo rm -rf /")
        assert server._is_safe_command("ls -la")
        assert server._is_safe_command("python --version")
```

---

## Checkpoint: Verification

```bash
# Run tests
pytest tests/unit/test_mcp_servers.py -v

# Test filesystem server directly
cd ~/projects/entropi
python -c "
import asyncio
from pathlib import Path
from entropi.mcp.servers.filesystem import FilesystemServer

async def test():
    server = FilesystemServer(Path('.'))
    result = await server._list_directory('.', False)
    print(result)

asyncio.run(test())
"
```

**Success Criteria:**
- [ ] All server tests pass
- [ ] Filesystem operations work
- [ ] Path traversal is blocked
- [ ] Dangerous commands blocked

---

## Next Phase

Proceed to **Implementation 05: Agentic Loop** to implement the core agent execution loop.
