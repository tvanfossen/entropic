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
        logger.debug("\n=== Parsing tool calls (Qwen2) ===")
        logger.debug(f"Content length: {len(content)}")
        logger.debug(f"Content:\n{content}")

        tool_calls = []
        lines_to_remove = []

        # Parse each line looking for JSON tool calls
        for line in content.split("\n"):
            stripped = line.strip()
            if not stripped:
                continue

            # Check if line looks like a JSON tool call
            if stripped.startswith("{") and '"name"' in stripped:
                try:
                    data = json.loads(stripped)
                    if "name" in data:
                        # Handle both {"name": ..., "arguments": ...}
                        # and {"name": ..., "parameters": ...} formats
                        arguments = data.get("arguments", data.get("parameters", {}))
                        tool_call = ToolCall(
                            id=str(uuid.uuid4()),
                            name=data["name"],
                            arguments=arguments,
                        )
                        tool_calls.append(tool_call)
                        lines_to_remove.append(line)
                        logger.info(f"Parsed Qwen2 tool call: {tool_call.name}")
                except json.JSONDecodeError:
                    # Try to recover malformed JSON
                    recovered = self._try_recover_json(stripped)
                    if recovered:
                        tool_calls.append(recovered)
                        lines_to_remove.append(line)

        # Also check for markdown-wrapped JSON (any language: ```json, ```bash, etc.)
        if not tool_calls:
            # Multiple patterns to catch various code block formats
            code_block_patterns = [
                # Standard: ```lang\ncontent\n```
                re.compile(r"```\w*\s*\n([\s\S]*?)\n```", re.MULTILINE),
                # No trailing newline: ```lang\ncontent```
                re.compile(r"```\w*\s*\n([\s\S]*?)```", re.MULTILINE),
                # Content on same line: ```lang content```
                re.compile(r"```\w*\s+(\{[^`]*\})```", re.MULTILINE),
                # Most permissive: anything between triple backticks
                re.compile(r"```(?:\w*\s*)?([\s\S]*?)```", re.MULTILINE),
            ]

            for pattern in code_block_patterns:
                if tool_calls:
                    break  # Already found tool calls
                for block_content in pattern.findall(content):
                    block_stripped = block_content.strip()
                    if not block_stripped:
                        continue
                    logger.debug(f"Found code block ({pattern.pattern}): {block_stripped}")
                    # Check if it looks like JSON with a name field
                    if block_stripped.startswith("{") and '"name"' in block_stripped:
                        try:
                            data = json.loads(block_stripped)
                            if "name" in data:
                                arguments = data.get("arguments", data.get("parameters", {}))
                                tool_call = ToolCall(
                                    id=str(uuid.uuid4()),
                                    name=data["name"],
                                    arguments=arguments,
                                )
                                tool_calls.append(tool_call)
                                logger.info(f"Parsed Qwen2 markdown tool call: {tool_call.name}")
                        except json.JSONDecodeError as e:
                            logger.debug(f"JSON parse failed in code block: {e}")
                            # Try recovery
                            recovered = self._try_recover_json(block_stripped)
                            if recovered:
                                tool_calls.append(recovered)
                                logger.info(f"Recovered Qwen2 markdown tool call: {recovered.name}")

        # Check for function-call syntax: tool_name({...}) in code blocks or bare
        # Pattern: tool.name({ ... }) or tool.name({"key": "value", ...})
        if not tool_calls:
            # Use pattern that captures content between { and } more robustly
            func_call_pattern = re.compile(
                r"([a-zA-Z_][a-zA-Z0-9_.]*)\s*\(\s*(\{.*?\})\s*\)",
                re.DOTALL,
            )
            for match in func_call_pattern.finditer(content):
                func_name = match.group(1)
                args_json = match.group(2)

                # Validate we have balanced braces
                if args_json.count("{") != args_json.count("}"):
                    continue

                # Skip if it looks like Python/JS code (has = or other code patterns)
                if "=" in args_json and ":" not in args_json:
                    continue

                try:
                    # Try to parse the arguments as JSON
                    arguments = json.loads(args_json)
                    if isinstance(arguments, dict):
                        tool_call = ToolCall(
                            id=str(uuid.uuid4()),
                            name=func_name,
                            arguments=arguments,
                        )
                        tool_calls.append(tool_call)
                        logger.info(f"Parsed Qwen2 function-call syntax: {func_name}")
                except json.JSONDecodeError:
                    # Try to fix common JSON issues
                    fixed_args = self._fix_json_syntax(args_json)
                    if fixed_args:
                        try:
                            arguments = json.loads(fixed_args)
                            if isinstance(arguments, dict):
                                tool_call = ToolCall(
                                    id=str(uuid.uuid4()),
                                    name=func_name,
                                    arguments=arguments,
                                )
                                tool_calls.append(tool_call)
                                logger.info(f"Parsed Qwen2 function-call (recovered): {func_name}")
                        except json.JSONDecodeError:
                            pass

        # Check for Python-style function calls: tool_name(key="value", key2="value2")
        # Only match known MCP tool prefixes to avoid false positives with regular code
        if not tool_calls:
            # Pattern for Python kwargs: tool.name(arg="value") or tool.name(arg='value')
            python_call_pattern = re.compile(
                r"([a-zA-Z_][a-zA-Z0-9_.]*)\s*\(\s*([^)]+)\s*\)",
                re.DOTALL,
            )
            for match in python_call_pattern.finditer(content):
                func_name = match.group(1)
                args_str = match.group(2).strip()

                # Skip if it doesn't look like a tool name (must have a dot for namespaced tools)
                if "." not in func_name:
                    continue

                # Only match known MCP tool prefixes - skip stdlib calls like argparse.*, parser.*, etc.
                if not self._is_known_tool_prefix(func_name):
                    continue

                # Skip if it looks like a regular function call (no = signs)
                if "=" not in args_str:
                    continue

                # Try to parse Python kwargs
                arguments = self._parse_python_kwargs(args_str)
                if arguments:
                    tool_call = ToolCall(
                        id=str(uuid.uuid4()),
                        name=func_name,
                        arguments=arguments,
                    )
                    tool_calls.append(tool_call)
                    logger.info(f"Parsed Qwen2 Python-style call: {func_name}")

        # Clean content
        cleaned = content
        for line in lines_to_remove:
            cleaned = cleaned.replace(line, "")

        # Clean markdown blocks if we parsed from them
        if tool_calls:
            # Clean JSON markdown blocks
            markdown_pattern = re.compile(
                r"```(?:\w+)?\s*\n?\s*\{[^`]*\"name\"\s*:\s*\"[^\"]+\"[^`]*\}\s*\n?\s*```",
                re.DOTALL,
            )
            cleaned = markdown_pattern.sub("", cleaned)

            # Clean function-call syntax blocks
            func_block_pattern = re.compile(
                r"```(?:\w+)?\s*\n[^`]*?[a-zA-Z_][a-zA-Z0-9_.]*\s*\(\s*\{[^}]*\}\s*\)[^`]*```",
                re.DOTALL,
            )
            cleaned = func_block_pattern.sub("", cleaned)

            # Clean bare function-call lines (JSON style)
            for tc in tool_calls:
                func_pattern = re.compile(
                    re.escape(tc.name) + r"\s*\(\s*\{[^}]*\}\s*\)",
                    re.DOTALL,
                )
                cleaned = func_pattern.sub("", cleaned)

            # Clean bare function-call lines (Python kwarg style)
            for tc in tool_calls:
                func_pattern = re.compile(
                    re.escape(tc.name) + r"\s*\([^)]*\)",
                    re.DOTALL,
                )
                cleaned = func_pattern.sub("", cleaned)

            # Clean code blocks containing tool calls
            tool_block_pattern = re.compile(
                r"```\w*\s*\n?"
                + r"[^`]*?"
                + r"[a-zA-Z_][a-zA-Z0-9_.]*\.[a-zA-Z_][a-zA-Z0-9_]*\s*\([^)]*\)"
                + r"[^`]*?"
                + r"\n?```",
                re.DOTALL,
            )
            cleaned = tool_block_pattern.sub("", cleaned)

        cleaned = cleaned.strip()

        if tool_calls:
            logger.info(f"Parsed {len(tool_calls)} tool calls: {[tc.name for tc in tool_calls]}")
        else:
            logger.debug("No tool calls parsed")
            logger.debug(f"  - Contains '{{': {'{' in content}")
            logger.debug(f"  - Contains '\"name\"': {'\"name\"' in content}")

        return cleaned, tool_calls

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
        arguments = {}

        # Pattern for key=value pairs
        # Handles: key="value", key='value', key=value
        kwarg_pattern = re.compile(r'(\w+)\s*=\s*(?:"([^"]*)"|\'([^\']*)\'|(\S+))')

        for match in kwarg_pattern.finditer(args_str):
            key = match.group(1)
            # Value is in one of the capture groups
            value = match.group(2) or match.group(3) or match.group(4)

            if value is not None:
                # Try to convert to appropriate type
                if value.lower() == "true":
                    arguments[key] = True
                elif value.lower() == "false":
                    arguments[key] = False
                elif value.lower() == "none" or value.lower() == "null":
                    arguments[key] = None
                else:
                    try:
                        arguments[key] = int(value)
                    except ValueError:
                        try:
                            arguments[key] = float(value)
                        except ValueError:
                            arguments[key] = value

        return arguments if arguments else None

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

Use this information to respond to the user."""

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
            code_block_pattern = re.compile(r"```\w*\s*([\s\S]*?)```")
            for block in code_block_pattern.findall(content):
                block_stripped = block.strip()

                # Check for JSON-style tool call
                if block_stripped.startswith("{") and '"name"' in block_stripped:
                    logger.debug("Found unparsed JSON tool call in code block - continuing loop")
                    return False

                # Check for Python-style function call: tool.name(args)
                # Only match known MCP tool prefixes to avoid false positives with explained code
                python_call_pattern = re.compile(
                    r"([a-zA-Z_][a-zA-Z0-9_.]*\.[a-zA-Z_][a-zA-Z0-9_]*)\s*\([^)]*\)"
                )
                for match in python_call_pattern.finditer(block_stripped):
                    func_name = match.group(1)
                    # Only trigger if it matches a known tool prefix
                    if self._is_known_tool_prefix(func_name):
                        logger.debug(f"Found unparsed tool call: {func_name} - continuing loop")
                        return False

        return True


# Register adapter
register_adapter("qwen2", Qwen2Adapter)
