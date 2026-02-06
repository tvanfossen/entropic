"""
System MCP server.

Provides system-level operations including tier handoff for multi-model orchestration.
"""

import asyncio
import json
from typing import Any

from mcp.types import Tool

from entropi.mcp.servers.base import BaseMCPServer, create_tool, load_tool_description


class SystemServer(BaseMCPServer):
    """System operations MCP server."""

    def __init__(self) -> None:
        """Initialize system server."""
        super().__init__("system")

    def get_tools(self) -> list[Tool]:
        """Get available system tools."""
        return [
            create_tool(
                name="handoff",
                description=load_tool_description("system_handoff"),
                properties={
                    "target_tier": {
                        "type": "string",
                        "enum": ["simple", "normal", "code", "thinking"],
                        "description": "Target model tier to hand off to",
                    },
                    "reason": {
                        "type": "string",
                        "description": "Brief explanation of why handoff is needed",
                    },
                    "task_state": {
                        "type": "string",
                        "enum": ["not_started", "in_progress", "blocked", "plan_ready"],
                        "description": "Current state of the task",
                    },
                },
                required=["target_tier", "reason", "task_state"],
            ),
        ]

    async def execute_tool(self, name: str, arguments: dict[str, Any]) -> str:
        """Execute a system tool."""
        if name == "handoff":
            return await self._execute_handoff(arguments)
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
