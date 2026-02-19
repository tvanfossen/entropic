"""
Falcon H1R adapter.

Falcon H1R models use:
- ChatML format (<|im_start|>, <|im_end|>)
- <tool_call>{"name": "...", "arguments": {...}}</tool_call> for tool calls
- <think>...</think> blocks for reasoning
"""

import re

from entropi.core.base import ToolCall
from entropi.inference.adapters.base import ChatAdapter, register_adapter


class FalconAdapter(ChatAdapter):
    """Adapter for Falcon H1R models.

    Inherits shared parsing, logging, think-block handling, and completion
    detection from ChatAdapter. Falcon uses tagged and bare JSON tool calls
    (no shell-style fallback).
    """

    @property
    def chat_format(self) -> str:
        """Get llama-cpp chat format name."""
        return "chatml"

    def parse_tool_calls(self, content: str) -> tuple[str, list[ToolCall]]:
        """Parse tool calls from Falcon output.

        Tries in order: tagged, bare JSON.
        """
        self._log_parse_start(content)
        self._log_thinking_content(content)

        tool_calls = self._parse_tagged_tool_calls(content)
        if not tool_calls:
            tool_calls = self._parse_bare_json_tool_calls(content)

        cleaned = self._clean_content(content, tool_calls)
        self._log_parse_result(content, tool_calls)

        return cleaned, tool_calls

    def _clean_content(self, content: str, tool_calls: list[ToolCall]) -> str:
        """Remove tool calls and think blocks from content."""
        tool_pattern = re.compile(
            rf"{re.escape(self.TOOL_CALL_START)}\s*(.*?)\s*{re.escape(self.TOOL_CALL_END)}",
            re.DOTALL,
        )
        cleaned = tool_pattern.sub("", content).strip()
        cleaned = self._strip_think_blocks(cleaned)
        if tool_calls:
            cleaned = self._remove_bare_json_lines(cleaned)
        return cleaned


# Register adapter
register_adapter("falcon", FalconAdapter)
