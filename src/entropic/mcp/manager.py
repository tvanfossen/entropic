"""
MCP server manager.

Manages lifecycle of multiple MCP servers and provides
unified tool access.
"""

from __future__ import annotations

import asyncio
import fnmatch
from pathlib import Path
from typing import TYPE_CHECKING, Any

from entropic.config.schema import EntropyConfig
from entropic.core.base import ToolCall, ToolProvider, ToolResult
from entropic.core.logging import get_logger
from entropic.mcp.client import MCPClient
from entropic.mcp.provider import InProcessProvider
from entropic.mcp.servers.base import BaseMCPServer

if TYPE_CHECKING:
    from entropic.lsp.manager import LSPManager

logger = get_logger("mcp.manager")


class PermissionDeniedError(Exception):
    """Raised when tool execution is denied by permissions."""

    pass


# Alias for backwards compatibility
PermissionDenied = PermissionDeniedError


class ServerManager:
    """
    Manages multiple MCP servers.

    Provides unified interface for tool discovery and execution
    with permission checking.
    """

    def __init__(
        self,
        config: EntropyConfig,
        project_dir: Path | None = None,
        tier_names: list[str] | None = None,
    ) -> None:
        """Initialize server manager.

        Args:
            config: Application configuration
            project_dir: Project root directory for server working dirs
            tier_names: Custom tier names for the handoff tool schema.
                When ``None``, uses default tiers from ``handoff.json``.
        """
        self.config = config
        self._project_dir = project_dir or Path.cwd()
        self._tier_names = tier_names
        self._clients: dict[str, ToolProvider] = {}
        self._server_classes: dict[str, type[BaseMCPServer]] = {}
        self._permissions = config.permissions
        self._lsp_manager: LSPManager | None = None

    def register_server(self, server: BaseMCPServer) -> None:
        """Register a custom in-process MCP server.

        Must be called before initialize(). The server will be wrapped
        in an InProcessProvider and connected during initialize().

        Args:
            server: BaseMCPServer instance to register
        """
        name = server.name
        if name in self._clients:
            logger.warning(f"Server '{name}' already registered, replacing")
        self._server_classes[name] = type(server)
        self._clients[name] = InProcessProvider(name, server)

    async def initialize(self) -> None:
        """Initialize and connect to all configured servers."""
        logger.info("Initializing MCP server manager")

        mcp_config = self.config.mcp

        # Built-in servers (in-process providers)
        self._register_builtin_servers(mcp_config)

        # External servers from config
        for name, server_config in mcp_config.external_servers.items():
            self._clients[name] = MCPClient(
                name=name,
                command=server_config.get("command", ""),
                args=server_config.get("args", []),
                env=server_config.get("env", {}),
            )

        # Connect to all servers concurrently
        connect_tasks = []
        for client in self._clients.values():
            connect_tasks.append(self._safe_connect(client))

        await asyncio.gather(*connect_tasks)

        connected = self.get_connected_servers()
        logger.info(f"Connected to {len(connected)} servers: {connected}")

        await self._validate_allowed_tools()

    async def _validate_allowed_tools(self) -> None:
        """Warn about allowed_tools entries that don't match any registered tool."""
        all_tools = await self.list_tools()
        known_names = {t.get("name", "") for t in all_tools}

        for tier_name, model_cfg in self.config.models.tiers.items():
            if model_cfg.allowed_tools is None:
                continue
            unknown = set(model_cfg.allowed_tools) - known_names
            if unknown:
                logger.warning(
                    "Tier '%s' allowed_tools references unknown tools: %s. "
                    "Check for typos — these tools will never be available.",
                    tier_name,
                    sorted(unknown),
                )

    def _register_builtin_servers(self, mcp_config: Any) -> None:
        """Register built-in MCP servers as in-process providers."""
        from entropic.mcp.servers.bash import BashServer
        from entropic.mcp.servers.diagnostics import DiagnosticsServer
        from entropic.mcp.servers.entropic import EntropicServer
        from entropic.mcp.servers.filesystem import FilesystemServer
        from entropic.mcp.servers.git import GitServer

        lsp_manager = self._create_lsp_manager(mcp_config)
        model_context_bytes = self._compute_model_context_bytes()

        if mcp_config.enable_filesystem:
            server = FilesystemServer(
                root_dir=self._project_dir,
                lsp_manager=lsp_manager,
                config=mcp_config.filesystem,
                model_context_bytes=model_context_bytes,
            )
            self._server_classes["filesystem"] = FilesystemServer
            self._clients["filesystem"] = InProcessProvider("filesystem", server)

        if mcp_config.enable_bash:
            self._server_classes["bash"] = BashServer
            self._clients["bash"] = InProcessProvider(
                "bash", BashServer(working_dir=self._project_dir)
            )

        if mcp_config.enable_git:
            self._server_classes["git"] = GitServer
            self._clients["git"] = InProcessProvider("git", GitServer(repo_dir=self._project_dir))

        if mcp_config.enable_diagnostics:
            if lsp_manager:
                server = DiagnosticsServer(lsp_manager=lsp_manager, root_dir=self._project_dir)
                self._server_classes["diagnostics"] = DiagnosticsServer
                self._clients["diagnostics"] = InProcessProvider("diagnostics", server)
            else:
                logger.warning("Diagnostics enabled but LSP not available")

        self._server_classes["entropic"] = EntropicServer
        self._clients["entropic"] = InProcessProvider(
            "entropic", EntropicServer(tier_names=self._tier_names)
        )

    def _create_lsp_manager(self, mcp_config: Any) -> LSPManager | None:
        """Create and start LSP manager if needed by any server."""
        needs_lsp = (
            mcp_config.enable_diagnostics or mcp_config.filesystem.diagnostics_on_edit
        ) and self.config.lsp.enabled
        if not needs_lsp:
            return None
        from entropic.lsp.manager import LSPManager as _LSPManager

        lsp = _LSPManager(self.config.lsp, self._project_dir)
        lsp.start()
        self._lsp_manager = lsp
        logger.info("LSP manager started for in-process tools")
        return lsp

    def _compute_model_context_bytes(self) -> int | None:
        """Compute model context window in bytes for filesystem size gate."""
        default_tier = self.config.models.default
        model_cfg = self.config.models.tiers.get(default_tier)
        if model_cfg and model_cfg.context_length:
            return int(model_cfg.context_length) * 4
        return None

    def get_permission_pattern(
        self,
        tool_name: str,
        arguments: dict[str, Any],
    ) -> str:
        """Generate permission pattern via server class inheritance.

        Delegates to the server class's get_permission_pattern().
        Falls back to BaseMCPServer default (tool-level) for
        external or unregistered servers.
        """
        prefix = tool_name.split(".")[0]
        server_cls = self._server_classes.get(prefix, BaseMCPServer)
        return server_cls.get_permission_pattern(tool_name, arguments)

    def skip_duplicate_check(self, tool_call: ToolCall) -> bool:
        """Check if a tool should skip duplicate detection.

        Delegates to the server class's skip_duplicate_check().
        """
        prefix = tool_call.name.split(".")[0]
        server_cls = self._server_classes.get(prefix)
        if server_cls:
            local_name = (
                tool_call.name.split(".", 1)[1] if "." in tool_call.name else tool_call.name
            )
            return server_cls.skip_duplicate_check(local_name)
        return False

    async def _safe_connect(self, client: ToolProvider) -> None:
        """Connect to a client/provider, logging errors but not failing."""
        try:
            await client.connect()
        except Exception as e:
            logger.error(f"Failed to connect to {client.name}: {e}")

    async def shutdown(self) -> None:
        """Disconnect from all servers and stop LSP."""
        logger.info("Shutting down MCP servers")

        disconnect_tasks = []
        for client in self._clients.values():
            if client.is_connected:
                disconnect_tasks.append(client.disconnect())

        await asyncio.gather(*disconnect_tasks, return_exceptions=True)

        if self._lsp_manager:
            self._lsp_manager.stop()
            self._lsp_manager = None

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
                logger.debug(f"Server {client.name}: {len(tools)} tools")
                all_tools.extend(tools)
            else:
                logger.debug(f"Server {client.name}: not connected, skipping")

        logger.debug(f"Total tools available: {len(all_tools)}")
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

        Only returns False if tool is EXPLICITLY denied.
        Unknown tools return True - engine handles prompting.

        Args:
            tool_name: Tool name (e.g., "filesystem.read_file")
            pattern: Tool pattern with args (e.g., "filesystem.read_file:/path")

        Returns:
            True unless explicitly denied
        """
        # Only hard-deny if in deny list
        if self._is_denied(tool_name, pattern):
            return False
        # Allow everything else - engine handles prompting for unknown tools
        return True

    def is_explicitly_allowed(self, tool_call: ToolCall) -> bool:
        """
        Check if tool is explicitly in allow list (skip prompting).

        Args:
            tool_call: Tool call to check

        Returns:
            True if explicitly allowed, False if needs prompting
        """
        tool_pattern = f"{tool_call.name}:{self._args_to_pattern(tool_call.arguments)}"
        return self._is_allowed(tool_call.name, tool_pattern)

    def _is_denied(self, tool_name: str, pattern: str) -> bool:
        """Check if tool matches any deny pattern."""
        for deny_pattern in self._permissions.deny:
            if self._pattern_matches(tool_name, pattern, deny_pattern):
                return True
        return False

    def _is_allowed(self, tool_name: str, pattern: str) -> bool:
        """Check if tool matches any allow pattern."""
        for allow_pattern in self._permissions.allow:
            if self._pattern_matches(tool_name, pattern, allow_pattern):
                return True
        return False

    def _pattern_matches(self, tool_name: str, full_pattern: str, permission_pattern: str) -> bool:
        """Check if tool matches a permission pattern."""
        pattern_tool = permission_pattern.split(":")[0]
        if not fnmatch.fnmatch(tool_name, pattern_tool):
            return False
        if ":" not in permission_pattern:
            return True
        return fnmatch.fnmatch(full_pattern, permission_pattern)

    def add_permission(self, pattern: str, allow: bool) -> None:
        """
        Add a permission pattern to the in-memory config.

        Args:
            pattern: Permission pattern (e.g., "bash.execute:python -m venv *")
            allow: True to add to allow list, False to add to deny list
        """
        if allow:
            if pattern not in self._permissions.allow:
                self._permissions.allow.append(pattern)
        else:
            if pattern not in self._permissions.deny:
                self._permissions.deny.append(pattern)

    def get_connected_servers(self) -> list[str]:
        """Get list of connected server names."""
        return [name for name, client in self._clients.items() if client.is_connected]

    def get_all_servers(self) -> list[str]:
        """Get list of all configured server names."""
        return list(self._clients.keys())
