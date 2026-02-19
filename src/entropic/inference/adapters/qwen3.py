"""
Qwen 3 adapter.

Qwen 3 models use:
- <tool_call>{"name": "...", "arguments": {...}}</tool_call> for tool calls
- <think>...</think> blocks for reasoning (thinking mode)
- Shell-style tool calls in code blocks (fallback)
"""

import re
import uuid

from entropic.core.base import ToolCall
from entropic.inference.adapters.base import ChatAdapter, register_adapter


class Qwen3Adapter(ChatAdapter):
    """Adapter for Qwen 3 models.

    Inherits shared parsing, logging, think-block handling, and completion
    detection from ChatAdapter. Adds shell-style tool call parsing as a
    Qwen3-specific fallback.
    """

    @property
    def chat_format(self) -> str:
        """Get llama-cpp chat format name."""
        return "chatml"

    def parse_tool_calls(self, content: str) -> tuple[str, list[ToolCall]]:
        """Parse tool calls from Qwen 3 output.

        Tries in order: tagged, bare JSON, shell-style (Qwen3-specific fallback).
        """
        self._log_parse_start(content)
        self._log_thinking_content(content)

        tool_calls = self._parse_tagged_tool_calls(content)
        if not tool_calls:
            tool_calls = self._parse_bare_json_tool_calls(content)
        if not tool_calls:
            tool_calls = self._parse_shell_style_tool_calls(content)

        cleaned = self._clean_content(content, tool_calls)
        self._log_parse_result(content, tool_calls)

        return cleaned, tool_calls

    # --- Qwen3-specific: shell-style tool call parsing ---

    def _parse_shell_style_tool_calls(self, content: str) -> list[ToolCall]:
        """Parse shell-style tool calls from code blocks."""
        tool_calls = []
        code_block_pattern = re.compile(r"```\w*\s*\n?([\s\S]*?)\n?```", re.MULTILINE)
        shell_pattern = re.compile(r"^([a-zA-Z_][a-zA-Z0-9_.]*\.[a-zA-Z_][a-zA-Z0-9_]*)\s+(.+)$")
        for block in code_block_pattern.findall(content):
            tool_call = self._try_parse_shell_block(block.strip(), shell_pattern)
            if tool_call:
                tool_calls.append(tool_call)
        return tool_calls

    def _try_parse_shell_block(self, block: str, shell_pattern: re.Pattern[str]) -> ToolCall | None:
        """Try to parse a shell-style tool call from a code block."""
        match = shell_pattern.match(block)
        if not match:
            return None
        tool_name, args_str = match.group(1), match.group(2)
        if not self._is_known_tool_prefix(tool_name):
            return None
        arguments = self._parse_key_value_args(args_str)
        result = None
        if arguments is not None:
            self._logger.info(f"Parsed {type(self).__name__} shell-style tool call: {tool_name}")
            result = ToolCall(id=str(uuid.uuid4()), name=tool_name, arguments=arguments)
        return result

    # --- Qwen3-specific: content cleaning with shell block removal ---

    def _clean_content(self, content: str, tool_calls: list[ToolCall]) -> str:
        """Remove tool calls, think blocks, and shell blocks from content."""
        tool_pattern = re.compile(
            rf"{re.escape(self.TOOL_CALL_START)}\s*(.*?)\s*{re.escape(self.TOOL_CALL_END)}",
            re.DOTALL,
        )
        cleaned = tool_pattern.sub("", content).strip()
        cleaned = self._strip_think_blocks(cleaned)
        if tool_calls:
            cleaned = self._remove_bare_json_lines(cleaned)
            cleaned = self._remove_shell_blocks(cleaned)
        return cleaned

    def _remove_shell_blocks(self, content: str) -> str:
        """Remove shell-style code blocks."""
        shell_block_pattern = re.compile(
            r"```\w*\s*\n?[a-zA-Z_][a-zA-Z0-9_.]*\.[a-zA-Z_][a-zA-Z0-9_]*\s+\S.*?\n?```",
            re.DOTALL,
        )
        return shell_block_pattern.sub("", content).strip()

    # --- Qwen3-specific: unparsed tool call detection (adds shell-style) ---

    def _has_unparsed_tool_calls(self, content: str) -> bool:
        """Check for unparsed tool call indicators including shell-style."""
        if "```" not in content:
            return False
        code_block_pattern = re.compile(r"```\w*\s*\n?([\s\S]*?)\n?```", re.MULTILINE)
        shell_pattern = re.compile(r"^[a-zA-Z_][a-zA-Z0-9_.]*\.[a-zA-Z_][a-zA-Z0-9_]*[^\S\n]+\S")
        for block in code_block_pattern.findall(content):
            if self._is_unparsed_tool_block(block.strip(), shell_pattern):
                return True
        return False

    def _is_unparsed_tool_block(self, block: str, shell_pattern: re.Pattern[str]) -> bool:
        """Check if a code block contains an unparsed tool call."""
        if block.startswith("{") and '"name"' in block:
            self._logger.debug("Found unparsed JSON tool call in code block - continuing loop")
            return True
        if shell_pattern.match(block):
            self._logger.debug(
                "Found unparsed shell-style tool call in code block - continuing loop"
            )
            return True
        return False


# Register adapter
register_adapter("qwen3", Qwen3Adapter)
