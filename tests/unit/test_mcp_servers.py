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
        """Test reading a file."""
        (tmp_path / "test.txt").write_text("hello world")
        result = await server._read_file("test.txt")
        assert result == "hello world"

    @pytest.mark.asyncio
    async def test_read_file_not_found(self, server: FilesystemServer) -> None:
        """Test reading non-existent file."""
        result = await server._read_file("nonexistent.txt")
        assert "not found" in result.lower()

    @pytest.mark.asyncio
    async def test_write_file(self, server: FilesystemServer, tmp_path: Path) -> None:
        """Test writing a file."""
        await server._write_file("new.txt", "content")
        assert (tmp_path / "new.txt").read_text() == "content"

    @pytest.mark.asyncio
    async def test_write_file_creates_directories(
        self, server: FilesystemServer, tmp_path: Path
    ) -> None:
        """Test writing file creates parent directories."""
        await server._write_file("nested/dir/file.txt", "content")
        assert (tmp_path / "nested" / "dir" / "file.txt").read_text() == "content"

    @pytest.mark.asyncio
    async def test_list_directory(self, server: FilesystemServer, tmp_path: Path) -> None:
        """Test listing directory."""
        (tmp_path / "file1.txt").write_text("a")
        (tmp_path / "file2.txt").write_text("b")
        (tmp_path / "subdir").mkdir()

        result = await server._list_directory(".", False)

        assert "file1.txt" in result
        assert "file2.txt" in result
        assert "subdir" in result

    @pytest.mark.asyncio
    async def test_list_directory_recursive(self, server: FilesystemServer, tmp_path: Path) -> None:
        """Test recursive directory listing."""
        (tmp_path / "src").mkdir()
        (tmp_path / "src" / "main.py").write_text("code")

        result = await server._list_directory(".", True)

        assert "src" in result
        assert "main.py" in result

    @pytest.mark.asyncio
    async def test_file_exists(self, server: FilesystemServer, tmp_path: Path) -> None:
        """Test file exists check."""
        (tmp_path / "exists.txt").write_text("yes")

        result = await server._file_exists("exists.txt")
        assert "Yes" in result
        assert "file" in result

        result = await server._file_exists("nope.txt")
        assert "No" in result

    def test_path_traversal_blocked(self, server: FilesystemServer) -> None:
        """Test that path traversal is blocked."""
        with pytest.raises(ValueError, match="outside root"):
            server._resolve_path("../../../etc/passwd")

    def test_path_traversal_with_absolute(self, server: FilesystemServer) -> None:
        """Test that absolute path outside root is blocked."""
        with pytest.raises(ValueError, match="outside root"):
            server._resolve_path("/etc/passwd")

    @pytest.mark.asyncio
    async def test_search_files(self, server: FilesystemServer, tmp_path: Path) -> None:
        """Test file search."""
        (tmp_path / "file1.py").write_text("python code")
        (tmp_path / "file2.py").write_text("more python")
        (tmp_path / "readme.md").write_text("docs")

        result = await server._search_files("*.py", None)

        assert "file1.py" in result
        assert "file2.py" in result
        assert "readme.md" not in result

    @pytest.mark.asyncio
    async def test_search_files_with_content(
        self, server: FilesystemServer, tmp_path: Path
    ) -> None:
        """Test file search with content pattern."""
        (tmp_path / "file1.py").write_text("def hello():")
        (tmp_path / "file2.py").write_text("class Foo:")

        result = await server._search_files("*.py", r"def \w+")

        assert "file1.py" in result
        assert "file2.py" not in result


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
        """Test that non-zero exit code is reported."""
        server = BashServer()
        result = await server._execute_command("exit 1", None)
        assert "[exit code: 1]" in result

    def test_tool_list(self) -> None:
        """Test that tools are properly defined."""
        server = BashServer()
        tools = server.get_tools()

        assert len(tools) == 1
        assert tools[0].name == "execute"
        assert "command" in tools[0].inputSchema["properties"]
