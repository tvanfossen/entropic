"""Unit tests for P2-026: Runtime MCP server registration.

Tests:
- MCPClient SSE transport inference
- .mcp.json parsing and merge with YAML config
- Self-detection (own socket skipped, other socket connected)
- connect_server / disconnect_server lifecycle
- compute_socket_path stability and uniqueness
"""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from entropic.mcp.client import MCPClient
from entropic.mcp.manager import ServerInfo, ServerManager
from entropic.mcp.servers.external import compute_socket_path

# ---------------------------------------------------------------------------
# compute_socket_path
# ---------------------------------------------------------------------------


class TestComputeSocketPath:
    def test_returns_path_in_socks_dir(self) -> None:
        p = compute_socket_path(Path("/home/user/proj"))
        assert p.parent.name == "socks"
        assert p.suffix == ".sock"

    def test_stable_across_calls(self) -> None:
        path = Path("/home/user/proj-a")
        assert compute_socket_path(path) == compute_socket_path(path)

    def test_different_projects_differ(self) -> None:
        a = compute_socket_path(Path("/home/user/proj-a"))
        b = compute_socket_path(Path("/home/user/proj-b"))
        assert a != b

    def test_hash_is_8_chars(self) -> None:
        p = compute_socket_path(Path("/some/path"))
        assert len(p.stem) == 8


# ---------------------------------------------------------------------------
# MCPClient transport inference
# ---------------------------------------------------------------------------


class TestMCPClientTransport:
    def test_stdio_when_command_only(self) -> None:
        c = MCPClient(name="test", command="my-server")
        assert c.transport == "stdio"
        assert c.sse_url == ""

    def test_sse_when_sse_url(self) -> None:
        c = MCPClient(name="test", sse_url="http://localhost:8080/sse")
        assert c.transport == "sse"
        assert c.command == ""

    def test_is_connected_false_initially(self) -> None:
        c = MCPClient(name="test", command="cmd")
        assert not c.is_connected


# ---------------------------------------------------------------------------
# .mcp.json parsing
# ---------------------------------------------------------------------------


def _make_manager(project_dir: Path, yaml_external: dict | None = None) -> ServerManager:
    """Construct a minimal ServerManager without full LibraryConfig."""
    from unittest.mock import MagicMock

    from entropic.config.schema import MCPConfig, PermissionsConfig

    mcp_config = MCPConfig(external_servers=yaml_external or {})
    perms = PermissionsConfig()

    config = MagicMock()
    config.mcp = mcp_config
    config.permissions = perms
    config.models.tiers = {}

    mgr = ServerManager.__new__(ServerManager)
    mgr.config = config  # type: ignore[assignment]
    mgr._project_dir = project_dir
    mgr._tier_names = None
    mgr._clients = {}
    mgr._server_classes = {}
    mgr._server_info = {}
    mgr._clients_lock = asyncio.Lock()
    mgr._permissions = perms
    mgr._lsp_manager = None
    return mgr


class TestMcpJsonParsing:
    def test_sse_entry_creates_client(self, tmp_path: Path) -> None:
        mcp_json = tmp_path / ".mcp.json"
        mcp_json.write_text(
            json.dumps(
                {"mcpServers": {"my-tool": {"type": "sse", "url": "http://127.0.0.1:9000/sse"}}}
            )
        )
        mgr = _make_manager(tmp_path)
        mgr._load_mcp_json_servers()

        assert "my-tool" in mgr._clients
        assert isinstance(mgr._clients["my-tool"], MCPClient)
        info = mgr._server_info["my-tool"]
        assert info.transport == "sse"
        assert info.url == "http://127.0.0.1:9000/sse"

    def test_stdio_entry_creates_client(self, tmp_path: Path) -> None:
        mcp_json = tmp_path / ".mcp.json"
        mcp_json.write_text(
            json.dumps(
                {
                    "mcpServers": {
                        "my-stdio": {
                            "type": "stdio",
                            "command": "my-server",
                            "args": ["--verbose"],
                        }
                    }
                }
            )
        )
        mgr = _make_manager(tmp_path)
        mgr._load_mcp_json_servers()

        assert "my-stdio" in mgr._clients
        info = mgr._server_info["my-stdio"]
        assert info.transport == "stdio"
        assert info.command == "my-server"

    def test_missing_url_skips_entry(self, tmp_path: Path) -> None:
        mcp_json = tmp_path / ".mcp.json"
        mcp_json.write_text(json.dumps({"mcpServers": {"bad": {"type": "sse"}}}))
        mgr = _make_manager(tmp_path)
        mgr._load_mcp_json_servers()
        assert "bad" not in mgr._clients

    def test_missing_command_skips_entry(self, tmp_path: Path) -> None:
        mcp_json = tmp_path / ".mcp.json"
        mcp_json.write_text(json.dumps({"mcpServers": {"bad": {"type": "stdio"}}}))
        mgr = _make_manager(tmp_path)
        mgr._load_mcp_json_servers()
        assert "bad" not in mgr._clients

    def test_unsupported_transport_skips(self, tmp_path: Path) -> None:
        mcp_json = tmp_path / ".mcp.json"
        mcp_json.write_text(
            json.dumps({"mcpServers": {"ws": {"type": "websocket", "url": "ws://localhost"}}})
        )
        mgr = _make_manager(tmp_path)
        mgr._load_mcp_json_servers()
        assert "ws" not in mgr._clients

    def test_yaml_config_takes_priority(self, tmp_path: Path) -> None:
        mcp_json = tmp_path / ".mcp.json"
        mcp_json.write_text(
            json.dumps({"mcpServers": {"shared": {"type": "sse", "url": "http://localhost/sse"}}})
        )
        mgr = _make_manager(tmp_path, yaml_external={"shared": {"command": "yaml-server"}})
        # Pre-populate _clients to simulate YAML server already registered
        from entropic.mcp.client import MCPClient as _MCPClient

        yaml_client = _MCPClient(name="shared", command="yaml-server")
        mgr._clients["shared"] = yaml_client

        mgr._load_mcp_json_servers()

        # YAML entry should still be there, not replaced by .mcp.json
        assert mgr._clients["shared"] is yaml_client

    def test_no_mcp_json_is_noop(self, tmp_path: Path) -> None:
        mgr = _make_manager(tmp_path)
        mgr._load_mcp_json_servers()
        assert mgr._clients == {}

    def test_invalid_json_is_logged_not_raised(self, tmp_path: Path) -> None:
        mcp_json = tmp_path / ".mcp.json"
        mcp_json.write_text("not json")
        mgr = _make_manager(tmp_path)
        mgr._load_mcp_json_servers()  # should not raise
        assert mgr._clients == {}

    def test_builtin_name_shadowed_not_replaced(self, tmp_path: Path) -> None:
        """Simulate the real .mcp.json: 'entropic' entry is already a built-in.

        When .mcp.json has {"entropic": stdio, "pycommander": sse}, the
        pre-registered "entropic" built-in must not be replaced, but
        "pycommander" must be registered.
        """
        from unittest.mock import MagicMock

        mcp_json = tmp_path / ".mcp.json"
        mcp_json.write_text(
            json.dumps(
                {
                    "mcpServers": {
                        "entropic": {
                            "type": "stdio",
                            "command": "entropic",
                            "args": ["mcp-bridge"],
                        },
                        "pycommander": {
                            "type": "sse",
                            "url": "http://127.0.0.1:6277/sse",
                        },
                    }
                }
            )
        )
        mgr = _make_manager(tmp_path)
        builtin_stub = MagicMock()
        mgr._clients["entropic"] = builtin_stub  # simulate pre-registered built-in

        mgr._load_mcp_json_servers()

        assert mgr._clients["entropic"] is builtin_stub  # not replaced
        assert "pycommander" in mgr._clients
        info = mgr._server_info["pycommander"]
        assert info.transport == "sse"
        assert info.url == "http://127.0.0.1:6277/sse"
        assert info.source == "mcp_json"


class TestSelfDetection:
    def test_own_socket_skipped(self, tmp_path: Path) -> None:
        own_socket = compute_socket_path(tmp_path)
        mcp_json = tmp_path / ".mcp.json"
        mcp_json.write_text(
            json.dumps(
                {
                    "mcpServers": {
                        "self": {"type": "socket", "path": str(own_socket)},
                        "other": {"type": "sse", "url": "http://other:8080/sse"},
                    }
                }
            )
        )
        mgr = _make_manager(tmp_path)
        mgr._load_mcp_json_servers()

        assert "self" not in mgr._clients
        assert "other" in mgr._clients

    def test_different_project_not_skipped(self, tmp_path: Path) -> None:
        other_dir = tmp_path / "other-project"
        other_socket = compute_socket_path(other_dir)
        mcp_json = tmp_path / ".mcp.json"
        mcp_json.write_text(
            json.dumps({"mcpServers": {"peer": {"type": "sse", "url": "http://peer/sse"}}})
        )
        mgr = _make_manager(tmp_path)

        # The manager's own socket is different from other_socket
        assert compute_socket_path(tmp_path) != other_socket

        mgr._load_mcp_json_servers()
        assert "peer" in mgr._clients


# ---------------------------------------------------------------------------
# connect_server / disconnect_server
# ---------------------------------------------------------------------------


class TestRuntimeConnectDisconnect:
    @pytest.mark.asyncio
    async def test_connect_sse_server(self, tmp_path: Path) -> None:
        mgr = _make_manager(tmp_path)

        mock_client = MagicMock(spec=MCPClient)
        mock_client.is_connected = True
        mock_client.list_tools = AsyncMock(return_value=[{"name": "my-tool.do_thing"}])

        with patch("entropic.mcp.manager.MCPClient", return_value=mock_client):
            with patch.object(mgr, "_safe_connect", new_callable=AsyncMock) as mock_connect:
                tool_names = await mgr.connect_server(
                    name="my-tool", sse_url="http://localhost:9000/sse"
                )

        mock_connect.assert_called_once_with(mock_client)
        assert "my-tool" in mgr._clients
        assert "my-tool.do_thing" in tool_names
        assert mgr._server_info["my-tool"].status == "connected"
        assert mgr._server_info["my-tool"].source == "runtime"

    @pytest.mark.asyncio
    async def test_connect_stdio_server(self, tmp_path: Path) -> None:
        mgr = _make_manager(tmp_path)

        mock_client = MagicMock(spec=MCPClient)
        mock_client.is_connected = True
        mock_client.list_tools = AsyncMock(return_value=[])

        with patch("entropic.mcp.manager.MCPClient", return_value=mock_client):
            with patch.object(mgr, "_safe_connect", new_callable=AsyncMock):
                await mgr.connect_server(name="cli-tool", command="my-cli", args=["--flag"])

        assert "cli-tool" in mgr._clients
        assert mgr._server_info["cli-tool"].transport == "stdio"  # from ServerInfo

    @pytest.mark.asyncio
    async def test_connect_duplicate_raises(self, tmp_path: Path) -> None:
        mgr = _make_manager(tmp_path)
        mgr._clients["existing"] = MagicMock()

        with pytest.raises(ValueError, match="already connected"):
            await mgr.connect_server(name="existing", command="cmd")

    @pytest.mark.asyncio
    async def test_connect_no_transport_raises(self, tmp_path: Path) -> None:
        mgr = _make_manager(tmp_path)

        with pytest.raises(ValueError, match="command.*sse_url"):
            await mgr.connect_server(name="nothing")

    @pytest.mark.asyncio
    async def test_disconnect_removes_server(self, tmp_path: Path) -> None:
        mgr = _make_manager(tmp_path)

        mock_client = MagicMock(spec=MCPClient)
        mock_client.is_connected = True
        mock_client.disconnect = AsyncMock()
        mgr._clients["bye"] = mock_client
        mgr._server_info["bye"] = ServerInfo(
            name="bye",
            transport="sse",
            url="http://x/sse",
            command=None,
            status="connected",
            source="runtime",
        )

        await mgr.disconnect_server("bye")

        assert "bye" not in mgr._clients
        assert "bye" not in mgr._server_info
        mock_client.disconnect.assert_called_once()

    @pytest.mark.asyncio
    async def test_disconnect_unknown_raises(self, tmp_path: Path) -> None:
        mgr = _make_manager(tmp_path)

        with pytest.raises(KeyError):
            await mgr.disconnect_server("nonexistent")

    def test_list_servers_returns_snapshot(self, tmp_path: Path) -> None:
        mgr = _make_manager(tmp_path)
        info = ServerInfo(
            name="s1",
            transport="stdio",
            url=None,
            command="cmd",
            status="connected",
            source="config",
        )
        mgr._server_info["s1"] = info

        result = mgr.list_servers()

        assert "s1" in result
        assert result["s1"] is info
        # Snapshot: mutating result doesn't affect internal state
        result["s1"] = MagicMock()  # type: ignore[assignment]
        assert mgr._server_info["s1"] is info
