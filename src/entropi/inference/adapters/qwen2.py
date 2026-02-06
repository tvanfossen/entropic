"""
Qwen 2.5 adapter (including Qwen2.5-Coder).

Qwen 2.5 models output tool calls as bare JSON:
{"name": "tool.name", "arguments": {...}}
"""

import json
import re
import uuid
from typing import Any

from entropi.core.base import Message, ToolCall
from entropi.core.logging import get_logger
from entropi.inference.adapters.base import ChatAdapter, register_adapter

logger = get_logger("adapter.qwen2")


class Qwen2Adapter(ChatAdapter):
    """Adapter for Qwen 2.5 models (including Qwen2.5-Coder)."""

    @property
    def chat_format(self) -> str:
        """Get llama-cpp chat format name."""
        return "chatml"

    def convert_tools_to_openai_format(
        self, mcp_tools: list[dict[str, Any]]
    ) -> list[dict[str, Any]]:
        """Convert MCP tool definitions to OpenAI function calling format."""
        return [
            {
                "type": "function",
                "function": {
                    "name": tool.get("name", "unknown"),
                    "description": tool.get("description", ""),
                    "parameters": tool.get("inputSchema", {"type": "object", "properties": {}}),
                },
            }
            for tool in mcp_tools
        ]

    def format_system_prompt(
        self,
        base_prompt: str,
        tools: list[dict[str, Any]] | None = None,
    ) -> str:
        """Format system prompt with identity and tool definitions for Qwen 2.5."""
        # Start with identity (who the assistant is and core behaviors)
        identity = self._get_identity_prompt()
        prompt_parts = [identity]

        # Add user-provided base prompt if any
        if base_prompt:
            prompt_parts.append(base_prompt)

        if not tools:
            return "\n\n".join(prompt_parts)

        # Extract tool prefixes for use in parse_tool_calls
        self._extract_tool_prefixes(tools)

        tools_text = self._format_tools(tools)
        tool_usage = self._get_tool_usage_prompt()

        prompt_parts.append(tool_usage)
        prompt_parts.append(tools_text)

        return "\n\n".join(prompt_parts)

    def _format_tools(self, tools: list[dict[str, Any]]) -> str:
        """Format tool definitions for the prompt."""
        lines = ["## Available Tools\n"]
        for tool in tools:
            name = tool.get("name", "unknown")
            description = tool.get("description", "No description")
            schema = tool.get("inputSchema", {})

            lines.append(f"### {name}")
            lines.append(f"{description}")

            if schema.get("properties"):
                lines.append("Parameters:")
                for param, details in schema["properties"].items():
                    param_type = details.get("type", "any")
                    param_desc = details.get("description", "")
                    required = param in schema.get("required", [])
                    req_marker = " (required)" if required else ""
                    lines.append(f"  - {param} ({param_type}){req_marker}: {param_desc}")

            lines.append("")

        return "\n".join(lines)

    def parse_tool_calls(self, content: str) -> tuple[str, list[ToolCall]]:
        """
        Parse tool calls from Qwen 2.5 output.

        Supports multiple formats:
        1. Bare JSON: {"name": "...", "arguments": {...}}
        2. Markdown JSON: ```json\n{"name": ...}\n```
        3. Function-call syntax: tool_name({...}) or tool_name({"key": "value"})

        Args:
            content: Model output

        Returns:
            Tuple of (cleaned content, tool calls)
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

    def _log_parse_start(self, content: str) -> None:
        """Log debug info at start of parsing."""
        logger.debug("\n=== Parsing tool calls (Qwen2) ===")
        logger.debug(f"Content length: {len(content)}")
        logger.debug(f"Content:\n{content}")

    def _parse_json_lines(self, content: str) -> tuple[list[ToolCall], list[str]]:
        """Parse bare JSON tool calls from lines."""
        tool_calls = []
        lines_to_remove = []
        for line in content.split("\n"):
            stripped = line.strip()
            if not stripped or not (stripped.startswith("{") and '"name"' in stripped):
                continue
            tool_call = self._try_parse_json_tool_call(stripped)
            if tool_call:
                tool_calls.append(tool_call)
                lines_to_remove.append(line)
                logger.info(f"Parsed Qwen2 tool call: {tool_call.name}")
            else:
                recovered = self._try_recover_json(stripped)
                if recovered:
                    tool_calls.append(recovered)
                    lines_to_remove.append(line)
        return tool_calls, lines_to_remove

    def _try_parse_json_tool_call(self, json_str: str) -> ToolCall | None:
        """Try to parse a JSON string as a tool call."""
        try:
            data = json.loads(json_str)
            if "name" in data:
                arguments = data.get("arguments", data.get("parameters", {}))
                return ToolCall(id=str(uuid.uuid4()), name=data["name"], arguments=arguments)
        except json.JSONDecodeError:
            pass
        return None

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
        logger.debug(f"Found code block ({pattern.pattern}): {block}")
        tool_call = self._try_parse_json_tool_call(block)
        if tool_call:
            logger.info(f"Parsed Qwen2 markdown tool call: {tool_call.name}")
            return tool_call
        recovered = self._try_recover_json(block)
        if recovered:
            logger.info(f"Recovered Qwen2 markdown tool call: {recovered.name}")
        return recovered

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
            logger.info(f"Parsed Qwen2 function-call syntax: {func_name}")
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
        arguments = self._parse_python_kwargs(args_str)
        result = None
        if arguments:
            logger.info(f"Parsed Qwen2 Python-style call: {func_name}")
            result = ToolCall(id=str(uuid.uuid4()), name=func_name, arguments=arguments)
        return result

    def _is_valid_python_call(self, func_name: str, args_str: str) -> bool:
        """Check if this looks like a valid Python-style tool call."""
        if "." not in func_name or not self._is_known_tool_prefix(func_name):
            return False
        return "=" in args_str

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

    def _log_parse_result(self, content: str, tool_calls: list[ToolCall]) -> None:
        """Log parsing results."""
        if tool_calls:
            logger.info(f"Parsed {len(tool_calls)} tool calls: {[tc.name for tc in tool_calls]}")
        else:
            logger.debug("No tool calls parsed")
            logger.debug(f"  - Contains '{{': {'{' in content}")
            logger.debug(f"  - Contains '\"name\"': {'\"name\"' in content}")

    def _try_recover_json(self, json_str: str) -> ToolCall | None:
        """Attempt to recover malformed JSON."""
        # Fix common issues
        fixed = json_str
        fixed = re.sub(r",\s*}", "}", fixed)
        fixed = re.sub(r",\s*]", "]", fixed)
        fixed = fixed.replace("'", '"')

        try:
            data = json.loads(fixed)
            if "name" in data:
                arguments = data.get("arguments", data.get("parameters", {}))
                return ToolCall(
                    id=str(uuid.uuid4()),
                    name=data["name"],
                    arguments=arguments,
                )
        except json.JSONDecodeError:
            pass

        # Extract with regex as last resort
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

    def _parse_python_kwargs(self, args_str: str) -> dict[str, Any] | None:
        """
        Parse Python-style keyword arguments into a dict.

        Handles: key="value", key='value', key=123, key=True
        """
        arguments: dict[str, Any] = {}
        kwarg_pattern = re.compile(r'(\w+)\s*=\s*(?:"([^"]*)"|\'([^\']*)\'|(\S+))')
        for match in kwarg_pattern.finditer(args_str):
            key = match.group(1)
            value = match.group(2) or match.group(3) or match.group(4)
            if value is not None:
                arguments[key] = self._convert_kwarg_value(value)
        return arguments if arguments else None

    def _convert_kwarg_value(self, value: str) -> Any:
        """Convert keyword argument value to appropriate Python type."""
        lower = value.lower()
        literal_map = {"true": True, "false": False, "none": None, "null": None}
        if lower in literal_map:
            return literal_map[lower]
        return self._try_numeric_conversion(value)

    def _try_numeric_conversion(self, value: str) -> int | float | str:
        """Try to convert value to int or float, fall back to string."""
        try:
            return int(value)
        except ValueError:
            try:
                return float(value)
            except ValueError:
                return value

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
        # Only if there are no double quotes in the string
        if '"' not in fixed:
            fixed = fixed.replace("'", '"')

        # Fix unquoted keys: {path: "value"} -> {"path": "value"}
        fixed = re.sub(r"{\s*([a-zA-Z_][a-zA-Z0-9_]*)\s*:", r'{"\1":', fixed)
        fixed = re.sub(r",\s*([a-zA-Z_][a-zA-Z0-9_]*)\s*:", r', "\1":', fixed)

        return fixed if fixed != json_str else None

    def format_tool_result(self, tool_call: ToolCall, result: str) -> Message:
        """Format tool result as a user message."""
        content = f"""Tool `{tool_call.name}` returned:

{result}

Continue with the task. Call more tools if needed, or respond when complete."""

        return Message(role="user", content=content)

    def is_response_complete(self, content: str, tool_calls: list[ToolCall]) -> bool:
        """
        Determine if this response represents task completion for Qwen2.

        For Qwen2 (no think blocks), we check:
        1. If tool calls were parsed, not complete
        2. If content contains unparsed tool call patterns for KNOWN tools, not complete
        3. Otherwise, this is the final response

        Args:
            content: Model output content
            tool_calls: Parsed tool calls

        Returns:
            True if this is a final response, False if model is still working
        """
        # If there are tool calls, not complete
        if tool_calls:
            return False

        # Check for unparsed tool call indicators in code blocks
        if "```" in content:
            if self._has_unparsed_tool_calls(content):
                return False

        return True

    def _has_unparsed_tool_calls(self, content: str) -> bool:
        """Check if content has unparsed tool calls in code blocks."""
        code_block_pattern = re.compile(r"```\w*\s*([\s\S]*?)```")
        python_call_pattern = re.compile(
            r"([a-zA-Z_][a-zA-Z0-9_.]*\.[a-zA-Z_][a-zA-Z0-9_]*)\s*\([^)]*\)"
        )

        for block in code_block_pattern.findall(content):
            block_stripped = block.strip()

            # Check for JSON-style tool call
            if block_stripped.startswith("{") and '"name"' in block_stripped:
                logger.debug("Found unparsed JSON tool call in code block - continuing loop")
                return True

            # Check for Python-style function call matching known tools
            for match in python_call_pattern.finditer(block_stripped):
                func_name = match.group(1)
                if self._is_known_tool_prefix(func_name):
                    logger.debug(f"Found unparsed tool call: {func_name} - continuing loop")
                    return True

        return False


# Register adapter
register_adapter("qwen2", Qwen2Adapter)
