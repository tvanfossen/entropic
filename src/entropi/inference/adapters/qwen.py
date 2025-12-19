"""
Qwen-specific chat adapter.

Handles Qwen's ChatML format and tool call conventions.
"""

import json
import re
import uuid
from typing import Any

from entropi.core.base import Message, ToolCall
from entropi.inference.adapters.base import ChatAdapter


class QwenAdapter(ChatAdapter):
    """Adapter for Qwen models."""

    # Qwen tool call markers
    TOOL_CALL_START = "<tool_call>"
    TOOL_CALL_END = "</tool_call>"

    @property
    def chat_format(self) -> str:
        """Get llama-cpp chat format name."""
        return "chatml"

    def format_system_prompt(
        self,
        base_prompt: str,
        tools: list[dict[str, Any]] | None = None,
    ) -> str:
        """
        Format system prompt with tool definitions.

        Args:
            base_prompt: Base system prompt
            tools: Tool definitions

        Returns:
            Formatted system prompt
        """
        if not tools:
            return base_prompt

        # Format tools in Qwen's expected format
        tools_text = self._format_tools(tools)

        return f"""{base_prompt}

# Available Tools

You have access to the following tools. To use a tool, respond with a tool call in this format:

{self.TOOL_CALL_START}
{{"name": "tool_name", "arguments": {{"arg1": "value1"}}}}
{self.TOOL_CALL_END}

{tools_text}

After receiving tool results, continue your response or make additional tool calls as needed.
"""

    def _format_tools(self, tools: list[dict[str, Any]]) -> str:
        """Format tool definitions for the prompt."""
        lines = []
        for tool in tools:
            name = tool.get("name", "unknown")
            description = tool.get("description", "No description")
            schema = tool.get("inputSchema", {})

            lines.append(f"## {name}")
            lines.append(f"{description}")

            if schema.get("properties"):
                lines.append("\nParameters:")
                for param, details in schema["properties"].items():
                    param_type = details.get("type", "any")
                    param_desc = details.get("description", "")
                    required = param in schema.get("required", [])
                    req_marker = " (required)" if required else ""
                    lines.append(f"- {param} ({param_type}){req_marker}: {param_desc}")

            lines.append("")

        return "\n".join(lines)

    def parse_tool_calls(self, content: str) -> tuple[str, list[ToolCall]]:
        """
        Parse tool calls from Qwen output.

        Args:
            content: Model output

        Returns:
            Tuple of (cleaned content, tool calls)
        """
        tool_calls = []

        # Find all tool call blocks
        pattern = re.compile(
            rf"{re.escape(self.TOOL_CALL_START)}\s*(.*?)\s*{re.escape(self.TOOL_CALL_END)}",
            re.DOTALL,
        )

        matches = pattern.findall(content)

        for match in matches:
            try:
                # Parse JSON
                data = json.loads(match.strip())
                tool_call = ToolCall(
                    id=str(uuid.uuid4()),
                    name=data.get("name", ""),
                    arguments=data.get("arguments", {}),
                )
                tool_calls.append(tool_call)
            except json.JSONDecodeError:
                # Try to recover with fallback parsing
                recovered = self._try_recover_json(match.strip())
                if recovered:
                    tool_calls.append(recovered)

        # Remove tool call blocks from content
        cleaned = pattern.sub("", content).strip()

        return cleaned, tool_calls

    def _try_recover_json(self, json_str: str) -> ToolCall | None:
        """
        Attempt to recover malformed JSON.

        Args:
            json_str: Potentially malformed JSON string

        Returns:
            ToolCall if recovery successful, None otherwise
        """
        # Strategy 1: Fix common issues
        fixed = json_str
        # Remove trailing commas
        fixed = re.sub(r",\s*}", "}", fixed)
        fixed = re.sub(r",\s*]", "]", fixed)
        # Fix single quotes
        fixed = fixed.replace("'", '"')

        try:
            data = json.loads(fixed)
            if "name" in data:
                return ToolCall(
                    id=str(uuid.uuid4()),
                    name=data["name"],
                    arguments=data.get("arguments", {}),
                )
        except json.JSONDecodeError:
            pass

        # Strategy 2: Extract with regex as last resort
        name_match = re.search(r'"name"\s*:\s*"([^"]+)"', json_str)
        if name_match:
            name = name_match.group(1)
            arguments = {}

            args_match = re.search(r'"arguments"\s*:\s*(\{[^}]+\})', json_str)
            if args_match:
                try:
                    arguments = json.loads(args_match.group(1))
                except json.JSONDecodeError:
                    pass

            return ToolCall(
                id=str(uuid.uuid4()),
                name=name,
                arguments=arguments,
            )

        return None

    def format_tool_result(self, tool_call: ToolCall, result: str) -> Message:
        """
        Format tool result for conversation.

        Args:
            tool_call: Original tool call
            result: Tool execution result

        Returns:
            Message to inject
        """
        content = f"""<tool_result>
Tool: {tool_call.name}
Call ID: {tool_call.id}
Result:
{result}
</tool_result>"""

        return Message(
            role="tool",
            content=content,
            tool_results=[
                {
                    "call_id": tool_call.id,
                    "name": tool_call.name,
                    "result": result,
                }
            ],
        )
