"""
System MCP server.

Provides system-level operations including tier handoff for multi-model orchestration.
"""

import asyncio
import json
from typing import Any

from mcp.types import Tool

from entropi.mcp.servers.base import BaseMCPServer, load_tool_definition


class SystemServer(BaseMCPServer):
    """System operations MCP server."""

    def __init__(self) -> None:
        """Initialize system server."""
        super().__init__("system")

    def get_tools(self) -> list[Tool]:
        """Get available system tools."""
        return [
            load_tool_definition("handoff", "system"),
            load_tool_definition("prune_context", "system"),
        ]

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """Execute a system tool."""
        if name == "handoff":
            return await self._execute_handoff(arguments)
        if name == "prune_context":
            return json.dumps({"note": "Handled internally by engine"})
        return json.dumps({"error": f"Unknown tool: {name}"})

    async def _execute_handoff(self, arguments: dict[str, Any]) -> str:
        """
        Execute handoff tool.

        Returns a structured response that the engine intercepts to trigger
        the actual tier switch. The MCP server signals intent; the engine
        validates routing rules and performs the switch.
        """
        target_tier = arguments.get("target_tier", "")
        reason = arguments.get("reason", "")
        task_state = arguments.get("task_state", "in_progress")

        return json.dumps(
            {
                "handoff_request": True,
                "target_tier": target_tier,
                "reason": reason,
                "task_state": task_state,
            }
        )


async def main() -> None:
    """Run the system server."""
    server = SystemServer()
    await server.run()


if __name__ == "__main__":
    asyncio.run(main())
