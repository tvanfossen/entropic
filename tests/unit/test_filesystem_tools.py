"""Unit tests for filesystem glob, grep, and list_directory tools."""

import json
from pathlib import Path

import pytest
from entropic.mcp.servers.filesystem import FilesystemServer, _expand_braces


@pytest.fixture
def server(tmp_path: Path) -> FilesystemServer:
    """Create FilesystemServer rooted at tmp_path."""
    return FilesystemServer(tmp_path)


@pytest.fixture
def populated_tree(tmp_path: Path) -> Path:
    """Create a test directory tree for glob/grep/list_directory tests."""
    # src/main.py
    src = tmp_path / "src"
    src.mkdir()
    (src / "main.py").write_text("def main():\n    print('hello')\n")
    (src / "utils.py").write_text("def helper():\n    return 42\n")

    # src/sub/deep.py
    sub = src / "sub"
    sub.mkdir()
    (sub / "deep.py").write_text("# deep module\nclass Deep:\n    pass\n")

    # tests/test_main.py
    tests = tmp_path / "tests"
    tests.mkdir()
    (tests / "test_main.py").write_text("def test_main():\n    assert True\n")

    # root files
    (tmp_path / "README.md").write_text("# Project\nSome description\n")
    (tmp_path / "config.json").write_text('{"key": "value"}\n')

    # .git directory (should be skipped)
    git_dir = tmp_path / ".git"
    git_dir.mkdir()
    (git_dir / "config").write_text("[core]\n")

    # __pycache__ (should be skipped)
    cache = src / "__pycache__"
    cache.mkdir()
    (cache / "main.cpython-312.pyc").write_bytes(b"\x00\x01\x02")

    return tmp_path


class TestGlobTool:
    """Tests for filesystem.glob tool."""

    @pytest.fixture
    def server(self, populated_tree: Path) -> FilesystemServer:
        return FilesystemServer(populated_tree)

    @pytest.mark.asyncio
    async def test_glob_all_python(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("glob", {"pattern": "**/*.py"}))
        assert result["total"] >= 4
        paths = result["matches"]
        assert "src/main.py" in paths
        assert "src/utils.py" in paths
        assert "src/sub/deep.py" in paths
        assert "tests/test_main.py" in paths

    @pytest.mark.asyncio
    async def test_glob_specific_dir(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("glob", {"pattern": "src/*.py"}))
        paths = result["matches"]
        assert "src/main.py" in paths
        assert "src/utils.py" in paths
        assert "src/sub/deep.py" not in paths

    @pytest.mark.asyncio
    async def test_glob_skips_git(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("glob", {"pattern": "**/*"}))
        paths = result["matches"]
        assert not any(".git" in p for p in paths)

    @pytest.mark.asyncio
    async def test_glob_skips_pycache(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("glob", {"pattern": "**/*"}))
        paths = result["matches"]
        assert not any("__pycache__" in p for p in paths)

    @pytest.mark.asyncio
    async def test_glob_no_matches(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("glob", {"pattern": "**/*.rs"}))
        assert result["total"] == 0
        assert result["matches"] == []

    @pytest.mark.asyncio
    async def test_glob_truncation(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("glob", {"pattern": "**/*.py"}))
        assert result["truncated"] is False

    @pytest.mark.asyncio
    async def test_glob_json_files(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("glob", {"pattern": "*.json"}))
        assert "config.json" in result["matches"]

    @pytest.mark.asyncio
    async def test_glob_brace_expansion(self, server: FilesystemServer) -> None:
        """Brace syntax {py,json} expands to multiple patterns."""
        result = json.loads(await server.execute_tool("glob", {"pattern": "**/*.{py,json}"}))
        paths = result["matches"]
        assert "src/main.py" in paths
        assert "config.json" in paths

    @pytest.mark.asyncio
    async def test_glob_brace_no_duplicates(self, server: FilesystemServer) -> None:
        """Overlapping brace expansions don't produce duplicate results."""
        result = json.loads(await server.execute_tool("glob", {"pattern": "**/*.{py,py}"}))
        paths = result["matches"]
        assert len(paths) == len(set(paths))


class TestExpandBraces:
    """Unit tests for _expand_braces helper."""

    def test_no_braces(self) -> None:
        assert _expand_braces("**/*.py") == ["**/*.py"]

    def test_single_group(self) -> None:
        assert sorted(_expand_braces("*.{html,css}")) == ["*.css", "*.html"]

    def test_multiple_groups(self) -> None:
        result = _expand_braces("{src,tests}/*.{py,md}")
        assert sorted(result) == ["src/*.md", "src/*.py", "tests/*.md", "tests/*.py"]

    def test_single_alternative(self) -> None:
        assert _expand_braces("*.{py}") == ["*.py"]


class TestGrepTool:
    """Tests for filesystem.grep tool."""

    @pytest.fixture
    def server(self, populated_tree: Path) -> FilesystemServer:
        return FilesystemServer(populated_tree)

    @pytest.mark.asyncio
    async def test_grep_simple(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("grep", {"pattern": "def main"}))
        assert result["total"] >= 1
        match = result["matches"][0]
        assert match["path"] == "src/main.py"
        assert match["line"] == 1
        assert "def main" in match["content"]

    @pytest.mark.asyncio
    async def test_grep_with_glob_filter(self, server: FilesystemServer) -> None:
        result = json.loads(
            await server.execute_tool("grep", {"pattern": "def", "glob": "tests/**/*.py"})
        )
        paths = {m["path"] for m in result["matches"]}
        assert "tests/test_main.py" in paths
        assert "src/main.py" not in paths

    @pytest.mark.asyncio
    async def test_grep_regex(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("grep", {"pattern": r"class\s+\w+"}))
        assert result["total"] >= 1
        assert any("class Deep" in m["content"] for m in result["matches"])

    @pytest.mark.asyncio
    async def test_grep_invalid_regex(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("grep", {"pattern": "[invalid"}))
        assert result["error"] == "invalid_regex"

    @pytest.mark.asyncio
    async def test_grep_no_matches(self, server: FilesystemServer) -> None:
        result = json.loads(
            await server.execute_tool("grep", {"pattern": "nonexistent_string_xyz"})
        )
        assert result["total"] == 0

    @pytest.mark.asyncio
    async def test_grep_skips_git(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("grep", {"pattern": "core"}))
        paths = {m["path"] for m in result["matches"]}
        assert not any(".git" in p for p in paths)

    @pytest.mark.asyncio
    async def test_grep_skips_binary(self, populated_tree: Path) -> None:
        # Create a binary file
        (populated_tree / "binary.bin").write_bytes(b"\x00\x01\x02\xff")
        server = FilesystemServer(populated_tree)
        result = json.loads(await server.execute_tool("grep", {"pattern": ".*"}))
        paths = {m["path"] for m in result["matches"]}
        assert "binary.bin" not in paths


class TestListDirectoryTool:
    """Tests for filesystem.list_directory tool."""

    @pytest.fixture
    def server(self, populated_tree: Path) -> FilesystemServer:
        return FilesystemServer(populated_tree)

    @pytest.mark.asyncio
    async def test_list_root(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("list_directory", {"path": "."}))
        names = {e["name"] for e in result["entries"]}
        assert "README.md" in names
        assert "config.json" in names
        assert "src/" in names
        assert "tests/" in names

    @pytest.mark.asyncio
    async def test_list_skips_git(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("list_directory", {"path": "."}))
        names = {e["name"] for e in result["entries"]}
        assert ".git/" not in names

    @pytest.mark.asyncio
    async def test_list_subdirectory(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("list_directory", {"path": "src"}))
        names = {e["name"] for e in result["entries"]}
        assert "src/main.py" in names
        assert "src/utils.py" in names
        assert "src/sub/" in names

    @pytest.mark.asyncio
    async def test_list_file_sizes(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("list_directory", {"path": "."}))
        files = [e for e in result["entries"] if e["type"] == "file"]
        for f in files:
            assert "size" in f
            assert f["size"] >= 0

    @pytest.mark.asyncio
    async def test_list_recursive(self, server: FilesystemServer) -> None:
        result = json.loads(
            await server.execute_tool("list_directory", {"path": "src", "recursive": True})
        )
        names = {e["name"] for e in result["entries"]}
        assert "src/main.py" in names
        assert "src/sub/" in names
        assert "src/sub/deep.py" in names

    @pytest.mark.asyncio
    async def test_list_recursive_skips_pycache(self, server: FilesystemServer) -> None:
        result = json.loads(
            await server.execute_tool("list_directory", {"path": "src", "recursive": True})
        )
        names = {e["name"] for e in result["entries"]}
        assert not any("__pycache__" in n for n in names)

    @pytest.mark.asyncio
    async def test_list_max_depth(self, populated_tree: Path) -> None:
        # Create deeper nesting
        deep = populated_tree / "a" / "b" / "c" / "d"
        deep.mkdir(parents=True)
        (deep / "file.txt").write_text("deep")
        server = FilesystemServer(populated_tree)

        result = json.loads(
            await server.execute_tool(
                "list_directory", {"path": "a", "recursive": True, "max_depth": 1}
            )
        )
        names = {e["name"] for e in result["entries"]}
        assert "a/b/" in names
        assert "a/b/c/" in names
        # depth=1 means we go into a/ and a/b/ but not further
        assert "a/b/c/d/" not in names

    @pytest.mark.asyncio
    async def test_list_not_a_directory(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("list_directory", {"path": "README.md"}))
        assert result["error"] == "not_a_directory"

    @pytest.mark.asyncio
    async def test_list_default_path(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("list_directory", {}))
        assert result["total"] > 0
        assert result["path"] == "."

    @pytest.mark.asyncio
    async def test_dir_entries_have_type(self, server: FilesystemServer) -> None:
        result = json.loads(await server.execute_tool("list_directory", {"path": "."}))
        dirs = [e for e in result["entries"] if e["type"] == "dir"]
        assert len(dirs) > 0
        for d in dirs:
            assert d["name"].endswith("/")
