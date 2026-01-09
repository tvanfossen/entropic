"""Built-in MCP servers for Entropi."""

from entropi.mcp.servers.base import BaseMCPServer, create_tool
from entropi.mcp.servers.bash import BashServer
from entropi.mcp.servers.diagnostics import DiagnosticsServer
from entropi.mcp.servers.filesystem import FilesystemServer
from entropi.mcp.servers.git import GitServer

__all__ = [
    "BaseMCPServer",
    "BashServer",
    "DiagnosticsServer",
    "FilesystemServer",
    "GitServer",
    "create_tool",
]
