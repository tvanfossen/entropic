"""Tests for MCP components."""

from entropi.config.schema import PermissionsConfig
from entropi.mcp.manager import ServerManager


class TestPermissions:
    """Tests for permission checking."""

    def _create_manager_with_permissions(
        self,
        allow: list[str],
        deny: list[str],
    ) -> ServerManager:
        """Create a ServerManager with specific permissions."""
        permissions = PermissionsConfig(allow=allow, deny=deny)

        # Create minimal manager without full config
        manager = ServerManager.__new__(ServerManager)
        manager._permissions = permissions
        manager._clients = {}

        return manager

    def test_allow_wildcard(self) -> None:
        """Test wildcard allow pattern."""
        manager = self._create_manager_with_permissions(
            allow=["filesystem.*"],
            deny=[],
        )

        assert manager._check_permission("filesystem.read_file", "filesystem.read_file:/test")
        assert manager._check_permission("filesystem.write_file", "filesystem.write_file:/test")
        assert not manager._check_permission("bash.execute", "bash.execute:ls")

    def test_deny_overrides_allow(self) -> None:
        """Test deny patterns override allow."""
        manager = self._create_manager_with_permissions(
            allow=["bash.execute:*"],
            deny=["bash.execute:rm -rf *"],
        )

        assert manager._check_permission("bash.execute", "bash.execute:ls")
        assert not manager._check_permission("bash.execute", "bash.execute:rm -rf /")

    def test_specific_path_allow(self) -> None:
        """Test specific path patterns."""
        manager = self._create_manager_with_permissions(
            allow=["filesystem.write_file:src/*"],
            deny=[],
        )

        assert manager._check_permission(
            "filesystem.write_file", "filesystem.write_file:src/test.py"
        )
        assert not manager._check_permission(
            "filesystem.write_file", "filesystem.write_file:/etc/passwd"
        )

    def test_default_deny(self) -> None:
        """Test default deny behavior."""
        manager = self._create_manager_with_permissions(
            allow=[],
            deny=[],
        )

        assert not manager._check_permission("filesystem.read_file", "filesystem.read_file:/test")

    def test_multiple_allow_patterns(self) -> None:
        """Test multiple allow patterns."""
        manager = self._create_manager_with_permissions(
            allow=["filesystem.*", "git.*"],
            deny=[],
        )

        assert manager._check_permission("filesystem.read_file", "filesystem.read_file:/test")
        assert manager._check_permission("git.status", "git.status:*")
        assert not manager._check_permission("bash.execute", "bash.execute:ls")

    def test_pattern_with_partial_match(self) -> None:
        """Test that patterns require proper matching."""
        manager = self._create_manager_with_permissions(
            allow=["filesystem.read*"],
            deny=[],
        )

        assert manager._check_permission("filesystem.read_file", "filesystem.read_file:/test")
        assert manager._check_permission(
            "filesystem.read_directory", "filesystem.read_directory:/test"
        )
        assert not manager._check_permission("filesystem.write_file", "filesystem.write_file:/test")

    def test_args_to_pattern_empty(self) -> None:
        """Test args to pattern with empty arguments."""
        manager = self._create_manager_with_permissions(allow=[], deny=[])

        assert manager._args_to_pattern({}) == "*"

    def test_args_to_pattern_single_arg(self) -> None:
        """Test args to pattern with single argument."""
        manager = self._create_manager_with_permissions(allow=[], deny=[])

        assert manager._args_to_pattern({"path": "/test/file.py"}) == "/test/file.py"

    def test_args_to_pattern_multiple_args(self) -> None:
        """Test args to pattern uses first argument."""
        manager = self._create_manager_with_permissions(allow=[], deny=[])

        # First arg in insertion order
        result = manager._args_to_pattern({"path": "/test", "content": "data"})
        assert result == "/test"


class TestServerManager:
    """Tests for ServerManager functionality."""

    def test_get_connected_servers_empty(self) -> None:
        """Test getting connected servers when none connected."""
        manager = ServerManager.__new__(ServerManager)
        manager._clients = {}

        assert manager.get_connected_servers() == []

    def test_get_all_servers(self) -> None:
        """Test getting all configured servers."""
        from unittest.mock import MagicMock

        manager = ServerManager.__new__(ServerManager)

        mock_client = MagicMock()
        mock_client.is_connected = False

        manager._clients = {
            "filesystem": mock_client,
            "bash": mock_client,
        }

        assert set(manager.get_all_servers()) == {"filesystem", "bash"}
