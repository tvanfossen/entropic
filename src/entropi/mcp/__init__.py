"""MCP (Model Context Protocol) module for Entropi."""

from entropi.mcp.client import MCPClient
from entropi.mcp.manager import PermissionDenied, ServerManager

__all__ = [
    "MCPClient",
    "PermissionDenied",
    "ServerManager",
]
