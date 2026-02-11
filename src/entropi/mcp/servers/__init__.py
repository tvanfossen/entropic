"""Built-in MCP servers for Entropi."""

from entropi.mcp.servers.base import BaseMCPServer, load_tool_definition
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
    "load_tool_definition",
]
