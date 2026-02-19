"""
Qwen 2.5 adapter (including Qwen2.5-Coder).

Qwen 2.5 models output tool calls as bare JSON:
{"name": "tool.name", "arguments": {...}}
"""

import json
import re
import uuid
from typing import Any

from entropi.core.base import ToolCall
from entropi.inference.adapters.base import ChatAdapter, register_adapter


class Qwen2Adapter(ChatAdapter):
    """Adapter for Qwen 2.5 models (including Qwen2.5-Coder).

    Inherits shared JSON recovery, logging, and value conversion from
    ChatAdapter. Overrides completion detection (no think blocks) and
    adds markdown/function-call/Python-style parsing unique to Qwen2.
    """

    @property
    def chat_format(self) -> str:
        """Get llama-cpp chat format name."""
        return "chatml"

    def parse_tool_calls(self, content: str) -> tuple[str, list[ToolCall]]:
        """Parse tool calls from Qwen 2.5 output.

        Tries in order: bare JSON lines, markdown blocks, function-call syntax,
        Python-style kwargs.
        """
        self._log_parse_start(content)

        tool_calls, lines_to_remove = self._parse_json_lines(content)
        if not tool_calls:
            tool_calls = self._parse_markdown_blocks(content)
        if not tool_calls:
            tool_calls = self._parse_function_call_syntax(content)
        if not tool_calls:
            tool_calls = self._parse_python_style_calls(content)

        cleaned = self._clean_content(content, tool_calls, lines_to_remove)
        self._log_parse_result(content, tool_calls)

        return cleaned, tool_calls

    # --- Qwen2-specific: JSON line parsing (returns lines_to_remove) ---

    def _parse_json_lines(self, content: str) -> tuple[list[ToolCall], list[str]]:
        """Parse bare JSON tool calls from lines, tracking which to remove."""
        tool_calls = []
        lines_to_remove = []
        for line in content.split("\n"):
            stripped = line.strip()
            if not stripped or not (stripped.startswith("{") and '"name"' in stripped):
                continue
            tool_call = self._parse_single_tool_call(stripped)
            if tool_call:
                tool_calls.append(tool_call)
                lines_to_remove.append(line)
                self._logger.info(f"Parsed {type(self).__name__} tool call: {tool_call.name}")
        return tool_calls, lines_to_remove

    # --- Qwen2-specific: markdown block parsing ---

    def _parse_markdown_blocks(self, content: str) -> list[ToolCall]:
        """Parse tool calls from markdown code blocks."""
        tool_calls = []
        patterns = self._get_code_block_patterns()
        for pattern in patterns:
            if tool_calls:
                break
            for block_content in pattern.findall(content):
                tool_call = self._try_parse_code_block(block_content.strip(), pattern)
                if tool_call:
                    tool_calls.append(tool_call)
        return tool_calls

    def _get_code_block_patterns(self) -> list[re.Pattern[str]]:
        """Get patterns for matching code blocks."""
        return [
            re.compile(r"```\w*\s*\n([\s\S]*?)\n```", re.MULTILINE),
            re.compile(r"```\w*\s*\n([\s\S]*?)```", re.MULTILINE),
            re.compile(r"```\w*\s+(\{[^`]*\})```", re.MULTILINE),
            re.compile(r"```(?:\w*\s*)?([\s\S]*?)```", re.MULTILINE),
        ]

    def _try_parse_code_block(self, block: str, pattern: re.Pattern[str]) -> ToolCall | None:
        """Try to parse a tool call from a code block."""
        if not block or not (block.startswith("{") and '"name"' in block):
            return None
        self._logger.debug(f"Found code block ({pattern.pattern}): {block}")
        tool_call = self._parse_single_tool_call(block)
        if tool_call:
            self._logger.info(f"Parsed {type(self).__name__} markdown tool call: {tool_call.name}")
        return tool_call

    # --- Qwen2-specific: function-call syntax ---

    def _parse_function_call_syntax(self, content: str) -> list[ToolCall]:
        """Parse function-call syntax: tool_name({...})."""
        tool_calls = []
        pattern = re.compile(r"([a-zA-Z_][a-zA-Z0-9_.]*)\s*\(\s*(\{.*?\})\s*\)", re.DOTALL)
        for match in pattern.finditer(content):
            tool_call = self._try_parse_func_call_match(match)
            if tool_call:
                tool_calls.append(tool_call)
        return tool_calls

    def _try_parse_func_call_match(self, match: re.Match[str]) -> ToolCall | None:
        """Try to parse a function call match."""
        func_name, args_json = match.group(1), match.group(2)
        if not self._is_valid_func_call_args(args_json):
            return None
        arguments = self._try_parse_func_arguments(args_json)
        result = None
        if arguments is not None:
            self._logger.info(f"Parsed {type(self).__name__} function-call syntax: {func_name}")
            result = ToolCall(id=str(uuid.uuid4()), name=func_name, arguments=arguments)
        return result

    def _is_valid_func_call_args(self, args_json: str) -> bool:
        """Check if function call arguments are valid for parsing."""
        if args_json.count("{") != args_json.count("}"):
            return False
        if "=" in args_json and ":" not in args_json:
            return False
        return True

    def _try_parse_func_arguments(self, args_json: str) -> dict[str, Any] | None:
        """Try to parse function arguments as JSON."""
        try:
            arguments = json.loads(args_json)
            if isinstance(arguments, dict):
                return arguments
        except json.JSONDecodeError:
            fixed_args = self._fix_json_syntax(args_json)
            if fixed_args:
                try:
                    arguments = json.loads(fixed_args)
                    if isinstance(arguments, dict):
                        return arguments
                except json.JSONDecodeError:
                    pass
        return None

    # --- Qwen2-specific: Python-style kwargs ---

    def _parse_python_style_calls(self, content: str) -> list[ToolCall]:
        """Parse Python-style function calls: tool_name(key="value")."""
        tool_calls = []
        pattern = re.compile(r"([a-zA-Z_][a-zA-Z0-9_.]*)\s*\(\s*([^)]+)\s*\)", re.DOTALL)
        for match in pattern.finditer(content):
            tool_call = self._try_parse_python_call_match(match)
            if tool_call:
                tool_calls.append(tool_call)
        return tool_calls

    def _try_parse_python_call_match(self, match: re.Match[str]) -> ToolCall | None:
        """Try to parse a Python-style call match."""
        func_name, args_str = match.group(1), match.group(2).strip()
        if not self._is_valid_python_call(func_name, args_str):
            return None
        arguments = self._parse_key_value_args(args_str)
        result = None
        if arguments:
            self._logger.info(f"Parsed {type(self).__name__} Python-style call: {func_name}")
            result = ToolCall(id=str(uuid.uuid4()), name=func_name, arguments=arguments)
        return result

    def _is_valid_python_call(self, func_name: str, args_str: str) -> bool:
        """Check if this looks like a valid Python-style tool call."""
        if "." not in func_name or not self._is_known_tool_prefix(func_name):
            return False
        return "=" in args_str

    # --- Qwen2-specific: content cleaning ---

    def _clean_content(
        self, content: str, tool_calls: list[ToolCall], lines_to_remove: list[str]
    ) -> str:
        """Remove tool call artifacts from content."""
        cleaned = content
        for line in lines_to_remove:
            cleaned = cleaned.replace(line, "")
        if tool_calls:
            cleaned = self._remove_markdown_tool_blocks(cleaned)
            cleaned = self._remove_function_call_blocks(cleaned, tool_calls)
        return cleaned.strip()

    def _remove_markdown_tool_blocks(self, content: str) -> str:
        """Remove markdown blocks containing tool calls."""
        pattern = re.compile(
            r"```(?:\w+)?\s*\n?\s*\{[^`]*\"name\"\s*:\s*\"[^\"]+\"[^`]*\}\s*\n?\s*```",
            re.DOTALL,
        )
        return pattern.sub("", content)

    def _remove_function_call_blocks(self, content: str, tool_calls: list[ToolCall]) -> str:
        """Remove function call syntax from content."""
        cleaned = content
        func_block_pattern = re.compile(
            r"```(?:\w+)?\s*\n[^`]*?[a-zA-Z_][a-zA-Z0-9_.]*\s*\(\s*\{[^}]*\}\s*\)[^`]*```",
            re.DOTALL,
        )
        cleaned = func_block_pattern.sub("", cleaned)
        for tc in tool_calls:
            cleaned = self._remove_tool_call_patterns(cleaned, tc.name)
        tool_block_pattern = re.compile(
            r"```\w*\s*\n?[^`]*?[a-zA-Z_][a-zA-Z0-9_.]*\.[a-zA-Z_][a-zA-Z0-9_]*\s*\([^)]*\)[^`]*?\n?```",
            re.DOTALL,
        )
        cleaned = tool_block_pattern.sub("", cleaned)
        return cleaned

    def _remove_tool_call_patterns(self, content: str, tool_name: str) -> str:
        """Remove patterns for a specific tool call."""
        json_pattern = re.compile(re.escape(tool_name) + r"\s*\(\s*\{[^}]*\}\s*\)", re.DOTALL)
        content = json_pattern.sub("", content)
        kwarg_pattern = re.compile(re.escape(tool_name) + r"\s*\([^)]*\)", re.DOTALL)
        return kwarg_pattern.sub("", content)

    # --- Qwen2-specific: JSON syntax fixing ---

    def _fix_json_syntax(self, json_str: str) -> str | None:
        """Fix common JSON syntax issues from model output."""
        fixed = json_str.strip()

        # Replace JavaScript-style booleans/null
        fixed = re.sub(r"\bfalse\b", "false", fixed)
        fixed = re.sub(r"\btrue\b", "true", fixed)
        fixed = re.sub(r"\bnull\b", "null", fixed)

        # Fix trailing commas
        fixed = re.sub(r",\s*}", "}", fixed)
        fixed = re.sub(r",\s*]", "]", fixed)

        # Fix single quotes to double quotes (carefully)
        if '"' not in fixed:
            fixed = fixed.replace("'", '"')

        # Fix unquoted keys: {path: "value"} -> {"path": "value"}
        fixed = re.sub(r"{\s*([a-zA-Z_][a-zA-Z0-9_]*)\s*:", r'{"\1":', fixed)
        fixed = re.sub(r",\s*([a-zA-Z_][a-zA-Z0-9_]*)\s*:", r', "\1":', fixed)

        return fixed if fixed != json_str else None

    # --- Qwen2-specific: response completion (no think blocks) ---

    def is_response_complete(self, content: str, tool_calls: list[ToolCall]) -> bool:
        """Determine if response is complete for Qwen2 (no think blocks)."""
        if tool_calls:
            return False
        if "```" in content and self._has_unparsed_tool_calls(content):
            return False
        return True

    def _has_unparsed_tool_calls(self, content: str) -> bool:
        """Check for unparsed tool calls including Python-style in code blocks."""
        code_block_pattern = re.compile(r"```\w*\s*([\s\S]*?)```")
        python_call_pattern = re.compile(
            r"([a-zA-Z_][a-zA-Z0-9_.]*\.[a-zA-Z_][a-zA-Z0-9_]*)\s*\([^)]*\)"
        )

        for block in code_block_pattern.findall(content):
            block_stripped = block.strip()

            if block_stripped.startswith("{") and '"name"' in block_stripped:
                self._logger.debug("Found unparsed JSON tool call in code block - continuing loop")
                return True

            for match in python_call_pattern.finditer(block_stripped):
                func_name = match.group(1)
                if self._is_known_tool_prefix(func_name):
                    self._logger.debug(f"Found unparsed tool call: {func_name} - continuing loop")
                    return True

        return False


# Register adapter
register_adapter("qwen2", Qwen2Adapter)
