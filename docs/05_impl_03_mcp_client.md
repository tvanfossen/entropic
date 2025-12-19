# Implementation 03: MCP Client

> Model Context Protocol client for tool integration

**Prerequisites:** Implementation 02 complete
**Estimated Time:** 2-3 hours with Claude Code
**Checkpoint:** Can list tools from MCP servers

---

## Objectives

1. Implement MCP client using official Python SDK
2. Create server manager for lifecycle management
3. Build tool execution framework
4. Handle tool permissions

---

## 1. MCP Client

### File: `src/entropi/mcp/client.py`

```python
"""
MCP client implementation using official SDK.

Provides a unified interface for communicating with MCP servers.
"""
import asyncio
from typing import Any

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

from entropi.core.base import ToolCall, ToolResult
from entropi.core.logging import get_logger

logger = get_logger("mcp.client")


class MCPClient:
    """Client for communicating with a single MCP server."""

    def __init__(
        self,
        name: str,
        command: str,
        args: list[str] | None = None,
        env: dict[str, str] | None = None,
    ) -> None:
        """
        Initialize MCP client.

        Args:
            name: Server name
            command: Command to start server
            args: Command arguments
            env: Environment variables
        """
        self.name = name
        self.command = command
        self.args = args or []
        self.env = env or {}

        self._session: ClientSession | None = None
        self._read = None
        self._write = None
        self._tools: list[dict[str, Any]] = []

    async def connect(self) -> None:
        """Connect to the MCP server."""
        logger.info(f"Connecting to MCP server: {self.name}")

        server_params = StdioServerParameters(
            command=self.command,
            args=self.args,
            env=self.env,
        )

        self._read, self._write = await stdio_client(server_params).__aenter__()
        self._session = ClientSession(self._read, self._write)
        await self._session.__aenter__()

        # Initialize and get capabilities
        await self._session.initialize()

        # Cache available tools
        tools_response = await self._session.list_tools()
        self._tools = [
            {
                "name": f"{self.name}.{tool.name}",
                "description": tool.description or "",
                "inputSchema": tool.inputSchema if hasattr(tool, 'inputSchema') else {},
            }
            for tool in tools_response.tools
        ]

        logger.info(f"Connected to {self.name}, {len(self._tools)} tools available")

    async def disconnect(self) -> None:
        """Disconnect from the MCP server."""
        if self._session:
            await self._session.__aexit__(None, None, None)
            self._session = None

        logger.info(f"Disconnected from {self.name}")

    async def list_tools(self) -> list[dict[str, Any]]:
        """List available tools."""
        return self._tools

    async def execute(
        self,
        tool_name: str,
        arguments: dict[str, Any],
    ) -> ToolResult:
        """
        Execute a tool.

        Args:
            tool_name: Full tool name (server.tool)
            arguments: Tool arguments

        Returns:
            Tool execution result
        """
        if not self._session:
            raise RuntimeError(f"Not connected to {self.name}")

        # Extract local tool name
        if tool_name.startswith(f"{self.name}."):
            local_name = tool_name[len(self.name) + 1:]
        else:
            local_name = tool_name

        logger.debug(f"Executing {self.name}.{local_name}")

        try:
            import time
            start = time.time()

            result = await self._session.call_tool(local_name, arguments)

            duration = int((time.time() - start) * 1000)

            # Extract text content from result
            content = ""
            if result.content:
                for item in result.content:
                    if hasattr(item, 'text'):
                        content += item.text

            return ToolResult(
                call_id=tool_name,
                name=tool_name,
                result=content,
                is_error=result.isError if hasattr(result, 'isError') else False,
                duration_ms=duration,
            )

        except Exception as e:
            logger.error(f"Tool execution failed: {e}")
            return ToolResult(
                call_id=tool_name,
                name=tool_name,
                result=str(e),
                is_error=True,
            )

    @property
    def is_connected(self) -> bool:
        """Check if connected to server."""
        return self._session is not None
```

---

## 2. Server Manager

### File: `src/entropi/mcp/manager.py`

```python
"""
MCP server manager.

Manages lifecycle of multiple MCP servers and provides
unified tool access.
"""
import asyncio
import fnmatch
from typing import Any

from entropi.config.schema import EntropyConfig, PermissionsConfig
from entropi.core.base import ToolCall, ToolResult
from entropi.core.logging import get_logger
from entropi.mcp.client import MCPClient

logger = get_logger("mcp.manager")


class PermissionDenied(Exception):
    """Raised when tool execution is denied by permissions."""

    pass


class ServerManager:
    """
    Manages multiple MCP servers.

    Provides unified interface for tool discovery and execution
    with permission checking.
    """

    def __init__(self, config: EntropyConfig) -> None:
        """
        Initialize server manager.

        Args:
            config: Application configuration
        """
        self.config = config
        self._clients: dict[str, MCPClient] = {}
        self._permissions = config.permissions

    async def initialize(self) -> None:
        """Initialize and connect to all configured servers."""
        logger.info("Initializing MCP server manager")

        mcp_config = self.config.mcp

        # Built-in servers (implemented in next phase)
        if mcp_config.enable_filesystem:
            self._clients["filesystem"] = MCPClient(
                name="filesystem",
                command="python",
                args=["-m", "entropi.mcp.servers.filesystem"],
            )

        if mcp_config.enable_bash:
            self._clients["bash"] = MCPClient(
                name="bash",
                command="python",
                args=["-m", "entropi.mcp.servers.bash"],
            )

        if mcp_config.enable_git:
            self._clients["git"] = MCPClient(
                name="git",
                command="python",
                args=["-m", "entropi.mcp.servers.git"],
            )

        # External servers from config
        for name, server_config in mcp_config.external_servers.items():
            self._clients[name] = MCPClient(
                name=name,
                command=server_config.get("command", ""),
                args=server_config.get("args", []),
                env=server_config.get("env", {}),
            )

        # Connect to all servers
        connect_tasks = []
        for client in self._clients.values():
            connect_tasks.append(client.connect())

        results = await asyncio.gather(*connect_tasks, return_exceptions=True)

        # Log any connection failures
        for client, result in zip(self._clients.values(), results):
            if isinstance(result, Exception):
                logger.error(f"Failed to connect to {client.name}: {result}")

    async def shutdown(self) -> None:
        """Disconnect from all servers."""
        logger.info("Shutting down MCP servers")

        disconnect_tasks = []
        for client in self._clients.values():
            if client.is_connected:
                disconnect_tasks.append(client.disconnect())

        await asyncio.gather(*disconnect_tasks, return_exceptions=True)

    async def list_tools(self) -> list[dict[str, Any]]:
        """
        List all available tools from all servers.

        Returns:
            List of tool definitions
        """
        all_tools = []

        for client in self._clients.values():
            if client.is_connected:
                tools = await client.list_tools()
                all_tools.extend(tools)

        return all_tools

    async def execute(self, tool_call: ToolCall) -> ToolResult:
        """
        Execute a tool call.

        Args:
            tool_call: Tool call to execute

        Returns:
            Tool result

        Raises:
            PermissionDenied: If tool is not allowed
        """
        # Check permissions
        tool_pattern = f"{tool_call.name}:{self._args_to_pattern(tool_call.arguments)}"

        if not self._check_permission(tool_call.name, tool_pattern):
            raise PermissionDenied(f"Permission denied for {tool_call.name}")

        # Find the right client
        server_name = tool_call.name.split(".")[0]
        if server_name not in self._clients:
            return ToolResult(
                call_id=tool_call.id,
                name=tool_call.name,
                result=f"Unknown server: {server_name}",
                is_error=True,
            )

        client = self._clients[server_name]
        if not client.is_connected:
            return ToolResult(
                call_id=tool_call.id,
                name=tool_call.name,
                result=f"Server not connected: {server_name}",
                is_error=True,
            )

        result = await client.execute(tool_call.name, tool_call.arguments)
        result.call_id = tool_call.id

        return result

    def _args_to_pattern(self, arguments: dict[str, Any]) -> str:
        """Convert arguments to pattern string for permission matching."""
        if not arguments:
            return "*"
        # Use first argument value as pattern
        first_value = next(iter(arguments.values()), "*")
        return str(first_value)

    def _check_permission(self, tool_name: str, pattern: str) -> bool:
        """
        Check if tool execution is allowed.

        Args:
            tool_name: Tool name (e.g., "filesystem.read_file")
            pattern: Tool pattern with args (e.g., "filesystem.read_file:/path")

        Returns:
            True if allowed
        """
        # Check deny list first
        for deny_pattern in self._permissions.deny:
            if fnmatch.fnmatch(tool_name, deny_pattern.split(":")[0]):
                if ":" in deny_pattern:
                    if fnmatch.fnmatch(pattern, deny_pattern):
                        return False
                else:
                    return False

        # Check allow list
        for allow_pattern in self._permissions.allow:
            if fnmatch.fnmatch(tool_name, allow_pattern.split(":")[0]):
                if ":" in allow_pattern:
                    if fnmatch.fnmatch(pattern, allow_pattern):
                        return True
                else:
                    return True

        # Default deny
        return False

    def get_connected_servers(self) -> list[str]:
        """Get list of connected server names."""
        return [name for name, client in self._clients.items() if client.is_connected]
```

---

## 3. Tests

### File: `tests/unit/test_mcp.py`

```python
"""Tests for MCP components."""
import pytest

from entropi.config.schema import PermissionsConfig
from entropi.mcp.manager import ServerManager


class TestPermissions:
    """Tests for permission checking."""

    def test_allow_wildcard(self) -> None:
        """Test wildcard allow pattern."""
        # Setup permissions directly
        permissions = PermissionsConfig(
            allow=["filesystem.*"],
            deny=[],
        )

        # Create minimal config mock
        class MockConfig:
            permissions = permissions

        manager = ServerManager.__new__(ServerManager)
        manager._permissions = permissions

        assert manager._check_permission("filesystem.read_file", "filesystem.read_file:/test")
        assert manager._check_permission("filesystem.write_file", "filesystem.write_file:/test")
        assert not manager._check_permission("bash.execute", "bash.execute:ls")

    def test_deny_overrides_allow(self) -> None:
        """Test deny patterns override allow."""
        permissions = PermissionsConfig(
            allow=["bash.execute:*"],
            deny=["bash.execute:rm -rf *"],
        )

        manager = ServerManager.__new__(ServerManager)
        manager._permissions = permissions

        assert manager._check_permission("bash.execute", "bash.execute:ls")
        assert not manager._check_permission("bash.execute", "bash.execute:rm -rf /")

    def test_specific_path_allow(self) -> None:
        """Test specific path patterns."""
        permissions = PermissionsConfig(
            allow=["filesystem.write_file:src/*"],
            deny=[],
        )

        manager = ServerManager.__new__(ServerManager)
        manager._permissions = permissions

        assert manager._check_permission("filesystem.write_file", "filesystem.write_file:src/test.py")
        assert not manager._check_permission("filesystem.write_file", "filesystem.write_file:/etc/passwd")
```

---

## Checkpoint: Verification

```bash
pytest tests/unit/test_mcp.py -v
```

**Success Criteria:**
- [ ] Permission tests pass
- [ ] MCPClient structure is correct
- [ ] ServerManager initializes without errors

---

## Next Phase

Proceed to **Implementation 04: MCP Servers** to implement the built-in tool servers.
