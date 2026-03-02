"""
Qwen 3.5 MoE adapter.

Qwen 3.5 models use:
- XML-style tool calls: <tool_call><function=name><parameter=key>value</parameter></function></tool_call>
- Tool definitions in <tools> tags with JSON
- <think>...</think> blocks for reasoning (controlled via chat template enable_thinking param)
- <tool_response>...</tool_response> for tool results

Key differences from Qwen3Adapter (Qwen 3 dense):
- Tool call OUTPUT format is XML (<function=name><parameter=key>value</parameter></function>),
  not JSON ({"name": "...", "arguments": {...}})
- Tool definitions are wrapped in <tools> tags
- Think control via chat template `enable_thinking` parameter, not /no-think suffix
- Tool results use <tool_response> tags
"""

import json
import re
import uuid
from typing import Any

from entropic.core.base import Message, ToolCall
from entropic.inference.adapters.base import (
    TOOL_RESULT_SUFFIX,
    ChatAdapter,
    register_adapter,
)


class Qwen35Adapter(ChatAdapter):
    """Adapter for Qwen 3.5 MoE models.

    Inherits think-block handling, logging, and completion detection from
    ChatAdapter. Overrides tool call parsing (XML format), tool formatting
    (<tools> tags), and tool result formatting (<tool_response> tags).
    """

    @property
    def chat_format(self) -> str:
        """Get llama-cpp chat format name."""
        return "chatml"

    # --- Tool definition formatting ---

    def _format_tools(self, tools: list[dict[str, Any]]) -> str:
        """Format tool definitions using <tools> tags with XML call instructions.

        Matches the format expected by the Qwen3.5 chat template: JSON tool
        definitions inside <tools> tags, with XML call format instructions.
        """
        tool_defs = [self._tool_to_openai_function(t) for t in tools]

        lines = [
            "# Tools\n",
            "You may call one or more functions to assist with the user query.",
            "Put your final answer OUTSIDE of any tool calls.\n",
            "Here are the available tools:",
            "<tools>",
            json.dumps(tool_defs, indent=2),
            "</tools>\n",
            "For each function call, return within <tool_call></tool_call> XML tags:",
            "<tool_call>",
            "<function=example_function>",
            "<parameter=param_name>value</parameter>",
            "</function>",
            "</tool_call>",
        ]
        return "\n".join(lines)

    def _tool_to_openai_function(self, tool: dict[str, Any]) -> dict[str, Any]:
        """Convert an MCP tool definition to OpenAI function format."""
        return {
            "type": "function",
            "function": {
                "name": tool.get("name", "unknown"),
                "description": tool.get("description", ""),
                "parameters": tool.get("inputSchema", {}),
            },
        }

    # --- Tool call parsing (XML format) ---

    def parse_tool_calls(self, content: str) -> tuple[str, list[ToolCall]]:
        """Parse tool calls from Qwen 3.5 output.

        Primary: XML function calls (<function=name><parameter=key>value</parameter>).
        Fallback: tagged JSON (base class), for robustness.
        """
        self._log_parse_start(content)
        self._log_thinking_content(content)

        tool_calls = self._parse_xml_function_calls(content)
        if not tool_calls:
            tool_calls = self._parse_tagged_tool_calls(content)

        cleaned = self._clean_content(content)
        self._log_parse_result(content, tool_calls)

        return cleaned, tool_calls

    def _parse_xml_function_calls(self, content: str) -> list[ToolCall]:
        """Parse XML-style function calls from content.

        Format: <function=name><parameter=key>value</parameter></function>
        Optionally wrapped in <tool_call>...</tool_call> tags.
        """
        tool_calls: list[ToolCall] = []
        func_pattern = re.compile(r"<function=([^>]+)>(.*?)</function>", re.DOTALL)

        for func_match in func_pattern.finditer(content):
            func_name = func_match.group(1).strip()
            func_body = func_match.group(2)
            arguments = self._extract_xml_parameters(func_body)

            tool_call = ToolCall(
                id=str(uuid.uuid4()),
                name=func_name,
                arguments=arguments,
            )
            tool_calls.append(tool_call)
            self._logger.info(f"Parsed {type(self).__name__} XML tool call: {func_name}")

        return tool_calls

    def _extract_xml_parameters(self, func_body: str) -> dict[str, Any]:
        """Extract parameter key-value pairs from XML function body."""
        arguments: dict[str, Any] = {}
        param_pattern = re.compile(r"<parameter=([^>]+)>(.*?)</parameter>", re.DOTALL)

        for param_match in param_pattern.finditer(func_body):
            key = param_match.group(1).strip()
            value = param_match.group(2).strip()
            arguments[key] = self._convert_typed_value(value)

        return arguments

    # --- Tool result formatting ---

    def format_tool_result(self, tool_call: ToolCall, result: str) -> Message:
        """Format tool result using <tool_response> tags.

        Matches the Qwen3.5 chat template which wraps tool results
        in <tool_response>...</tool_response> inside a user message.
        """
        content = f"<tool_response>\n{result}\n</tool_response>\n\n{TOOL_RESULT_SUFFIX}"
        return Message(role="user", content=content)

    # --- Content cleaning ---

    def _clean_content(self, content: str) -> str:
        """Remove tool calls and think blocks from content."""
        # Remove <tool_call>...</tool_call> blocks (may contain XML function calls)
        tool_call_pattern = re.compile(r"<tool_call>\s*.*?\s*</tool_call>", re.DOTALL)
        cleaned = tool_call_pattern.sub("", content)

        # Remove standalone <function=...>...</function> not wrapped in tool_call tags
        func_pattern = re.compile(r"<function=[^>]+>.*?</function>", re.DOTALL)
        cleaned = func_pattern.sub("", cleaned)

        cleaned = self._strip_think_blocks(cleaned)
        return cleaned.strip()

    # --- Unparsed tool call detection ---

    def _has_unparsed_tool_calls(self, content: str) -> bool:
        """Check for unparsed XML function calls or JSON tool calls."""
        if "<function=" in content:
            return True
        return super()._has_unparsed_tool_calls(content)


# Register adapter
register_adapter("qwen35", Qwen35Adapter)
