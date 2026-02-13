"""Tests for MCP servers."""

from pathlib import Path

import pytest
from entropi.mcp.servers.bash import BashServer
from entropi.mcp.servers.filesystem import FilesystemServer


class TestFilesystemServer:
    """Tests for filesystem server."""

    @pytest.fixture
    def server(self, tmp_path: Path) -> FilesystemServer:
        """Create server with temp directory."""
        return FilesystemServer(tmp_path)

    @pytest.mark.asyncio
    async def test_read_file(self, server: FilesystemServer, tmp_path: Path) -> None:
        """Test reading a file returns JSON with line numbers."""
        (tmp_path / "test.txt").write_text("hello world")
        result = await server._read_file("test.txt")
        # Result is JSON with line-numbered content
        import json

        data = json.loads(result)
        assert data["path"] == "test.txt"
        assert data["lines"]["1"] == "hello world"

    @pytest.mark.asyncio
    async def test_read_file_not_found(self, server: FilesystemServer) -> None:
        """Test reading non-existent file."""
        result = await server._read_file("nonexistent.txt")
        assert "not_found" in result.lower() or "not found" in result.lower()

    @pytest.mark.asyncio
    async def test_write_file(self, server: FilesystemServer, tmp_path: Path) -> None:
        """Test writing a new file (no read required for new files)."""
        result = await server._write_file("new.txt", "content")
        assert (tmp_path / "new.txt").read_text() == "content"
        import json

        data = json.loads(result)
        assert data["success"] is True

    @pytest.mark.asyncio
    async def test_write_file_creates_directories(
        self, server: FilesystemServer, tmp_path: Path
    ) -> None:
        """Test writing file creates parent directories."""
        await server._write_file("nested/dir/file.txt", "content")
        assert (tmp_path / "nested" / "dir" / "file.txt").read_text() == "content"

    def test_path_traversal_blocked(self, server: FilesystemServer) -> None:
        """Test that path traversal is blocked."""
        with pytest.raises(ValueError, match="outside root"):
            server._resolve_path("../../../etc/passwd")

    def test_path_traversal_with_absolute(self, server: FilesystemServer) -> None:
        """Test that absolute path outside root is blocked."""
        with pytest.raises(ValueError, match="outside root"):
            server._resolve_path("/etc/passwd")

    @pytest.mark.asyncio
    async def test_str_replace_edit(self, server: FilesystemServer, tmp_path: Path) -> None:
        """Test edit_file with str_replace mode."""
        import json

        (tmp_path / "edit.txt").write_text("hello world")

        # Must read first
        await server._read_file("edit.txt")

        # Then edit
        result = await server._str_replace("edit.txt", "world", "universe", False)
        data = json.loads(result)
        assert data["success"] is True
        assert (tmp_path / "edit.txt").read_text() == "hello universe"

    @pytest.mark.asyncio
    async def test_edit_requires_read(self, server: FilesystemServer, tmp_path: Path) -> None:
        """Test that edit requires reading the file first."""
        import json

        (tmp_path / "noedit.txt").write_text("content")

        # Try to edit without reading first
        result = await server._str_replace("noedit.txt", "content", "new", False)
        data = json.loads(result)
        assert data["error"] == "read_required"

    @pytest.mark.asyncio
    async def test_insert_at_line(self, server: FilesystemServer, tmp_path: Path) -> None:
        """Test edit_file with insert mode."""
        import json

        (tmp_path / "insert.txt").write_text("line1\nline2\n")

        # Must read first
        await server._read_file("insert.txt")

        # Insert after line 1
        result = await server._insert_at_line("insert.txt", 1, "inserted")
        data = json.loads(result)
        assert data["success"] is True
        assert (tmp_path / "insert.txt").read_text() == "line1\ninserted\nline2\n"


class TestFilesystemSizeGate:
    """File size gate blocks oversized reads."""

    @pytest.fixture
    def server(self, tmp_path: Path) -> FilesystemServer:
        """Create server with a small max_read_bytes for testing."""
        from entropi.config.schema import FilesystemConfig

        config = FilesystemConfig(max_read_bytes=1000)
        return FilesystemServer(tmp_path, config=config)

    @pytest.mark.asyncio
    async def test_blocks_oversized_file(self, server: FilesystemServer, tmp_path: Path) -> None:
        """File larger than max_read_bytes is blocked."""
        import json

        (tmp_path / "big.txt").write_text("x" * 2000)
        result = await server._read_file("big.txt")
        data = json.loads(result)
        assert data["blocked"] is True
        assert "2,000" in data["reason"]
        assert "entropi.todo_write" in data["suggestion"]

    @pytest.mark.asyncio
    async def test_allows_small_file(self, server: FilesystemServer, tmp_path: Path) -> None:
        """File within max_read_bytes is read normally."""
        import json

        (tmp_path / "small.txt").write_text("hello")
        result = await server._read_file("small.txt")
        data = json.loads(result)
        assert "blocked" not in data
        assert data["path"] == "small.txt"
        assert "bytes" in data


class TestSkipDuplicateCheck:
    """skip_duplicate_check hook for side-effect tools."""

    def test_filesystem_read_skips_duplicate(self) -> None:
        """read_file should always execute (side effect: FileAccessTracker)."""
        assert FilesystemServer.skip_duplicate_check("read_file") is True

    def test_filesystem_write_does_not_skip(self) -> None:
        """write_file should use duplicate detection."""
        assert FilesystemServer.skip_duplicate_check("write_file") is False

    def test_base_server_default(self) -> None:
        """BaseMCPServer default is False (no skip)."""
        from entropi.mcp.servers.base import BaseMCPServer

        assert BaseMCPServer.skip_duplicate_check("any_tool") is False


class TestBashServer:
    """Tests for bash server."""

    def test_blocked_commands(self) -> None:
        """Test dangerous commands are blocked."""
        server = BashServer()

        # Dangerous commands
        assert not server._is_safe_command("rm -rf /")
        assert not server._is_safe_command("rm -rf /*")
        assert not server._is_safe_command("sudo rm -rf /")
        assert not server._is_safe_command("dd if=/dev/zero of=/dev/sda")
        assert not server._is_safe_command(":(){:|:&};:")

        # Safe commands
        assert server._is_safe_command("ls -la")
        assert server._is_safe_command("python --version")
        assert server._is_safe_command("cat file.txt")
        assert server._is_safe_command("grep pattern file.txt")

    def test_sudo_blocked(self) -> None:
        """Test sudo commands are blocked."""
        server = BashServer()

        assert not server._is_safe_command("sudo apt update")
        assert not server._is_safe_command("su -c 'command'")

    @pytest.mark.asyncio
    async def test_execute_simple_command(self) -> None:
        """Test executing a simple command."""
        server = BashServer()
        result = await server._execute_command("echo hello", None)
        assert "hello" in result

    @pytest.mark.asyncio
    async def test_execute_with_working_dir(self, tmp_path: Path) -> None:
        """Test executing with specific working directory."""
        (tmp_path / "test.txt").write_text("content")
        server = BashServer()

        result = await server._execute_command("ls", str(tmp_path))
        assert "test.txt" in result

    @pytest.mark.asyncio
    async def test_execute_captures_stderr(self) -> None:
        """Test that stderr is captured."""
        server = BashServer()
        result = await server._execute_command("ls /nonexistent_dir_12345", None)
        assert "[stderr]" in result or "No such file" in result

    @pytest.mark.asyncio
    async def test_execute_returns_exit_code(self) -> None:
        """Test that non-zero exit code is reported with command echo."""
        server = BashServer()
        result = await server._execute_command("exit 1", None)
        assert "[exit code: 1]" in result
        assert "[command] exit 1" in result

    @pytest.mark.asyncio
    async def test_success_has_no_command_echo(self) -> None:
        """Test that successful commands don't echo the command."""
        server = BashServer()
        result = await server._execute_command("echo hello", None)
        assert "hello" in result
        assert "[command]" not in result

    @pytest.mark.asyncio
    async def test_error_has_labeled_sections(self) -> None:
        """Test that errors have labeled [stdout]/[stderr] sections."""
        server = BashServer()
        result = await server._execute_command("echo out && echo err >&2 && exit 1", None)
        assert "[command]" in result
        assert "[stdout]" in result
        assert "[stderr]" in result
        assert "[exit code: 1]" in result

    def test_tool_list(self) -> None:
        """Test that tools are properly defined."""
        server = BashServer()
        tools = server.get_tools()

        assert len(tools) == 1
        assert tools[0].name == "execute"
        assert "command" in tools[0].inputSchema["properties"]
